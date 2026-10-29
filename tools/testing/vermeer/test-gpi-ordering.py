#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real GPI helpers with delayed DMA-memory visibility at relaxed MMIO."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path, nargs='?',
                    default=Path(__file__).resolve().parents[3] /
                    'drivers/dma/qcom/gpi.c')
args = parser.parse_args()
source = args.source.read_text()
def function(name):
    match = re.search(r'static[\w\s*]+?\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', source)
    if match is None:
        raise ValueError(name)
    opening = match.end() - 1
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#define __iomem
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define smp_wmb() ((void)0)
#define BIT(x) (1U << (x))
#define MAX_CHANNELS_PER_GPII 2
#define CMD_TIMEOUT_MS 250
#define STATE_IGNORE 99
#define IS_CHAN_CMD(x) ((x) == GPI_CH_CMD_START)
#define GPII_n_CH_CMD(op,ch) (((op) << 24) | (ch))
#define GPII_n_EV_CMD(op,ch) (((op) << 24) | (ch))
#define XFER_COMPLETE_EV_TYPE 1
#define STALE_EV_TYPE 2
#define IMMEDIATE_DATA_EV_TYPE 3
#define QUP_NOTIF_EV_TYPE 4
#define msecs_to_jiffies(x) (x)
typedef uint32_t u32;
typedef uintptr_t phys_addr_t;
enum gpi_cmd { GPI_CH_CMD_START, GPI_EV_CMD_ALLOCATE, GPI_MAX_CMD };
static const struct { u32 opcode, state; } gpi_cmd_info[] = {{1, 1}, {0, 1}};
struct completion { unsigned done; };
struct gpi_ring { void *base, *rp, *wp; phys_addr_t phys_addr; unsigned len, el_size; };
struct gpii;
struct gchan { struct gpii *gpii; u32 chid, ch_state; void *ch_cntxt_db_reg, *ch_cmd_reg; };
struct gpii {
    struct gpi_ring ev_ring;
    void *ev_cntxt_db_reg, *ev_ring_rp_lsb_reg, *ev_cmd_reg, *ieob_clr_reg;
    struct gchan gchan[2];
    struct completion cmd_completion;
    enum gpi_cmd gpi_cmd;
    u32 ev_state;
};
struct xfer_compl_event { u32 chid, type, value; };
struct immediate_data_event { u32 chid, type, value; };
union gpi_event {
    struct xfer_compl_event xfer_compl_event;
    struct immediate_data_event immediate_data_event;
};
static u32 reg_tx, reg_ev, reg_rp, reg_cmd, reg_ev_cmd, reg_clear;
static struct gpii *active;
static union gpi_event cpu_events[4], device_events[4];
static unsigned producer, callbacks;
static bool writes_visible, descriptor_seen, ack_state_seen, event_phase;
static u32 readl_relaxed(void *address) {
    if (address == &reg_rp)
        return active->ev_ring.phys_addr + producer * sizeof(cpu_events[0]);
    return *(u32 *)address;
}
static u32 readl(void *address) {
    u32 value = readl_relaxed(address);
    if (address == &reg_rp)
        memcpy(cpu_events, device_events, producer * sizeof(cpu_events[0]));
    return value;
}
static void writel_relaxed(u32 value, void *address) {
    *(u32 *)address = value;
    if (address == &reg_tx)
        descriptor_seen = writes_visible;
    if (address == &reg_ev) {
        ack_state_seen = writes_visible;
        if (event_phase && producer == 1)
            producer = 2; /* another DMA event arrives during the first drain */
    }
    if (address == &reg_cmd || address == &reg_ev_cmd) {
        active->gchan[0].ch_state = 1;
        active->ev_state = 1;
        active->cmd_completion.done = writes_visible;
    }
}
static void writel(u32 value, void *address) {
    writes_visible = true;
    writel_relaxed(value, address);
}
static void reinit_completion(struct completion *completion) {
    completion->done = 0;
    writes_visible = false;
}
static unsigned long wait_for_completion_timeout(struct completion *completion, unsigned timeout) {
    (void)timeout;
    return completion->done;
}
static void gpi_process_xfer_compl_event(struct gchan *channel, struct xfer_compl_event *event) {
    (void)channel;
    assert(event->value == callbacks + 100);
    callbacks++;
}
static void gpi_process_imed_data_event(struct gchan *channel, struct immediate_data_event *event) {
    (void)channel; (void)event;
    assert(false);
}
'''
for name in ('to_physical', 'to_virtual', 'gpi_read_reg', 'gpi_write_reg',
             'gpi_send_cmd', 'gpi_write_ch_db', 'gpi_write_ev_db',
             'gpi_ring_recycle_ev_element', 'gpi_process_events'):
    code += '\n' + function(name) + '\n'
code += r'''
int main(void) {
    struct gpii controller = {0};
    struct gchan *channel = &controller.gchan[0];
    struct gpi_ring ring = {.base = cpu_events, .phys_addr = 0x1000};
    active = &controller;
    channel->gpii = active;
    channel->ch_cntxt_db_reg = &reg_tx;
    channel->ch_cmd_reg = &reg_cmd;
    controller.ev_cntxt_db_reg = &reg_ev;
    controller.ev_cmd_reg = &reg_ev_cmd;
    controller.ev_ring_rp_lsb_reg = &reg_rp;
    controller.ieob_clr_reg = &reg_clear;
    writes_visible = false;
    gpi_write_ch_db(channel, &ring, cpu_events + 1);
    assert(descriptor_seen && reg_tx == 0x1000 + sizeof(cpu_events[0]));
    writes_visible = false;
    gpi_write_ev_db(active, &ring, cpu_events + 1);
    assert(ack_state_seen);
    assert(!gpi_send_cmd(active, channel, GPI_CH_CMD_START));
    assert(!gpi_send_cmd(active, NULL, GPI_EV_CMD_ALLOCATE));
    controller.ev_ring = (struct gpi_ring){
        .base = cpu_events, .rp = cpu_events, .wp = cpu_events + 3,
        .phys_addr = 0x1000, .len = sizeof(cpu_events), .el_size = sizeof(cpu_events[0])
    };
    for (unsigned i = 0; i < 2; i++) {
        device_events[i].xfer_compl_event.type = XFER_COMPLETE_EV_TYPE;
        device_events[i].xfer_compl_event.value = 100 + i;
    }
    event_phase = true; producer = 1;
    gpi_process_events(active);
    assert(callbacks == 2);
    puts("PASS: descriptor publication, event-slot return, channel/event command completion, two DMA event batches");
}
'''
with tempfile.TemporaryDirectory(prefix='vermeer-gpi-ordering-') as directory:
    directory = Path(directory)
    (directory / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                    '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-variable',
                    str(directory / 'test.c'), '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True)
