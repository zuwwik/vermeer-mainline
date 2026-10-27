#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute actual battmgr request/decoder/notification C with a mock GLINK."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def block(source, marker, declaration=False):
    pos = source.index(marker)
    start = source.rfind("\n", 0, pos) + 1
    opening = source.index("{", pos)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end + int(declaration)]


def main():
    source = (ROOT / "drivers/power/supply/qcom_battmgr.c").read_text()
    defines = "\n".join(re.findall(r"^#define [A-Z][A-Z0-9_]*[ \t]+[^\n]+",
                                    source, re.M))
    declarations = "\n".join(block(source, marker, True) for marker in (
        "enum qcom_battmgr_variant {", "struct qcom_battmgr_property_request {",
        "struct qcom_battmgr_message {", "struct qcom_battmgr_info {",
        "struct qcom_battmgr_status {", "struct qcom_battmgr_usb {",
        "struct qcom_battmgr_wireless {",
    ))
    functions = "\n".join(block(source, name + "(") for name in (
        "qcom_battmgr_request", "qcom_battmgr_request_property",
        "qcom_battmgr_refresh_property",
        "qcom_battmgr_bat_property_to_wire", "qcom_battmgr_notification",
        "qcom_battmgr_sm8350_callback", "qcom_battmgr_callback",
        "qcom_battmgr_refresh_worker", "qcom_battmgr_pdr_notify",
        "qcom_battmgr_remove",
    ))
    preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef uint32_t u32; typedef int32_t s32;
