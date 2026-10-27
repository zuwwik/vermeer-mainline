#!/usr/bin/env python3
"""Read the stock ADSP charger log using Linux rpmsg_char, without SETs.

Temporarily binds the otherwise unbound PMIC_LOGS_ADSP_APPS log channel.
Unbinds on exit. This firmware closes that channel after a capture; start one
long capture per boot. Does not touch the PMIC_RTR battery transport.
Run as root on the phone. Protocol: drivers/soc/qcom/pmic_pdcharger_ulog.c.
"""
import argparse
import os
from pathlib import Path
import select
import signal
import struct
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--interval", type=float, default=1)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600:
        parser.error("--seconds must be in 1..3600")
    if not 1 <= args.interval <= 60:
        parser.error("--interval must be in 1..60")
    candidates = list(Path("/sys/bus/rpmsg/devices").glob(
        "6800000.remoteproc:*.PMIC_LOGS_ADSP_APPS.*"))
    if len(candidates) != 1:
        raise RuntimeError("Charger log channel is unavailable; capture once after a normal reboot")
    dev = candidates[0]
    if (dev / "driver").exists():
        raise RuntimeError("Charger log channel already has a driver; leaving it alone")
    override = dev / "driver_override"
    old = override.read_text().strip()
    if old not in ("", "(null)"):
        raise RuntimeError(f"Existing driver override: {old}")
    driver = Path("/sys/bus/rpmsg/drivers/rpmsg_chrdev")
    fd = None
    bound = False

    def interrupted(signum, _frame):
        raise InterruptedError(f"Signal {signum}")

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        override.write_text("rpmsg_chrdev\n")
        (driver / "bind").write_text(dev.name + "\n")
        bound = True
        endpoints = list((dev / "rpmsg").glob("rpmsg[0-9]*"))
        if len(endpoints) != 1:
            raise RuntimeError("Expected one rpmsg_char endpoint")
        path = Path("/dev") / endpoints[0].name
        for _ in range(100):
            if path.exists():
                break
            time.sleep(0.01)
        fd = os.open(path, os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
        poller = select.poll()
        poller.register(fd, select.POLLIN | select.POLLERR | select.POLLHUP)
        deadline = time.monotonic() + args.seconds
        request = struct.pack("<4I", 32778, 1, 0x18, 8192)
        while time.monotonic() < deadline:
            if os.write(fd, request) != len(request):
                raise RuntimeError("Short rpmsg write")
            events = poller.poll(2000)
            if not events:
                raise RuntimeError("ADSP charger log request timed out")
            if events[0][1] & (select.POLLERR | select.POLLHUP):
                raise RuntimeError("ADSP charger log channel disconnected")
            data = os.read(fd, 16384)
            if len(data) != 8204 or struct.unpack_from("<3I", data) != (32778, 1, 0x18):
                raise RuntimeError(f"Unexpected charger log reply, {len(data)} bytes")
            text = data[12:].split(b"\0", 1)[0].decode("utf-8", "replace")
            print(f"--- monotonic {time.monotonic():.3f} ---\n{text}", flush=True)
            time.sleep(min(args.interval, max(0, deadline - time.monotonic())))
    finally:
        if fd is not None:
            os.close(fd)
        if bound and dev.exists() and (dev / "driver").exists():
            (driver / "unbind").write_text(dev.name + "\n")
        if override.exists():
            override.write_text("\n")


if __name__ == "__main__":
    main()
