#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check a compiled DTB's RPMh constraints against its driver's selector tables.

Requires pylibfdt (also installed by dtschema). No device access is performed.
"""
import argparse
from pathlib import Path
import re
import struct
import sys

import libfdt


def voltage_tables(source):
    tables = {}
    pattern = r"static const struct rpmh_vreg_hw_data (\w+) = \{(.*?)\n\};"
    for name, body in re.findall(pattern, source, re.S):
        values = set()
        for raw in re.findall(r"REGULATOR_LINEAR_RANGE\(([^)]+)\)", body):
            base, first, last, step = (int(v.strip(), 0) for v in raw.split(","))
            values.update(base + (s - first) * step for s in range(first, last + 1))
        if values:
            tables[name] = values
    return tables


def pmic_tables(source):
    arrays = {}
    pattern = r"static const struct rpmh_vreg_init_data (\w+)\[\] = \{(.*?)\n\};"
    for name, body in re.findall(pattern, source, re.S):
        arrays[name] = dict(re.findall(
            r'RPMH_VREG\("([^"\n]+)",\s*\w+,\s*\d+,\s*&?(\w+),', body))
    return {compatible: arrays[name] for compatible, name in re.findall(
        r'\.compatible\s*=\s*"([^"]+)",\s*\.data\s*=\s*(\w+)', source)
        if name in arrays}


def property_bytes(tree, node, name):
    try:
        return bytes(tree.getprop(node, name))
    except libfdt.FdtException as exc:
        if exc.err == -libfdt.NOTFOUND:
            return None
        raise


def children(tree, node):
    child = tree.first_subnode(node, libfdt.QUIET_NOTFOUND)
    while child >= 0:
        yield child
        child = tree.next_subnode(child, libfdt.QUIET_NOTFOUND)


def available(tree, node):
    status = property_bytes(tree, node, "status")
    return status is None or status.rstrip(b"\0") in (b"okay", b"ok")


def walk(tree, node=0, path=""):
    if not available(tree, node):
        return
    yield node, path or "/"
    for child in children(tree, node):
        yield from walk(tree, child, path + "/" + tree.get_name(child))


def check(dtb, source):
    tree = libfdt.Fdt(dtb.read_bytes())
    hardware, pmics = voltage_tables(source), pmic_tables(source)
    checked, errors = 0, []
    for node, path in walk(tree):
        compatibles = (property_bytes(tree, node, "compatible") or b"").split(b"\0")
        bank = next((pmics[c.decode()] for c in compatibles if c.decode() in pmics), None)
        if bank is None:
            continue
        for child in children(tree, node):
            if not available(tree, child):
                continue
            lo = property_bytes(tree, child, "regulator-min-microvolt")
            hi = property_bytes(tree, child, "regulator-max-microvolt")
            if lo is None or hi is None:
                continue
            name = tree.get_name(child)
            hw = bank.get(name)
            if hw not in hardware:
                errors.append(f"{path}/{name}: no parsed voltage table for {hw}")
                continue
            lo, hi = struct.unpack(">I", lo)[0], struct.unpack(">I", hi)[0]
            allowed = sorted(v for v in hardware[hw] if lo <= v <= hi)
            checked += 1
            if not allowed:
                nearest = sorted(hardware[hw], key=lambda v: abs(v - lo))[:2]
                errors.append(f"{path}/{name}: {lo}..{hi} uV has no selector; "
                              f"nearest {sorted(nearest)}")
    if not checked:
        errors.append("no RPMh voltage constraints checked")
    return checked, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dtb", type=Path)
    parser.add_argument("--baseline", type=Path,
                        help="report existing constraints separately and fail on new errors")
    parser.add_argument("--driver", type=Path,
                        default=Path(__file__).resolve().parents[3] /
                        "drivers/regulator/qcom-rpmh-regulator.c")
    args = parser.parse_args()
    checked, errors = check(args.dtb, args.driver.read_text())
    existing = set()
    if args.baseline:
        _, baseline_errors = check(args.baseline, args.driver.read_text())
        existing = set(baseline_errors)
    for error in errors:
        prefix = "existing: " if error in existing else "error: "
        print(prefix + error, file=sys.stderr)
    new_errors = set(errors) - existing
    print(f"Checked {checked} RPMh voltage constraints; "
          f"new errors: {len(new_errors)}; existing: {len(set(errors) & existing)}")
    return bool(new_errors)


if __name__ == "__main__":
    sys.exit(main())
