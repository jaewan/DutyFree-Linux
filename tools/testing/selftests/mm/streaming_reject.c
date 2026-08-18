// SPDX-License-Identifier: GPL-2.0-only
/*
 * Negative cases for mprotect(PROT_STREAMING).
 *
 * The prototype refuses combinations the design plan calls out as
 * unsafe. We exercise the ones that are easy to set up from
 * userspace:
 *   - PROT_STREAMING | PROT_WRITE          -> EINVAL
 *   - MAP_SHARED + file fd                 -> EINVAL
 *   - MAP_SHARED file with PROT_WRITE      -> EINVAL (also covered above)
 *   - userfaultfd-WP registered VMA        -> EINVAL  (only if UFFD is built in)
 *   - bogus PROT bit (0x40)                -> EINVAL
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>

#include "../kselftest.h"

#ifndef PROT_STREAMING
#define PROT_STREAMING	0x10
#endif

#define REGION_SIZE	(64 * 1024UL)

static int expect_einval(int rc, const char *desc)
{
	if (rc == 0) {
		ksft_test_result_fail("%s: mprotect succeeded but should have failed\n",
				      desc);
		return 0;
	}
	if (errno != EINVAL) {
		ksft_test_result_fail("%s: errno=%d expected EINVAL\n",
				      desc, errno);
		return 0;
	}
	ksft_test_result_pass("%s -> EINVAL\n", desc);
	return 1;
}

int main(void)
{
	void *anon, *shared;
	int tmpfd;
	int n = 0;

	ksft_print_header();
	ksft_set_plan(4);

	anon = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (anon == MAP_FAILED)
		ksft_exit_fail_msg("anon mmap: %m\n");

	expect_einval(mprotect(anon, REGION_SIZE,
			       PROT_READ | PROT_WRITE | PROT_STREAMING),
		      "PROT_STREAMING with PROT_WRITE");
	n++;

	expect_einval(mprotect(anon, REGION_SIZE, 0x40),
		      "bogus PROT bit 0x40");
	n++;

	tmpfd = memfd_create("streaming-reject", 0);
	if (tmpfd < 0)
		ksft_exit_fail_msg("memfd_create: %m\n");
	if (ftruncate(tmpfd, REGION_SIZE) < 0)
		ksft_exit_fail_msg("ftruncate: %m\n");
	shared = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED, tmpfd, 0);
	if (shared == MAP_FAILED)
		ksft_exit_fail_msg("shared mmap: %m\n");

	expect_einval(mprotect(shared, REGION_SIZE,
			       PROT_READ | PROT_STREAMING),
		      "MAP_SHARED file VMA");
	n++;

#ifdef __NR_userfaultfd
	{
		int ufd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);

		if (ufd >= 0) {
			struct uffdio_api api = {
				.api = UFFD_API,
				.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP,
			};
			struct uffdio_register reg = {
				.range = {
					.start = (uintptr_t)anon,
					.len = REGION_SIZE,
				},
				.mode = UFFDIO_REGISTER_MODE_WP,
			};

			if (ioctl(ufd, UFFDIO_API, &api) == 0 &&
			    ioctl(ufd, UFFDIO_REGISTER, &reg) == 0) {
				expect_einval(mprotect(anon, REGION_SIZE,
						       PROT_READ | PROT_STREAMING),
					      "uffd-wp registered VMA");
			} else {
				ksft_test_result_skip("uffd-wp setup failed\n");
			}
			close(ufd);
		} else {
			ksft_test_result_skip("userfaultfd not available\n");
		}
		n++;
	}
#else
	ksft_test_result_skip("userfaultfd not compiled in\n");
	n++;
#endif

	munmap(anon, REGION_SIZE);
	munmap(shared, REGION_SIZE);
	close(tmpfd);
	ksft_finished();
}
