// SPDX-License-Identifier: GPL-2.0-only
/*
 * mprotect(PROT_STREAMING) on a sealed memfd.
 *
 * A sealed memfd is the one MAP_SHARED file-backed carrier the entry check
 * admits. F_SEAL_WRITE supplies I1 at object scope: it cannot be applied
 * while a writable mapping exists, and none can be created afterwards, so
 * there is no dirty page-cache writeback for the Streaming cache-bit rewrite
 * to race against.
 *
 *   1. sealed memfd, single mapper          -> accepted
 *   2. unsealed shared memfd                -> EINVAL
 *   3. sealed memfd with a second mapping   -> EINVAL (I0: with several
 *      mappers a frame's type would have to be agreed across address
 *      spaces, which this prototype does not do)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef PROT_STREAMING
#define PROT_STREAMING	0x10
#endif

#ifndef F_ADD_SEALS
#define F_ADD_SEALS	1033
#define F_SEAL_SHRINK	0x0002
#define F_SEAL_GROW	0x0004
#define F_SEAL_WRITE	0x0008
#endif

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING	0x0002U
#endif

#define REGION_SIZE	(2 * 1024 * 1024)

#define SEALS	(F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK)

/* Create a memfd of REGION_SIZE, populated, optionally sealed. */
static int make_memfd(const char *name, int seal)
{
	void *scratch;
	int fd;

	fd = memfd_create(name, MFD_ALLOW_SEALING);
	if (fd < 0)
		ksft_exit_fail_msg("memfd_create: %m\n");
	if (ftruncate(fd, REGION_SIZE))
		ksft_exit_fail_msg("ftruncate: %m\n");

	/*
	 * Populate through a temporary writable mapping, which must be gone
	 * before sealing: F_SEAL_WRITE fails with EBUSY while one exists.
	 */
	scratch = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (scratch == MAP_FAILED)
		ksft_exit_fail_msg("mmap scratch: %m\n");
	memset(scratch, 0xa5, REGION_SIZE);
	if (munmap(scratch, REGION_SIZE))
		ksft_exit_fail_msg("munmap scratch: %m\n");

	if (seal && fcntl(fd, F_ADD_SEALS, SEALS) < 0)
		ksft_exit_fail_msg("F_ADD_SEALS: %m\n");

	return fd;
}

int main(void)
{
	void *first, *second;
	int fd, ret;

	ksft_print_header();
	ksft_set_plan(3);

	/* 1. sealed, single mapper -> accepted */
	fd = make_memfd("streaming-sealed", 1);
	first = mmap(NULL, REGION_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (first == MAP_FAILED)
		ksft_exit_fail_msg("mmap sealed: %m\n");

	ret = mprotect(first, REGION_SIZE, PROT_READ | PROT_STREAMING);
	if (ret && errno == EINVAL)
		ksft_exit_skip("sealed memfd rejected; kernel without CONFIG_PAT_STREAMING or without sealed-memfd support: %m\n");
	ksft_test_result(ret == 0,
			 "sealed memfd, single mapper -> accepted\n");

	if (!ret && mprotect(first, REGION_SIZE, PROT_READ))
		ksft_exit_fail_msg("revert to PROT_READ: %m\n");
	munmap(first, REGION_SIZE);
	close(fd);

	/* 2. unsealed shared memfd -> EINVAL */
	fd = make_memfd("streaming-unsealed", 0);
	first = mmap(NULL, REGION_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (first == MAP_FAILED)
		ksft_exit_fail_msg("mmap unsealed: %m\n");

	ret = mprotect(first, REGION_SIZE, PROT_READ | PROT_STREAMING);
	ksft_test_result(ret == -1 && errno == EINVAL,
			 "unsealed shared memfd -> EINVAL\n");
	munmap(first, REGION_SIZE);
	close(fd);

	/* 3. sealed but multiply mapped -> EINVAL */
	fd = make_memfd("streaming-sealed-2map", 1);
	first = mmap(NULL, REGION_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	second = mmap(NULL, REGION_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (first == MAP_FAILED || second == MAP_FAILED)
		ksft_exit_fail_msg("mmap sealed x2: %m\n");

	ret = mprotect(first, REGION_SIZE, PROT_READ | PROT_STREAMING);
	ksft_test_result(ret == -1 && errno == EINVAL,
			 "sealed memfd with a second mapper -> EINVAL\n");
	munmap(first, REGION_SIZE);
	munmap(second, REGION_SIZE);
	close(fd);

	ksft_finished();
}
