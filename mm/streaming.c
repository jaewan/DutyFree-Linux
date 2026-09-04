// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/streaming.c - PAT-slot-6 Streaming memory type transitions
 *
 * Implements the OS half of the Streaming hardware-software contract
 * from *The Directory Tax* §4. Userspace toggles a region between
 * regular write-back and Streaming via mprotect(PROT_STREAMING); the
 * functions in this file own the PTE cache-bit rewrite.  Baseline H2 remains
 * coherent and needs no cache writeback; an optional build-time H3 oracle owns
 * the conservative WBNOINVD broadcast.
 */

#define pr_fmt(fmt)	"PAT-Streaming: " fmt

#include <linux/mm.h>
#include <linux/debugfs.h>
#include <linux/hugetlb.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/pagewalk.h>
#include <linux/sched/mm.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/userfaultfd_k.h>

#include <xen/xen.h>

#include <asm/cacheflush.h>
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

#ifdef CONFIG_HUGETLB_PAGE
/*
 * hugetlb leaves are rewritten in place at PMD (2 MiB) or PUD (1 GiB)
 * granularity. At leaf level bit 7 is PSE, so the PAT selector moves
 * to bit 12 (_PAGE_PAT_LARGE); protval_4k_2_large() converts the 4K
 * target encoding accordingly (slot 6 stays slot 6).
 *
 * walk_hugetlb_range() holds the hugetlb vma lock in read mode, which
 * keeps the page tables from being freed but does not protect the pte
 * contents - take the huge_pte lock ourselves. This prototype admits only
 * private hugetlb VMAs and preflight rejects multiply mapped folios, so PMD
 * sharing is outside the admitted state space.
 */
static int streaming_hugetlb_entry(pte_t *ptep, unsigned long hmask,
				   unsigned long addr, unsigned long next,
				   struct mm_walk *walk)
{
	struct streaming_walk_ctx *ctx = walk->private;
	struct hstate *h = hstate_vma(walk->vma);
	pteval_t target = protval_4k_2_large(ctx->target_cache_bits);
	spinlock_t *ptl;
	pteval_t newval;
	pte_t old;
	int ret = 0;

	ptl = huge_pte_lock(h, walk->mm, ptep);
	old = huge_ptep_get(ptep);
	if (huge_pte_none(old))
		/* hole - a later fault installs bits from vm_page_prot */
		goto out;
	if (!pte_present(old)) {
		/* migration / hwpoison / pte marker - prototype rejects */
		ret = -EBUSY;
		goto out;
	}

	newval = (pte_val(old) & ~_PAGE_LARGE_CACHE_MASK) | target;
	if (newval != pte_val(old))
		set_huge_pte_at(walk->mm, addr & hmask, ptep, __pte(newval),
				huge_page_size(h));
out:
	spin_unlock(ptl);
	return ret;
}
#else
#define streaming_hugetlb_entry	NULL
#endif

static const struct mm_walk_ops streaming_walk_ops = {
	.pmd_entry	= streaming_pmd_entry,
	.pte_entry	= streaming_pte_entry,
	.hugetlb_entry	= streaming_hugetlb_entry,
	.walk_lock	= PGWALK_WRLOCK_VERIFY,
};

/*
 * Validate a transition before mprotect() changes VMA flags or permissions.
 * Every page must already be present, normal, exclusively mapped, and free of
 * FOLL_PIN references.  In
 * particular, accepting a hole would let a later read fault install Linux's
 * globally shared zero page with a conflicting memory type. A present huge PMD
 * is fine because MM_CP_STREAMING_ENTER will split it before the rewrite pass.
 * Swap/migration markers are an explicit refusal: discovering one after
 * mutating protections used to leave a partial request.
 */
static int streaming_preflight_pte_entry(pte_t *pte, unsigned long addr,
					 unsigned long next,
					 struct mm_walk *walk)
{
	pte_t entry = ptep_get(pte);
	struct page *page;

	if (!pte_present(entry))
		return -EBUSY;

	page = vm_normal_page(walk->vma, addr, entry);
	if (!page || page_mapcount(page) > 1 || page_maybe_dma_pinned(page))
		return -EBUSY;

	return 0;
}

