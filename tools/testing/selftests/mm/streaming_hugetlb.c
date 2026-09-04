// SPDX-License-Identifier: GPL-2.0-only
/*
 * mprotect(PROT_STREAMING) on hugetlb mappings.
 *
 * hugetlb leaves are rewritten in place at PMD (2MB) / PUD (1GB)
 * granularity, where the PAT selector sits at bit 12 (_PAGE_PAT_LARGE)
 * instead of the 4K bit 7 (which is PSE at leaf level). Verify:
 *
 *   1. A MAP_PRIVATE 2MB hugetlb region entering Streaming gets
 *      PSE|PAT_LARGE|PCD with PWT clear, is read-only, and reverts
 *      cleanly.
 *   2. The debugfs pte_query third token reports level_shift 21.
 *   3. MAP_SHARED hugetlb + PROT_STREAMING is rejected with EINVAL.
 *   4. Optionally the same cycle for a 1GB page (level_shift 30);
 *      skipped when no 1GB pages are reserved.
 *
 * Requires reserved hugepages, e.g.:
 *   echo 8 > /proc/sys/vm/nr_hugepages
 *   (1GB: boot with hugepagesz=1G hugepages=1)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef PROT_STREAMING
#define PROT_STREAMING	0x10
#endif

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT	26
#endif
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB	(21 << MAP_HUGE_SHIFT)
#endif
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB	(30 << MAP_HUGE_SHIFT)
#endif

#define SIZE_2MB	(2UL * 1024 * 1024)
#define SIZE_1GB	(1024UL * 1024 * 1024)
#define DEBUGFS_QUERY	"/sys/kernel/debug/streaming/pte_query"

/* Leaf PMD/PUD bit positions on x86_64. */
#define BIT_PRESENT	(1ULL << 0)
#define BIT_RW		(1ULL << 1)
#define BIT_PWT		(1ULL << 3)
#define BIT_PCD		(1ULL << 4)
#define BIT_PSE		(1ULL << 7)
#define BIT_PAT_LARGE	(1ULL << 12)

static void ensure_debugfs(void)
{
	struct stat st;

	if (stat(DEBUGFS_QUERY, &st) == 0)
		return;
	(void)mkdir("/sys", 0755);
	(void)mount("none", "/sys", "sysfs", 0, NULL);
	(void)mkdir("/sys/kernel/debug", 0755);
	(void)mount("none", "/sys/kernel/debug", "debugfs", 0, NULL);
}

