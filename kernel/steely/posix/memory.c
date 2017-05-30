/*
 * This file is part of the Xenomai project.
 *
 * Copyright (C) 2014 Philippe Gerum <rpm@xenomai.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/vmalloc.h>
#include <steely/driver.h>
#include <steely/vdso.h>
#include "process.h"
#include "memory.h"

#define UMM_PRIVATE  0	/* Per-process user-mapped memory heap */
#define UMM_SHARED   1	/* Shared user-mapped memory heap */
#define SYS_GLOBAL   2	/* System heap (not mmapped) */

struct xnvdso *nkvdso;
EXPORT_SYMBOL_GPL(nkvdso);

static void umm_vmopen(struct vm_area_struct *vma)
{
	struct steely_umm *umm = vma->vm_private_data;

	atomic_inc(&umm->refcount);
}

static void umm_vmclose(struct vm_area_struct *vma)
{
	struct steely_umm *umm = vma->vm_private_data;

	steely_umm_destroy(umm);
}

static struct vm_operations_struct umm_vmops = {
	.open = umm_vmopen,
	.close = umm_vmclose,
};

static struct steely_umm *umm_from_fd(struct rtdm_fd *fd)
{
	struct steely_process *process;

	process = steely_current_process();
	if (process == NULL)
		return NULL;

	if (rtdm_fd_minor(fd) == UMM_PRIVATE)
		return &process->sys_ppd.umm;

	return &steely_kernel_ppd.umm;
}

static int umm_mmap(struct rtdm_fd *fd, struct vm_area_struct *vma)
{
	struct steely_umm *umm;
	size_t len;
	int ret;

	umm = umm_from_fd(fd);
	if (fd == NULL)
		return -ENODEV;

	len = vma->vm_end - vma->vm_start;
	if (len != xnheap_get_size(&umm->heap))
		return -EINVAL;

	vma->vm_private_data = umm;
	vma->vm_ops = &umm_vmops;
	if (xnarch_cache_aliasing())
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	ret = rtdm_mmap_vmem(vma, xnheap_get_membase(&umm->heap));
	if (ret)
		return ret;

	atomic_inc(&umm->refcount);

	return 0;
}

#ifndef CONFIG_MMU
static unsigned long umm_get_unmapped_area(struct rtdm_fd *fd,
					   unsigned long len,
					   unsigned long pgoff,
					   unsigned long flags)
{
	struct steely_umm *umm;

	umm = umm_from_fd(fd);
	if (umm == NULL)
		return -ENODEV;

	if (pgoff == 0)
		return (unsigned long)xnheap_get_membase(&umm->heap);

	return pgoff << PAGE_SHIFT;
}
#else
#define umm_get_unmapped_area	NULL
#endif

static int stat_umm(struct rtdm_fd *fd,
		    struct steely_umm __user *u_stat)
{
	struct steely_memdev_stat stat;
	struct steely_umm *umm;
	spl_t s;
	
	umm = umm_from_fd(fd);
	if (umm == NULL)
		return -ENODEV;

	xnlock_get_irqsave(&umm->heap.lock, s);
	stat.size = xnheap_get_size(&umm->heap);
	stat.free = xnheap_get_free(&umm->heap);
	xnlock_put_irqrestore(&umm->heap.lock, s);

	return rtdm_safe_copy_to_user(fd, u_stat, &stat, sizeof(stat));
}

