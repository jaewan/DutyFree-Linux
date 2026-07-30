// SPDX-License-Identifier: GPL-2.0
#include <asm/paravirt.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/topology.h>
#include <linux/export.h>

static void __wbinvd(void *dummy)
{
	wbinvd();
}

void wbinvd_on_cpu(int cpu)
{
	smp_call_function_single(cpu, __wbinvd, NULL, 1);
}
EXPORT_SYMBOL(wbinvd_on_cpu);

int wbinvd_on_all_cpus(void)
{
	on_each_cpu(__wbinvd, NULL, 1);
	return 0;
}
EXPORT_SYMBOL(wbinvd_on_all_cpus);

static void __wbnoinvd(void *dummy)
{
	wbnoinvd();
}

/*
 * Broadcast a writeback-without-invalidate to every online CPU.
 * Used to push dirty cache lines to memory across the system without
 * paying the cold-cache cost of WBINVD (the wbnoinvd() helper itself
 * falls back to WBINVD on CPUs that lack the WBNOINVD extension).
 */
void wbnoinvd_on_all_cpus(void)
{
	on_each_cpu(__wbnoinvd, NULL, 1);
}
EXPORT_SYMBOL_GPL(wbnoinvd_on_all_cpus);

/*
 * Broadcast a writeback-without-invalidate to ONE logical CPU per
 * physical core.
 *
 * SMT siblings share every cache level (per-core L1/L2, package L3),
 * so a single WBNOINVD on either sibling writes back everything both
 * can hold; running it on both merely doubles the per-core latency
 * because the siblings contend for the shared write-back machinery
 * (measured ~2x on Sapphire Rapids). Selecting one representative
 * per core gives the same memory-coherence guarantee as
 * wbnoinvd_on_all_cpus() at roughly half the wall-clock cost.
 *
 * May sleep (mask allocation, hotplug read lock); falls back to the
 * full broadcast if the mask cannot be allocated. mmap_lock ->
 * cpus_read_lock() nests in this order elsewhere too (e.g.
 * lru_add_drain_all() from mlock).
 */
void wbnoinvd_on_each_core(void)
{
	cpumask_var_t mask;
	int cpu;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL)) {
		wbnoinvd_on_all_cpus();
		return;
	}

	cpus_read_lock();
	for_each_online_cpu(cpu)
		cpumask_set_cpu(cpumask_first_and(topology_sibling_cpumask(cpu),
						  cpu_online_mask),
				mask);
	on_each_cpu_mask(mask, __wbnoinvd, NULL, 1);
	cpus_read_unlock();

	free_cpumask_var(mask);
}
EXPORT_SYMBOL_GPL(wbnoinvd_on_each_core);
