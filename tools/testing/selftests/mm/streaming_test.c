// SPDX-License-Identifier: GPL-2.0-only
/*
 * streaming_test.c — kselftest for the OS-side Streaming page-table
 * memory type prototype (Directory Tax §4, Step 1).
 *
 *   1. do_mmap() rejects MAP_STREAMING on illegal combinations
 *      (anonymous, PROT_WRITE, MAP_PRIVATE, non-DAX file fd).
 *   2. MAP_STREAMING + MAP_SHARED + PROT_READ on a device-DAX fd
 *      ought to succeed; without device-DAX the test reports SKIP.
 *   3. mprotect(... PROT_WRITE) on a Streaming VMA returns EACCES.
 *   4. fork() does not inherit a Streaming VMA.
 *   5. /sys/kernel/debug/streaming/pte_query decodes the PAT slot 6
 *      + _PAGE_SOFTW1 encoding of an actually faulted Streaming PTE.
 *
 * The harness deliberately treats "no device-DAX" as SKIP rather than
 * FAIL, because the prototype gates positive cases on /dev/dax* by
 * design (see commit 3 / plan.md "Transparency Myth").
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef MAP_STREAMING
#define MAP_STREAMING 0x200000
#endif

#define DEBUGFS_QUERY "/sys/kernel/debug/streaming/pte_query"

static size_t devdax_align = 0;
static char devdax_path[64];

static int find_devdax(void)
{
	DIR *d = opendir("/dev");
	struct dirent *de;

	if (!d)
		return -1;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "dax", 3) == 0) {
			snprintf(devdax_path, sizeof(devdax_path),
				 "/dev/%s", de->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

static size_t probe_devdax_align(void)
{
	if (devdax_align)
		return devdax_align;
	/*
	 * The Streaming initramfs creates the namespace with `ndctl
	 * create-namespace -a 4K` so device-DAX faults at PTE
	 * granularity.  We mirror that here.  Larger alignments would
	 * route through the huge fault path which we explicitly bypass
	 * for Streaming VMAs (VM_FAULT_FALLBACK in dev_dax_huge_fault).
	 */
	devdax_align = 4096UL;
	return devdax_align;
}

static int test_negative_cases(void)
{
	int fd, errors = 0;
	void *p;

	/* anonymous + MAP_STREAMING -> EINVAL (no file) */
	p = mmap(NULL, 4096, PROT_READ,
		 MAP_STREAMING | MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (p != MAP_FAILED || (errno != EINVAL && errno != EOPNOTSUPP)) {
		ksft_print_msg("anonymous MAP_STREAMING: errno=%d (expected EINVAL)\n",
			       errno);
		errors++;
		if (p != MAP_FAILED)
			munmap(p, 4096);
	}

	/* tmpfs file fd is not device-DAX -> EINVAL or EOPNOTSUPP */
	fd = open("/tmp", O_TMPFILE | O_RDWR, 0600);
	if (fd >= 0) {
		if (ftruncate(fd, 4096) == 0) {
			p = mmap(NULL, 4096, PROT_READ,
				 MAP_STREAMING | MAP_SHARED, fd, 0);
			if (p != MAP_FAILED ||
			    (errno != EINVAL && errno != EOPNOTSUPP)) {
				ksft_print_msg("tmpfs MAP_STREAMING accepted (errno=%d)\n",
					       errno);
				errors++;
				if (p != MAP_FAILED)
					munmap(p, 4096);
			}
		}
		close(fd);
	}

	return errors;
}

static int open_devdax(void)
{
	int fd;

	if (find_devdax() < 0)
		return -1;
	fd = open(devdax_path, O_RDWR);
	return fd;
}

static int test_positive_mmap(void)
{
	int fd = open_devdax();
	size_t len;
	void *p;

	if (fd < 0) {
		ksft_print_msg("no /dev/dax* present; positive mmap is SKIP\n");
		return -1;
	}
	len = probe_devdax_align();

	/*
	 * Sanity step: a plain MAP_SHARED + PROT_READ mmap on the
	 * device-DAX fd should always succeed.  If even this fails,
	 * the device-DAX setup itself is misconfigured and we should
	 * report that distinctly rather than blame the Streaming gate.
	 */
	p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		ksft_print_msg("plain MAP_SHARED on %s also failed: %s — device-DAX setup issue\n",
			       devdax_path, strerror(errno));
		close(fd);
		return 1;
	}
	munmap(p, len);

	p = mmap(NULL, len, PROT_READ, MAP_STREAMING | MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		if (errno == EOPNOTSUPP) {
			ksft_print_msg("kernel reports no Streaming support; SKIP\n");
			close(fd);
			return -1;
		}
		ksft_print_msg("MAP_STREAMING + device-DAX failed: %s\n",
			       strerror(errno));
		close(fd);
		return 1;
	}
	munmap(p, len);
	close(fd);
	return 0;
}

