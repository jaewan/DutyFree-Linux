/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_MMAN_H__
#define __ASM_MMAN_H__

#include <linux/types.h>
#include <uapi/asm/mman.h>

#ifdef CONFIG_X86_INTEL_MEMORY_PROTECTION_KEYS
#define pkey_to_vmflag_bits(key)			\
	(((key) & 0x1 ? VM_PKEY_BIT0 : 0) |		\
	 ((key) & 0x2 ? VM_PKEY_BIT1 : 0) |		\
	 ((key) & 0x4 ? VM_PKEY_BIT2 : 0) |		\
	 ((key) & 0x8 ? VM_PKEY_BIT3 : 0))
#else
#define pkey_to_vmflag_bits(key)	0UL
#endif

#ifdef CONFIG_PAT_STREAMING
#define streaming_prot_to_vmflag_bits(prot)		\
	(((prot) & PROT_STREAMING) ? VM_STREAMING : 0UL)
#define ARCH_PROT_VALID_MASK	(PROT_READ | PROT_WRITE | PROT_EXEC |	\
				 PROT_SEM | PROT_STREAMING)
#else
#define streaming_prot_to_vmflag_bits(prot)	0UL
#define ARCH_PROT_VALID_MASK	(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_SEM)
#endif

#if defined(CONFIG_X86_INTEL_MEMORY_PROTECTION_KEYS) || defined(CONFIG_PAT_STREAMING)
#define arch_calc_vm_prot_bits(prot, key)		\
	(pkey_to_vmflag_bits(key) | streaming_prot_to_vmflag_bits(prot))
#endif

#ifdef CONFIG_PAT_STREAMING
static inline bool arch_validate_prot(unsigned long prot, unsigned long addr)
{
	return (prot & ~ARCH_PROT_VALID_MASK) == 0;
}
#define arch_validate_prot arch_validate_prot
#endif

#endif /* __ASM_MMAN_H__ */
