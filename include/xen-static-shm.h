/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * User API of a Linux driver that exposes each Xen static shared memory
 * region of a domain as /dev/xen-shm/<xen,id>: XSHM_IOCTL_INFO reports the
 * region's size, XSHM_IOCTL_EXPORT a page aligned slice as a DMA-BUF, and
 * mmap maps the region. Used for vqda heap buffers; not part of upstream
 * Linux.
 */
#ifndef _UAPI_XEN_STATIC_SHM_H
#define _UAPI_XEN_STATIC_SHM_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct xshm_info {
	__u64 size;		/* out: bytes in the region */
};

struct xshm_export {
	__u64 offset;		/* in: page aligned, into the region */
	__u64 size;		/* in: page aligned */
	__u32 flags;		/* in: must be 0 */
	__s32 fd;		/* out: DMA-BUF of the slice */
};

#define XSHM_IOCTL_INFO		_IOR(0xB9, 0x00, struct xshm_info)
#define XSHM_IOCTL_EXPORT	_IOWR(0xB9, 0x01, struct xshm_export)

#endif
