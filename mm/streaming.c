// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/streaming.c - PAT-slot-6 Streaming memory type transitions
 *
 * Implements the OS half of the Streaming hardware-software contract
 * from *The Directory Tax* §4. Userspace toggles a region between
 * regular write-back and Streaming via mprotect(PROT_STREAMING); the
 * functions in this file own the PTE cache-bit rewrite and the
 * WBNOINVD broadcast that flush dirty data to memory before the
 * Streaming semantics take effect.
 */

#define pr_fmt(fmt)	"PAT-Streaming: " fmt

#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagewalk.h>
#include <linux/userfaultfd_k.h>

#include <xen/xen.h>

#include <asm/pgtable.h>
#include <asm/smp.h>
#include <asm/special_insns.h>
#include <asm/tlbflush.h>

#include "internal.h"

struct streaming_walk_ctx {
	pteval_t target_cache_bits;
};

static int streaming_pmd_entry(pmd_t *pmd, unsigned long addr,
			       unsigned long next, struct mm_walk *walk)
{
	/*
	 * The mprotect_fixup() caller asks change_protection() to split
	 * THP/devmap PMDs through MM_CP_STREAMING_ALL, so by the time we
	 * walk the range every PMD should be PTE-level. If a huge PMD
	 * still sits here something went wrong - bail rather than
	 * silently miss those bytes.
	 */
	if (pmd_trans_huge(*pmd) || pmd_devmap(*pmd))
		return -EBUSY;
	return 0;
}

static int streaming_pte_entry(pte_t *pte, unsigned long addr,
			       unsigned long next, struct mm_walk *walk)
{
	struct streaming_walk_ctx *ctx = walk->private;
	pte_t old = ptep_get(pte);
	pteval_t newval;

	if (pte_none(old))
		return 0;
	if (!pte_present(old))
		/* swap or migration entry - prototype rejects */
		return -EBUSY;

	newval = (pte_val(old) & ~_PAGE_CACHE_MASK) | ctx->target_cache_bits;
	if (newval == pte_val(old))
		return 0;

	set_pte_at(walk->mm, addr, pte, __pte(newval));
	return 0;
}

static const struct mm_walk_ops streaming_walk_ops = {
	.pmd_entry	= streaming_pmd_entry,
	.pte_entry	= streaming_pte_entry,
	.walk_lock	= PGWALK_WRLOCK_VERIFY,
};

/*
 * Reject VMA types where flipping the cache mode under the mapping
 * would corrupt other subsystems' invariants. Called with
 * mmap_write_lock held.
 */
int streaming_validate_entry(struct vm_area_struct *vma)
{
	/*
	 * Xen PV overrides ptep_modify_prot_transaction(), bypassing the
	 * cache-bit preservation we rely on in _PAGE_CHG_MASK.
	 */
	if (xen_pv_domain())
		return -EOPNOTSUPP;

	/*
	 * track_pfn_remap() already owns the cache bits of a VM_PAT VMA
	 * (typically device-DAX via remap_pfn_range); rewriting them
	 * would desynchronise the memtype tracker.
	 */
	if (vma->vm_flags & VM_PAT)
		return -EINVAL;

	/* Device-DAX / PFN-only mappings are out of prototype scope. */
	if (vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP))
		return -EINVAL;

	/*
	 * Writable file-backed mappings can have writeback I/O in flight
	 * against dirty page-cache entries; mixing that with Streaming
	 * cache semantics is a footgun.
	 */
	if ((vma->vm_flags & VM_SHARED) && vma->vm_file)
		return -EINVAL;

	/*
	 * Userfaultfd-WP installs PTE markers and serialises against
	 * change_protection() in ways that race with our post-pass
	 * cache-bit rewrite.
	 */
	if (userfaultfd_wp(vma))
		return -EINVAL;

	return 0;
}

/*
 * Walk [start, end) of @vma and rewrite each present PTE's cache-mode
 * bits to either Streaming (slot 6) or plain WB (slot 0). Must be
 * called after change_protection() so the PMD-level THP split has
 * already happened and the new R/W/Exec state is in place; the
 * cache-bit rewrite is then committed under each PTE lock and the
 * range is invalidated in every CPU's TLB.
 */
int streaming_apply_cache_bits(struct vm_area_struct *vma,
			       unsigned long start, unsigned long end,
			       bool to_streaming)
{
	struct streaming_walk_ctx ctx = {
		.target_cache_bits = to_streaming
			? cachemode2protval(_PAGE_CACHE_MODE_STREAMING)
			: cachemode2protval(_PAGE_CACHE_MODE_WB),
	};
	int err;

	err = walk_page_range_vma(vma, start, end,
				  &streaming_walk_ops, &ctx);
	if (err)
		return err;

	/*
	 * Make the new cache encoding observable system-wide. The earlier
	 * change_protection() flush only covered R/W/Exec changes; the
	 * cache bits were re-written after that, so we need a fresh
	 * range invalidation here.
	 */
	flush_tlb_range(vma, start, end);
	return 0;
}

/*
 * Push every dirty cache line on every CPU back to memory. Used when
 * transitioning a region from WB into Streaming, so that all data
 * the region used to hold is durable in RAM before the Streaming
 * semantics (no further writes, simulator may silently discard
 * clean evictions) become observable.
 */
void streaming_writeback_all(void)
{
	wbnoinvd_on_all_cpus();
}
