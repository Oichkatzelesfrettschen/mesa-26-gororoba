Terakan Debt Index
==================

Purpose
-------

This document is the Mesa-side index for Terakan-only debt.  It keeps the
implementation work tied to evidence instead of scattering decisions across
chat, build logs, and raw CTS output.

Scope
-----

Terakan-only means the Vulkan driver under ``src/amd/terascale/vulkan/`` plus
the shared ``src/amd/terascale/common/`` and ``src/amd/terascale/compiler/``
dependencies that are required to build and reason about that driver.

The active hardware target is Palm / Wrestler, Radeon HD 6310, PCI
``1002:9802``.  Other R600-class chips are future portability lanes; they must
not weaken the Palm correctness contract.

Debt Classes
------------

Track each item in one primary class, with secondary classes when useful:

``build-delivery``
   Build profiles, host packages, install prefix, root/no-root split,
   package-manager delivery, rollback, and runtime environment.

``evidence-provenance``
   Claims, source downloads, hashes, generated summaries, processed result
   bundles, and registry freshness.

``feature-exposure``
   Vulkan feature and extension bits that must match real implementation and
   conformance evidence.

``silicon-contract``
   Palm-specific behavior from PM4, SQ, CB/RAT, LS/KCACHE, cache flushes,
   tiling, and command submission.

``test-conformance``
   CTS shards, no-submit probes, hang-risk gates, smoke tests, unit tests, and
   result classifiers.

``structural-code``
   Large files, duplicated state emission, debug-gated ``FIX-*`` paths,
   TODO/FIXME items, and unclear ownership boundaries.

``portability``
   Differences between Palm, Sumo, Sumo2, Cedar, Redwood, Juniper, Cypress,
   Barts, Turks, Caicos, Cayman, and Aruba.

Current Authority
-----------------

The steinmarder tree is the evidence registry and reverse-engineering source
of truth.  Mesa patches should link to processed findings, not raw logs.

Primary local references:

* ``../../steinmarder/src/re/r600/docs/program/palm_wrestler_focus.md``
* ``../../steinmarder/src/re/r600/docs/PALM_WRESTLER_HW_CAPABILITY_MATRIX.md``
* ``../../steinmarder/src/re/r600/docs/VK10_MUSTPASS_CATEGORY_MATRIX.md``
* ``../../steinmarder/src/re/r600/registry/README.md``
* ``../../steinmarder/src/re/r600/docs/TOOLKIT_INDEX.md``
* ``../../steinmarder/src/re/r600/docs/isa_references/register-source-table.md``

Primary external source cache candidates:

* Khronos Vulkan specification registry.
* Khronos VK-GL-CTS and Vulkan CTS documentation.
* Mesa build and install documentation.
* AMD Evergreen ISA Reference Guide.
* AMD Evergreen 3D Register Reference Guide.

Phase Ledger
------------

P0 - Baseline and Authority
   Record commit, build profile, installed ICD, x130e kernel, CTS version,
   registry freshness, and external source cache hashes before changing code.

P1 - Build and Delivery
   Keep ``terakan-distcc-no-rusticl`` as the daily lane, keep Rusticl as a
   recovery lane, install with ``meson install --no-rebuild``, and validate
   the active prefix ``/usr/local/mesa-26-gororoba``.

   Current delivery decision: use the ``/usr/local/mesa-26-gororoba`` staging
   prefix for Terakan-only installs.  Rollback archives that prefix aside; it
   does not delete it.  A PKGBUILD remains a future task after the install
   manifest and stale-Rusticl cleanup contract are stable.

P2 - Evidence and Documentation
   For every claim or result, keep a processed bundle with README, manifest,
   parsed CSV/JSON, hashes, hazard classification, and registry row.

P3 - Feature Exposure Truth
   Audit every advertised feature in ``terakan_physical_device.c`` against
   CTS evidence and Palm hardware capability.

P4 - Vulkan 1.0 Critical Path
   Finish image.store buffer issues, packed-format precision, and
   ``opatomic_return_values.compex`` before widening to ``ssbo``, ``ubo``,
   ``compute``, ``glsl``, bounded ``api``, renderpass, and broader image
   groups.

P5 - Structural Code
   Graduate, remove, or document every ``FIX-*`` path.  Split storage-image,
   RAT, KCACHE, descriptor, and robustness policy into explicit tables and
   helpers before broad rewrites.

P6 - R600-Class Portability
   Add chip-family capability tables only after Palm invariants are stable.

P7 - Vulkan 1.1 Later Lane
   Defer API promotion until VK 1.0 frontier docs and broad shards are fresh.
   Order: maintenance3, descriptor update template hardening, external sync,
   device group, relaxed block layout, subgroups, multiview, API version.

Open High-Value Items
---------------------

* ``image.store`` buffer RCA: verify the format-aware Buffer-UAV fix and
  update aggregate counts.
* ``a2b10g10r10_uint_pack32`` image.store failures: prove shader pack/unpack
  precision or refute with descriptor/PM4 evidence.
* ``opatomic_return_values.compex``: separate PALM global CMPXCHG behavior,
  LDS CMPXCHG behavior, and CTS-safe speculative-XCHG semantics.
* Linear storage images: keep expected ``NotSupported`` classification unless
  a new RCA proves a safe implementation path.
* Image query: keep pipeline-create-only status until runtime readback evidence
  exists.
* Descriptor update templates: check unsupported-type handling against spec and
  CTS.
* Robustness: cover SSBO, texel-buffer image store/atomic, non-buffer image UAV
  writes, null descriptors, dynamic offsets, and descriptor copies/templates.
* Meta paths: test 3x-expanded format clear, MSAA color copy, depth/stencil
  copy, query copy barriers, CMask, and FMask behavior.

Acceptance Gates
----------------

Build gates:
   ``make audit``, Meson configure, warm build, pump build, no-rebuild install,
   ``make artifact-check``, and ``vulkaninfo --summary`` showing
   ``AMD R8xx Palm (Terakan)``.

Evidence gates:
   Registry verifier, source provenance verifier, NotSupported classifier,
   result manifest hash check, and ``git diff --check``.

Runtime gates:
   No-submit probe first, then one CTS case, then bounded shard, then broad
   shard.  Stop on crash, timeout, GPU hang, unexpected ``NotSupported``, or
   first new ``Fail``.

Review gates:
   Warnings as errors, no raw IPs, no root repo work, no undocumented
   compromises, and a WHY/WHAT/HOW note in docs or PR text.
