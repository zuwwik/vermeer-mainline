#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual UCSI teardown and GLINK code without restarting a DSP."""
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
        raise ValueError('function definition not found: ' + name)
    start = match.start()
    opening = source.index('{', match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define IS_ERR_OR_NULL(p) (!(p))
#define kfree free
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define spin_lock_irqsave(l,f) do { (void)(l); (f)=0; } while (0)
#define spin_unlock_irqrestore(l,f) do { (void)(l); (void)(f); } while (0)
struct work_struct { unsigned magic; bool disabled; };
struct completion { unsigned count; };
static inline void complete(struct completion *c) { c->count++; }
static unsigned queued;
static inline int schedule_work(struct work_struct *w) {
 assert(w->magic==0xcafe); if(w->disabled) return 0; queued++; return 1;
}
static inline void disable_work_sync(struct work_struct *w) { w->disabled=true; }
static inline void cancel_delayed_work_sync(struct work_struct *w) { (void)w; }
static inline void cancel_work_sync(struct work_struct *w) { (void)w; }
static inline void lockdep_unregister_key(int *p) { (void)*p; }
#define UCSI_ENABLE_NTFY_CONNECTOR_CHANGE 2
#define UCSI_SET_NOTIFICATION_ENABLE 5
#define UCSI_CCI_BUSY (1U<<28)
#define UCSI_CCI_ACK_COMPLETE (1U<<29)
#define UCSI_CCI_COMMAND_COMPLETE (1U<<31)
#define UCSI_CCI_CONNECTOR(x) (((x)>>1)&127)
enum { ACK_PENDING, COMMAND_PENDING, EVENT_PENDING };
static inline int test_and_clear_bit(unsigned bit,unsigned long *p) {
 int v=!!(*p&(1UL<<bit)); *p&=~(1UL<<bit); return v;
}
static inline int test_and_set_bit(unsigned bit,unsigned long *p) {
 int v=!!(*p&(1UL<<bit)); *p|=1UL<<bit; return v;
}
static inline void clear_bit(unsigned bit,unsigned long *p) { *p&=~(1UL<<bit); }
'''


def core_case(core, debug):
    return COMMON + r'''
struct ucsi;
struct ucsi_operations { int (*async_control)(struct ucsi *,u64); };
struct ucsi_connector { struct work_struct work; int lock_key; bool live; };
struct ucsi {
 struct work_struct work,resume_work;
 struct { unsigned num_connectors; } cap;
 struct ucsi_connector *connector;
 const struct ucsi_operations *ops;
 struct { void *dentry; } *debugfs;
 unsigned long flags; u64 ntfy; void *dev; struct completion complete;
};
static unsigned unregistered, debug_removed;
static void ucsi_unregister_port(struct ucsi_connector *c) { assert(c->live); c->live=false; unregistered++; }
static void debugfs_remove_recursive(void *p) { (void)p; debug_removed++; }
''' + function(debug, 'ucsi_debugfs_unregister') + '\n' + function(core, 'ucsi_connector_change') + '\n' + function(core, 'ucsi_notify_common') + '\n' + function(core, 'ucsi_unregister') + r'''
static int command(struct ucsi *u,u64 c) { (void)u; assert(c==UCSI_SET_NOTIFICATION_ENABLE); return -ECONNRESET; }
int main(void) {
 struct ucsi_operations ops={command}; struct ucsi u={.ops=&ops};
 u.cap.num_connectors=2; u.ntfy=UCSI_ENABLE_NTFY_CONNECTOR_CHANGE;
 u.flags=1UL<<EVENT_PENDING;
 u.connector=calloc(3,sizeof(*u.connector)); u.debugfs=calloc(1,sizeof(*u.debugfs));
 for(unsigned i=0;i<2;i++) {u.connector[i].live=true;u.connector[i].work.magic=0xcafe;}
 ucsi_unregister(&u);
 /* Firmware reset, failed re-registration, then another reset/late CCI. */
 ucsi_notify_common(&u,2);
 ucsi_unregister(&u);
 assert(unregistered==2 && !u.connector && u.cap.num_connectors==0);
 assert(!u.debugfs && debug_removed==1 && queued==0);
 assert(!(u.flags & (1UL<<EVENT_PENDING)));
 u.flags=(1UL<<ACK_PENDING)|(1UL<<COMMAND_PENDING);
 ucsi_notify_common(&u,UCSI_CCI_ACK_COMPLETE|UCSI_CCI_COMMAND_COMPLETE|2);
 assert(u.complete.count==2 && queued==0);
 ucsi_debugfs_unregister(&u); assert(debug_removed==1);
 puts("PASS: repeated reset after failed re-registration; late CCI cannot use freed connectors; command completions survive; debugfs teardown is idempotent");
}
'''


def decoder_case(glink):
    return COMMON + r'''
#define UCSI_BUF_V1_SIZE 48
#define UCSI_BUF_V2_SIZE 528
#define UCSI_VERSION_2_0 0x200
struct pmic_glink_hdr { u32 owner,type,opcode; };
struct __attribute__((packed)) ucsi_read_buf_resp_msg {
 struct pmic_glink_hdr hdr; union { u8 v1_buf[48]; u8 v2_buf[528]; } buf;u32 ret_code;
};
struct __attribute__((packed)) ucsi_write_buf_resp_msg {struct pmic_glink_hdr hdr;u32 ret_code;};
struct ucsi {u16 version;};
struct pmic_glink_ucsi {
 struct ucsi *ucsi; bool ucsi_registered; void *dev;
 u8 read_buf[528];struct completion read_ack,write_ack;
};
''' + function(glink, 'pmic_glink_ucsi_read_ack') + '\n' + function(glink, 'pmic_glink_ucsi_write_ack') + r'''
int main(void) {
 struct ucsi core={0}; struct pmic_glink_ucsi u={.ucsi=&core};
 for(unsigned version=0;version<3;version++) {
  core.version=version==2?0x200:version==1?0x105:0;
  /* The GLINK dispatcher has already rejected frames shorter than its header. */
  for(size_t len=sizeof(struct pmic_glink_hdr);len<560;len++) {
   unsigned char *p=calloc(len?len:1,1);
   u.read_ack.count=0;pmic_glink_ucsi_read_ack(&u,p,len);
   u.write_ack.count=0;pmic_glink_ucsi_write_ack(&u,p,len);
   assert(len>=16 || u.read_ack.count==0);
   assert((len==16)==(u.write_ack.count==1));
   free(p);
  }
 }
 unsigned char valid[64]={0}; core.version=0x105;valid[12]=0x5a;
 u.read_ack.count=0;pmic_glink_ucsi_read_ack(&u,valid,sizeof(valid));
 assert(u.read_ack.count==1 && u.read_buf[0]==0x5a);
 puts("PASS: all dispatcher-reachable reply lengths 12..559 for unknown, UCSI v1 and v2; short read/write replies never access beyond the packet");
}
'''


def lifecycle_case(glink):
    return COMMON + r'''
#define SERVREG_SERVICE_STATE_UP 1
struct ucsi {unsigned unused;};
struct pmic_glink_ucsi {
 void *dev;struct ucsi *ucsi;struct work_struct notify_work,register_work;
 int state_lock;bool pd_running,ucsi_registered,reset_pending,quiescing;
};
struct auxiliary_device {struct {void *data;} dev;};
static inline void *dev_get_drvdata(void *p) {return *(void **)p;}
static struct pmic_glink_ucsi *current;
static unsigned registrations,unregistrations;
static int register_error;static bool inject_restart;
static u32 test_cci,delivered;
static void pmic_glink_ucsi_pdr_notify(void *p,int state);
static int ucsi_register(struct ucsi *u) {
 (void)u;registrations++;
 if(inject_restart) {inject_restart=false;pmic_glink_ucsi_pdr_notify(current,0);pmic_glink_ucsi_pdr_notify(current,1);}
 return register_error;
}
static void ucsi_unregister(struct ucsi *u) {
 (void)u;assert(current->quiescing);assert(!current->notify_work.disabled);unregistrations++;
}
static int pmic_glink_ucsi_read(struct ucsi *u,unsigned off,void *p,size_t len) {
 (void)u;(void)off;assert(len==4);*(u32 *)p=test_cci;return 0;
}
#define UCSI_CCI 4
static void ucsi_notify_common(struct ucsi *u,u32 cci) {(void)u;delivered=cci;}
''' + function(glink, 'pmic_glink_ucsi_pdr_notify') + '\n' + function(glink, 'pmic_glink_ucsi_register') + '\n' + function(glink, 'pmic_glink_ucsi_notify') + '\n' + function(glink, 'pmic_glink_ucsi_remove') + r'''
int main(void) {
 struct ucsi core={0};struct pmic_glink_ucsi u={.ucsi=&core,.quiescing=true};current=&u;
 u.notify_work.magic=u.register_work.magic=0xcafe;
 pmic_glink_ucsi_pdr_notify(&u,1);pmic_glink_ucsi_register(&u.register_work);
 assert(u.ucsi_registered && !u.quiescing && registrations==1);
 test_cci=2|UCSI_CCI_COMMAND_COMPLETE;pmic_glink_ucsi_notify(&u.notify_work);assert(delivered==test_cci);
 pmic_glink_ucsi_pdr_notify(&u,0);pmic_glink_ucsi_notify(&u.notify_work);assert(delivered==UCSI_CCI_COMMAND_COMPLETE);
 pmic_glink_ucsi_pdr_notify(&u,1);pmic_glink_ucsi_register(&u.register_work);
 assert(registrations==2 && unregistrations==1 && u.ucsi_registered);
 pmic_glink_ucsi_pdr_notify(&u,0);pmic_glink_ucsi_register(&u.register_work);
 register_error=-EIO;pmic_glink_ucsi_pdr_notify(&u,1);pmic_glink_ucsi_register(&u.register_work);
 assert(!u.ucsi_registered && unregistrations==2);
 register_error=0;inject_restart=true;pmic_glink_ucsi_pdr_notify(&u,1);pmic_glink_ucsi_register(&u.register_work);
 assert(u.ucsi_registered && !u.quiescing && registrations==5 && unregistrations==3);
 struct auxiliary_device a={.dev={.data=&u}};pmic_glink_ucsi_remove(&a);
 assert(!u.ucsi_registered && u.quiescing && u.register_work.disabled && u.notify_work.disabled && unregistrations==4);
 unsigned count=queued;pmic_glink_ucsi_pdr_notify(&u,1);assert(queued==count);
 puts("PASS: coalesced DOWN/UP, restart during registration, failed registration, notification quiesce, completion delivery and callbacks after removal");
}
'''


def send_case(source):
    return COMMON + r'''
#define SERVREG_SERVICE_STATE_UP 1
#define PMIC_GLINK_SEND_TIMEOUT 5
#define guard(x) (void)
static unsigned long jiffies;
#define time_after(a,b) ((a)>(b))
struct pmic_glink {void *ept;int state_lock;unsigned client_state;};
struct pmic_glink_client {struct pmic_glink *pg;};
static unsigned sends,retries;
static int rpmsg_send(void *e,void *d,size_t n) {(void)e;(void)d;(void)n;sends++;return retries--?-EAGAIN:0;}
static void usleep_range(unsigned a,unsigned b) {(void)a;(void)b;jiffies++;}
''' + function(source, 'pmic_glink_send') + r'''
int main(void) {
 struct pmic_glink p={.ept=&p};struct pmic_glink_client c={.pg=&p};
 assert(pmic_glink_send(&c,NULL,0)==-ECONNRESET && sends==0);
 p.client_state=SERVREG_SERVICE_STATE_UP;p.ept=NULL;assert(pmic_glink_send(&c,NULL,0)==-ECONNRESET && sends==0);
 p.ept=&p;retries=0;assert(pmic_glink_send(&c,NULL,0)==0 && sends==1);
 sends=0;retries=2;assert(pmic_glink_send(&c,NULL,0)==0 && sends==3);
 sends=0;retries=100;assert(pmic_glink_send(&c,NULL,0)==-ETIMEDOUT && sends<10);
 puts("PASS: no requests while charger PD is down or endpoint absent; healthy sends and bounded backpressure retry");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--case', choices=['core', 'decoder', 'lifecycle', 'send'])
    args = parser.parse_args()
    ucsi = args.root / 'drivers/usb/typec/ucsi'
    core = (ucsi / 'ucsi.c').read_text()
    glink = (ucsi / 'ucsi_glink.c').read_text()
    debug = (ucsi / 'debugfs.c').read_text()
    transport = (args.root / 'drivers/soc/qcom/pmic_glink.c').read_text()
    cases = {'core': core_case(core, debug), 'decoder': decoder_case(glink),
             'lifecycle': lifecycle_case(glink), 'send': send_case(transport)}
    with tempfile.TemporaryDirectory(prefix='vermeer-adsp-transport-') as tmp:
        for name, code in cases.items():
            if args.case and name != args.case:
                continue
            source, binary = Path(tmp) / (name + '.c'), Path(tmp) / name
            source.write_text(code)
            subprocess.run(['cc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra',
                            '-Wno-unused-function', '-Wno-sign-compare',
                            '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
