#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Replay a controller updating between reads against the actual IRQ functions."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path, nargs='?',
                    default=Path(__file__).resolve().parents[3] /
                    'drivers/input/touchscreen/goodix_berlin_core.c')
args = parser.parse_args()
s = args.source.read_text()


def function(name):
    match = re.search(r'static [\w ]+\s+' + re.escape(name) + r'\s*\(', s)
    if match is None:
        raise ValueError('Driver function not found: ' + name)
    opening = s.index('{', match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[match.start():end]


code=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint16_t __le16;
typedef int irqreturn_t;
#define IRQ_HANDLED 1
#define BIT(x) (1U<<(x))
#define GENMASK(h,l) (((1U<<((h)+1))-1)&~((1U<<(l))-1))
#define FIELD_GET(mask,x) ((x)&(mask))
#define __packed __attribute__((packed))
#define GOODIX_BERLIN_MAX_TOUCH 10
#define GOODIX_BERLIN_CHECKSUM_SIZE 2
#define GOODIX_BERLIN_TOUCH_COUNT_MASK 15
#define GOODIX_BERLIN_TOUCH_EVENT 128
#define GOODIX_BERLIN_REQUEST_EVENT 64
#define GOODIX_BERLIN_REQUEST_CODE_RESET 3
static void quiet(void *dev,const char *text,...) {(void)dev;(void)text;}
#define dev_err quiet
#define dev_warn quiet
#define dev_err_ratelimited quiet
#define dev_warn_ratelimited quiet
static uint16_t get_unaligned_le16(const void *p) {const uint8_t *b=p;return b[0]+256*b[1];}
'''
code+=s[s.index('struct goodix_berlin_touch {'):s.index('struct goodix_berlin_core {')]
code+=r'''
struct goodix_berlin_core {void *dev,*regmap,*reset_gpio;u32 touch_data_addr;struct goodix_berlin_event event;};
static unsigned reads,reports,reported,clears,resets,touches,corrupt;
static void seal(u8 *p,unsigned n) {unsigned sum=0;for(unsigned i=0;i<n-2;i++)sum+=p[i];p[n-2]=sum&255;p[n-1]=sum>>8;}
static int regmap_raw_read(void *map,u32 address,void *out,size_t count) {
 (void)map;
 struct goodix_berlin_event bank={0};bank.hdr.status=128;bank.hdr.request_type=touches;
 seal((u8*)&bank.hdr,sizeof(bank.hdr));
 for(unsigned i=0;i<touches && i<10;i++) {
  bank.data[i*8]=i<<4;bank.data[i*8+2]=50+reads*3;bank.data[i*8+4]=80;bank.data[i*8+6]=9;
 }
 if(touches && touches<=10)seal(bank.data,touches*8+2);
 if(corrupt==1)bank.hdr.checksum^=1;
 if(corrupt==2)bank.data[4]^=1;
 assert(address+count<=sizeof(bank));memcpy(out,(u8*)&bank+address,count);reads++;return 0;
}
static int regmap_write(void *map,u32 address,int value) {(void)map;(void)address;(void)value;clears++;return 0;}
static void goodix_berlin_report_state(struct goodix_berlin_core *cd,int n) {(void)cd;reports++;reported=n;}
static int goodix_berlin_request_handle_reset(struct goodix_berlin_core *cd) {(void)cd;resets++;return 0;}
'''
code += function('goodix_berlin_checksum_valid') + '\n'
# Also support the old implementation so the regression can be demonstrated.
if 'static int goodix_berlin_get_remaining_contacts(' in s:
    code += function('goodix_berlin_get_remaining_contacts') + '\n'
code += function('goodix_berlin_touch_handler') + '\n'
code += function('goodix_berlin_irq') + '\n'
code+=r'''
int main(void) {
 for(touches=0;touches<=10;touches++) {
  struct goodix_berlin_core cd={0};reads=reports=clears=0;
  goodix_berlin_irq(1,&cd);assert(reports==1 && reported==touches);assert(clears==1);
 }
 for(corrupt=1;corrupt<=2;corrupt++) {
  struct goodix_berlin_core cd={0};touches=3;reads=reports=clears=0;
  goodix_berlin_irq(1,&cd);assert(!reports && clears==1);
 }
 corrupt=0;touches=11;reads=reports=clears=0;struct goodix_berlin_core cd={0};
 goodix_berlin_irq(1,&cd);assert(!reports && clears==1);
 puts("PASS: 0..10 contacts, controller updates between reads, header/payload checksum rejection and invalid contact count");
}
'''
with tempfile.TemporaryDirectory(prefix='vermeer-goodix-test-') as directory:
    directory = Path(directory)
    source, binary = directory / 'test.c', directory / 'test'
    source.write_text(code)
    subprocess.run(['cc', '-O1', '-g', '-fsanitize=address,undefined',
                    '-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-Wno-unused-variable', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