static int streaming_preflight_pte_hole(unsigned long addr, unsigned long next,
					int depth, struct mm_walk *walk)
{
	return -EBUSY;
}

static int streaming_preflight_pmd_entry(pmd_t *pmd, unsigned long addr,
					 unsigned long next,
					 struct mm_walk *walk)
{
	struct page *page;

	if (!pmd_trans_huge(*pmd))
		return 0;
	page = pmd_page(*pmd);
	return page_mapcount(page) > 1 || page_maybe_dma_pinned(page) ?
		-EBUSY : 0;
}

#ifdef CONFIG_HUGETLB_PAGE
static int streaming_preflight_hugetlb_entry(pte_t *ptep, unsigned long hmask,
					      unsigned long addr,
					      unsigned long next,
					      struct mm_walk *walk)
{
	pte_t entry = huge_ptep_get(ptep);

	if (!pte_present(entry))
		return -EBUSY;
	return page_mapcount(pte_page(entry)) > 1 ||
	       page_maybe_dma_pinned(pte_page(entry)) ? -EBUSY : 0;
}
#else
#define streaming_preflight_hugetlb_entry NULL
#endif

static const struct mm_walk_ops streaming_preflight_ops = {
	.pmd_entry	= streaming_preflight_pmd_entry,
	.pte_entry	= streaming_preflight_pte_entry,
	.pte_hole	= streaming_preflight_pte_hole,
	.hugetlb_entry	= streaming_preflight_hugetlb_entry,
	.walk_lock	= PGWALK_WRLOCK_VERIFY,
};

int streaming_preflight_range(struct vm_area_struct *vma,
			      unsigned long start, unsigned long end)
{
	return walk_page_range_vma(vma, start, end, &streaming_preflight_ops,
				   NULL);
}

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
	 * A shared-anonymous VMA is just as dangerous as a shared file VMA:
	 * a process inherited across fork can keep an alias to the same folio.
	 * Shared objects require persistent object-wide epoch state, which this
	 * VMA-local prototype deliberately does not pretend to provide.
	 */
	if (vma->vm_flags & VM_SHARED)
		return -EINVAL;

	/*
	 * Ordinary private file mappings can still alias the page cache through
	 * another process or mapping.  Private hugetlb VMAs carry a hugetlbfs
	 * file internally and are the controlled exception already covered by
	 * the hugetlb locking rules.
	 */
	if (vma->vm_file && !(vma->vm_flags & VM_SHARED) &&
	    !is_vm_hugetlb_page(vma))
		return -EINVAL;

	/* KSM could merge the folio with a differently typed VMA after entry. */
	if (vma->vm_flags & VM_MERGEABLE)
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
 *
 * hugetlb VMAs are not split; their PMD/PUD leaves are rewritten in
 * place with the large-page PAT encoding (bit 12 instead of bit 7)
 * by streaming_hugetlb_entry().
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
	if (is_vm_hugetlb_page(vma))
		flush_hugetlb_tlb_range(vma, start, end);
	else
		flush_tlb_range(vma, start, end);
	return 0;
}

/*
 * ---------------------------------------------------------------------
 * Ranged exit drain.
 *
 * Baseline H2 entry performs no cache writeback at all: it changes only
 * future shared-cache admission. do_mprotect_pkey() reaches the machine-wide
 * WBNOINVD solely under CONFIG_PAT_STREAMING_H3_SEAL_ORACLE, which is
 * default n and is documented as not part of baseline H2 semantics. That
 * oracle clean costs ~48 ms on the 64-logical-CPU evaluation host, is
 * size-independent, and scales with logical-CPU count rather than object
 * size -- the wrong granularity for an object-scoped contract, which is why
 * baseline entry does not use it.
 *
 * A drain is also conservative rather than necessary under H2-only semantics.
 * WB and Streaming are both coherent cacheable types, so a dirty line left
 * in a cache at entry is still findable by coherence and a later Streaming
 * read snoops it -- there is no data hazard on the read path. The hazard is
 * at the other end: stale clean Streaming lines must not survive into a
 * differently-typed reuse of the frame. That is an exit/reclaim concern, and
 * unlike a writeback of every cache it can be ranged over the object.
 *
 * NOT sound with H3. H3 declines coherence enrolment, so a dirty line left
 * behind at entry could no longer be located -- H3 requires the entry drain.
 * This prototype consequently has no runtime exit-drain mode: changing a
 * debugfs knob must never weaken the data-correctness contract.
 *
 * streaming_drain_range() below therefore has no caller. The exit-drain call
 * site went out with the drain_at_exit knob; the primitive is kept because an
 * H3 retirement path needs a ranged clean, and reintroducing it as a page walk
 * is the part that took the care.
 */

