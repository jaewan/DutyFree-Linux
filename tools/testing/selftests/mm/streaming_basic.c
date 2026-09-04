// SPDX-License-Identifier: GPL-2.0-only
/*
 * mprotect(PROT_STREAMING) end-to-end smoke test.
 *
 * 1. Map a private anonymous region RW and dirty it.
 * 2. mprotect(PROT_READ | PROT_STREAMING) - the region should
 *    flip to read-only, PAT slot 6 (PCD|PAT) should appear in
 *    every PTE, and writes should now SIGSEGV.
 * 3. mprotect(PROT_READ | PROT_WRITE) - the region should be
 *    plain WB again (cache bits cleared) and writable.
 *
 * The PTE inspection uses the debugfs prototype interface at
 * /sys/kernel/debug/streaming/pte_query.
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

#define PAGE_SIZE		4096UL
#define REGION_SIZE		(2 * 1024 * 1024UL)
#define DEBUGFS_QUERY	"/sys/kernel/debug/streaming/pte_query"

/* PTE bit positions for 4K mappings on x86_64. */
#define BIT_PRESENT		(1ULL << 0)
#define BIT_RW			(1ULL << 1)
#define BIT_PWT			(1ULL << 3)
#define BIT_PCD			(1ULL << 4)
#define BIT_PAT			(1ULL << 7)

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

static int read_pte(unsigned long vaddr, uint64_t *pte_out)
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

	if (sscanf(buf, "%*x %" SCNx64, pte_out) != 1)
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

int main(void)
{
	uint64_t pte;
	void *region;
	int ret;

	ksft_print_header();
	ksft_set_plan(10);

	region = mmap(NULL, REGION_SIZE,
		      PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %m\n");
	memset(region, 0xa5, REGION_SIZE);
	ksft_test_result_pass("mmap RW + populate\n");

	errno = 0;
	ret = mprotect(region, REGION_SIZE,
		       PROT_READ | PROT_WRITE | PROT_STREAMING);
	ksft_test_result(ret == -1 && errno == EINVAL,
			 "PROT_WRITE|PROT_STREAMING is rejected (errno=%d)\n", errno);

	/* A streaming transition is deliberately one-VMA-only until the kernel
	 * has a transactional cross-VMA/object epoch protocol. */
	ret = mprotect((char *)region + REGION_SIZE / 2, REGION_SIZE / 2,
		       PROT_READ);
	if (ret)
		ksft_exit_fail_msg("split VMA setup failed: %m\n");
	errno = 0;
	ret = mprotect(region, REGION_SIZE, PROT_READ | PROT_STREAMING);
	ksft_test_result(ret == -1 && errno == EINVAL,
			 "cross-VMA PROT_STREAMING is rejected (errno=%d)\n", errno);
	ret = mprotect(region, REGION_SIZE, PROT_READ | PROT_WRITE);
	if (ret)
		ksft_exit_fail_msg("restore split VMA setup failed: %m\n");
	ksft_test_result_pass("one-VMA setup restored\n");

	ret = mprotect(region, REGION_SIZE, PROT_READ | PROT_STREAMING);
	if (ret) {
		if (errno == EINVAL) {
			int remaining;

			/* Four generic admission checks already ran.  Report the six
			 * implementation-specific checks as skips instead of using
			 * ksft_exit_skip() after publishing a TAP plan. */
			for (remaining = 0; remaining < 6; remaining++)
				ksft_test_result_skip("PROT_STREAMING unsupported\n");
			munmap(region, REGION_SIZE);
			ksft_finished();
		}
		ksft_exit_fail_msg("mprotect(PROT_STREAMING) failed: %m\n");
	}
	ksft_test_result_pass("mprotect(PROT_READ|PROT_STREAMING)\n");

	if (read_pte((unsigned long)region, &pte))
		ksft_exit_fail_msg("debugfs pte_query unavailable; enable CONFIG_DEBUG_FS or run as root\n");

	ksft_test_result((pte & BIT_PRESENT) && (pte & BIT_PAT) &&
			 (pte & BIT_PCD) && !(pte & BIT_PWT),
			 "PTE cache bits == PAT slot 6 (raw=0x%" PRIx64 ")\n",
			 pte);
	ksft_test_result(!(pte & BIT_RW),
			 "PTE write bit cleared\n");

	ksft_test_result(write_traps((volatile char *)region),
			 "write to streaming region traps SIGSEGV\n");

	ret = mprotect(region, REGION_SIZE, PROT_READ | PROT_WRITE);
	if (ret)
		ksft_exit_fail_msg("mprotect back to RW failed: %m\n");

	if (read_pte((unsigned long)region, &pte))
		ksft_exit_fail_msg("debugfs pte_query failed after restore\n");

	ksft_test_result(!(pte & BIT_PAT) && !(pte & BIT_PCD) && !(pte & BIT_PWT),
			 "PTE cache bits cleared (raw=0x%" PRIx64 ")\n", pte);

	((volatile char *)region)[0] = 0x42;
	ksft_test_result(((volatile char *)region)[0] == 0x42,
			 "write succeeds after leaving streaming\n");

	munmap(region, REGION_SIZE);
	ksft_finished();
}
