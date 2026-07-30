.. SPDX-License-Identifier: GPL-2.0

============================================
PAT slot 6: Streaming memory type prototype
============================================

This document describes the prototype Streaming memory type added under
``CONFIG_PAT_STREAMING``. It is the OS half of the hardware-software
contract proposed in *The Directory Tax* §4: a page-table marker that an
appropriately enhanced page walker (gem5 or production silicon) can
recognise as a hint to bypass coherence-directory enrollment for
read-only streaming workloads. On current silicon the slot is
indistinguishable from regular write-back; the value is the contract,
not a behavioural change.

PTE encoding
============

PAT slot 6 is reprogrammed from its default UC- duplicate role to a
WB-equivalent encoding::

    Configuration [0-7]: WB WC UC- UC WB WP ST WT

The selector bits in a 4 KiB PTE are::

    _PAGE_PAT  = 1   (bit 7)
    _PAGE_PCD  = 1   (bit 4)
    _PAGE_PWT  = 0   (bit 3)

This is the value ``cachemode2protval(_PAGE_CACHE_MODE_STREAMING)``
returns and the value ``pgprot_streaming()`` ORs into a pgprot_t after
clearing ``_PAGE_CACHE_MASK``. Reverse lookup
(``pgprot2cachemode()``) maps the same bit pattern back to
``_PAGE_CACHE_MODE_STREAMING`` thanks to a fixup in
``init_cache_modes()``.

Large pages (hugetlb)
---------------------

At PMD (2 MiB) and PUD (1 GiB) leaf level bit 7 is PSE, so the PAT
selector moves to bit 12 (``_PAGE_PAT_LARGE``). The same slot 6 is
selected by::

    Level            PAT selector          slot-6 encoding
    4 KiB PTE        bit 7  (PAT)          PAT=1 PCD=1 PWT=0
    2 MiB PMD leaf   bit 12 (PAT_LARGE)    PSE=1 PAT_LARGE=1 PCD=1 PWT=0
    1 GiB PUD leaf   bit 12 (PAT_LARGE)    PSE=1 PAT_LARGE=1 PCD=1 PWT=0

``pgprot_streaming_huge()`` (= ``pgprot_4k_2_large(pgprot_streaming())``)
produces the large-level encoding; hugetlb streaming VMAs carry it in
``vma->vm_page_prot`` so that hugetlb faults, COW and folio migration
reinstall slot 6 without extra plumbing.

Userspace ABI
=============

Streaming is toggled by ``mprotect()``::

    /* Enter Streaming. */
    mprotect(addr, len, PROT_READ | PROT_STREAMING);

    /* Leave Streaming, restoring whatever permissions you want. */
    mprotect(addr, len, PROT_READ | PROT_WRITE);

Constraints checked in ``mm/streaming.c::streaming_validate_entry()``:

* Combining ``PROT_STREAMING`` with ``PROT_WRITE`` is rejected with
  ``-EINVAL``. Streaming pages must remain read-only for the lifetime
  of the mode.
* The target VMA must not be ``VM_PFNMAP``, ``VM_MIXEDMAP`` or
  ``VM_PAT`` - those VMAs already have an owner for the cache bits.
* Shared writable file-backed mappings are rejected to avoid colliding
  with the writeback path. Note that every hugetlb VMA carries a
  hugetlbfs ``vm_file``, so ``MAP_SHARED`` hugetlb is rejected by this
  rule while ``MAP_PRIVATE`` hugetlb (2 MiB and 1 GiB) is supported.
  Excluding shared hugetlb also excludes hugetlb PMD sharing
  (``VM_MAYSHARE``-only), which is what makes the read-mode hugetlb
  vma lock in the rewrite walk sufficient.
* VMAs registered with ``UFFDIO_REGISTER_MODE_WP`` are rejected.
* Xen PV guests are rejected because their override of
  ``ptep_modify_prot_transaction`` breaks the cache-bit preservation
  this code relies on.
* Pages that are paged out (swap or migration entries in the range)
  cause the PTE walker to bail with ``-EBUSY``. The caller is expected
  to populate the range up front, e.g. via ``mlock()`` or a forced read.

Leaving Streaming has no constraints beyond the user supplying a new
``prot`` value: the original permissions are *not* remembered by the
kernel; that is the caller's responsibility.

Transition mechanics
====================

Entry (WB to Streaming) under ``mmap_write_lock``:

#. ``mprotect_fixup()`` detects the ``VM_STREAMING`` transition and
   calls ``streaming_validate_entry()``.
