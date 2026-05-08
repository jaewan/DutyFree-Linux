// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/streaming.c — OS-side prototype of the Streaming page-table
 * memory type (Directory Tax §4).
 *
 * Step 1 deliverables:
 *   * streaming_pte_audit() — debug-time invariant check that the PAT
 *     slot 6 encoding plus _PAGE_SOFTW1 reach the installed PTE, and
 *     that the PTE is read-only (I1).
 *   * /sys/kernel/debug/streaming/ — small query interface that the
 *     kselftest suite uses to read back a PTE for a given user-space
 *     virtual address, proving that the Streaming intent reaches the
 *     hardware fill-pipeline encoding.
 *
 * Out of scope here (follow-up steps): socket-keyed I0 enforcement,
 * two-phase teardown, OOM quarantine, KVM EPT propagation.
 */

#include <linux/streaming.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <asm/pgtable.h>

/*
 * Sanity-check a PTE installed for a Streaming VMA.  Slow path: only
 * runs under CONFIG_DEBUG_VM, only for VMAs already carrying
 * VM_STREAMING.  A WARN_ONCE here is a kernel bug, never user-induced.
 *
 *   1. _PAGE_SOFTW1 must be set — the OS-side intent marker survives
 *      pte_modify() via _COMMON_PAGE_CHG_MASK (which already preserves
 *      _PAGE_SPECIAL, the alias of bit 9).
 *   2. Cache-mode bits must encode PAT slot 6.
 *   3. The PTE must be write-protected (I1).
 */
void streaming_pte_audit(struct vm_area_struct *vma, pte_t pte)
{
#ifdef CONFIG_DEBUG_VM
	pteval_t v;
	pteval_t streaming_cache;

	if (!vma || !is_streaming_vma(vma))
		return;

	v = pte_val(pte);
	streaming_cache = cachemode2protval(_PAGE_CACHE_MODE_STREAMING);

	WARN_ONCE(!(v & _PAGE_SOFTW1),
		  "streaming: PTE %lx for VMA %p missing _PAGE_SOFTW1 marker\n",
		  (unsigned long)v, vma);
	WARN_ONCE((v & _PAGE_CACHE_MASK) != streaming_cache,
		  "streaming: PTE %lx cache bits %lx != Streaming %lx\n",
		  (unsigned long)v,
		  (unsigned long)(v & _PAGE_CACHE_MASK),
		  (unsigned long)streaming_cache);
	WARN_ONCE(pte_write(pte),
		  "streaming: PTE %lx writable for read-only VMA %p (I1 violated)\n",
		  (unsigned long)v, vma);
#endif
}
EXPORT_SYMBOL_GPL(streaming_pte_audit);

/* ----------------------------------------------------------------- */
/* debugfs query interface used by the kselftest suite.              */
/* ----------------------------------------------------------------- */

struct streaming_query {
	spinlock_t lock;
	unsigned long vaddr;
	pteval_t pte_val;
	bool present;
	bool valid;
	bool huge;
};

/*
 * Walk the calling task's page tables for @addr and return the leaf
 * entry's raw value plus an "is huge" flag.  Device-DAX commonly faults
 * at PMD or PUD granularity, so we have to stop at whichever level the
 * leaf actually lives, not just at the PTE level.
 */
static int streaming_walk_pte(struct mm_struct *mm, unsigned long addr,
			      pteval_t *out, bool *is_huge)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	pte_t pte;

	*is_huge = false;
	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd))
		return -ENOENT;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d))
		return -ENOENT;
	if (p4d_leaf(*p4d)) {
		*out = p4d_val(*p4d);
		*is_huge = true;
		return 0;
	}
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud))
		return -ENOENT;
	if (pud_leaf(*pud)) {
		*out = pud_val(*pud);
		*is_huge = true;
		return 0;
	}
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return -ENOENT;
	/* PMD leaf: device-DAX 2 MiB or transparent hugepage. */
	if (pmd_leaf(*pmd)) {
		*out = pmd_val(*pmd);
		*is_huge = true;
		return 0;
	}
	ptep = pte_offset_kernel(pmd, addr);
	if (!ptep)
		return -ENOENT;
	pte = ptep_get(ptep);
	if (!pte_present(pte))
		return -ENOENT;
	*out = pte_val(pte);
	return 0;
}