static void streaming_flush_page(struct page *page, unsigned long size)
{
	if (!page)
		return;
	/*
	 * Flush through the kernel direct map rather than the user VA: a
	 * supervisor-mode access to a user page would trip SMAP.
	 */
	clflush_cache_range(page_address(page), size);
}

static int streaming_flush_pte_entry(pte_t *pte, unsigned long addr,
				     unsigned long next, struct mm_walk *walk)
{
	pte_t old = ptep_get(pte);

	if (pte_present(old))
		streaming_flush_page(pte_page(old), PAGE_SIZE);
	return 0;
}

#ifdef CONFIG_HUGETLB_PAGE
static int streaming_flush_hugetlb_entry(pte_t *ptep, unsigned long hmask,
					 unsigned long addr, unsigned long next,
					 struct mm_walk *walk)
{
	struct hstate *h = hstate_vma(walk->vma);
	pte_t old = huge_ptep_get(ptep);

	if (pte_present(old))
		streaming_flush_page(pte_page(old), huge_page_size(h));
	return 0;
}
#else
#define streaming_flush_hugetlb_entry NULL
#endif

static const struct mm_walk_ops streaming_flush_ops = {
	.pte_entry	= streaming_flush_pte_entry,
	.hugetlb_entry	= streaming_flush_hugetlb_entry,
	.walk_lock	= PGWALK_WRLOCK_VERIFY,
};

/*
 * Drain [start,end) by line, over whatever VMAs it spans. Only present PTEs
 * are touched, so an unpopulated hole costs nothing. Called after the PTE
 * type has been flipped back to WB and the TLB shot down, which is the
 * ordering the datapath requires.
 */
int streaming_drain_range(struct mm_struct *mm, unsigned long start,
			  unsigned long end)
{
	int err;

	if (start >= end)
		return 0;
	err = walk_page_range(mm, start, end, &streaming_flush_ops, NULL);
	return err;
}

/*
 * Push every dirty cache line back to memory for the optional conservative
 * H3 seal oracle.  Baseline H2 retains coherent owner lookup and never calls
 * this function.
 *
 * One WBNOINVD per physical core suffices: SMT siblings share every
 * cache level, and hitting both siblings only doubles the wall-clock
 * cost through shared write-back contention (measured ~21.5ms ->
 * ~12ms on a 2-socket 64-core SPR).
 */
void streaming_writeback_all(void)
{
	wbnoinvd_on_each_core();
}

/*
 * --------------------------------------------------------------------
 * Prototype debugfs interface: /sys/kernel/debug/streaming/pte_query
 *
 * Selftests need to observe the raw PTE value at a given virtual
 * address to confirm that PROT_STREAMING actually installs the
 * expected slot-6 cache encoding. /proc/PID/pagemap exposes the PFN
 * but not the PTE attribute bits, so we add a tiny debugfs entry
 * here. The interface is intentionally prototype-only - the long
 * term plan is to surface streaming state via /proc/PID/smaps.
 *
 * Protocol:
 *   - Write a hex virtual address (e.g. "7fff5e4c1000") to the file.
 *   - Read back a single line "<vaddr_hex> <pte_hex> <level_shift>\n"
 *     with the raw PTE value as observed by the caller's own mm.
 *     level_shift is 12 for a 4K PTE, 21 for a 2MB PMD leaf and 30
 *     for a 1GB PUD leaf (hugetlb). Consumers that only sscanf the
 *     first two tokens keep working.
 *
 * Only the writer's own address space is inspected; there is no path
 * to read another task's PTEs.
 * --------------------------------------------------------------------
 */
#ifdef CONFIG_DEBUG_FS

