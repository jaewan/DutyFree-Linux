// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the PAT-slot-6 Streaming machinery (Directory Tax §4).
 *
 * Tests here cover only what can be checked from within the kernel
 * without a userspace mprotect call: PAT MSR slot layout, the
 * cachemode <-> PTE-bit translation tables, pgprot_streaming() bit
 * pattern, VM_STREAMING bit position, and CP-flag plumbing.
 *
 * End-to-end mprotect behaviour is exercised by the matching kselftest
 * in tools/testing/selftests/mm/streaming/.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/rmap.h>

#include <asm/memtype.h>
#include <asm/msr.h>
#include <asm/pgtable.h>
#include <asm/pgtable_types.h>

#include "internal.h"

#define PAT_BYTE(pat, slot)	(((pat) >> ((slot) * 8)) & 0xff)
#define PAT_WB_ENCODING		6

static void pat_streaming_msr_test(struct kunit *test)
{
	u64 pat;

	rdmsrl(MSR_IA32_CR_PAT, pat);
	/*
	 * Slot 6 must encode WB (PAT_WB = 6) on full-PAT silicon.
	 * On platforms that fell into the pre-PAT / errata layouts
	 * slot 6 is not the Streaming slot, so skip rather than fail.
	 */
	if (PAT_BYTE(pat, 0) != 6 || PAT_BYTE(pat, 1) != 1)
		kunit_skip(test, "non full-PAT layout, slot 6 not Streaming");
	KUNIT_EXPECT_EQ(test, (unsigned int)PAT_BYTE(pat, 6),
			(unsigned int)PAT_WB_ENCODING);
}

static void cachemode_table_roundtrip_test(struct kunit *test)
{
	enum page_cache_mode modes[] = {
		_PAGE_CACHE_MODE_WB,
		_PAGE_CACHE_MODE_WC,
		_PAGE_CACHE_MODE_UC_MINUS,
		_PAGE_CACHE_MODE_UC,
		_PAGE_CACHE_MODE_WT,
		_PAGE_CACHE_MODE_WP,
		_PAGE_CACHE_MODE_STREAMING,
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		pgprot_t p = __pgprot(cachemode2protval(modes[i]));
		enum page_cache_mode round = pgprot2cachemode(p);

		KUNIT_EXPECT_EQ_MSG(test, (unsigned int)round,
				    (unsigned int)modes[i],
				    "cache mode %u did not round-trip",
				    (unsigned int)modes[i]);
	}
}

static void pgprot_streaming_bits_test(struct kunit *test)
{
	pgprot_t base = __pgprot(_PAGE_PRESENT | _PAGE_RW);
	pgprot_t s = pgprot_streaming(base);
	unsigned long v = pgprot_val(s);

	/* Slot 6 encoding: PAT=1, PCD=1, PWT=0 */
	KUNIT_EXPECT_TRUE(test, v & _PAGE_PAT);
	KUNIT_EXPECT_TRUE(test, v & _PAGE_PCD);
	KUNIT_EXPECT_FALSE(test, v & _PAGE_PWT);
	/* Non-cache bits from base are preserved. */
	KUNIT_EXPECT_TRUE(test, v & _PAGE_PRESENT);
}

static void pgprot_streaming_idempotent_test(struct kunit *test)
{
	pgprot_t once = pgprot_streaming(__pgprot(_PAGE_PRESENT));
	pgprot_t twice = pgprot_streaming(once);

	KUNIT_EXPECT_EQ(test, pgprot_val(once), pgprot_val(twice));
}

static void large_encoding_test(struct kunit *test)
{
	pgprotval_t slot6_4k = cachemode2protval(_PAGE_CACHE_MODE_STREAMING);
	pgprotval_t slot6_large = protval_4k_2_large(slot6_4k);
	pgprot_t huge = pgprot_streaming_huge(__pgprot(_PAGE_PRESENT));

	/* Slot 6 at PMD/PUD leaf level: PAT selector moves to bit 12. */
	KUNIT_EXPECT_EQ(test, slot6_large,
			(pgprotval_t)(_PAGE_PCD | _PAGE_PAT_LARGE));
	/* Round-trips back to the 4K encoding. */
	KUNIT_EXPECT_EQ(test, protval_large_2_4k(slot6_large), slot6_4k);
	/* pgprot_streaming_huge: PAT_LARGE=1, PAT(bit7)=0, PCD=1, PWT=0. */
	KUNIT_EXPECT_TRUE(test, pgprot_val(huge) & _PAGE_PAT_LARGE);
	KUNIT_EXPECT_FALSE(test, pgprot_val(huge) & _PAGE_PAT);
	KUNIT_EXPECT_TRUE(test, pgprot_val(huge) & _PAGE_PCD);
	KUNIT_EXPECT_FALSE(test, pgprot_val(huge) & _PAGE_PWT);
	KUNIT_EXPECT_TRUE(test, pgprot_val(huge) & _PAGE_PRESENT);
}

/*
 * The hazard the THP guard exists to prevent, stated as arithmetic rather than
 * as a comment: a non-hugetlb streaming VMA carries the 4K slot-6 encoding in
 * vm_page_prot, and if a PMD/PUD leaf is ever built from it, bit 7 is read as
 * PSE and the PAT selector moves to bit 12 -- which is clear.  The surviving
 * PCD-only encoding is slot 2, UC-, so the range would silently go
 * uncacheable.  If this test ever fails because the encoding changed, the
 * refusal in __thp_vma_allowable_orders() may no longer be the right fix.
 */
