#!/usr/bin/env python3
"""Exercise real FastRPC buffer code after closing its owning user.

Host-only regression test: a DMA-BUF or channel heap can survive its creator.
Uses ASan/UBSan, a fake DMA allocator and the kernel source's actual functions.
An optional source path permits testing the pre-fix driver as well.
"""
from pathlib import Path
import subprocess
import re
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[3]


def block(source, prefix, suffix=""):
    start = source.index(prefix)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + suffix


def main():
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "drivers/misc/fastrpc.c"
    source = source.read_text()
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
#define GFP_KERNEL 0
#define GENMASK_ULL(h,l) ((~0ULL >> (63-(h))) & (~0ULL << (l)))
#define kzalloc_obj(obj) calloc(1, sizeof(obj))
#define kfree free
#define __maybe_unused __attribute__((unused))
struct list_head { void *next, *prev; };
struct mutex { int unused; };
#define INIT_LIST_HEAD(p) ((void)(p))
#define mutex_init(p) ((void)(p))
#define mutex_destroy(p) ((void)(p))
struct device { unsigned int refs; };
static void get_device(struct device *d) { d->refs++; }
static void put_device(struct device *d) { assert(d->refs > 1); d->refs--; }
struct fastrpc_soc_data { unsigned int sid_pos; };
struct fastrpc_channel_ctx { const struct fastrpc_soc_data *soc_data; };
struct fastrpc_session_ctx { unsigned int sid; };
struct fastrpc_user {
    struct fastrpc_channel_ctx *cctx;
    struct fastrpc_session_ctx *sctx;
};
static dma_addr_t next_handle;
static unsigned int frees;
static void *dma_alloc_coherent(struct device *d, u64 size,
                               dma_addr_t *handle, int flags)
{
    (void)d; (void)flags;
    *handle = next_handle;
    return calloc(1, size);
}
static void dma_free_coherent(struct device *d, u64 size,
                             void *virt, dma_addr_t handle)
{
    assert(d && size == 4096 && handle == next_handle);
    frees++;
    free(virt);
}
'''
    harness += block(source, "struct fastrpc_buf {", ";\n")
    # The old decoder is used by the unfixed driver; retained to reproduce UAF.
    for name in ("fastrpc_ipa_to_dma_addr", "fastrpc_sid_offset",
                 "fastrpc_buf_free", "__fastrpc_buf_alloc", "fastrpc_buf_alloc"):
        start = re.search(r"\b" + name + r"\(", source).start()
        line = source.rfind("\n", 0, start) + 1
        harness += block(source, source[line:start + len(name) + 1]) + "\n"
    harness += r'''
int main(void)
{
    unsigned int width;
    for (width = 32; width <= 36; width += 4) {
        struct device dev = {.refs = 1};
        struct fastrpc_soc_data *soc = malloc(sizeof(*soc));
        struct fastrpc_channel_ctx *ctx = malloc(sizeof(*ctx));
        struct fastrpc_session_ctx *session = malloc(sizeof(*session));
        struct fastrpc_user *user = malloc(sizeof(*user));
        struct fastrpc_buf *buf = NULL;
        soc->sid_pos = width;
        ctx->soc_data = soc;
        session->sid = 7;
        user->sctx = session;
        user->cctx = ctx;
        next_handle = width == 32 ? 0x81234000ULL : 0x381234000ULL;
        assert(!fastrpc_buf_alloc(user, &dev, 4096, &buf));
        assert(buf->dma_addr == (next_handle + (7ULL << width)));
        /* The exported buffer survives its creating FastRPC descriptor. */
        free(user);
        free(session);
        free(ctx);
        free(soc);
        fastrpc_buf_free(buf);
        assert(dev.refs == 1);
    }
    assert(frees == 2);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="vermeer-fastrpc-buffer-") as tmp:
        cfile, binary = Path(tmp) / "buffer.c", Path(tmp) / "buffer"
        cfile.write_text(harness)
        subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra",
                        "-Wno-unused-function", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: original 32/34-bit DMA handles survive owner teardown; device references balanced")


if __name__ == "__main__":
    main()