struct streaming_debug_state {
	struct mutex lock;
	bool valid;
	unsigned long vaddr;
	u64 pteval;
	unsigned int level_shift;
};

static struct streaming_debug_state debug_state = {
	.lock = __MUTEX_INITIALIZER(debug_state.lock),
};

static int streaming_debug_lookup(unsigned long vaddr, u64 *out,
				  unsigned int *level_shift)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;
	int ret = -ENOENT;

	if (!mm)
		return -ESRCH;

	mmap_read_lock(mm);
	vma = vma_lookup(mm, vaddr);
	if (!vma)
		goto unlock;

	pgd = pgd_offset(mm, vaddr);
	if (pgd_none_or_clear_bad(pgd))
		goto unlock;
	p4d = p4d_offset(pgd, vaddr);
	if (p4d_none_or_clear_bad(p4d))
		goto unlock;
	pud = pud_offset(p4d, vaddr);
	if (pud_none(*pud))
		goto unlock;
	if (pud_leaf(*pud)) {
		/*
		 * 1GB hugetlb leaf. Must be tested before
		 * pud_none_or_clear_bad(): pud_bad() is true for a leaf
		 * PUD and pud_clear_bad() would wipe the live mapping.
		 */
		*out = pud_val(*pud);
		*level_shift = PUD_SHIFT;
		ret = 0;
		goto unlock;
	}
	if (pud_bad(*pud))
		goto unlock;
	pmd = pmd_offset(pud, vaddr);
	if (pmd_none(*pmd))
		goto unlock;
	if (pmd_leaf(*pmd) || pmd_devmap(*pmd)) {
		/*
		 * 2MB leaf (hugetlb or a stray THP/devmap). Streaming
		 * splits THP but keeps hugetlb PMDs; report as-is.
		 */
		*out = pmd_val(*pmd);
		*level_shift = PMD_SHIFT;
		ret = 0;
		goto unlock;
	}
	if (pmd_bad(*pmd))
		goto unlock;

	pte = pte_offset_map_lock(mm, pmd, vaddr, &ptl);
	if (!pte)
		goto unlock;
	*out = pte_val(ptep_get(pte));
	*level_shift = PAGE_SHIFT;
	ret = 0;
	pte_unmap_unlock(pte, ptl);

unlock:
	mmap_read_unlock(mm);
	return ret;
}

static ssize_t streaming_pte_query_write(struct file *f,
					 const char __user *ubuf,
					 size_t len, loff_t *ppos)
{
	char buf[24];
	unsigned long vaddr;
	unsigned int level_shift;
	u64 pteval;
	int ret;

	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	ret = kstrtoul(strim(buf), 16, &vaddr);
	if (ret)
		return ret;

	ret = streaming_debug_lookup(vaddr, &pteval, &level_shift);
	if (ret)
		return ret;

	mutex_lock(&debug_state.lock);
	debug_state.vaddr = vaddr;
	debug_state.pteval = pteval;
	debug_state.level_shift = level_shift;
	debug_state.valid = true;
	mutex_unlock(&debug_state.lock);
	return len;
}

static int streaming_pte_query_show(struct seq_file *s, void *v)
{
	mutex_lock(&debug_state.lock);
	if (debug_state.valid)
		seq_printf(s, "%016lx %016llx %u\n",
			   debug_state.vaddr, debug_state.pteval,
			   debug_state.level_shift);
	else
		seq_puts(s, "no-query\n");
	mutex_unlock(&debug_state.lock);
	return 0;
}

static int streaming_pte_query_open(struct inode *inode, struct file *file)
{
	return single_open(file, streaming_pte_query_show, NULL);
}

static const struct file_operations streaming_pte_query_fops = {
	.owner		= THIS_MODULE,
	.open		= streaming_pte_query_open,
	.read		= seq_read,
	.write		= streaming_pte_query_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init streaming_debugfs_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("streaming", NULL);
	if (IS_ERR(dir))
		return PTR_ERR(dir);
	debugfs_create_file("pte_query", 0600, dir, NULL,
			    &streaming_pte_query_fops);
	return 0;
}
late_initcall(streaming_debugfs_init);

#endif /* CONFIG_DEBUG_FS */
