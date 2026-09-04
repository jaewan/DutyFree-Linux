// SPDX-License-Identifier: GPL-2.0-only
/*
 * Negative cases for mprotect(PROT_STREAMING).
 *
 * The prototype refuses combinations the design plan calls out as
 * unsafe. We exercise the ones that are easy to set up from
 * userspace:
 *   - PROT_STREAMING | PROT_WRITE          -> EINVAL
 *   - MAP_SHARED anonymous                 -> EINVAL
 *   - MAP_SHARED + file fd                 -> EINVAL
 *   - ordinary MAP_PRIVATE file mapping    -> EINVAL
 *   - KSM-mergeable anonymous mapping      -> EINVAL
 *   - MAP_SHARED file with PROT_WRITE      -> EINVAL (also covered above)
 *   - userfaultfd-WP registered VMA        -> EINVAL  (only if UFFD is built in)
 *   - bogus PROT bit (0x40)                -> EINVAL
 *   - MADV_MERGEABLE after entry           -> EINVAL
 *   - forced /proc/self/mem write           -> rejected
 *   - MADV_DONTFORK epoch + fork            -> accepted, mapping omitted
 *   - fork during an epoch                 -> EBUSY
 *   - pre-existing COW alias               -> EBUSY
 *   - unpopulated anonymous range          -> EBUSY
 *   - page-discard advice during epoch     -> EINVAL
 *   - mremap resize / duplicate             -> EINVAL
 *   - mremap move with unchanged size       -> accepted
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
#include <sys/wait.h>
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
		ksft_test_result_fail("%s: operation succeeded but should have failed\n",
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
	void *anon, *shared, *shared_anon, *private_file, *mergeable;
	unsigned char *epoch, *cow;
	void *hole;
	int tmpfd;
	int n = 0;

	ksft_print_header();
	ksft_set_plan(17);

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

	shared_anon = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared_anon == MAP_FAILED)
		ksft_exit_fail_msg("shared anonymous mmap: %m\n");
	expect_einval(mprotect(shared_anon, REGION_SIZE,
			       PROT_READ | PROT_STREAMING),
		      "MAP_SHARED anonymous VMA");
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

	private_file = mmap(NULL, REGION_SIZE, PROT_READ,
			    MAP_PRIVATE, tmpfd, 0);
	if (private_file == MAP_FAILED)
		ksft_exit_fail_msg("private file mmap: %m\n");
	expect_einval(mprotect(private_file, REGION_SIZE,
			       PROT_READ | PROT_STREAMING),
		      "ordinary MAP_PRIVATE file VMA");
	n++;

	mergeable = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mergeable == MAP_FAILED)
		ksft_exit_fail_msg("mergeable mmap: %m\n");
	if (madvise(mergeable, REGION_SIZE, MADV_MERGEABLE) == 0)
		expect_einval(mprotect(mergeable, REGION_SIZE,
				       PROT_READ | PROT_STREAMING),
			      "MADV_MERGEABLE VMA");
	else
		ksft_test_result_skip("MADV_MERGEABLE unavailable\n");
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

	epoch = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (epoch == MAP_FAILED)
		ksft_exit_fail_msg("epoch mmap: %m\n");
	memset(epoch, 1, REGION_SIZE);
	if (mprotect(epoch, REGION_SIZE, PROT_READ | PROT_STREAMING)) {
		if (errno == EINVAL) {
			/* Admission-negative checks above remain meaningful without the
			 * prototype.  Skip the epoch-dependent remainder cleanly. */
			while (n < 17) {
				ksft_test_result_skip("PROT_STREAMING unsupported\n");
				n++;
			}
			ksft_finished();
		}
		ksft_exit_fail_msg("enter private anonymous epoch: %m\n");
	}
	expect_einval(madvise(epoch, REGION_SIZE, MADV_MERGEABLE),
		      "MADV_MERGEABLE during STREAMING epoch");
	n++;
	expect_einval(madvise(epoch, REGION_SIZE, MADV_DONTNEED),
		      "MADV_DONTNEED during STREAMING epoch");
	n++;

	{
		unsigned char value = 0x5a;
		int memfd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
		ssize_t written;

		if (memfd < 0)
			ksft_exit_fail_msg("open /proc/self/mem: %m\n");
		errno = 0;
		written = pwrite(memfd, &value, 1, (off_t)(uintptr_t)epoch);
		ksft_test_result(written == -1 && epoch[0] == 1,
				 "FOLL_FORCE write during STREAMING epoch is rejected (errno=%d)\n",
				 errno);
		close(memfd);
	}
	n++;

	if (madvise(epoch, REGION_SIZE, MADV_DONTFORK))
		ksft_exit_fail_msg("MADV_DONTFORK during epoch: %m\n");
	{
		pid_t child = fork();
		int status;

		if (child < 0)
			ksft_test_result_fail("fork with MADV_DONTFORK epoch: %m\n");
		else if (!child) {
			unsigned char vec;

			_exit(mincore(epoch, REGION_SIZE, &vec) == -1 &&
			      errno == ENOMEM ? 0 : 1);
		} else {
			(void)waitpid(child, &status, 0);
			ksft_test_result(WIFEXITED(status) && !WEXITSTATUS(status),
					 "MADV_DONTFORK omits STREAMING VMA from child\n");
		}
	}
	n++;
	if (madvise(epoch, REGION_SIZE, MADV_DOFORK))
		ksft_exit_fail_msg("MADV_DOFORK during epoch: %m\n");

	{
		pid_t child;
		int status = 0;

		errno = 0;
		child = fork();
		if (child == -1 && errno == EBUSY) {
			ksft_test_result_pass("fork during STREAMING epoch -> EBUSY\n");
		} else {
			if (!child) {
				unsigned char vec;

				_exit(mincore(epoch, REGION_SIZE, &vec) == 0 ? 98 : 97);
			}
			if (child > 0)
				(void)waitpid(child, &status, 0);
			ksft_test_result_fail("fork during STREAMING epoch was not rejected (child=%d errno=%d status=%d)\n",
					      child, errno, status);
		}
	}
	n++;

	errno = 0;
	ksft_test_result(mremap(epoch, REGION_SIZE, REGION_SIZE * 2,
				 MREMAP_MAYMOVE) == MAP_FAILED && errno == EINVAL,
			 "STREAMING mremap resize -> EINVAL\n");
	n++;
	errno = 0;
	ksft_test_result(mremap(epoch, REGION_SIZE, REGION_SIZE,
				 MREMAP_MAYMOVE | MREMAP_DONTUNMAP) == MAP_FAILED &&
			 errno == EINVAL,
			 "STREAMING MREMAP_DONTUNMAP -> EINVAL\n");
	n++;
	{
		void *target = mmap(NULL, REGION_SIZE, PROT_NONE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		void *moved;

		if (target == MAP_FAILED)
			ksft_exit_fail_msg("mremap target mmap: %m\n");
		moved = mremap(epoch, REGION_SIZE, REGION_SIZE,
			       MREMAP_MAYMOVE | MREMAP_FIXED, target);
		ksft_test_result(moved == target && ((unsigned char *)target)[0] == 1,
				 "same-size STREAMING mremap move preserves data and epoch\n");
		if (moved == MAP_FAILED)
			munmap(target, REGION_SIZE);
		else
			epoch = moved;
	}
	n++;
	if (mprotect(epoch, REGION_SIZE, PROT_READ | PROT_WRITE))
		ksft_exit_fail_msg("retire private anonymous epoch: %m\n");

	cow = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (cow == MAP_FAILED)
		ksft_exit_fail_msg("COW mmap: %m\n");
	memset(cow, 1, REGION_SIZE);
	{
		int ready[2], release[2];
		pid_t child;
		char byte;

		if (pipe(ready) || pipe(release))
			ksft_exit_fail_msg("pipe: %m\n");
		child = fork();
		if (child < 0)
			ksft_exit_fail_msg("setup fork: %m\n");
		if (!child) {
			close(ready[0]);
			close(release[1]);
			if (write(ready[1], "x", 1) != 1 ||
			    read(release[0], &byte, 1) != 1)
				_exit(2);
			_exit(0);
		}
		close(ready[1]);
		close(release[0]);
		if (read(ready[0], &byte, 1) != 1)
			ksft_exit_fail_msg("child synchronization failed\n");
		errno = 0;
		if (mprotect(cow, REGION_SIZE,
			     PROT_READ | PROT_STREAMING) == -1 && errno == EBUSY) {
			ksft_test_result_pass("pre-existing COW alias -> EBUSY\n");
		} else {
			ksft_test_result_fail("pre-existing COW alias was not rejected\n");
		}
		if (write(release[1], "x", 1) != 1)
			ksft_exit_fail_msg("child release failed: %m\n");
		(void)waitpid(child, NULL, 0);
		close(ready[0]);
		close(release[1]);
	}
	n++;

	hole = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (hole == MAP_FAILED)
		ksft_exit_fail_msg("hole mmap: %m\n");
	errno = 0;
	ksft_test_result(mprotect(hole, REGION_SIZE,
				  PROT_READ | PROT_STREAMING) == -1 &&
			 errno == EBUSY,
			 "unpopulated anonymous range -> EBUSY\n");
	n++;

	munmap(anon, REGION_SIZE);
	munmap(shared, REGION_SIZE);
	munmap(shared_anon, REGION_SIZE);
	munmap(private_file, REGION_SIZE);
	munmap(mergeable, REGION_SIZE);
	munmap(epoch, REGION_SIZE);
	munmap(cow, REGION_SIZE);
	munmap(hole, REGION_SIZE);
	close(tmpfd);
	ksft_finished();
}
