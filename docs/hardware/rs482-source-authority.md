# RS482 cross-repository source authority and claim ledger

This document defines how the RS480/RS482/RS485 work is divided between
`mesa-26-gororoba`, `radeon-custom`, and `steinmarder-r300`, and how a claim is
resolved when those repositories disagree. It is a routing and evidence
contract, not a replacement for the owning source.

The scope is the R3xx/RS4xx lane used by the Dell Vostro 1000 target. PALM,
Wrestler, R6xx/Evergreen, Terakan, and later Radeon generations are separate
hardware lanes. Similar register names or optimization ideas do not transfer a
silicon verdict across that boundary.

## Ownership matrix

| Claim class | Source of record | What belongs there |
| --- | --- | --- |
| Executable Mesa userspace behavior | `mesa-26-gororoba` | `src/gallium/drivers/r300/`, `src/amd/r300/vulkan/r3v/`, build definitions, unit tests, and userspace-facing status text |
| Radeon kernel mechanism and package contents | `radeon-custom` | ordered patch series, generated/vendored source, DKMS metadata, package dependencies, build/install tooling, and safe defaults |
| Retained hardware evidence and falsification | `steinmarder-r300` | probes, manifests, logs, result bundles, findings, hazard policy, and the current verdict assigned to a hardware run |
| Board-specific non-Radeon platform support | `vostro1000-re` | SB600 watchdog plumbing, EC/hwmon work, and machine recovery integration; this is outside the three-repository code merge |

A repository may summarize another repository's state, but it must name the
owner and link to the owning path. A summary never becomes the source of record
merely because it is newer or more prominent.

## Claim types are not interchangeable

Every cross-repository statement must first be classified as one of these:

| Status | Meaning |
| --- | --- |
| `proposed` | design or hypothesis; no implementation claim |
| `implemented` | code exists in the owning tree |
| `compile-verified` | the relevant target compiled; no installation or silicon claim |
| `installed` | the artifact was loaded or deployed; the path may still be unexercised |
| `hardware-run` | the path executed on named silicon with retained evidence |
| `hardware-pass` | the named acceptance property was achieved on hardware |
| `partial` | the path executed, but only part of the acceptance property was achieved |
| `refuted` | retained evidence disproved the stated hypothesis |
| `unverified` | implementation or installation exists, but the required hardware verdict is still open |
| `nonconformant` | deliberately does not satisfy the advertised API's full conformance contract |

The acceptance property must accompany `pass`. “Host survived,” “GPU resumed
accelerated work,” “display scanout recovered,” and “a SIGBUS isolation gate
fired” are four different properties; success in one does not imply success in
another.

## Conflict-resolution procedure

1. Classify the claim: executable behavior, package mechanism, hardware verdict,
   or derived interpretation.
2. Consult the owning repository from the matrix above.
3. Within that claim class, prefer retained evidence in this order:
   silicon run with manifest and logs; public specification/register material;
   kernel behavior/source; Mesa implementation; focused tests; prose.
4. Preserve a contradiction until the higher-ranked source actually resolves
   it. Do not average incompatible claims and do not promote an unrun patch to a
   hardware result.
5. Record both the positive result and its falsifier or remaining open boundary.
6. Keep generation-specific evidence firewalled. An Evergreen/PALM result may
   motivate an RS482 experiment, but it cannot validate an R300 mechanism.

For package membership and patch order, the live `radeon-custom` manifest is
more authoritative than an evidence note. For whether that mechanism worked on
RS482 silicon, the retained `steinmarder-r300` result and verdict are more
authoritative than package comments. For what r3v executes today, the Mesa
classifier, synthesis, and replay code are more authoritative than a README.

## Reconciled status on 2026-07-10

### Radeon kernel lane

- `radeon-custom` is the sole active build source for the unified out-of-tree
  `radeon` module. `packaging/arch/radeon-unified-dkms/dkms.conf` is the ordered
  patch manifest. The older `radeon-rs480-safe-regs-dkms` and
  `radeon-palm-gate-dkms` package names are migration history: the unified
  package provides/replaces them rather than consuming them as independent
  active sources.
- `radeon-unified-dkms` 0.3-83 is recorded as installed with the BASELINE reset
  mask. The 0063-0068 experimental reset-mask candidates are implemented,
  compile-verified, and installed, but no non-baseline candidate has been fired.