#. ``MM_CP_STREAMING_ENTER`` is added to ``mm_cp_flags``; this forces
   ``pgtable_split_needed()`` to return true, so
   ``change_pmd_range()`` splits any THP/devmap PMD in the range
   before the post-pass touches PTEs.
#. ``MM_CP_TRY_CHANGE_WRITABLE`` is suppressed so
   ``can_change_pte_writable()`` cannot re-mark a dirty page
   writable underneath the new R-only invariant.
#. ``change_protection()`` clears the write bit and applies the new
   pgprot. Because PWT/PCD/PAT live in ``_PAGE_CHG_MASK``,
   ``pte_modify()`` preserves the OLD cache bits at this point.
#. ``streaming_apply_cache_bits()`` walks the now-PTE-only range,
   rewrites the cache bits to slot 6, and issues
   ``flush_tlb_range()``. After this point every CPU sees
   read-only-Streaming PTEs.
#. ``do_mprotect_pkey()`` calls ``tlb_finish_mmu()`` (the usual
   change_protection flush) and then ``wbnoinvd_on_all_cpus()`` to
   push any residual dirty cache lines to RAM. On CPUs without
   ``X86_FEATURE_WBNOINVD`` this falls back to ``WBINVD`` via the
   ALTERNATIVE() machinery; the memory-coherence guarantee is the
   same, only the cache-preservation property is lost.

Exit (Streaming to WB) is the same shape minus WBNOINVD: the
``MM_CP_STREAMING_LEAVE`` flag still forces THP splits and the cache
bits are rewritten to slot 0, after which ``change_protection()`` and
``flush_tlb_range()`` make the region a normal WB mapping again with
whatever permissions the caller passed.

hugetlb VMAs take the same path with two differences. First, they are
never split: ``change_protection()`` diverges into
``hugetlb_change_protection()`` (which only handles R/W/Exec) and the
cache bits are then rewritten in place at PMD/PUD leaf granularity by
``streaming_hugetlb_entry()`` under ``huge_pte_lock()``, using
``_PAGE_LARGE_CACHE_MASK`` and the bit-12 encoding above, followed by
``flush_hugetlb_tlb_range()``. Second, because ``huge_pte_modify()``
does not preserve bit 12, a huge pte passing through
``hugetlb_change_protection()`` mid-transition briefly carries a
different slot (slot 4, programmed WB, on entry; slot 2, UC-, on
exit). Both transients are coherent and bounded by the
``mmap_write_lock`` holder; the rewrite pass immediately follows.

Future-fault PTE construction is handled by an override in
``vma_set_page_prot()``: when ``VM_STREAMING`` is set, the cached
``vma->vm_page_prot`` is post-processed by ``pgprot_streaming()``
(``pgprot_streaming_huge()`` for hugetlb) so that page faults, COW and
swap-in all install slot-6 PTEs without any extra plumbing in the
fault path.

Observability
=============

* ``/proc/PID/smaps`` marks streaming VMAs with ``sm`` in the
  ``VmFlags`` line.
* ``/sys/kernel/debug/streaming/pte_query`` (prototype, root only)
  takes a hex virtual address and reports
  ``<vaddr_hex> <pte_hex> <level_shift>`` for the writer's own ``mm``,
  where ``level_shift`` is 12 (4 KiB PTE), 21 (2 MiB PMD leaf) or
  30 (1 GiB PUD leaf). Used by the kselftests to verify that the
  expected cache bits are installed at the expected level.

Out of scope
============

This prototype intentionally does not handle:

* device-DAX / PFNMAP backings (validation rejects them);
* KSM / autoNUMA / compaction interactions (the prototype refuses
  swap-paged pages instead of integrating with the page-out path);
* KVM memslot fences (a guest may observe a slot 6 mapping today);
* multiple concurrent streaming users contending on the same
  physical pages.

These are tracked as follow-up steps B-F in the design plan.

Hand-off to gem5 / simulators
==============================

A page walker that wants to enable the H2 hardware obligation
(directory bypass + silent clean discard) should treat a 4 KiB PTE as
Streaming iff::

    (PTE & _PAGE_PRESENT) &&
    (PTE & _PAGE_PAT) &&
    (PTE & _PAGE_PCD) &&
    !(PTE & _PAGE_PWT)

and a PMD/PUD *leaf* entry (PSE set) as Streaming iff::

    (E & _PAGE_PRESENT) &&
    (E & _PAGE_PSE) &&
    (E & _PAGE_PAT_LARGE) &&      /* bit 12 */
    (E & _PAGE_PCD) &&
    !(E & _PAGE_PWT)

Reads behave exactly like WB; the additional semantics are entirely a
contract between the kernel and the simulator.
