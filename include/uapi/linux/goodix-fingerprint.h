/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_GOODIX_FINGERPRINT_H
#define _UAPI_LINUX_GOODIX_FINGERPRINT_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Power is held by an exclusive open, and released on close. */
#define GOODIX_FP_IOC_RESET		_IO('g', 2)
#define GOODIX_FP_IOC_ENABLE_IRQ		_IO('g', 3)
#define GOODIX_FP_IOC_DISABLE_IRQ		_IO('g', 4)

/* read() consumes all pending interrupts; poll() waits for them. */
struct goodix_fp_event {
	__u32 irq_count;
	__u32 reserved;
};

#endif /* _UAPI_LINUX_GOODIX_FINGERPRINT_H */
