// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Streaming page-table memory type prototype
 * (Directory Tax §4, Step 1).
 *
 *   1. PAT MSR slot 6 was programmed as WB.
 *   2. The cache-mode ↔ PTE-bits round-trip is identity, including
 *      STREAMING.
 *   3. pgprot_streaming() emits exactly (PAT=1, PCD=1, PWT=0,
 *      SOFTW1=1).
 *   4. _COMMON_PAGE_CHG_MASK preserves _PAGE_SOFTW1 across pte_modify().
 */

#include <kunit/test.h>
#include <linux/pgtable.h>
#include <linux/streaming.h>
#include <asm/msr.h>
#include <asm/msr-index.h>
#include <asm/pgtable_types.h>
#include <asm/memtype.h>

static void streaming_msr_slot6_is_wb(struct kunit *test)
{
	u64 pat;
	u8 slot6;

	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	rdmsrq(MSR_IA32_CR_PAT, pat);
	slot6 = (pat >> (6 * 8)) & 7;
	KUNIT_EXPECT_EQ(test, (int)slot6, X86_MEMTYPE_WB);
}

static void streaming_cachemode_roundtrip(struct kunit *test)
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
	int i;

	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		pgprot_t p = __pgprot(cachemode2protval(modes[i]));
		enum page_cache_mode back = pgprot2cachemode(p);

		KUNIT_EXPECT_EQ_MSG(test, (int)back, (int)modes[i],
				    "cache mode %d did not round-trip", modes[i]);
	}
}

static void streaming_pgprot_bits(struct kunit *test)
{
	pgprot_t base = PAGE_KERNEL;
	pgprot_t s = pgprot_streaming(base);
	pteval_t v = pgprot_val(s);
	pteval_t streaming_cache = cachemode2protval(_PAGE_CACHE_MODE_STREAMING);

	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	KUNIT_EXPECT_NE_MSG(test, (long)(v & _PAGE_SOFTW1), 0L,
			    "_PAGE_SOFTW1 missing in pgprot_streaming()");
	KUNIT_EXPECT_EQ_MSG(test, (long)(v & _PAGE_CACHE_MASK),
			    (long)streaming_cache,
			    "cache bits in pgprot_streaming() != STREAMING");
	KUNIT_EXPECT_NE_MSG(test, (long)(v & _PAGE_PAT), 0L,
			    "_PAGE_PAT bit missing");
	KUNIT_EXPECT_NE_MSG(test, (long)(v & _PAGE_PCD), 0L,
			    "_PAGE_PCD bit missing");
	KUNIT_EXPECT_EQ_MSG(test, (long)(v & _PAGE_PWT), 0L,
			    "_PAGE_PWT must be clear for slot 6");
}

static void streaming_chg_mask_preserves_softw1(struct kunit *test)
{
	pte_t old = __pte(_PAGE_PRESENT | _PAGE_USER | _PAGE_SOFTW1 |
			  cachemode2protval(_PAGE_CACHE_MODE_STREAMING));
	/* Simulate a protection change: drop write, keep read. */
	pgprot_t newprot = __pgprot(_PAGE_PRESENT | _PAGE_USER);
	pte_t after = pte_modify(old, newprot);
	pteval_t v = pte_val(after);

	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	KUNIT_EXPECT_NE_MSG(test, (long)(v & _PAGE_SOFTW1), 0L,
			    "pte_modify dropped _PAGE_SOFTW1 — _COMMON_PAGE_CHG_MASK regressed");
	KUNIT_EXPECT_EQ_MSG(test, (long)(v & _PAGE_CACHE_MASK),
			    (long)cachemode2protval(_PAGE_CACHE_MODE_STREAMING),
			    "pte_modify dropped Streaming cache bits");
}

static struct kunit_case streaming_test_cases[] = {
	KUNIT_CASE(streaming_msr_slot6_is_wb),
	KUNIT_CASE(streaming_cachemode_roundtrip),
	KUNIT_CASE(streaming_pgprot_bits),
	KUNIT_CASE(streaming_chg_mask_preserves_softw1),
	{}
};

static struct kunit_suite streaming_test_suite = {
	.name = "streaming",
	.test_cases = streaming_test_cases,
};
kunit_test_suite(streaming_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Streaming page-table memory type");
MODULE_LICENSE("GPL");
