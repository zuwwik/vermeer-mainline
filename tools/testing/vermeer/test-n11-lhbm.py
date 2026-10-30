#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise N11 LHBM calibration and DSI helpers without device access.

Optionally compare the emitted commands against an independently extracted stock
panel property JSON (path -> hex string). Vendor firmware is not bundled.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--vendor-panel-properties", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
source = (root / "drivers/gpu/drm/panel/panel-n11-42-02-0a-dsc.c").read_text()
header = (root / "drivers/gpu/drm/panel/panel-n11-42-02-0a-dsc-lhbm.h").read_text()


def function(name):
    match = re.search(r"static[\w\s*]+?\b" + name + r"\s*\([^;{}]*\)\s*\{", source)
    opening = match.end() - 1
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "lhbm.h"
#define MIPI_DSI_MODE_LPM 1
#define DIV_ROUND_UP(x,y) (((x)+(y)-1)/(y))
struct mipi_dsi_device { unsigned long mode_flags; };
struct mipi_dsi_multi_context { struct mipi_dsi_device *dsi; int accum_err; };
struct n11_42_02_0a_dsc { struct mipi_dsi_device *dsi; unsigned refresh_rate; bool fingerprint_hbm; };
static uint8_t commands[64][32];
static unsigned lengths[64], writes, fail_at, slept;
static bool bad_calibration;
static int mipi_dsi_dcs_write_buffer(struct mipi_dsi_device *dsi, const void *data, size_t size)
{
    assert(!(dsi->mode_flags & MIPI_DSI_MODE_LPM) && size <= 32 && writes < 64);
    memcpy(commands[writes], data, size); lengths[writes++] = size;
    return writes == fail_at ? -EIO : (int)size;
}
static void mipi_dsi_dcs_write_buffer_multi(struct mipi_dsi_multi_context *ctx, const void *data, size_t size)
{
    if (ctx->accum_err) return;
    int ret = mipi_dsi_dcs_write_buffer(ctx->dsi, data, size);
    if (ret < 0) ctx->accum_err = ret;
}
#define mipi_dsi_dcs_write_seq_multi(ctx, ...) do { \
    const u8 data[] = { __VA_ARGS__ }; \
    mipi_dsi_dcs_write_buffer_multi(ctx, data, sizeof(data)); \
} while (0)
static void msleep(unsigned value) { slept = value; }
static int n11_42_02_0a_dsc_lhbm_parameters(struct n11_42_02_0a_dsc *ctx,
    struct n11_lhbm_parameters *parameters)
{
    (void)ctx;
    const u8 calibration[] = { 0x0b,0x06,0x09,0x97,0x0b,0x69 };
    if (bad_calibration) return -ENODATA;
    return n11_lhbm_parameters(2047, calibration, parameters);
}
"""
suffix = r"""
int main(void)
{
    struct n11_lhbm_parameters p;
    u8 calibration[] = { 0x0b,0x06,0x09,0x97,0x0b,0x69 };
    assert(!n11_lhbm_parameters(2047, calibration, &p));
    assert(p.alpha == 0x1fff && p.dbv == 8188);
    assert(get_unaligned_be16(p.rgb) == 0x0ba4);
    assert(get_unaligned_be16(p.rgb + 2) == 0x0a20);
    assert(get_unaligned_be16(p.rgb + 4) == 0x0c0c);
    assert(!n11_lhbm_parameters(2294, calibration, &p));
    assert(get_unaligned_be16(p.rgb) == 0x0b96);
    assert(!n11_lhbm_parameters(3900, calibration, &p));
    assert(get_unaligned_be16(p.rgb) == 0x0b40);
    assert(!n11_lhbm_parameters(4095, calibration, &p));
    assert(get_unaligned_be16(p.rgb) == 0x0b40);
    for (unsigned i = 0; i <= 4095; i++) {
        assert(!n11_lhbm_parameters(i, calibration, &p));
        assert(p.alpha >= 0x1000 && p.alpha <= 0x1fff && p.dbv <= 16380);
    }
    assert(n11_lhbm_parameters(4096, calibration, &p) == -EINVAL);
    calibration[0] = calibration[1] = 0;
    assert(n11_lhbm_parameters(2047, calibration, &p) == -ENODATA);
    calibration[0] = 0x10;
    assert(n11_lhbm_parameters(2047, calibration, &p) == -ENODATA);
    calibration[0] = 0xf; calibration[1] = 0xff;
    assert(n11_lhbm_parameters(2047, calibration, &p) == -ERANGE);
    struct mipi_dsi_device dsi = { .mode_flags = MIPI_DSI_MODE_LPM | 16 };
    struct n11_42_02_0a_dsc ctx = { .dsi = &dsi, .refresh_rate = 120 };
    assert(!n11_42_02_0a_dsc_lhbm_on(&ctx));
    assert(ctx.fingerprint_hbm && slept == 34 && writes == 32 && dsi.mode_flags == 17);
    for (unsigned i = 0; i < writes; i++) {
        printf("DCS "); for (unsigned j = 0; j < lengths[i]; j++) printf("%02x", commands[i][j]); puts("");
    }
    writes = 0; assert(!n11_42_02_0a_dsc_lhbm_off(&ctx));
    assert(!ctx.fingerprint_hbm && writes == 3 && dsi.mode_flags == 17);
    for (unsigned failure = 1; failure <= 32; failure++) {
        writes = slept = 0; fail_at = failure;
        assert(n11_42_02_0a_dsc_lhbm_on(&ctx) == -EIO);
        assert(!ctx.fingerprint_hbm && !slept && writes == failure + 3 && dsi.mode_flags == 17);
        assert(commands[writes - 2][0] == 0x86 && commands[writes - 1][0] == 0x87);
    }
    for (unsigned failure = 1; failure <= 3; failure++) {
        writes = 0; fail_at = failure; ctx.fingerprint_hbm = true;
        assert(n11_42_02_0a_dsc_lhbm_off(&ctx) == -EIO);
        assert(ctx.fingerprint_hbm && writes == 3 && dsi.mode_flags == 17);
    }
    fail_at = writes = 0; bad_calibration = true; ctx.fingerprint_hbm = false;
    assert(n11_42_02_0a_dsc_lhbm_on(&ctx) == -ENODATA);
    assert(!writes && !ctx.fingerprint_hbm && dsi.mode_flags == 17);
    puts("calibration bounds, original DSI helpers and all transaction failure paths passed");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="vermeer-lhbm-") as tmp:
    d = Path(tmp)
    (d / "linux").mkdir()
    (d / "linux/types.h").write_text("#include <stdint.h>\ntypedef uint8_t u8; typedef uint16_t u16;\n")
    (d / "linux/errno.h").write_text("#include_next <linux/errno.h>\n")
    (d / "linux/array_size.h").write_text("#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))\n")
    (d / "linux/unaligned.h").write_text("""static inline unsigned get_unaligned_be16(const void *p) {
const uint8_t *b = p; return ((unsigned)b[0] << 8) | b[1]; }
static inline void put_unaligned_be16(unsigned value, void *p) {
uint8_t *b = p; b[0] = value >> 8; b[1] = value; }
""")
    (d / "lhbm.h").write_text(header)
    (d / "test.c").write_text(prefix + "\n" + function("n11_42_02_0a_dsc_lhbm_off") + "\n" +
                              function("n11_42_02_0a_dsc_lhbm_on") + suffix)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                    "-fsanitize=address,undefined", "-I" + tmp, str(d / "test.c"),
                    "-o", str(d / "test")], check=True)
    output = subprocess.check_output([str(d / "test")], text=True)
    print(output.splitlines()[-1])
    commands = [bytes.fromhex(line[4:]) for line in output.splitlines() if line.startswith("DCS ")]
    if args.vendor_panel_properties:
        props = json.loads(args.vendor_panel_properties.read_text())
        matching = [bytes.fromhex(v) for k, v in props.items()
                    if k.endswith("/mi,mdss-dsi-local-hbm-normal-white-1000nit-command")]
        assert matching, "no independent stock white-1000nit table found"
        for blob in matching:
            original, offset = [], 0
            while offset < len(blob):
                size = int.from_bytes(blob[offset + 5:offset + 7], "big")
                assert size and offset + 7 + size <= len(blob)
                original.append(blob[offset + 7:offset + 7 + size])
                offset += 7 + size
            assert len(original) == len(commands) == 32
            # Only the three payloads listed in the stock update descriptors differ.
            original[25] = bytes.fromhex("df1ffc")
            original[27] = bytes.fromhex("d00ba40a200c0c")
            original[29] = bytes.fromhex("871fff")
            assert original == commands, "native DSI sequence differs from stock"
        print(f"all 32 commands match {len(matching)} independent stock mode tables")