static ssize_t streaming_query_write(struct file *f, const char __user *ubuf,
				     size_t len, loff_t *pos)
{
	struct streaming_query *q = f->private_data;
	char buf[32];
	unsigned long addr;
	pteval_t v;
	bool huge = false;
	int ret;

	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (kstrtoul(strim(buf), 0, &addr))
		return -EINVAL;

	mmap_read_lock(current->mm);
	ret = streaming_walk_pte(current->mm, addr, &v, &huge);
	mmap_read_unlock(current->mm);

	spin_lock(&q->lock);
	q->vaddr = addr;
	q->pte_val = (ret == 0) ? v : 0;
	q->present = (ret == 0);
	q->huge = (ret == 0) ? huge : false;
	q->valid = true;
	spin_unlock(&q->lock);

	return len;
}

static ssize_t streaming_query_read(struct file *f, char __user *ubuf,
				    size_t len, loff_t *pos)
{
	struct streaming_query *q = f->private_data;
	char out[128];
	int n;
	bool valid, present, huge;
	unsigned long addr;
	pteval_t v, pat_bit;

	spin_lock(&q->lock);
	valid = q->valid;
	present = q->present;
	huge = q->huge;
	addr = q->vaddr;
	v = q->pte_val;
	spin_unlock(&q->lock);

	if (!valid)
		n = scnprintf(out, sizeof(out),
			      "no query yet; write a hex vaddr first\n");
	else if (!present)
		n = scnprintf(out, sizeof(out),
			      "addr=0x%lx not-present\n", addr);
	else {
		/*
		 * For huge mappings the PAT bit lives at _PAGE_PAT_LARGE
		 * (bit 12) instead of _PAGE_PAT (bit 7, which is _PAGE_PSE
		 * on huge entries).  Report a normalised "pat=" reading so
		 * the kselftest does not have to special-case page size.
		 */
		pat_bit = huge ? _PAGE_PAT_LARGE : _PAGE_PAT;
		n = scnprintf(out, sizeof(out),
			      "addr=0x%lx pte=0x%lx softw1=%d pat=%d pcd=%d pwt=%d write=%d huge=%d\n",
			      addr, (unsigned long)v,
			      !!(v & _PAGE_SOFTW1),
			      !!(v & pat_bit),
			      !!(v & _PAGE_PCD),
			      !!(v & _PAGE_PWT),
			      !!(v & _PAGE_RW),
			      huge);
	}

	return simple_read_from_buffer(ubuf, len, pos, out, n);
}

static int streaming_query_open(struct inode *inode, struct file *f)
{
	struct streaming_query *q;

	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	spin_lock_init(&q->lock);
	f->private_data = q;
	return 0;
}

static int streaming_query_release(struct inode *inode, struct file *f)
{
	kfree(f->private_data);
	return 0;
}

static const struct file_operations streaming_query_fops = {
	.owner		= THIS_MODULE,
	.open		= streaming_query_open,
	.release	= streaming_query_release,
	.read		= streaming_query_read,
	.write		= streaming_query_write,
	.llseek		= default_llseek,
};

static int __init streaming_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("streaming", NULL);
	if (IS_ERR(dir)) {
		pr_warn("streaming: debugfs root creation failed (%ld)\n",
			PTR_ERR(dir));
		return 0;
	}
	debugfs_create_file("pte_query", 0600, dir, NULL,
			    &streaming_query_fops);
	pr_info("streaming: PAT slot 6 + _PAGE_SOFTW1 prototype ready\n");
	return 0;
}
late_initcall(streaming_init);