static int do_umm_ioctls(struct rtdm_fd *fd,
			 unsigned int request, void __user *arg)
{
	int ret;

	switch (request) {
	case MEMDEV_RTIOC_STAT:
		ret = stat_umm(fd, arg);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int umm_ioctl_rt(struct rtdm_fd *fd,
			unsigned int request, void __user *arg)
{
	return do_umm_ioctls(fd, request, arg);
}

static int umm_ioctl_nrt(struct rtdm_fd *fd,
			 unsigned int request, void __user *arg)
{
	return do_umm_ioctls(fd, request, arg);
}

static int sysmem_open(struct rtdm_fd *fd, int oflags)
{
	if ((oflags & O_ACCMODE) != O_RDONLY)
		return -EACCES;

	return 0;
}

static int do_sysmem_ioctls(struct rtdm_fd *fd,
			    unsigned int request, void __user *arg)
{
	struct steely_memdev_stat stat;
	spl_t s;
	int ret;

	switch (request) {
	case MEMDEV_RTIOC_STAT:
		xnlock_get_irqsave(&steely_heap.lock, s);
		stat.size = xnheap_get_size(&steely_heap);
		stat.free = xnheap_get_free(&steely_heap);
		xnlock_put_irqrestore(&steely_heap.lock, s);
		ret = rtdm_safe_copy_to_user(fd, arg, &stat, sizeof(stat));
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int sysmem_ioctl_rt(struct rtdm_fd *fd,
			   unsigned int request, void __user *arg)
{
	return do_sysmem_ioctls(fd, request, arg);
}

static int sysmem_ioctl_nrt(struct rtdm_fd *fd,
			   unsigned int request, void __user *arg)
{
	return do_sysmem_ioctls(fd, request, arg);
}

static struct rtdm_driver umm_driver = {
	.profile_info	=	RTDM_PROFILE_INFO(umm,
						  RTDM_CLASS_MEMORY,
						  RTDM_SUBCLASS_GENERIC,
						  0),
	.device_flags	=	RTDM_NAMED_DEVICE|RTDM_FIXED_MINOR,
	.device_count	=	2,
	.ops = {
		.ioctl_rt		=	umm_ioctl_rt,
		.ioctl_nrt		=	umm_ioctl_nrt,
		.mmap			=	umm_mmap,
		.get_unmapped_area	=	umm_get_unmapped_area,
	},
};

static struct rtdm_device umm_devices[] = {
	[ UMM_PRIVATE ] = {
		.driver = &umm_driver,
		.label = STEELY_MEMDEV_PRIVATE,
		.minor = UMM_PRIVATE,
	},
	[ UMM_SHARED ] = {
		.driver = &umm_driver,
		.label = STEELY_MEMDEV_SHARED,
		.minor = UMM_SHARED,
	},
};

static struct rtdm_driver sysmem_driver = {
	.profile_info	=	RTDM_PROFILE_INFO(sysmem,
						  RTDM_CLASS_MEMORY,
						  SYS_GLOBAL,
						  0),
	.device_flags	=	RTDM_NAMED_DEVICE,
	.device_count	=	1,
	.ops = {
		.open		=	sysmem_open,
		.ioctl_rt	=	sysmem_ioctl_rt,
		.ioctl_nrt	=	sysmem_ioctl_nrt,
	},
};

static struct rtdm_device sysmem_device = {
	.driver = &sysmem_driver,
	.label = STEELY_MEMDEV_SYS,
};

static inline void init_vdso(void)
{
	nkvdso->features = XNVDSO_FEATURES;
}

int steely_memdev_init(void)
{
	int ret;

	ret = steely_umm_init(&steely_kernel_ppd.umm,
			      CONFIG_STEELY_SHARED_HEAPSZ * 1024, NULL);
	if (ret)
		return ret;

	steely_umm_set_name(&steely_kernel_ppd.umm, "shared heap");

	nkvdso = steely_umm_alloc(&steely_kernel_ppd.umm, sizeof(*nkvdso));
	if (nkvdso == NULL) {
		ret = -ENOMEM;
		goto fail_vdso;
	}

	init_vdso();

	ret = rtdm_dev_register(umm_devices + UMM_PRIVATE);
	if (ret)
		goto fail_private;

	ret = rtdm_dev_register(umm_devices + UMM_SHARED);
	if (ret)
		goto fail_shared;

	ret = rtdm_dev_register(&sysmem_device);
	if (ret)
		goto fail_sysmem;

	return 0;

fail_sysmem:
	rtdm_dev_unregister(umm_devices + UMM_SHARED);
fail_shared:
	rtdm_dev_unregister(umm_devices + UMM_PRIVATE);
fail_private:
	steely_umm_free(&steely_kernel_ppd.umm, nkvdso);
fail_vdso:
	steely_umm_destroy(&steely_kernel_ppd.umm);

	return ret;
}

void steely_memdev_cleanup(void)
{
	rtdm_dev_unregister(&sysmem_device);
	rtdm_dev_unregister(umm_devices + UMM_SHARED);
	rtdm_dev_unregister(umm_devices + UMM_PRIVATE);
	steely_umm_free(&steely_kernel_ppd.umm, nkvdso);
	steely_umm_destroy(&steely_kernel_ppd.umm);
}

int steely_umm_init(struct steely_umm *umm, u32 size,
		    void (*release)(struct steely_umm *umm))
{
	void *basemem;
	int ret;

	secondary_mode_only();

	size = PAGE_ALIGN(size);
	basemem = __vmalloc(size, GFP_KERNEL|__GFP_ZERO,
			    xnarch_cache_aliasing() ?
			    pgprot_noncached(PAGE_KERNEL) : PAGE_KERNEL);
	if (basemem == NULL)
		return -ENOMEM;

	ret = xnheap_init(&umm->heap, basemem, size);
	if (ret) {
		vfree(basemem);
		return ret;
	}

	umm->release = release;
	atomic_set(&umm->refcount, 1);
	smp_mb();

	return 0;
}

void steely_umm_destroy(struct steely_umm *umm)
{
	secondary_mode_only();

	if (atomic_dec_and_test(&umm->refcount)) {
		xnheap_destroy(&umm->heap);
		vfree(xnheap_get_membase(&umm->heap));
		if (umm->release)
			umm->release(umm);
	}
}
