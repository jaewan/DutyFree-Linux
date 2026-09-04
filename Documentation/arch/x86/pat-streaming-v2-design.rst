.. SPDX-License-Identifier: GPL-2.0

===================================================
STREAMING_RO: immutable read epochs and fast sealing
===================================================

Status
======

This document is the design contract for the next iteration of the
``CONFIG_PAT_STREAMING`` prototype.  It deliberately does *not* introduce a
writable Streaming memory type.  The public capability is::

        STREAMING_RO = immutable, read-only consumption epoch

The motivation is an enhanced HNF which can avoid LLC fills (H2) and, when
the stronger hardware is present, directory/snoop-filter enrolment (H3).
Those optimisations are only correct when the object has no writers.

Object state machine
====================

An object progresses through the following states::

        WB_MUTABLE -- seal_and_publish() --> STREAMING_RO -- retire() --> WB_MUTABLE

``WB_MUTABLE`` is ordinary Linux memory.  Builders may use ordinary cached
stores, or an explicitly selected non-temporal construction library.
``STREAMING_RO`` permits loads only.  Stores, RMWs, atomics, writable aliases,
and DMA writes are forbidden for the whole physical object, not merely one
VMA.

The transition has a release boundary:

1. all builders finish and publish their writes;
2. the kernel proves or establishes exclusive object ownership and removes
   writable aliases;
3. dirty data is made globally visible;
4. the kernel publishes slot-6, read-only PTEs and completes TLB shootdown;
5. readers may issue H2/H3 STREAMING requests.

The inverse transition must invalidate or otherwise account for any retained
copies before the frame can be reused with a different type.

When an H3 realization needs a drain
====================================

H2 keeps coherent lookup and can find a dirty producer line.  Skipping the new
reader's directory enrollment under H3 does not by itself remove that lookup:
a coherent ``ReadOnce`` realization can still snoop a pre-existing dirty owner
and then decline to retain or enroll the reader.  Such a realization does not
intrinsically require a clean-at-home entry.

An H3 realization that also bypasses owner lookup cannot do so.  A dirty cached
store made before that epoch must reach the shared home first.  The required
seal operation is therefore a property of the selected H3 datapath, not of the
baseline STREAMING declaration.

The optional ``CONFIG_PAT_STREAMING_H3_SEAL_ORACLE`` uses ``WBNOINVD`` on each
physical core.  That is a conservative implementation for an H3 realization
that cannot coherently resolve an existing dirty owner, but it is a large,
machine-wide transition cost and is not part of baseline H2.

Fast sealing is an optimisation, not a weaker correctness rule
===============================================================

For a trusted producer runtime, a fast path may be used only when every byte
of the published object was constructed by full-line non-temporal stores and
each producer has completed ``SFENCE`` before the publish barrier.  Under the
same object-wide no-writer/no-alias proof, this avoids ordinary dirty cache
state and can avoid the machine-wide drain.

PAT cannot enforce this property: a PTE does not transform an arbitrary MOV
into MOVNT*.  Consequently, a generic ``mprotect`` operation cannot claim NT
provenance.  Baseline H2 needs no such claim because it retains coherent owner
lookup.  A future H3 realization that cannot resolve an existing dirty owner
must use the conservative seal oracle or a distinct interface that can attest
to the construction discipline.  Normal cached writes are allowed only in
``WB_MUTABLE``.

Write-through is not a substitute
==================================

PAT WT describes a hardware cacheability type; it neither forces compiler
generated stores to be non-temporal nor establishes ownership, invalidation,
atomic, DMA, or aliasing rules.  It may also conflict with WB aliases of
ordinary System RAM.  It is therefore not a writable H3 contract.

A future writable streaming facility would require a separate hardware
protocol: distinct read/write tags, coherent writer ownership, invalidation,
ordering/fence semantics, atomic/DMA rules, and likely full-line append-only
restrictions.  It is out of scope for STREAMING_RO.

Required kernel work before production/shared-object claims
============================================================

* Maintain per-folio/object epoch state and use reverse mapping to ensure all
  aliases agree with the state; reject or retag conflicting maps and prevent
  a second mmap/fork escape while sealed.
* Make transitions transactional: preflight non-present/migration entries,
  then change PTEs, or roll back fully on failure.
* Coordinate reclaim and migration with the epoch.  The VMA prototype rejects
  KSM and userfaultfd transitions and rejects standard ``FOLL_PIN`` DMA pins,
  but production support needs persistent object/folio state rather than
  checks tied only to the current VMA.
* Make the sealing policy and its cost observable and record it with every
  experiment.

Until then, H3 experiments are restricted to prepopulated private objects in
the controlled single-process/full-system apparatus.  Ray objects and shared
embedding tables require the object-wide protocol above.

Current prototype boundary
==========================

The present ``mprotect(PROT_STREAMING)`` implementation accepts only an
already-populated range contained in one VMA.  It rejects grow flags and a
range spanning VMAs before changing any PTE.  This is intentionally narrower
than ordinary ``mprotect``: it prevents the generic per-VMA loop from exposing
a partially converted object while the transactional object protocol does not
yet exist.  An ``-EINVAL`` here is a refused transition, not a fallback to a
weaker STREAMING guarantee.

Validation matrix
=================

The implementation must demonstrate:

* producer (cached and NT) -> seal -> multi-core readers equals a WB oracle;
* stores, atomics, and DMA during ``STREAMING_RO`` are rejected;
* second mmap, fork/COW, remap, and concurrent transition attempts are
  rejected or retain the same object state;
* H3 receives no tagged write or atomic request;
* seal latency is reported separately from steady-state read benefit, with an
  amortisation/break-even analysis.
