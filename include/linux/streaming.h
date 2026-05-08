/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Streaming page-table memory type — OS-side prototype of the contract
 * proposed in the Directory Tax paper (§4): a read-only, prefetchable,
 * directory-bypassable mapping for fabric-attached memory.  The OS
 * enforces I0 (per-frame type uniformity) and I1 (read-only via PTE
 * r/w bit); H1/H2 (prefetch activation, directory bypass) require
 * Streaming-aware silicon and are exercised through gem5.
 *
 * This header lives outside arch/ so generic mm/ files (mmap.c,
 * mprotect.c, …) compile on every architecture without conditional
 * inclusions.
 */
#ifndef _LINUX_STREAMING_H
#define _LINUX_STREAMING_H

#include <linux/types.h>
#include <linux/mm_types.h>

struct vm_area_struct;

#ifdef CONFIG_PAT_STREAMING
#include <asm/memtype.h>		/* pat_enabled() */
#include <asm/pgtable_types.h>

/**
 * streaming_supported() - is MAP_STREAMING usable on this kernel?
 *
 * Returns true only when both the kernel was built with the Streaming
 * prototype (CONFIG_PAT_STREAMING) and the hardware actually advertises
 * PAT.  do_mmap() short-circuits with -EOPNOTSUPP otherwise; userspace
 * then knows to fall back to plain WB.
 */
static inline bool streaming_supported(void)
{
	return pat_enabled();
}

void streaming_pte_audit(struct vm_area_struct *vma, pte_t pte);

#else /* !CONFIG_PAT_STREAMING */

static inline bool streaming_supported(void)
{
	return false;
}

static inline void streaming_pte_audit(struct vm_area_struct *vma, pte_t pte)
{
}

#endif /* CONFIG_PAT_STREAMING */

#endif /* _LINUX_STREAMING_H */
