#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check actual FastRPC send/reply reference ownership without using a DSP."""
from pathlib import Path
import argparse
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
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
typedef uint32_t u32;
#define FASTRPC_CTXID_MASK 0xff0
#define PAGE_SIZE 4096
#define roundup(x,y) (((x)+(y)-1)&~((y)-1))
#define dev_err(...) ((void)0)
#define mutex_lock(l) assert(!pthread_mutex_lock(l))
#define mutex_unlock(l) assert(!pthread_mutex_unlock(l))
#define spin_lock_irqsave(l,f) do { (f)=0; assert(!pthread_mutex_lock(l)); } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(!pthread_mutex_unlock(l)); } while(0)
struct fastrpc_invoke_ctx;
struct work_struct { struct fastrpc_invoke_ctx *ctx; };
struct completion { unsigned count; struct fastrpc_invoke_ctx *ctx; };
struct device { void *data; };
struct rpmsg_device { struct device dev; void *ept; };
struct fastrpc_channel_ctx {
 pthread_mutex_t lock,send_lock; struct fastrpc_invoke_ctx *ctx_idr; struct rpmsg_device *rpdev;
};
struct fastrpc_user { struct fastrpc_channel_ctx *cctx; u32 client_id,pd; };
struct fastrpc_session_ctx { int unused; };
struct fastrpc_msg { u32 client_id,tid,handle,sc; uint64_t ctx,addr,size; };
struct fastrpc_invoke_rsp { uint64_t ctx; int retval; };
struct fastrpc_invoke_ctx {
 struct fastrpc_user *fl; struct fastrpc_msg msg; uint64_t ctxid,msg_sz;
 struct { uint64_t dma_addr; } *buf;
 u32 sc; int retval; bool reply_pending; unsigned refs;
 struct completion work; struct work_struct put_work;
};
static struct { unsigned pid; } task={123}, *current=&task;
static unsigned completed,scheduled,freed;
static bool drop_caller_on_complete,reply_in_send;
static int send_error;
static struct rpmsg_device rpdev;
static struct fastrpc_channel_ctx channel;
static struct fastrpc_user user;
static struct fastrpc_invoke_rsp response={.ctx=0x11,.retval=42};
static void *dev_get_drvdata(struct device *d) {return d->data;}
static struct fastrpc_invoke_ctx *idr_find(struct fastrpc_invoke_ctx **p,unsigned long id) {
 assert(id==1);return *p;
}
static void fastrpc_context_get(struct fastrpc_invoke_ctx *ctx) {assert(ctx->refs);ctx->refs++;}
static void fastrpc_context_put(struct fastrpc_invoke_ctx *ctx) {
 assert(ctx->refs);
 if(!--ctx->refs) {assert(channel.ctx_idr==ctx);channel.ctx_idr=NULL;freed++;free(ctx);}
}
static void complete(struct completion *c) {
 completed++;c->count++;
 if(drop_caller_on_complete) {drop_caller_on_complete=false;fastrpc_context_put(c->ctx);}
}
/* Run the release worker immediately: a duplicate must not release it again. */
static int schedule_work(struct work_struct *w) {scheduled++;fastrpc_context_put(w->ctx);return 1;}
static int fastrpc_rpmsg_callback(struct rpmsg_device *,void *,int,void *,u32);
static int rpmsg_send(void *ept,void *data,size_t len) {
 assert(ept==&rpdev && len==sizeof(struct fastrpc_msg));
 assert(pthread_mutex_trylock(&channel.send_lock)==EBUSY);
 struct fastrpc_msg *msg=data;assert(msg->ctx==response.ctx);
 if(reply_in_send) assert(!fastrpc_rpmsg_callback(&rpdev,&response,sizeof(response),NULL,0));
 return send_error;
}
'''

MAIN = r'''
static struct fastrpc_invoke_ctx *new_context(void) {
 struct fastrpc_invoke_ctx *ctx=calloc(1,sizeof(*ctx));assert(ctx);
 ctx->fl=&user;ctx->ctxid=0x10;ctx->refs=1;
 ctx->work.ctx=ctx;ctx->put_work.ctx=ctx;channel.ctx_idr=ctx;
 return ctx;
}
static void *reply_thread(void *p) {
 (void)p;int r=fastrpc_rpmsg_callback(&rpdev,&response,sizeof(response),NULL,0);
 assert(!r || r==-ENOENT);return NULL;
}
int main(void) {
 assert(!pthread_mutex_init(&channel.lock,NULL));
 assert(!pthread_mutex_init(&channel.send_lock,NULL));
 rpdev.dev.data=&channel;rpdev.ept=&rpdev;channel.rpdev=&rpdev;
 user.cctx=&channel;user.pd=1;
 assert(fastrpc_rpmsg_callback(&rpdev,&response,0,NULL,0)==-EINVAL);
 assert(fastrpc_rpmsg_callback(&rpdev,&response,sizeof(response),NULL,0)==-ENOENT);
 struct fastrpc_invoke_ctx *ctx=new_context();
 /* IDR publication precedes the send: no send reference exists yet. */
 assert(fastrpc_rpmsg_callback(&rpdev,&response,sizeof(response),NULL,0)==-ENOENT);
 assert(ctx->refs==1 && !completed && !scheduled);
 assert(!fastrpc_invoke_send(NULL,ctx,0,10) && ctx->refs==2);
 assert(!fastrpc_rpmsg_callback(&rpdev,&response,sizeof(response),NULL,0));
 assert(ctx->refs==1 && completed==1 && scheduled==1 && ctx->retval==42);
 assert(fastrpc_rpmsg_callback(&rpdev,&response,sizeof(response),NULL,0)==-ENOENT);
 assert(ctx->refs==1 && completed==1 && scheduled==1);
 fastrpc_context_put(ctx);
 /* A send failure drops its reference; a late reply cannot drop the caller's. */
 ctx=new_context();send_error=-EIO;
 assert(fastrpc_invoke_send(NULL,ctx,0,10)==-EIO && ctx->refs==1);
 assert(fastrpc_rpmsg_callback(&rpdev,&response,sizeof(response),NULL,0)==-ENOENT);
 fastrpc_context_put(ctx);
 /* An already delivered reply owns the send reference, even on send error. */
 ctx=new_context();reply_in_send=true;
 assert(fastrpc_invoke_send(NULL,ctx,0,10)==-EIO && ctx->refs==1);
 fastrpc_context_put(ctx);reply_in_send=false;send_error=0;
 /* The awakened caller can release its reference before the worker is queued. */
 ctx=new_context();assert(!fastrpc_invoke_send(NULL,ctx,0,10));
 drop_caller_on_complete=true;
 assert(!fastrpc_rpmsg_callback(&rpdev,&response,sizeof(response),NULL,0));
 assert(!channel.ctx_idr);
 assert(fastrpc_rpmsg_callback(&rpdev,&response,sizeof(response),NULL,0)==-ENOENT);
 /* Parallel duplicate replies may consume only one send reference. */
 ctx=new_context();assert(!fastrpc_invoke_send(NULL,ctx,0,10));
 unsigned before=scheduled;pthread_t threads[16];
 for(unsigned i=0;i<16;i++) assert(!pthread_create(&threads[i],NULL,reply_thread,NULL));
 for(unsigned i=0;i<16;i++) assert(!pthread_join(threads[i],NULL));
 assert(scheduled==before+1 && ctx->refs==1);
 fastrpc_context_put(ctx);assert(freed==5 && !channel.ctx_idr);
 /* Channel removal after internal_invoke's initial check, before the send. */
 ctx=new_context();channel.rpdev=NULL;
 assert(fastrpc_invoke_send(NULL,ctx,0,10)==-EPIPE);
 assert(ctx->refs==1 && !ctx->reply_pending);
 fastrpc_context_put(ctx);assert(freed==6);
 pthread_mutex_destroy(&channel.send_lock);
 pthread_mutex_destroy(&channel.lock);
 puts("PASS: unsent, duplicate, parallel and late replies; send failure/reply race; caller release before worker; endpoint removed before send; exactly one release per send");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', nargs='?', type=Path,
                        default=ROOT / 'drivers/misc/fastrpc.c')
    args = parser.parse_args()
    source = args.source.read_text()
    code = HARNESS + function(source, 'fastrpc_invoke_send') + '\n'
    code += function(source, 'fastrpc_rpmsg_callback') + MAIN
    with tempfile.TemporaryDirectory(prefix='vermeer-fastrpc-reply-') as tmp:
        cfile, binary = Path(tmp) / 'reply.c', Path(tmp) / 'reply'
        cfile.write_text(code)
        subprocess.run(['cc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra',
                        '-Wno-unused-parameter', '-Wno-sign-compare',
                        '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-pthread', str(cfile),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
