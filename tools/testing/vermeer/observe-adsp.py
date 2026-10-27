#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Record bounded ADSP diagnostics without restarting firmware or changing audio."""
import argparse
import hashlib
import json
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import queue
import selectors
import signal
import subprocess
import sys
import threading
import time


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError as error:
        return {"error": str(error)}


def health():
    result = {"adsp": read("/sys/class/remoteproc/remoteproc1/state"),
              "cards": read("/proc/asound/cards")}
    battery = Path("/sys/class/power_supply/qcom-battmgr-bat")
    for name in ("present", "capacity", "temp", "status", "current_now", "voltage_now"):
        result[name] = read(battery / name)
    memory = read("/proc/meminfo")
    result["memory"] = [line for line in memory.splitlines()
                        if line.startswith(("MemAvailable:", "Slab:", "SUnreclaim:"))
                        ] if isinstance(memory, str) else memory
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=3600)
    parser.add_argument("--directory", type=Path, default=Path("/var/log/vermeer-adsp"))
    parser.add_argument("--no-firmware-log", action="store_true")
    args = parser.parse_args()
    if not 2 <= args.seconds <= 3600:
        parser.error("--seconds must be in 2..3600")
    if os.geteuid() != 0:
        parser.error("Run as root on the phone")
    compatible = Path("/proc/device-tree/compatible").read_bytes().split(b"\0")
    if b"xiaomi,vermeer" not in compatible:
        parser.error("This diagnostic is for Xiaomi Vermeer")
    boot = read("/proc/sys/kernel/random/boot_id")
    directory = args.directory / boot
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    logger = logging.getLogger("vermeer-adsp")
    logger.setLevel(logging.INFO)
    handler = RotatingFileHandler(directory / "events.jsonl", maxBytes=8 * 1024 * 1024,
                                  backupCount=1, encoding="utf-8")
    handler.setFormatter(logging.Formatter("%(message)s"))
    logger.addHandler(handler)

    def event(kind, value):
        logger.info(json.dumps({"boottime": time.clock_gettime(time.CLOCK_BOOTTIME),
                                "time": time.time(), "kind": kind, "value": value}))

    stopped = False

    def stop(signum, _frame):
        nonlocal stopped
        event("signal", signum)
        stopped = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    loaded_note = Path("/sys/module/fastrpc/notes/.note.gnu.build-id")
    installed = Path("/usr/lib/modules") / os.uname().release / "kernel/drivers/misc/fastrpc.ko"
    event("start", {"boot": boot, "kernel": list(os.uname()), "duration": args.seconds,
                    "fastrpc_note": loaded_note.read_bytes().hex() if loaded_note.exists() else None,
                    "installed_fastrpc_sha256": hashlib.sha256(installed.read_bytes()).hexdigest()
                    if installed.exists() else None})
    print(directory, flush=True)
    selector = selectors.DefaultSelector()
    children = []
    results = queue.Queue()
    pending = False

    def collect_health():
        try:
            results.put(("health", health()))
        except Exception as error:
            results.put(("health_error", str(error)))
    next_health = 0.0
    deadline = time.clock_gettime(time.CLOCK_BOOTTIME) + args.seconds
    last_sync = 0.0
    try:
        commands = [("kernel", ["journalctl", "-k", "-b", "-f", "--no-pager",
                                "--output=short-monotonic", "--lines=40"])]
        if not args.no_firmware_log:
            commands.append(("charger", [sys.executable, "-u",
                            str(Path(__file__).with_name("read-charger-log.py")),
                            "--seconds", str(args.seconds), "--interval", "5"]))
        for kind, command in commands:
            child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            os.set_blocking(child.stdout.fileno(), False)
            selector.register(child.stdout, selectors.EVENT_READ,
                              {"kind": kind, "buffer": b"", "child": child})
            children.append(child)
        while not stopped and time.clock_gettime(time.CLOCK_BOOTTIME) < deadline:
            now = time.clock_gettime(time.CLOCK_BOOTTIME)
            try:
                kind, value = results.get_nowait()
                event(kind, value)
                pending = False
                if kind == "health":
                    try:
                        low_battery = (int(value["capacity"]) <= 15 and
                                       int(value["current_now"]) < 0)
                    except (TypeError, ValueError, KeyError):
                        low_battery = False
                    if low_battery:
                        event("stop_reason", "Battery at or below 15% with net discharge")
                        break
            except queue.Empty:
                pass
            if not pending and now >= next_health:
                # A blocked sysfs read must not block the journal recorder.
                threading.Thread(target=collect_health, daemon=True).start()
                pending = True
                next_health = now + 10
            for key, _ in selector.select(timeout=0.5):
                state = key.data
                chunk = os.read(key.fileobj.fileno(), 65536)
                state["buffer"] += chunk
                while b"\n" in state["buffer"]:
                    line, state["buffer"] = state["buffer"].split(b"\n", 1)
                    text = line.decode("utf-8", "replace")
                    event(state["kind"], text)
                    if state["kind"] == "kernel" and "fatal error received:" in text:
                        handler.flush()
                        os.fsync(handler.stream.fileno())
                        deadline = min(deadline, now + 60)
                if not chunk:
                    if state["buffer"]:
                        event(state["kind"], state["buffer"].decode("utf-8", "replace"))
                    selector.unregister(key.fileobj)
                    key.fileobj.close()
                    event(state["kind"] + "_exit", state["child"].poll())
            if now - last_sync >= 10:
                handler.flush()
                os.fsync(handler.stream.fileno())
                last_sync = now
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
        for child in children:
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=3)
        selector.close()
        event("stop", {"boot": read("/proc/sys/kernel/random/boot_id")})
        handler.flush()
        os.fsync(handler.stream.fileno())
        handler.close()


if __name__ == "__main__":
    main()
