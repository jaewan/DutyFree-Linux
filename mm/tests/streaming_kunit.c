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
#include <linux/mm.h>
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

/*
 * pgprot_modify(streaming_old, plain_new) must keep the result Streaming.
 * This is the path mprotect_fixup() drives for a downgrade-only
 * protection change; the cache bits and SOFTW1 must survive.
 */
static void streaming_pgprot_modify_streaming_to_plain(struct kunit *test)
{
	pgprot_t old = pgprot_streaming(PAGE_KERNEL);
	pgprot_t new_in = PAGE_READONLY;
	pgprot_t out = pgprot_modify(old, new_in);
	pteval_t v = pgprot_val(out);

	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	KUNIT_EXPECT_NE_MSG(test, (long)(v & _PAGE_SOFTW1), 0L,
			    "pgprot_modify lost _PAGE_SOFTW1 from Streaming oldprot");
	KUNIT_EXPECT_EQ_MSG(test, (long)(v & _PAGE_CACHE_MASK),
			    (long)cachemode2protval(_PAGE_CACHE_MODE_STREAMING),
			    "pgprot_modify lost Streaming cache bits from oldprot");
}

/*
 * pgprot_modify(plain_old, plain_new) must NOT spontaneously upgrade
 * to Streaming.  The streaming branch in pgprot_modify() is gated on
 * "oldprot was already Streaming"; a plain-to-plain transition stays
 * plain.
 */
static void streaming_pgprot_modify_plain_stays_plain(struct kunit *test)
{
	pgprot_t old = PAGE_KERNEL;
	pgprot_t out = pgprot_modify(old, PAGE_READONLY);
	pteval_t v = pgprot_val(out);

	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	KUNIT_EXPECT_EQ_MSG(test, (long)(v & _PAGE_SOFTW1), 0L,
			    "plain->plain pgprot_modify spuriously set _PAGE_SOFTW1");
	KUNIT_EXPECT_NE_MSG(test, (long)(v & _PAGE_CACHE_MASK),
			    (long)cachemode2protval(_PAGE_CACHE_MODE_STREAMING),
			    "plain->plain pgprot_modify spuriously set Streaming cache bits");
}

/*
 * pgprot_streaming() is idempotent — applying it twice yields the
 * same encoding.  Fault paths and audit hooks rely on this so a VMA
 * whose vm_page_prot was already stamped is not double-mutated.
 */
static void streaming_pgprot_streaming_idempotent(struct kunit *test)
{
	pgprot_t once = pgprot_streaming(PAGE_KERNEL);
	pgprot_t twice = pgprot_streaming(once);

	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	KUNIT_EXPECT_EQ_MSG(test, (unsigned long)pgprot_val(once),
			    (unsigned long)pgprot_val(twice),
			    "pgprot_streaming() is not idempotent");
}

/*
 * pgprot_streaming() must clear PWT regardless of the base protection.
 * Slot 6 is selected by (PAT=1, PCD=1, PWT=0); a stray PWT would route
 * to slot 7 (WT) instead.
 */
static void streaming_pgprot_streaming_clears_pwt(struct kunit *test)
{
	pgprot_t base = __pgprot(pgprot_val(PAGE_KERNEL) | _PAGE_PWT);
	pgprot_t out = pgprot_streaming(base);

	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	KUNIT_EXPECT_EQ_MSG(test, (long)(pgprot_val(out) & _PAGE_PWT), 0L,
			    "pgprot_streaming() failed to clear PWT");
}

/*
 * Direct sanity check on _COMMON_PAGE_CHG_MASK: it must include
 * _PAGE_SPECIAL (which aliases _PAGE_SOFTW1).  If a future cleanup
 * accidentally drops _PAGE_SPECIAL from the mask, mprotect would
 * silently strip the Streaming intent marker.
 */