- Fire 28 establishes the RAD-05i **host-survival containment** property after a
  failed reset and client thaw/close. It does not establish GPU recovery,
  display recovery, or execution of the SIGBUS fault gate.
- The GA-rooted wedge remains. GPU recovery and display recovery are not
  achieved; the GPU remains parked and a reboot is required for display use.
- `radeon.lockup_timeout=0` therefore remains the safe default. Patch presence,
  build success, and host containment do not justify automatic reset enablement.
- The SB600 watchdog driver remains useful as platform substrate and for the
  fired-latch fix, but active watchdog feeding is retired as a RAD-05 dead-man
  fuse: retained calibration shows the reset event is not deferred by
  `WDIOC_SETTIMEOUT`, `WDIOC_KEEPALIVE`, or magic close. Hazard runs rely on
  explicit preflight, off-box capture, and manual recovery rather than a claimed
  deferrable fuse.

### Mesa/r3v userspace lane

- r3v exposes one queue family. By default it advertises graphics and transfer
  only. Exact opt-in `R3V_HYBRID_COMPUTE_EXPERIMENTAL=1` adds
  `VK_QUEUE_COMPUTE_BIT` to that same queue; there is no native or separate
  compute engine.
- The opt-in path implements a restricted compute-as-raster experiment. The NIR
  classifier admits bounded shapes, pipeline creation synthesizes graphics
  state/shaders, and dispatch replay maps those shapes to fragment, ROP, blend,
  depth/stencil, query, and host-staged orchestration paths. Unknown or rejected
  shapes remain defined no-ops with diagnostics.
- Neither default mode nor the hybrid-compute mode is Vulkan conformant. The
  hybrid gate does not imply general compute semantics, shared memory, arbitrary
  atomics, or CTS coverage.
- The Gallium-mediated replay backend is the implemented execution path. The
  direct command-stream backend remains an explicit hazard-gated gap and falls
  back to Gallium after reporting that gap.
- The route-membership predicate, shader synthesis, and replay dispatcher are a
  three-part executable contract. `r3v_compute_verb_reachability_test.py`
  verifies that their verb sets remain identical.

## Canonical paths

### `mesa-26-gororoba`

- queue exposure and Vulkan properties:
  `src/amd/r300/vulkan/r3v/r3v_physical_device.c`
- compute admission and detector definitions:
  `src/gallium/drivers/r300/r300_compute_admission.[ch]`
- r3v route membership and pipeline state:
  `src/amd/r300/vulkan/r3v/r3v_pipeline.h`
- classifier integration and shader synthesis:
  `src/amd/r300/vulkan/r3v/r3v_pipeline.c`
- dispatch replay:
  `src/amd/r300/vulkan/r3v/r3v_queue.c` and
  `src/amd/r300/vulkan/r3v/r3v_identity_map.c`
- user-facing status:
  `src/amd/r300/vulkan/r3v/README.md`

### `radeon-custom`

- package source of record:
  `packaging/arch/radeon-unified-dkms/`
- ordered patch list:
  `packaging/arch/radeon-unified-dkms/dkms.conf`
- RS480 reset and containment patches:
  `patches/rs480/`
- package migration/provenance:
  `MIGRATION.md`
- hazardous-run package and preflight:
  `packaging/arch/rs480-reset-hazard-stack/`

### `steinmarder-r300`

- current reset/containment verdict table:
  `src/re/r300/findings/rs480-reset-recovery-patch-status-table.md`
- retained result bundles:
  `src/re/r300/results/`
- probes and run tooling:
  `src/re/r300/probes/` and `src/re/r300/scripts/`
- hardware stop-lines:
  `src/re/r300/hazard_policy.json`

## Drift-prevention rules

- Stable cross-repository documents name owner paths and status classes; they do
  not copy volatile patch lists, detector inventories, or local workspace paths.
- A new r3v compute verb must update admission, synthesis, dispatch, teardown,
  and the route-membership predicate in one change. The reachability test is the
  mechanical backstop, not a substitute for hardware validation.
- A kernel patch may be called `implemented`, `compile-verified`, or `installed`
  from package evidence. Only a retained RS482 run may promote it to
  `hardware-run`, `hardware-pass`, `partial`, or `refuted`.
- Findings retain superseded interpretations when needed for provenance, but
  their front matter or status section must point to the current verdict.
- Cross-generation analogies must be labeled as hypotheses and routed to a new
  target-specific probe before they become claims.
