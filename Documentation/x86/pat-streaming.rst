.. SPDX-License-Identifier: GPL-2.0

==========================================================
Streaming page-table memory type — Step 1 OS-side prototype
==========================================================

This document describes the OS-side prototype of the **Streaming**
page-table memory type proposed in *The Directory Tax: Coherence as a
Shared-Resource Problem for Fabric-Attached Memory* (§4).  It is a
research-stage feature gated on ``CONFIG_PAT_STREAMING`` and is not
intended for production use.

What this step does and does not do
===================================

**Does:**

* Reserves PAT MSR slot 6 with the WB raw encoding and re-tags it as
  ``_PAGE_CACHE_MODE_STREAMING`` in the kernel cache-mode tables.
* Defines the PTE encoding contract: ``PAT=1, PCD=1, PWT=0, SOFTW1=1``
  (SOFTW1 aliases ``_PAGE_SPECIAL``, which is always set on
  ``VM_PFNMAP`` VMAs anyway).
* Exposes ``MAP_STREAMING`` (``0x200000``) through the ``mmap(2)`` UAPI.
* Restricts ``MAP_STREAMING`` to **device-DAX** file descriptors
  (struct dev_pagemap memory).  This anchors the mapping outside
  ``ZONE_NORMAL`` and structurally excludes kswapd, AutoNUMA, KSM, and
  compaction.
* Stamps qualifying VMAs with ``VM_STREAMING | VM_DONTCOPY | VM_PFNMAP``
  and applies ``pgprot_streaming()`` to ``vm_page_prot``, so faults
  install the Streaming encoding in the PTE.
* Enforces I1 (read-only) in :c:func:`do_mmap` (rejects ``PROT_WRITE``,
  ``MAP_PRIVATE``, anonymous, non-DAX) and in :c:func:`mprotect_fixup`
  (rejects upgrade to ``PROT_WRITE`` and any drop of structural flags).
* Preserves the Streaming intent across :c:func:`pte_modify` via
  ``_COMMON_PAGE_CHG_MASK`` (which already preserves
  ``_PAGE_SPECIAL`` = ``_PAGE_SOFTW1``) and the streaming branch in
  :c:func:`pgprot_modify`.
* Surfaces a slow-path :c:func:`streaming_pte_audit` ``WARN_ONCE`` when
  ``CONFIG_DEBUG_VM=y``.
* Exposes ``/sys/kernel/debug/streaming/pte_query`` for the kselftest
  harness to read back PTEs of the calling task.

**Does not yet:**

* Track Streaming reservations per socket (Step 2).
* Implement the two-phase teardown drain (Step 3).
* Provide an OOM-quarantine for Streaming-tagged frames (Step 4).
* Reject ``MAP_STREAMING``-backed KVM memslots (Step 5).
* Wire CXL-side PMU proxy measurements (Step 6).

Hardware contract for Streaming-aware silicon
=============================================

The PTE pattern that gem5 (or any future Streaming-aware
microarchitecture) must pattern-match on:

* PAT bit (bit 7 of the 4 KiB PTE): 1
* PCD bit (bit 4): 1
* PWT bit (bit 3): 0
* ``_PAGE_SOFTW1`` (bit 9): 1

When all four conditions hold, hardware should:

* Service reads with WB semantics.
* Treat clean-line evictions as silent discards.
* Skip directory-entry allocation on the fill path.

If the high PAT bit is dropped by errata, the encoding falls back to
PAT slot 2 (UC-), which is correct (just slow).

Building
========

Use the project's ``vmconfig`` baseline and add the Streaming options::

    cd linux
    cp ../vmconfig .config
    ./scripts/config \
        --enable CONFIG_PAT_STREAMING \
        --enable CONFIG_DEBUG_VM \
        --enable CONFIG_KUNIT \
        --enable CONFIG_KUNIT_TEST \
        --enable CONFIG_DEV_DAX \
        --enable CONFIG_PAT_STREAMING_KUNIT_TEST
    make olddefconfig
    make -j$(nproc) bzImage