static void streaming_4k_encoding_at_pmd_is_uc_minus_test(struct kunit *test)
{
	pgprotval_t slot6_4k = cachemode2protval(_PAGE_CACHE_MODE_STREAMING);
	pgprotval_t as_leaf;

	/* Bit 7 of the 4K encoding is the PAT selector; at a leaf it is PSE. */
	KUNIT_EXPECT_TRUE(test, slot6_4k & _PAGE_PAT);

	/* Reinterpreted at leaf level, only the PCD/PWT bits carry over. */
	as_leaf = slot6_4k & (_PAGE_PCD | _PAGE_PWT);

	/* PAT=0, PCD=1, PWT=0 selects PAT entry 2, which Linux maps to UC-. */
	KUNIT_EXPECT_EQ(test, as_leaf, (pgprotval_t)_PAGE_PCD);
	KUNIT_EXPECT_FALSE(test, as_leaf & _PAGE_PAT_LARGE);
	KUNIT_EXPECT_NE(test, as_leaf, protval_4k_2_large(slot6_4k));
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/* A streaming range must be ineligible for every THP order. */
static void streaming_vma_allows_no_thp_order_test(struct kunit *test)
{
	struct mm_struct mm = {};
	struct vm_area_struct vma = {};
	struct anon_vma av = {};

	/*
	 * A PMD-sized, PMD-aligned anonymous range: vm_ops and vm_file stay NULL
	 * so this is anonymous, and the size/alignment filter that runs for the
	 * khugepaged path (in_pf=false) has something it can accept.  Without a
	 * range that would otherwise qualify, both assertions below would read
	 * zero and the test would pass while checking nothing.
	 *
	 * anon_vma has to be non-NULL for the same reason: the collapse path
	 * returns no orders for a VMA that has not faulted yet, which would mask
	 * the refusal under test.  It is only tested against NULL here, never
	 * walked.
	 */
	vma.vm_mm = &mm;
	vma.vm_start = 0;
	vma.vm_end = PMD_SIZE;
	vma.anon_vma = &av;

	/*
	 * Pass the flags explicitly: __thp_vma_allowable_orders() takes vm_flags
	 * as a parameter so callers can ask about hypothetical flags, and the
	 * streaming refusal has to answer for what it was asked.
	 *
	 * in_pf=false is the khugepaged collapse path, in_pf=true the fault-time
	 * path.  The type has to be refused on both; a collapse is what turns an
	 * already-sealed range uncacheable, and a huge fault is what would have
	 * built it that way to begin with.
	 */
	KUNIT_EXPECT_EQ(test,
			__thp_vma_allowable_orders(&vma, VM_READ | VM_STREAMING,
						   false, false, false,
						   THP_ORDERS_ALL_ANON),
			0UL);
	KUNIT_EXPECT_EQ(test,
			__thp_vma_allowable_orders(&vma, VM_READ | VM_STREAMING,
						   false, true, false,
						   THP_ORDERS_ALL_ANON),
			0UL);

	/*
	 * Non-vacuity: the identical VMA without the type is allowed at least
	 * one order, so the zeros above are the streaming refusal and not some
	 * unrelated property of a synthetic VMA.
	 */
	KUNIT_EXPECT_NE(test,
			__thp_vma_allowable_orders(&vma, VM_READ, false, false,
						   false, THP_ORDERS_ALL_ANON),
			0UL);
	KUNIT_EXPECT_NE(test,
			__thp_vma_allowable_orders(&vma, VM_READ, false, true,
						   false, THP_ORDERS_ALL_ANON),
			0UL);
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

static void vm_streaming_bit_test(struct kunit *test)
{
	/* VM_STREAMING aliases VM_HIGH_ARCH_4 = bit 36. */
	KUNIT_EXPECT_EQ(test, (unsigned long)VM_STREAMING, 1UL << 36);
}

static void arch_calc_streaming_bit_test(struct kunit *test)
{
	unsigned long vm_no  = calc_vm_prot_bits(PROT_READ, -1);
	unsigned long vm_yes = calc_vm_prot_bits(PROT_READ | PROT_STREAMING, -1);

	KUNIT_EXPECT_FALSE(test, vm_no  & VM_STREAMING);
	KUNIT_EXPECT_TRUE(test,  vm_yes & VM_STREAMING);
}

static void arch_validate_prot_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, arch_validate_prot(PROT_READ, 0));
	KUNIT_EXPECT_TRUE(test, arch_validate_prot(PROT_READ | PROT_STREAMING,
						   0));
	/* Unknown PROT bits remain rejected. */
	KUNIT_EXPECT_FALSE(test, arch_validate_prot(0x40, 0));
}

static struct kunit_case streaming_test_cases[] = {
	KUNIT_CASE(pat_streaming_msr_test),
	KUNIT_CASE(cachemode_table_roundtrip_test),
	KUNIT_CASE(pgprot_streaming_bits_test),
	KUNIT_CASE(pgprot_streaming_idempotent_test),
	KUNIT_CASE(large_encoding_test),
	KUNIT_CASE(streaming_4k_encoding_at_pmd_is_uc_minus_test),
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	KUNIT_CASE(streaming_vma_allows_no_thp_order_test),
#endif
	KUNIT_CASE(vm_streaming_bit_test),
	KUNIT_CASE(arch_calc_streaming_bit_test),
	KUNIT_CASE(arch_validate_prot_test),
	{},
};

static struct kunit_suite streaming_test_suite = {
	.name = "pat_streaming",
	.test_cases = streaming_test_cases,
};

kunit_test_suite(streaming_test_suite);

MODULE_DESCRIPTION("KUnit tests for PAT-slot-6 Streaming memory type");
MODULE_LICENSE("GPL");
