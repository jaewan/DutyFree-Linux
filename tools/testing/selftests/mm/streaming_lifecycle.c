// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dirty-producer and reuse test for the coherent H2 transition.
 *
 * The producer intentionally uses ordinary WB stores and performs no CLWB,
 * CLFLUSH or WBINVD.  A reader on another CPU must observe the completed
 * generation after PROT_STREAMING, and repeated retire/rebuild cycles must not
 * expose an older generation.  The test also reports transition latency so a
 * machine-wide seal oracle cannot silently reappear in the baseline path.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef PROT_STREAMING
#define PROT_STREAMING 0x10
#endif

#define REGION_SIZE (8UL * 1024 * 1024)
#define CACHELINE 64UL
#define GENERATIONS 8

struct producer_args {
	uint64_t *region;
	uint64_t generation;
	int cpu;
	int actual_cpu;
};

static int pin_to_cpu(int cpu)
{
	cpu_set_t set;

	if (cpu < 0)
		return 0;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static uint64_t value_at(uint64_t generation, size_t line)
{
	return 0x9e3779b97f4a7c15ULL * generation ^
	       0xd1b54a32d192ed03ULL * (line + 1);
}

static void *produce(void *opaque)
{
	struct producer_args *args = opaque;
	size_t lines = REGION_SIZE / CACHELINE;
	size_t line;

	pin_to_cpu(args->cpu);
	args->actual_cpu = sched_getcpu();
	/*
	 * Write line zero last so at least the first reader access targets the
	 * producer's youngest dirty WB line.
	 */
	for (line = 1; line < lines; line++)
		args->region[line * CACHELINE / sizeof(uint64_t)] =
			value_at(args->generation, line);
	args->region[0] = value_at(args->generation, 0);
	return NULL;
}

static int verify(const uint64_t *region, uint64_t generation)
{
	size_t lines = REGION_SIZE / CACHELINE;
	size_t line;

	for (line = 0; line < lines; line++) {
		uint64_t got = region[line * CACHELINE / sizeof(uint64_t)];

		if (got != value_at(generation, line)) {
			ksft_print_msg("generation %llu line %zu: got %#llx expected %#llx\n",
				       (unsigned long long)generation, line,
				       (unsigned long long)got,
				       (unsigned long long)value_at(generation, line));
			return -1;
		}
	}
	return 0;
}

int main(void)
{
	uint64_t enter_total = 0, exit_total = 0, enter_max = 0;
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	uint64_t *region;
	int generation;
	int ok = 1;
	int cross_cpu_ok = 1;
	int last_producer_cpu = -1, last_reader_cpu = -1;

	ksft_print_header();
	ksft_set_plan(4);

	region = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %m\n");
	ksft_test_result_pass("private WB object allocated\n");

	for (generation = 1; generation <= GENERATIONS; generation++) {
		struct producer_args args = {
			.region = region,
			.generation = generation,
			.cpu = cpus > 1 ? 0 : -1,
			.actual_cpu = -1,
		};
		pthread_t producer;
		uint64_t begin, elapsed;

		if (pthread_create(&producer, NULL, produce, &args) ||
		    pthread_join(producer, NULL))
			ksft_exit_fail_msg("producer thread failed\n");

		if (pin_to_cpu(cpus > 1 ? 1 : -1))
			cross_cpu_ok = 0;
		last_producer_cpu = args.actual_cpu;
		last_reader_cpu = sched_getcpu();
		if (cpus > 1 && last_producer_cpu == last_reader_cpu)
			cross_cpu_ok = 0;
		begin = now_ns();
		if (mprotect(region, REGION_SIZE,
			     PROT_READ | PROT_STREAMING)) {
			if (errno == EINVAL) {
				/* The allocation check above is still useful on a host that
				 * lacks the prototype; finish the published TAP plan cleanly. */
				ksft_test_result_skip("PROT_STREAMING unsupported\n");
				ksft_test_result_skip("PROT_STREAMING unsupported\n");
				ksft_test_result_skip("PROT_STREAMING unsupported\n");
				munmap(region, REGION_SIZE);
				ksft_finished();
			}
			ksft_exit_fail_msg("enter generation %d: %m\n", generation);
		}
		elapsed = now_ns() - begin;
		enter_total += elapsed;
		if (elapsed > enter_max)
			enter_max = elapsed;

		if (verify(region, generation))
			ok = 0;

		begin = now_ns();
		if (mprotect(region, REGION_SIZE, PROT_READ | PROT_WRITE))
			ksft_exit_fail_msg("retire generation %d: %m\n", generation);
		exit_total += now_ns() - begin;
	}

	ksft_test_result(ok && cross_cpu_ok,
			 "dirty WB producer is visible to cross-CPU STREAMING reader\n");
	ksft_test_result(ok,
			 "eight retire/rebuild generations never expose stale data\n");
	ksft_print_msg("H2 transition 8MiB: enter_avg=%llu us enter_max=%llu us exit_avg=%llu us\n",
		       (unsigned long long)(enter_total / GENERATIONS / 1000),
		       (unsigned long long)(enter_max / 1000),
		       (unsigned long long)(exit_total / GENERATIONS / 1000));
	ksft_print_msg("H2 affinity: producer_cpu=%d reader_cpu=%d online_cpus=%ld\n",
		       last_producer_cpu, last_reader_cpu, cpus);
	ksft_test_result_pass("transition latency reported separately\n");

	munmap(region, REGION_SIZE);
	ksft_finished();
}