The kselftest binary builds under ``tools/testing/selftests/mm/``::

    cd tools/testing/selftests/mm
    make streaming_test

KUnit
=====

The KUnit suite registers itself with ``kunit_test_suite()`` and runs
automatically at boot when ``CONFIG_PAT_STREAMING_KUNIT_TEST=y``.  Two
ways to exercise it:

1. Boot the patched kernel under QEMU and grep ``dmesg`` for ``KTAP``::

       qemu-system-x86_64 -M q35 -cpu host -enable-kvm -m 4G \
           -kernel arch/x86/boot/bzImage -nographic
       # in the guest:
       dmesg | grep -A20 'KTAP version'

2. Run the suite under UML in a clean tree::

       make ARCH=um mrproper
       ./tools/testing/kunit/kunit.py run \
           --kunitconfig=tools/testing/kunit/configs/streaming.config

   KUnit insists on a clean source tree; UML cross-builds will refuse
   if x86 in-tree build artifacts are present.

   Cases:
     * ``streaming_msr_slot6_is_wb`` — ``MSR_IA32_CR_PAT`` byte 6
       == ``X86_MEMTYPE_WB``.
     * ``streaming_cachemode_roundtrip`` — every cache mode round-trips.
     * ``streaming_pgprot_bits`` — ``pgprot_streaming(PAGE_KERNEL)``
       produces (PAT=1, PCD=1, PWT=0, SOFTW1=1).
     * ``streaming_chg_mask_preserves_softw1`` — :c:func:`pte_modify`
       preserves ``_PAGE_SOFTW1`` and the cache bits.

kselftest
=========

After booting the patched kernel, set up a device-DAX namespace
(``ndctl create-namespace -m devdax`` on a NVDIMM-backed QEMU target),
then run::

    cd tools/testing/selftests/mm
    ./streaming_test

It exercises five paths:

* Negative cases: anonymous, ``PROT_WRITE``, ``MAP_PRIVATE``, non-DAX
  fd are all rejected.
* Positive: ``mmap(MAP_STREAMING | MAP_SHARED, PROT_READ)`` on a
  device-DAX fd succeeds.
* ``mprotect`` upgrade is rejected with ``EACCES``.
* ``fork()`` does not inherit the Streaming VMA; the child segfaults
  on access.
* ``/sys/kernel/debug/streaming/pte_query`` decodes the actual PTE
  and confirms (PAT=1, PCD=1, PWT=0, SOFTW1=1, write=0).

Without device-DAX, the positive cases report SKIP rather than FAIL —
the negative cases (illegal-fd rejection) still run.

Known limitations
=================

* Slot 6 was previously a duplicate of UC-.  ``init_cache_modes()``
  re-installs the reverse-lookup row only when the MSR confirms the
  Streaming encoding, so unrelated UC- mappings are not affected.
* ``MAP_STREAMING`` consumes flag bit ``0x200000``; that allocation is
  prototype-scoped.
* ``VM_STREAMING`` aliases ``VMA_HIGH_ARCH_6_BIT``, which is unused on
  x86 but consumed by ARM64 GCS — same-binary collisions are
  impossible, but cross-arch tooling that inspects ``vm_flags``
  numerically should be aware.
* ``_PAGE_SOFTW1`` aliases ``_PAGE_SPECIAL`` (bit 9).  Because every
  Streaming VMA is ``VM_PFNMAP``, the ``_PAGE_SPECIAL`` bit is set on
  the PTE by construction, so the alias is semantically harmless and
  the existing preservation in ``_COMMON_PAGE_CHG_MASK`` works for
  free.
* No ISA guarantee yet exists for the cross-core ordering property
  (assumption A1 in the paper).  The teardown protocol will note this
  explicitly when it lands.