typedef uint32_t __le32; typedef uint16_t __le16;
#define HZ 1000
#define PMIC_GLINK_OWNER_BATTMGR 32778
#define PMIC_GLINK_REQ_RESP 1
#define SERVREG_SERVICE_STATE_UP 1
#define le32_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x) = (v))
#define smp_store_release(p,v) (*(p) = (v))
#define smp_load_acquire(p) (*(p))
#define __set_bit(n,p) (*(p) |= 1UL << (n))
#define test_bit(n,p) (!!(*(p) & (1UL << (n))))
#define DIV_ROUND_CLOSEST(x,d) ((x) >= 0 ? ((x)+(d)/2)/(d) : ((x)-(d)/2)/(d))
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
#define dev_warn(...) do {} while (0)
#define dev_err(...) do {} while (0)
#define fallthrough __attribute__((fallthrough))
struct pmic_glink_hdr { __le32 owner, type, opcode; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
static unsigned int scheduled, queued, cancelled, cancelled_sync;
static unsigned long queue_delay;
static int freezable_queue;
#define system_freezable_wq (&freezable_queue)
static void complete(int *ack) { ++*ack; }
static void reinit_completion(int *ack) { *ack = 0; }
static unsigned long wait_for_completion_timeout(int *ack, unsigned long delay)
{ (void)delay; return *ack ? 1 : 0; }
static void power_supply_changed(int *changes) { ++*changes; }
static void strscpy(char *dst, const char *src, size_t len)
{ snprintf(dst, len, "%.*s", (int)len - 1, src); }
static void queue_delayed_work(int *queue, struct delayed_work *work, unsigned long delay)
{ assert(queue == system_freezable_wq); (void)work; ++queued; queue_delay = delay; }
static void mod_delayed_work(int *queue, struct delayed_work *work, unsigned long delay)
{ queue_delayed_work(queue, work, delay); }
static void schedule_work(struct work_struct *work) { (void)work; ++scheduled; }
static void cancel_delayed_work(struct delayed_work *work) { (void)work; ++cancelled; }
static void cancel_delayed_work_sync(struct delayed_work *work) { (void)work; ++cancelled_sync; }
static void cancel_work_sync(struct work_struct *work) { (void)work; ++cancelled_sync; }
static void debugfs_remove_recursive(void *dir) { (void)dir; }
static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
'''
    middle = r'''
struct qcom_battmgr {
    void *dev, *client;
    enum qcom_battmgr_variant variant;
    struct qcom_battmgr_info info;
    struct qcom_battmgr_status status;
    struct qcom_battmgr_usb usb;
    struct qcom_battmgr_wireless wireless;
    int error, ack, *bat_psy, *usb_psy, *wls_psy;
    unsigned int pending_opcode, pending_property;
    u32 xiaomi_value;
    void *debugfs_dir;
    int lock;
    bool service_up, stopping, is_vermeer;
    unsigned long bat_valid, usb_valid, wls_valid;
    struct work_struct enable_work;
    struct delayed_work refresh_work;
};
struct auxiliary_device { struct qcom_battmgr *data; };
static struct qcom_battmgr *auxiliary_get_drvdata(struct auxiliary_device *dev)
{ return dev->data; }
static unsigned int legacy_calls, wire_property, request_count;
static unsigned int request_opcodes[128], request_properties[128];
static u32 fw_value, fw_result;
static int send_result;
static bool short_reply, no_reply, wrong_property;
static void qcom_battmgr_sc8280xp_callback(struct qcom_battmgr *b,
    const struct qcom_battmgr_message *r, size_t len)
{ (void)b; (void)r; (void)len; ++legacy_calls; }
static int pmic_glink_send(void *client, void *data, size_t len);
'''
    harness = r'''
static int pmic_glink_send(void *client, void *data, size_t len)
{
    struct qcom_battmgr *b = client;
    struct qcom_battmgr_property_request *req = data;
    struct qcom_battmgr_message reply = {0};
    size_t size = sizeof(reply.hdr) + sizeof(reply.intval);
    assert(len == sizeof(*req));
    assert(request_count < 128);
    request_opcodes[request_count] = req->hdr.opcode;
    request_properties[request_count++] = req->property;
    if (send_result) return send_result;
    if (no_reply) return 0;
    wire_property = req->property;
    /* A vendor broadcast cannot ACK the in-flight property request. */
    reply.hdr.opcode = 0x50;
    reply.intval.property = UINT32_MAX;
    qcom_battmgr_callback(&reply, size, b);
    assert(!b->ack);
    reply.hdr.opcode = req->hdr.opcode;
    reply.intval.property = req->property + 1;
    qcom_battmgr_callback(&reply, size, b);
    assert(!b->ack);
    if (wrong_property) return 0;
    reply.intval.property = req->property;
    reply.intval.value = fw_value;
    reply.intval.result = fw_result;
    if (req->hdr.opcode == BATTMGR_BAT_PROPERTY_GET &&
        req->property == (b->variant == XIAOMI_BATTMGR_SM8550 ? 19 : 18)) {
        memset(reply.strval.model, 0, sizeof(reply.strval.model));
        strcpy(reply.strval.model, "Vermeer test battery");
        size = sizeof(reply.hdr) + sizeof(reply.strval);
    }
    if (short_reply) size = sizeof(reply.hdr) + sizeof(__le32);
    qcom_battmgr_callback(&reply, size, b);
    return 0;
}

static int get(struct qcom_battmgr *b, unsigned int property, u32 value)
{
    int ret;
    fw_value = value;
    ret = qcom_battmgr_request_property(b, BATTMGR_BAT_PROPERTY_GET,
        qcom_battmgr_bat_property_to_wire(b, property), 0);
    assert(!b->pending_opcode);
    return ret;
}

int main(void)
{
    int bat = 0, usb = 0, wls = 0;
    unsigned int v, property, old;
    struct qcom_battmgr b = {.bat_psy=&bat, .usb_psy=&usb, .wls_psy=&wls};
    struct qcom_battmgr_message n = {0};
    struct auxiliary_device adev = {.data=&b};
    b.client = &b;
    for (v = QCOM_BATTMGR_SC8280XP; v <= XIAOMI_BATTMGR_SM8550; ++v) {
        b.variant = v;
        for (property = 0; property <= BATT_POWER_AVG; ++property)
            assert(qcom_battmgr_bat_property_to_wire(&b, property) ==
                property + (v == XIAOMI_BATTMGR_SM8550 && property >= BATT_TEMP));
    }
    b.variant = XIAOMI_BATTMGR_SM8550;
    assert(get(&b, BATT_TEMP, 3150) == 0 && wire_property == 13);
    assert((int)b.status.temperature == 315);
    assert(get(&b, BATT_TEMP, (u32)-525) == 0 && (int)b.status.temperature == -53);
    assert(get(&b, BATT_CAPACITY, 8178) == 0 && b.status.percent == 82);
    assert(get(&b, BATT_MODEL_NAME, 0) == 0 && wire_property == 19);
    assert(!strcmp(b.info.model_number, "Vermeer test battery"));
    assert(get(&b, BATT_CYCLE_COUNT, 321) == 0 && wire_property == 16);
    assert(b.info.cycle_count == 321);
    assert(get(&b, BATT_CHG_FULL_DESIGN, 2500000) == 0 && wire_property == 17);
    assert(b.info.design_capacity == 2500000);
    assert(get(&b, BATT_CURR_NOW, (u32)-120000) == 0 && b.status.current_now == 120000);
    fw_value = 7489;
    assert(qcom_battmgr_request_property(&b, BATTMGR_XIAOMI_PROPERTY_GET, 165, 0) == 0);
    assert(wire_property == 165 && b.xiaomi_value == 7489);
    short_reply = true;
    assert(qcom_battmgr_request_property(&b, BATTMGR_XIAOMI_PROPERTY_GET, 165, 0) == -ENODATA);
    short_reply = false;
    fw_result = 1;
    assert(qcom_battmgr_request_property(&b, BATTMGR_XIAOMI_PROPERTY_GET, 165, 0) == -EREMOTEIO);
    fw_result = 0;
    short_reply = true;
    assert(get(&b, BATT_TEMP, 0) == -ENODATA && b.ack == 1);
    assert(get(&b, BATT_MODEL_NAME, 0) == -ENODATA && b.ack == 1);
    short_reply = false;
    wrong_property = true;
    assert(get(&b, BATT_TEMP, 0) == -ETIMEDOUT && !b.ack);
    wrong_property = false;
    no_reply = true;
    assert(get(&b, BATT_TEMP, 0) == -ETIMEDOUT && !b.ack);
    no_reply = false;
    send_result = -ECONNRESET;
    assert(get(&b, BATT_TEMP, 0) == -ECONNRESET);
    send_result = 0;
    fw_result = 1;
    assert(get(&b, BATT_TEMP, 0) == -EREMOTEIO);
    fw_result = 0;
    b.variant = QCOM_BATTMGR_SM8350;
    assert(get(&b, BATT_TEMP, 3150) == 0 && wire_property == 12);
    assert(get(&b, BATT_CAPACITY, 8178) == 0 && b.status.percent == 81);
    assert(get(&b, BATT_CURR_NOW, (u32)-120000) == 0 && b.status.current_now == -120000);
    n.hdr.opcode = BATTMGR_NOTIFICATION;
    n.notification = NOTIF_XIAOMI_PROPERTY;
    b.ack = 0;
    qcom_battmgr_callback(&n, sizeof(n.hdr)+4, &b);
    assert(!bat && !usb && !b.ack);
    b.variant = XIAOMI_BATTMGR_SM8550;
    qcom_battmgr_callback(&n, sizeof(n.hdr)+4, &b);
    assert(bat == 1 && usb == 1 && !b.ack);
    n.notification = NOTIF_USB_PROPERTY;
    qcom_battmgr_callback(&n, sizeof(n.hdr)+4, &b);
    assert(bat == 2 && usb == 2 && !b.ack);
    qcom_battmgr_refresh_worker(&b.refresh_work.work);
    assert(!queued && bat == 2);
    b.is_vermeer = true;
    qcom_battmgr_pdr_notify(&b, SERVREG_SERVICE_STATE_UP);
    assert(b.service_up && scheduled == 1 && queued == 1);
    assert(queue_delay == 10 * HZ);
    old = request_count;
    qcom_battmgr_refresh_worker(&b.refresh_work.work);
    assert(request_count == old + 2);
    assert(request_opcodes[old] == BATTMGR_BAT_PROPERTY_GET);
    assert(request_properties[old] == BATT_CAPACITY);
    assert(request_opcodes[old + 1] == BATTMGR_XIAOMI_PROPERTY_GET);
    assert(request_properties[old + 1] == VERMEER_THERMAL_TEMP);
    assert(queued == 2 && bat == 3 && usb == 3);
    qcom_battmgr_pdr_notify(&b, 0);
    assert(!b.service_up && cancelled == 1);
    qcom_battmgr_refresh_worker(&b.refresh_work.work);
    assert(queued == 2 && bat == 3);
    qcom_battmgr_remove(&adev);
    assert(b.stopping && cancelled_sync == 2);
    assert(qcom_battmgr_request_property(&b, BATTMGR_XIAOMI_PROPERTY_GET, 165, 0) == -ENODEV);
    old = scheduled;
    qcom_battmgr_pdr_notify(&b, SERVREG_SERVICE_STATE_UP);
    qcom_battmgr_callback(&n, sizeof(n.hdr)+4, &b);
    assert(scheduled == old && bat == 3 && usb == 3);
    b.stopping = false;
    b.is_vermeer = false;
    b.variant = QCOM_BATTMGR_SM8350;
    old = queued;
    qcom_battmgr_pdr_notify(&b, SERVREG_SERVICE_STATE_UP);
    assert(b.service_up && queued == old);

    /* A cold device must never report zero-initialized cache fields. */
    b.is_vermeer = true;
    b.variant = XIAOMI_BATTMGR_SM8550;
    b.service_up = false;
    old = request_count;
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_BAT_PROPERTY_GET,
        BATT_CAPACITY, &b.bat_valid) == -EAGAIN);
    assert(request_count == old && !b.bat_valid);

    /* Keep a known reading while the service is down; refresh after UP. */
    b.service_up = true;
    fw_value = 4400;
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_BAT_PROPERTY_GET,
        BATT_CAPACITY, &b.bat_valid) == 0 && b.status.percent == 44);
    b.service_up = false;
    old = request_count;
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_BAT_PROPERTY_GET,
        BATT_CAPACITY, &b.bat_valid) == 0 && b.status.percent == 44);
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_BAT_PROPERTY_GET,
        13, &b.bat_valid) == -EAGAIN);
    assert(request_count == old);
    b.service_up = true;
    send_result = -ECONNRESET;
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_BAT_PROPERTY_GET,
        BATT_CAPACITY, &b.bat_valid) == 0 && b.status.percent == 44);
    send_result = 0;
    no_reply = true;
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_BAT_PROPERTY_GET,
        BATT_CAPACITY, &b.bat_valid) == 0 && b.status.percent == 44);
    no_reply = false;
    fw_result = 1;
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_BAT_PROPERTY_GET,
        BATT_CAPACITY, &b.bat_valid) == -EREMOTEIO);
    fw_result = 0;
    assert(get(&b, BATT_CAPACITY, UINT32_MAX) == -ENODATA && b.status.percent == 44);
    assert(get(&b, BATT_CAPACITY, 10001) == -ENODATA && b.status.percent == 44);
    fw_value = 0; /* A genuine empty battery must still be reported as 0%. */
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_BAT_PROPERTY_GET,
        BATT_CAPACITY, &b.bat_valid) == 0 && b.status.percent == 0);
    fw_value = 4300;
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_BAT_PROPERTY_GET,
        BATT_CAPACITY, &b.bat_valid) == 0 && b.status.percent == 43);
    fw_value = 0;
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_USB_PROPERTY_GET,
        USB_ONLINE, &b.usb_valid) == 0 && !b.usb.online);
    b.service_up = false;
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_USB_PROPERTY_GET,
        USB_ONLINE, &b.usb_valid) == 0);
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_WLS_PROPERTY_GET,
        WLS_ONLINE, &b.wls_valid) == -EAGAIN);
    b.is_vermeer = false;
    assert(qcom_battmgr_refresh_property(&b, BATTMGR_BAT_PROPERTY_GET,
        BATT_CAPACITY, &b.bat_valid) == -EAGAIN);
    for (v = 0; v < request_count; v++)
        assert(request_opcodes[v] == BATTMGR_BAT_PROPERTY_GET ||
               request_opcodes[v] == BATTMGR_USB_PROPERTY_GET ||
               request_opcodes[v] == BATTMGR_XIAOMI_PROPERTY_GET);
    assert(!legacy_calls);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="vermeer-battmgr-test-") as tmp:
        cfile, binary = Path(tmp) / "test.c", Path(tmp) / "test"
        cfile.write_text("\n".join((preamble, defines, declarations, middle, functions, harness)))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: actual C request/decoder maps Xiaomi properties in both directions")
    print("PASS: signed temperature, capacity rounding, model and current sign")
    print("PASS: unsolicited/mismatched frames cannot ACK requests; errors complete correctly")
    print("PASS: vendor/USB events update both supplies; stock read-only CAPACITY/THERMAL poll")
    print("PASS: service-down and remove cancel/stop refresh; generic property mapping unchanged")
    print("PASS: recovery uses only valid cached readings; fresh and genuine zero SOC preserved")


if __name__ == "__main__":
    main()