static int read_pte(unsigned long vaddr, uint64_t *pte_out,
		    unsigned int *level_shift_out)
{
	char buf[64];
	int fd, ret;
	ssize_t n;

	ensure_debugfs();
	fd = open(DEBUGFS_QUERY, O_RDWR);
	if (fd < 0)
		return -errno;

	n = snprintf(buf, sizeof(buf), "%lx", vaddr);
	if (write(fd, buf, n) != n) {
		ret = -errno;
		close(fd);
		return ret;
	}
	if (lseek(fd, 0, SEEK_SET) < 0) {
		ret = -errno;
		close(fd);
		return ret;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -EIO;
	buf[n] = '\0';

	*level_shift_out = 0;
	if (sscanf(buf, "%*x %" SCNx64 " %u", pte_out, level_shift_out) < 1)
		return -EIO;
	return 0;
}

static sigjmp_buf segv_jmp;

static void segv_handler(int sig)
{
	siglongjmp(segv_jmp, 1);
}

static int write_traps(volatile char *p)
{
	struct sigaction sa = { .sa_handler = segv_handler };
	struct sigaction old;
	int trapped = 0;

	sigaction(SIGSEGV, &sa, &old);
	if (sigsetjmp(segv_jmp, 1) == 0)
		*p = 0x55;
	else
		trapped = 1;
	sigaction(SIGSEGV, &old, NULL);
	return trapped;
}

/*
 * One full WB -> Streaming -> WB cycle on a populated hugetlb region.
 * Returns the number of subtests reported (constant HUGE_CYCLE_TESTS).
 */
#define HUGE_CYCLE_TESTS 6
#define HUGE_TEST_PLAN (2 * HUGE_CYCLE_TESTS + 1)

static void skip_all_hugetlb_tests(const char *reason)
{
	int i;

	for (i = 0; i < HUGE_TEST_PLAN; i++)
		ksft_test_result_skip("%s\n", reason);
	ksft_finished();
}

static void huge_cycle(void *region, size_t size, unsigned int want_shift,
		       const char *tag)
{
	unsigned int shift;
	uint64_t pte;
	int ret;

	memset(region, 0xa5, size);

	ret = mprotect(region, size, PROT_READ | PROT_STREAMING);
	if (ret)
		ksft_exit_fail_msg("%s: mprotect(PROT_STREAMING): %m\n", tag);
	ksft_test_result_pass("%s: mprotect(PROT_READ|PROT_STREAMING)\n", tag);

	if (read_pte((unsigned long)region, &pte, &shift))
		ksft_exit_fail_msg("%s: debugfs pte_query unavailable; enable CONFIG_DEBUG_FS and run as root\n",
				   tag);

	ksft_test_result((pte & BIT_PRESENT) && (pte & BIT_PSE) &&
			 (pte & BIT_PAT_LARGE) && (pte & BIT_PCD) &&
			 !(pte & BIT_PWT),
			 "%s: leaf cache bits == large slot 6 (raw=0x%" PRIx64 ")\n",
			 tag, pte);
	ksft_test_result(!(pte & BIT_RW), "%s: write bit cleared\n", tag);
	ksft_test_result(shift == want_shift,
			 "%s: level_shift == %u (got %u)\n",
			 tag, want_shift, shift);

	ksft_test_result(write_traps((volatile char *)region),
			 "%s: write to streaming region traps SIGSEGV\n", tag);

	if (mprotect(region, size, PROT_READ | PROT_WRITE))
		ksft_exit_fail_msg("%s: mprotect back to RW: %m\n", tag);
	if (read_pte((unsigned long)region, &pte, &shift))
		ksft_exit_fail_msg("%s: pte_query failed after restore\n", tag);

	((volatile char *)region)[0] = 0x42;
	ksft_test_result(!(pte & BIT_PAT_LARGE) && !(pte & BIT_PCD) &&
			 !(pte & BIT_PWT) &&
			 ((volatile char *)region)[0] == 0x42,
			 "%s: cache bits cleared and write OK after leaving (raw=0x%" PRIx64 ")\n",
			 tag, pte);
}

int main(void)
{
	void *region, *shared, *big;
	int ret;

	ksft_print_header();
	ksft_set_plan(HUGE_TEST_PLAN);

	region = mmap(NULL, SIZE_2MB, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE |
		      MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
	if (region == MAP_FAILED)
		skip_all_hugetlb_tests("no 2MB hugepages available; reserve nr_hugepages before running");

	/* Probe kernel support before committing to the plan. */
	ret = mprotect(region, SIZE_2MB, PROT_READ | PROT_STREAMING);
	if (ret && errno == EINVAL)
		skip_all_hugetlb_tests("PROT_STREAMING unsupported");
	if (mprotect(region, SIZE_2MB, PROT_READ | PROT_WRITE))
		ksft_exit_fail_msg("mprotect probe restore: %m\n");

	huge_cycle(region, SIZE_2MB, 21, "2MB");
	munmap(region, SIZE_2MB);

	/* MAP_SHARED hugetlb must be rejected (VM_SHARED file rule). */
	shared = mmap(NULL, SIZE_2MB, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE |
		      MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
	if (shared == MAP_FAILED) {
		ksft_test_result_skip("MAP_SHARED hugetlb mmap failed: %m\n");
	} else {
		ret = mprotect(shared, SIZE_2MB, PROT_READ | PROT_STREAMING);
		ksft_test_result(ret != 0 && errno == EINVAL,
				 "MAP_SHARED hugetlb -> EINVAL\n");
		munmap(shared, SIZE_2MB);
	}

	/* Optional 1GB pass. */
	big = mmap(NULL, SIZE_1GB, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE |
		   MAP_HUGETLB | MAP_HUGE_1GB, -1, 0);
	if (big == MAP_FAILED) {
		for (int i = 0; i < HUGE_CYCLE_TESTS; i++)
			ksft_test_result_skip("1GB: no 1GB hugepages reserved\n");
	} else {
		huge_cycle(big, SIZE_1GB, 30, "1GB");
		munmap(big, SIZE_1GB);
	}

	ksft_finished();
}