static void streaming_chg_mask_includes_special(struct kunit *test)
{
	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	KUNIT_EXPECT_NE_MSG(test,
			    (long)(_COMMON_PAGE_CHG_MASK & _PAGE_SPECIAL),
			    0L,
			    "_COMMON_PAGE_CHG_MASK no longer preserves _PAGE_SPECIAL/_PAGE_SOFTW1");
	/* Sanity: _PAGE_SOFTW1 and _PAGE_SPECIAL are the same bit. */
	KUNIT_EXPECT_EQ_MSG(test,
			    (long)_PAGE_SOFTW1, (long)_PAGE_SPECIAL,
			    "_PAGE_SOFTW1 is no longer aliased to _PAGE_SPECIAL");
}

/*
 * VM_STREAMING must alias HIGH_ARCH_6 (bit 38) on builds that have
 * CONFIG_PAT_STREAMING.  The bit number is the gem5 / userspace
 * tooling contract: smaps printers and any debug consumer scrape
 * vm_flags numerically.
 */
static void streaming_vm_flag_is_high_arch_6(struct kunit *test)
{
	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	KUNIT_EXPECT_NE_MSG(test, (long)VM_STREAMING, 0L,
			    "VM_STREAMING is VM_NONE despite CONFIG_PAT_STREAMING=y");
	KUNIT_EXPECT_EQ_MSG(test,
			    (unsigned long)VM_STREAMING,
			    (unsigned long)(1UL << 38),
			    "VM_STREAMING is not bit 38 (HIGH_ARCH_6)");
}

/*
 * The cache mask must be exactly (PWT|PCD|PAT) — three bits.  The
 * Streaming encoding sets two of them (PCD|PAT) and leaves PWT clear.
 * If the cache mask ever grew, slot selection arithmetic in
 * __cm_idx2pte() would change and silently re-route Streaming PTEs.
 */
static void streaming_cache_mask_shape(struct kunit *test)
{
	if (!streaming_supported())
		kunit_skip(test, "Streaming or PAT not active on this CPU");

	KUNIT_EXPECT_EQ_MSG(test,
			    (unsigned long)_PAGE_CACHE_MASK,
			    (unsigned long)(_PAGE_PWT | _PAGE_PCD | _PAGE_PAT),
			    "_PAGE_CACHE_MASK is not exactly PWT|PCD|PAT");
	KUNIT_EXPECT_EQ_MSG(test,
			    (unsigned long)(cachemode2protval(_PAGE_CACHE_MODE_STREAMING)
					     & _PAGE_PWT), 0UL,
			    "Streaming encoding has PWT set (would route to slot 7, WT)");
	KUNIT_EXPECT_EQ_MSG(test,
			    (unsigned long)(cachemode2protval(_PAGE_CACHE_MODE_STREAMING)
					     & (_PAGE_PCD | _PAGE_PAT)),
			    (unsigned long)(_PAGE_PCD | _PAGE_PAT),
			    "Streaming encoding missing PCD or PAT");
}

static struct kunit_case streaming_test_cases[] = {
	KUNIT_CASE(streaming_msr_slot6_is_wb),
	KUNIT_CASE(streaming_cachemode_roundtrip),
	KUNIT_CASE(streaming_pgprot_bits),
	KUNIT_CASE(streaming_chg_mask_preserves_softw1),
	KUNIT_CASE(streaming_pgprot_modify_streaming_to_plain),
	KUNIT_CASE(streaming_pgprot_modify_plain_stays_plain),
	KUNIT_CASE(streaming_pgprot_streaming_idempotent),
	KUNIT_CASE(streaming_pgprot_streaming_clears_pwt),
	KUNIT_CASE(streaming_chg_mask_includes_special),
	KUNIT_CASE(streaming_vm_flag_is_high_arch_6),
	KUNIT_CASE(streaming_cache_mask_shape),
	{}
};

static struct kunit_suite streaming_test_suite = {
	.name = "streaming",
	.test_cases = streaming_test_cases,
};
kunit_test_suite(streaming_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Streaming page-table memory type");
MODULE_LICENSE("GPL");
