#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual QTEE pool helpers with allocation/bridge fault injection."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[3] /
                    "drivers/tee/qcomtee/shm.c")
args = parser.parse_args()
source = args.source.read_text()

def function(name):
    match = re.search(r"static[\w\s*]+?\b" + re.escape(name) +
                      r"\s*\([^;{}]*\)\s*\{", source)
    if not match:
        raise ValueError(name)
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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#define IS_ENABLED(x) (x)
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define MAX_PAGE_ORDER 10
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define GFP_KERNEL 0
typedef uint8_t u8;
struct page { unsigned char *address; };
struct cma { int present; };
struct tee_context { int unused; };
struct tee_shm {
    struct tee_context *ctx;
    uintptr_t paddr;
    void *kaddr;
    size_t size;
    struct page **pages;
    size_t num_pages;
    uint64_t sec_world_id;
};
struct tee_shm_pool { void *private_data; };
static bool fail_cma, fail_array, fail_address, fail_bridge, wipe_required;
static unsigned arrays, cma_live, bridge_live, buddy_calls, buddy_frees;
static struct page *allocation;
static unsigned char *allocation_data;
static size_t allocation_pages;
static unsigned get_order(size_t size)
{
    size_t pages = (size - 1) >> PAGE_SHIFT;
    unsigned order = 0;
    while (pages) { pages >>= 1; order++; }
    return order;
}
static void *page_array_alloc(size_t count, size_t size)
{
    if (fail_array) return NULL;
    void *p = calloc(count, size);
    assert(p); arrays++;
    return p;
}
#define kzalloc_objs(obj, count) page_array_alloc((count), sizeof(obj))
static void kfree(void *p)
{
    if (p) { assert(arrays); arrays--; free(p); }
}
static struct page *cma_alloc(struct cma *area, size_t count, unsigned align, bool no_warn)
{
    assert(area && count && align == 0 && no_warn);
    if (fail_cma) return NULL;
    assert(!cma_live);
    allocation = calloc(count, sizeof(*allocation));
    allocation_data = malloc(count * PAGE_SIZE);
    assert(allocation && allocation_data);
    memset(allocation_data, 0xa5, count * PAGE_SIZE);
    for (size_t i = 0; i < count; i++) allocation[i].address = allocation_data + i * PAGE_SIZE;
    allocation_pages = count; cma_live++;
    return allocation;
}
static bool cma_release(struct cma *area, struct page *page, size_t count)
{
    assert(area && cma_live == 1 && page == allocation && count == allocation_pages);
    assert(!bridge_live);
    if (wipe_required) {
        for (size_t i = 0; i < count * PAGE_SIZE; i++) assert(allocation_data[i] == 0);
        wipe_required = false;
    }
    free(allocation); free(allocation_data);
    allocation = NULL; allocation_data = NULL; allocation_pages = 0; cma_live--;
    return true;
}
static void *page_address(struct page *page) { return fail_address ? NULL : page->address; }
static uintptr_t page_to_phys(struct page *page) { return (uintptr_t)page->address; }
static struct page *virt_to_page(void *address)
{
    size_t offset = (unsigned char *)address - allocation_data;
    assert(offset < allocation_pages * PAGE_SIZE && !(offset % PAGE_SIZE));
    return allocation + offset / PAGE_SIZE;
}
static void memzero_explicit(void *p, size_t size) { memset(p, 0, size); }
static int qcom_tzmem_shm_bridge_create(uintptr_t pa, size_t size, uint64_t *cookie)
{
    assert(pa && size);
    if (fail_bridge) return -EIO;
    *cookie = 0x1234; bridge_live++;
    return 0;
}
static void qcom_tzmem_shm_bridge_delete(uint64_t cookie)
{
    assert(cookie == 0x1234 && bridge_live); bridge_live--;
}
static int tee_dyn_shm_alloc_helper(struct tee_shm *shm, size_t size, size_t align,
    int (*register_fn)(struct tee_context *, struct tee_shm *, struct page **, size_t, unsigned long))
{
    (void)align; (void)register_fn;
    assert(get_order(size) <= MAX_PAGE_ORDER && size);
    buddy_calls++; shm->size = size; shm->kaddr = malloc(size); assert(shm->kaddr);
    return 0;
}
static void tee_dyn_shm_free_helper(struct tee_shm *shm,
    int (*unregister_fn)(struct tee_context *, struct tee_shm *))
{
    (void)unregister_fn;
    assert(get_order(shm->size) <= MAX_PAGE_ORDER);
    buddy_frees++; free(shm->kaddr); shm->kaddr = NULL;
}
"""
helpers = "\n".join(function(name) for name in [
    "qcomtee_shm_register", "qcomtee_shm_unregister", "qcomtee_large_shm_alloc",
    "pool_op_alloc", "pool_op_free"])
suffix = r"""
static void assert_clean(void) { assert(!arrays && !cma_live && !bridge_live); }
int main(void)
{
    struct cma area = { .present = 1 };
    struct tee_shm_pool pool = { .private_data = &area };
    struct tee_shm shm = {};
    assert(pool_op_alloc(&pool, &shm, 0, 0) == -EINVAL);
    assert(pool_op_alloc(&pool, &shm, SIZE_MAX, 0) == -EINVAL);
    size_t boundary = PAGE_SIZE << MAX_PAGE_ORDER;
    assert(pool_op_alloc(&pool, &shm, boundary, 0) == 0);
    assert(buddy_calls == 1); pool_op_free(&pool, &shm); assert(buddy_frees == 1);
#if CONFIG_DMA_CMA
    size_t size = (15UL << 20) + 17;
    assert(pool_op_alloc(&pool, &shm, size, 0) == 0);
    assert(shm.size == PAGE_ALIGN(size) && shm.num_pages == shm.size / PAGE_SIZE);
    assert(shm.paddr == (uintptr_t)shm.kaddr);
    for (size_t i = 0; i < shm.num_pages; i++) {
        assert(shm.pages[i]->address == (unsigned char *)shm.kaddr + i * PAGE_SIZE);
        assert(((unsigned char *)shm.kaddr)[i * PAGE_SIZE] == 0);
    }
    memset(shm.kaddr, 0x77, shm.size); wipe_required = true;
    /* Check the wipe before the CMA allocator gets the memory back. */
    pool_op_free(&pool, &shm); assert_clean();
    shm = (struct tee_shm){};
    pool.private_data = NULL;
    assert(pool_op_alloc(&pool, &shm, size, 0) == -ENOMEM); assert_clean();
    pool.private_data = &area;
    fail_cma = true;
    assert(pool_op_alloc(&pool, &shm, size, 0) == -ENOMEM); assert_clean(); fail_cma = false;
    fail_address = true;
    assert(pool_op_alloc(&pool, &shm, size, 0) == -ENOMEM); assert_clean(); fail_address = false;
    fail_array = true;
    assert(pool_op_alloc(&pool, &shm, size, 0) == -ENOMEM); assert_clean(); fail_array = false;
    fail_bridge = true;
    assert(pool_op_alloc(&pool, &shm, size, 0) == -EIO); assert_clean(); fail_bridge = false;
    assert(pool_op_alloc(&pool, &shm, boundary + 1, 0) == 0);
    assert(shm.num_pages == (boundary >> PAGE_SHIFT) + 1);
    pool_op_free(&pool, &shm); assert_clean();
#else
    assert(pool_op_alloc(&pool, &shm, boundary + 1, 0) == -ENOMEM); assert_clean();
#endif
    puts("large QTEE allocation, boundary, failure cleanup and no-CMA tests passed");
    return 0;
}
"""
# Confirm that clearing precedes the actual allocator release on success.
free_body = function("pool_op_free")
assert free_body.index("memzero_explicit") < free_body.index("cma_release")
with tempfile.TemporaryDirectory(prefix="vermeer-qtee-shm-") as tmp:
    tmp = Path(tmp)
    (tmp / "test.c").write_text(prefix + helpers + suffix)
    for cma in [1, 0]:
        binary = tmp / f"test-{cma}"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-unused-function",
                        "-Wno-unused-variable", "-fsanitize=address,undefined",
                        f"-DCONFIG_DMA_CMA={cma}", str(tmp / "test.c"), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)
