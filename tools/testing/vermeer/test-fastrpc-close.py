#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual FastRPC file release with parked invoke references."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def function(source, name):
    match = re.search(r'(?m)^[\w *]+\b' + re.escape(name) +
                      r'\([^;{}]*\)\n\{', source)
    if not match:
        raise ValueError(name)
    start = match.start()
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct list_head { struct list_head *next,*prev; };
#define LIST_HEAD(n) struct list_head n={&n,&n}
static void init(struct list_head *h) { h->next=h->prev=h; }
static bool empty(struct list_head *h) { return h->next==h; }
static void list_add_tail(struct list_head *n,struct list_head *h) {
 n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;
}
static void list_del(struct list_head *n) {
 n->prev->next=n->next;n->next->prev=n->prev;n->next=n->prev=NULL;
}
static void list_splice_init(struct list_head *h,struct list_head *to) {
 if(empty(h)) return;
 h->next->prev=to;h->prev->next=to->next;to->next->prev=h->prev;to->next=h->next;init(h);
}
#define list_for_each_entry_safe(p,n,h,m) \
 for(p=container_of((h)->next,__typeof__(*p),m), \
     n=container_of(p->m.next,__typeof__(*p),m);&p->m!=(h); \
     p=n,n=container_of(n->m.next,__typeof__(*n),m))
#define spin_lock(p) ((void)(p))
#define spin_unlock(p) ((void)(p))
#define spin_lock_irqsave(p,f) do {(void)(p);(f)=0;}while(0)
#define spin_unlock_irqrestore(p,f) do {(void)(p);(void)(f);}while(0)
struct inode {int unused;};
struct file {void *private_data;};
struct fastrpc_channel_ctx {int lock;};
struct fastrpc_user {
 struct fastrpc_channel_ctx *cctx;struct list_head user,pending;
 int lock;void *sctx;atomic_uint refs;
};
struct fastrpc_invoke_ctx {
 struct list_head node;struct fastrpc_user *fl;
 atomic_uint refs;atomic_bool dsp_owned;unsigned char *dma;
};
static struct fastrpc_channel_ctx channel;
static atomic_uint contexts_freed,users_freed,buffers_freed;
static struct fastrpc_invoke_ctx *release_timeout;
static bool inject_release_timeout;
static void fastrpc_user_put(struct fastrpc_user *fl) {
 assert(fl->refs);
 if(!--fl->refs) {assert(empty(&fl->pending));users_freed++;free(fl);}
}
static void fastrpc_context_put(struct fastrpc_invoke_ctx *ctx) {
 assert(ctx->refs);
 if(!--ctx->refs) {
  assert(!ctx->node.next && !ctx->dsp_owned);
  struct fastrpc_user *fl=ctx->fl;
  buffers_freed++;free(ctx->dma);contexts_freed++;free(ctx);fastrpc_user_put(fl);
 }
}
static struct fastrpc_invoke_ctx *park(struct fastrpc_user *fl,bool dsp_owned) {
 struct fastrpc_invoke_ctx *ctx=calloc(1,sizeof(*ctx));assert(ctx);
 ctx->dma=malloc(4096);assert(ctx->dma);ctx->dma[0]=0xa5;
 ctx->fl=fl;ctx->refs=1+dsp_owned;ctx->dsp_owned=dsp_owned;fl->refs++;
 list_add_tail(&ctx->node,&fl->pending);return ctx;
}
static int fastrpc_release_current_dsp_process(struct fastrpc_user *fl) {
 if(inject_release_timeout) release_timeout=park(fl,true);
 return 0;
}
static void fastrpc_session_free(struct fastrpc_channel_ctx *c,void *p) {(void)c;(void)p;}
'''

MAIN = r'''
static void reply(struct fastrpc_invoke_ctx *ctx) {
 assert(ctx->dsp_owned && ctx->dma[0]==0xa5);
 ctx->dsp_owned=false;fastrpc_context_put(ctx);
}
static void scenario(unsigned state) {
 LIST_HEAD(users);
 struct fastrpc_user *fl=calloc(1,sizeof(*fl));assert(fl);
 fl->cctx=&channel;fl->refs=1;init(&fl->pending);list_add_tail(&fl->user,&users);
 struct file file={fl};
 /* 0: unsent/interrupted send; 1: late reply; 2: reply precedes close;
  * 3: queued reply after close; 4: process-release RPC itself times out. */
 struct fastrpc_invoke_ctx *ctx=park(fl,state!=0);
 unsigned old_users=users_freed,old_contexts=contexts_freed,old_buffers=buffers_freed;
 if(state==2) reply(ctx);
 inject_release_timeout=state==4;release_timeout=NULL;
 assert(!fastrpc_device_release(NULL,&file));
 assert(!file.private_data && empty(&users));
 if(state==0 || state==2) {
  assert(users_freed==old_users+1 && contexts_freed==old_contexts+1);
 } else {
  /* In-flight DMA must stay valid even after the file and caller are gone. */
  assert(ctx->refs==1 && ctx->dma[0]==0xa5);
  assert(users_freed==old_users && buffers_freed==old_buffers);
  reply(ctx);
  if(state==4) {
   assert(release_timeout->refs==1 && users_freed==old_users);
   reply(release_timeout);
  }
  assert(users_freed==old_users+1);
  assert(contexts_freed==old_contexts+1+(state==4));
 }
}
struct race {struct fastrpc_invoke_ctx *ctx;atomic_bool go;};
static void *reply_thread(void *data) {
 struct race *race=data;
 while(!atomic_load(&race->go)) sched_yield();
 reply(race->ctx);
 return NULL;
}
static void concurrent_reply(void) {
 LIST_HEAD(users);
 struct fastrpc_user *fl=calloc(1,sizeof(*fl));assert(fl);
 fl->cctx=&channel;fl->refs=1;init(&fl->pending);list_add_tail(&fl->user,&users);
 struct file file={fl};
 struct race race={.ctx=park(fl,true)};
 unsigned old_users=users_freed,old_contexts=contexts_freed;
 inject_release_timeout=false;
 pthread_t thread;assert(!pthread_create(&thread,NULL,reply_thread,&race));
 atomic_store(&race.go,true);
 assert(!fastrpc_device_release(NULL,&file));
 assert(!file.private_data && empty(&users));
 assert(!pthread_join(thread,NULL));
 assert(users_freed==old_users+1 && contexts_freed==old_contexts+1);
}
int main(void) {
 for(unsigned repeat=0;repeat<200;repeat++)
  for(unsigned state=0;state<5;state++) scenario(state);
 assert(users_freed==1000 && contexts_freed==1200 && buffers_freed==1200);
 for(unsigned repeat=0;repeat<500;repeat++) concurrent_reply();
 assert(users_freed==1500 && contexts_freed==1700 && buffers_freed==1700);
 puts("PASS: 1000 parked-invoke closes and 500 concurrent reply/close races; process-release timeout; DSP-owned DMA stays alive until reply; no caller-reference cycle");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', nargs='?', type=Path,
                        default=ROOT / 'drivers/misc/fastrpc.c')
    args = parser.parse_args()
    code = HARNESS + function(args.source.read_text(), 'fastrpc_device_release') + MAIN
    with tempfile.TemporaryDirectory(prefix='vermeer-fastrpc-close-') as tmp:
        cfile, binary = Path(tmp) / 'close.c', Path(tmp) / 'close'
        cfile.write_text(code)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
                        str(cfile), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