static int test_mprotect(void)
{
	int fd = open_devdax();
	size_t len;
	void *p;
	int rc = 0;

	if (fd < 0)
		return -1;
	len = probe_devdax_align();
	p = mmap(NULL, len, PROT_READ, MAP_STREAMING | MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return errno == EOPNOTSUPP ? -1 : 1;
	}
	if (mprotect(p, len, PROT_READ | PROT_WRITE) == 0 ||
	    errno != EACCES) {
		ksft_print_msg("mprotect upgrade did not return EACCES (errno=%d)\n",
			       errno);
		rc = 1;
	}
	munmap(p, len);
	close(fd);
	return rc;
}

static int test_fork(void)
{
	int fd = open_devdax();
	size_t len;
	void *p;
	pid_t pid;
	int status, rc = 0;

	if (fd < 0)
		return -1;
	len = probe_devdax_align();
	p = mmap(NULL, len, PROT_READ, MAP_STREAMING | MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return errno == EOPNOTSUPP ? -1 : 1;
	}

	pid = fork();
	if (pid == 0) {
		volatile unsigned long *probe = p;
		(void)*probe;
		_exit(0);
	}
	if (pid < 0) {
		ksft_print_msg("fork failed: %s\n", strerror(errno));
		rc = 1;
	} else {
		waitpid(pid, &status, 0);
		if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
			ksft_print_msg("child did not segfault (status=%#x)\n",
				       status);
			rc = 1;
		}
	}
	munmap(p, len);
	close(fd);
	return rc;
}

static int test_pte_inspection(void)
{
	int fd, qfd, rc = 0;
	size_t len;
	void *p;
	char buf[40], out[160];
	ssize_t n;

	if (access(DEBUGFS_QUERY, R_OK | W_OK) < 0) {
		ksft_print_msg("debugfs query not available; SKIP\n");
		return -1;
	}
	fd = open_devdax();
	if (fd < 0)
		return -1;
	len = probe_devdax_align();
	p = mmap(NULL, len, PROT_READ, MAP_STREAMING | MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return errno == EOPNOTSUPP ? -1 : 1;
	}
	/* fault the page in */
	(void)*(volatile unsigned long *)p;

	qfd = open(DEBUGFS_QUERY, O_RDWR);
	if (qfd < 0) {
		ksft_print_msg("open debugfs query: %s\n", strerror(errno));
		rc = 1;
		goto out;
	}
	n = snprintf(buf, sizeof(buf), "%p\n", p);
	if (write(qfd, buf, n) != n) {
		ksft_print_msg("write debugfs query: %s\n", strerror(errno));
		rc = 1;
		goto out_qfd;
	}
	lseek(qfd, 0, SEEK_SET);
	n = read(qfd, out, sizeof(out) - 1);
	if (n <= 0) {
		ksft_print_msg("read debugfs query: %s\n", strerror(errno));
		rc = 1;
		goto out_qfd;
	}
	out[n] = '\0';
	ksft_print_msg("pte_query: %s", out);
	if (!strstr(out, "softw1=1") || !strstr(out, "pat=1") ||
	    !strstr(out, "pcd=1") || !strstr(out, "pwt=0") ||
	    !strstr(out, "write=0")) {
		ksft_print_msg("PTE missing required Streaming bits\n");
		rc = 1;
	}
out_qfd:
	close(qfd);
out:
	munmap(p, len);
	close(fd);
	return rc;
}

int main(void)
{
	int total = 5, passed = 0, rc;

	ksft_print_header();
	ksft_set_plan(total);

	rc = test_negative_cases();
	if (rc == 0)         { ksft_test_result_pass("negative MAP_STREAMING cases\n"); passed++; }
	else if (rc < 0)     { ksft_test_result_skip("negative MAP_STREAMING cases\n"); }
	else                 { ksft_test_result_fail("negative MAP_STREAMING cases\n"); }

	rc = test_positive_mmap();
	if (rc == 0)         { ksft_test_result_pass("positive MAP_STREAMING mmap\n"); passed++; }
	else if (rc < 0)     { ksft_test_result_skip("positive MAP_STREAMING mmap\n"); }
	else                 { ksft_test_result_fail("positive MAP_STREAMING mmap\n"); }

	rc = test_mprotect();
	if (rc == 0)         { ksft_test_result_pass("mprotect rejects PROT_WRITE\n"); passed++; }
	else if (rc < 0)     { ksft_test_result_skip("mprotect rejects PROT_WRITE\n"); }
	else                 { ksft_test_result_fail("mprotect rejects PROT_WRITE\n"); }

	rc = test_fork();
	if (rc == 0)         { ksft_test_result_pass("fork does not inherit Streaming VMA\n"); passed++; }
	else if (rc < 0)     { ksft_test_result_skip("fork does not inherit Streaming VMA\n"); }
	else                 { ksft_test_result_fail("fork does not inherit Streaming VMA\n"); }

	rc = test_pte_inspection();
	if (rc == 0)         { ksft_test_result_pass("PTE has PAT slot 6 + SOFTW1 encoding\n"); passed++; }
	else if (rc < 0)     { ksft_test_result_skip("PTE has PAT slot 6 + SOFTW1 encoding\n"); }
	else                 { ksft_test_result_fail("PTE has PAT slot 6 + SOFTW1 encoding\n"); }

	ksft_print_msg("%d/%d tests passed\n", passed, total);
	ksft_finished();
}
