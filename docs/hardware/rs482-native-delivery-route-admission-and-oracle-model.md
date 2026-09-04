# RS485M native delivery route admission and oracle model

This document is the home for how a vertex-delivery route in the native R3V lane
earns admission and how a delivered result is decided. It carries the route
topology as the source establishes it, the admission calculus and the soundness
argument behind its stream-equality predicate, the host-coherency model both
delivery directions rest on, the structure of the oracles that produce a
verdict, the kernel-validation boundary, the evidence ladder that separates the
classes of result, and the artifact profile of the lane. Every constant it cites
is derived elsewhere and named here.

## Scope and companion documents

The native lane is `src/amd/r300/`: the CPU vertex job (`cpu/`), the NIR front
end that produces one (`compiler/r300_vertex_job_nir.c`), the emitters and route
resolver (`common/`), and the Vulkan driver that drives them
(`vulkan/r3v_native_*.c`). It builds with no Gallium include root.

Four documents own facts this one uses by name:

- `rs482-hybrid-vertex-tcl-design.md` owns the VAP register table, the wedge
  taxonomy, and the hybrid architecture. It is the authority for
  `num_vert_fpus = 0` and the PVS-port read-wedge asymmetry.
- `rs482-producer-alu-compaction-design.md` owns the algebraic-compaction pass
  design and the FP24 bound correction.
- `rs482-r2vb-producer-plan-evidence-architecture.md` owns the Gallium R2VB
  campaign's evidence contract and coefficient derivations, including the
  `2^17` window and the `B <= 128` inclusive versus `127` strict thresholds.
- `r3v-implementation-boundaries.md` owns the ownership boundary, the landed and
  open surfaces, and the completion criteria.

The attended procedures (`r3v-native-attended-*-procedure.md`) own their run
recipes, their run digests, and their retained-bundle shapes.

Two R2VB engines exist in this tree and their ceilings are not interchangeable.
The Gallium engine in `src/gallium/drivers/r300/` carries the slot-grid fold
measured exact through 2049 records. The native producer pass in
`src/amd/r300/common/r300_r2vb_producer_pass.c` carries
`R300_R2VB_PRODUCER_MAX_COUNT = 1024` as its admission cap. A number quoted from
one engine says nothing about the other.

## The substituted-function problem

RS485M exposes a 3D core whose vertex engine is unusable for the transform: the
chip reports `num_vert_fpus = 0`, the PVS ports at `0x2200` and above wedge on
read, and HW-TCL first draws hang. The VAP still assembles and output-format-maps
pre-transformed vertices under `R300_VAP_TCL_BYPASS`, so the transform moves off
the vertex stage and its result arrives as bytes in a carrier the VAP fetches.

That makes vertex transform a substituted function with two implementations:

- the CPU route, where `r300_cpu_vertex_job_execute` interprets a vertex job on
  the host and writes the carrier directly;
- the GPU producer route, where a fragment program computes the transform and
  RB3D writes the carrier as a render target, the R2VB shape.  It has two
  producer forms: the immediate producer embeds the admitted records as
  `3D_DRAW_IMMD_2` body dwords (the silicon-delivered form), and the fetched
  producer (`common/r300_r2vb_fetched_producer.c`) reads the application's
  vertex BO and a driver-owned slot-position BO through the two-array
  `LOAD_VBPNTR` + `DRAW_VBUF_2` body, with the `(z, y, x, w)` reordering moved
  from the embedded record into the source element's PSC swizzle so every US,
  RS, and RB register value of the qualified immediate pass stays identical.

Both deliver into the same carrier and the same consumer draw reads it. The
engineering problem is therefore route equivalence rather than route correctness
in isolation: the second implementation is admissible only against the first,
and the first is admissible only against a declared arithmetic semantics.

## Route topology

`r3v_delivery_route_resolve` takes the three cached gate values and the source
format and returns one decision: the CPU route by default, the R2VB host model
under `R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL=1`, the immediate GPU producer
with `R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL=1` added, and the fetched GPU
producer with `R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL=1` added to both;
the fetched F32_4 route carries the silicon identity `597b762d...` (547
dwords, split 316, four relocations), pinned in
`common/tests/r300_fetched_route_digests.h` and delivered on RS485M as the
steinmarder-r300 bundle
`r3v-native-fetched-gpu-producer-route-first-delivery-rs482` (carrier
read-back equal to the delivery identity, target equal to the analytic
triangle, empty dmesg delta); the third gate keeps the qualified immediate
route reachable beside it. The device caches each gate at creation as the
literal `"1"` or closed, so the route cannot drift mid-process, and a harness
that varies a gate on one device calls
`r3v_native_device_refresh_delivery_gates` to re-run the same creation-time read.

The submit path composes as one sequence. `cflow --omit-arguments --depth=5
--main=r3v_native_queue_submit` over `r3v_native_queue.c`, `r3v_native_cell.c`,
and `r3v_native_arming.c` resolves it to six stages:

1. prerequisite sync wait, and refusal of any command buffer whose recording
   result is poisoned;
2. deferred copies and the deferred draw, where the route resolves and either
   `r300_cpu_vertex_job_execute` fills the carrier or
   `r3v_native_deferred_draw_admit_gpu_producer` composes the producer prefix
   ahead of the recorded consumer in one IB;
3. relocation-list build and cache publication over every referenced memory that
   holds a live host mapping;
4. evidence emission, both the semantic manifest and the submit object, each
   published through `mkstemp`, `fchmod`, `write`, `fsync`, `close`, `link`, and
   a directory `fsync`, so the record survives a wedge that kills the process;
5. arming evaluation over collected facts and a one-shot disarm;
6. `radeon_drm_vk_cs_submit`, the bounded completion wait,
   `r3v_native_deferred_draw_verify_gpu_producer`, status finalization, and
   sync signaling.

The two routes differ only inside stage 2 and stage 6. Stage 6 runs the
read-back verdict for the GPU route alone, because the CPU route's carrier bytes
never left the host.

## The admission calculus

The GPU producer route reaches an application-shaped `vkCmdDraw` plus
`vkQueueSubmit` behind an exact double opt-in, and three predicates decide the
admission:

- Structural: the vertex job is the identity job over the F32_4 position stream
  on the recorded triangle consumer.
- Transport: `r300_r2vb_producer_pass_semantic_equal` proves the composed pass
  dword-identical to the silicon-qualified reference emission outside the
  embedded per-vertex record payloads, located from the publication tail's fixed
  length.
- Numeric: the emitter refuses with `-EDOM` any record outside the FP24
  fixed-point domain, and the gathered records are retained as the
  post-completion read-back oracle.

The arming gate binds the composed IB's BLAKE3 digest. The producer embeds the
application's gathered records as literal `DRAW_IMMD_2` body dwords, so the
digest is a function of the vertex payload: one authorized payload per arming.

### Why the located payload window is interpretation-inert

The transport predicate admits a stream that differs from a qualified reference
inside a window `W` of dwords. That is sound only when no dword in `W` can change
how any dword outside `W` is interpreted, and the emission satisfies that for two
reasons the code enforces rather than assumes.

The extent of `W` is set by the `PACKET3` header's count field, and that header
lies outside `W` and is compared. A payload of a different length therefore
changes a compared dword before it changes an uncompared one, so `W` cannot
silently grow.

No dword inside `W` is a relocation site.
`r300_r2vb_public_route_validate_reloc_sites` pins the site sequence
CARRIER, COLOR, CARRIER by slot, requires each site to be preceded by its
relocation NOP header so an ordinary dword equal to a payload does not qualify,
requires strictly increasing indices, and places site 0 inside the producer half
and site 1 past the split. The array bound is a `static_assert` against
`R300_R2VB_PUBLIC_ROUTE_MAX_RELOC_SITES`.

The falsifier is direct: a payload dword that is itself a register-write target
or a relocation site breaks inertness, and the validator is the check that would
catch it.

## Host coherency model

The RS480-family GART runs with request snooping disabled: `rs400_gart_enable`
programs `RS480_AGP_MODE_CNTL` with `REQ_TYPE_SNOOP_DIS`, and `radeon_bo_create`
strips `RADEON_GEM_GTT_WC` and `RADEON_GEM_GTT_UC` on every non-PCIE device, so
a GTT mapping is always `ttm_cached`. The driver therefore keeps the
`HOST_COHERENT` promise itself, in both directions, and `CLFLUSH` serves both
because it writes back and invalidates.

The model has exactly two obligations and one witness apiece.

Host writes publish before the ioctl. `radeon_drm_vk_cs_submit` issues a
sequentially consistent fence and snapshots `cache_sync_count` into
`submit_boundary_sync_count` immediately before `DRM_RADEON_CS`, and the
triangle-cell harness compares that boundary count against the post-completion
count as separate quantities.

Device writes invalidate before the host reads them. The public path carries
this at `r3v_MapMemory`, which invalidates on every fresh mapping because a new
mapping aliases cache lines from an earlier map window while the GPU wrote the
pages past the cache. The submit path carries it after completion over every
referenced memory that holds a live mapping.

The rule those two sites share is the general one: **every host read of device
output invalidates over the mapping it is about to read through**. The
live-mapping condition on the post-completion loop is a necessary
implementation detail, not the rule, and a BO with no live mapping at that moment
is outside the loop's reach rather than outside the rule's. The GPU-producer
carrier is exactly such a BO, and its read-back now invalidates its own read
extent through the BO-aware form, which names the carrier handle in the
host-model event record so a harness can witness the flush by handle.

## Oracle structure and independence

Two oracles decide a public GPU-producer delivery:

- the driver's carrier read-back, a `memcmp` of the mapped carrier against the
  records the CPU gathered before submission, with the observed and expected
  bytes retained beside each other on agreement as well as on divergence;
- the color target, compared against the analytic triangle by the attended
  runner through `vkMapMemory`.

They are not independent. The consumer draw fetches its vertices from the
carrier, so the color target is downstream of the carrier: a correct carrier and
a correct consumer produce a correct color, and a wrong carrier produces a wrong
color. Agreement between them is one confirmation observed twice along one
chain, not two confirmations.

What makes the verdict sound is a different property. Each oracle is seeded
before submission with a value the correct path cannot manufacture: the carrier
is filled with `R300_R2VB_PRODUCER_POISON_DWORD` (`0xdeadbeef`) and the render
pass clears to `R300_TRIANGLE_COLOR_SENTINEL` (`0xa5a5a5a5`), which the target
oracle also reads as its exterior and canary value. The common-mode failure both
oracles are exposed to is a stale host mapping that returns pre-submission bytes,
and against that mode each oracle fails closed on its own: a stale read returns
the seed, the seed is not the expected value, and the verdict is divergence.
A stale read cannot fabricate agreement.

The evidentiary strength therefore comes from pre-seeded non-producible values,
not from oracle orthogonality. The distinction matters because the repository's
own proof-backed failure modes name the trap directly:
`moreno29_orthogonal_iff` states that three orthogonal nulls construct a zero
divisor, so independent-looking local passes can compose into a false global
conclusion, and `cd_fidelity_stability` states that the Lipschitz bound holds
only for orthogonal sources, so correlated sources amplify uncertainty
non-linearly. Reporting a serially dependent pair as two independent oracles
claims exactly the bound those results deny.

The verdict does not weaken; its justification changes. The correct claim
language is two fail-closed oracles over one delivery chain, each armed by a
non-producible pre-seed. The read-back's positive reading became authoritative
rather than accidental only once its invalidate landed, because before that the
freshness of its bytes rested on whatever eviction the surrounding work
happened to cause.

## Kernel validation boundary

`r100_cs_track_check`'s vertex-array bound is offset-blind: it checks
`esize * max_indx * 4` against the BO size and does not account for the fetch
offset. A widened fetch over a narrower vertex object therefore passes kernel
validation whenever the BO is large enough, which the FLOAT_2 tuple object at 72
bytes demonstrates against the widened FLOAT_4 model fetch. The userspace
emitter's 64-bit last-byte refusal is the enforcing layer for the fetch tail.

The kernel bound is necessary and not sufficient, so a replay that reports
`verdict=ACCEPT` proves the stream is admissible, not that the fetch stays inside
its intended element window.

## Evidence ladder

Each rung answers a different question and carries a different evidence class.
A result from one rung does not substitute for a result from a higher one.

| Rung | Instrument | Decides | Cannot decide |
| --- | --- | --- | --- |
| Offline composition | `r300_r2vb_public_route_compose` plus the BLAKE3 digest | the exact dwords an arming authorizes | whether the kernel accepts them |
| Kernel replay | the CS-tracker replay tool over the composed stream | that the radeon CS validator accepts the stream at its own arithmetic bounds | whether the silicon executes it as intended |
| Host model | the drm-shim harnesses, closed-gate and open-gate | driver state transitions, refusal shapes, host-model cache events | anything about the GPU |
| Arming runner | the non-submitting runner | the digest, the route split, and every arming factor without opening a device | delivery |
| Attended run | the runner on RS485M with a retained bundle | delivery, by both oracles over the chain above | a population, from one payload |

The public GPU-producer route's composed stream replays at `dwords=547
relocs=2 draws=2 passed=2 verdict=ACCEPT`, and the truncated-packet,
undersized-carrier, and undersized-color arms each reject. The depth-control
workload replays at 245 dwords and 3 relocations with ten controls at the
kernel's own arithmetic bounds.

## Numeric domain

The RS485M shader float is `s1e7m16`: 1 sign, 7 exponent, 16 stored mantissa bits,
a 17-bit significand with the implicit leading bit. The exact-integer window is
`2^17`, and the DP4-chain bound is `8*B^2 <= 2^17`, giving `B <= 128` inclusive
with `127` as the strict production gate. Those thresholds are derived in
`rs482-r2vb-producer-plan-evidence-architecture.md` and proven in open_gororoba:
`IDCT8DP4ExactBound.v` `dp8_exact_threshold` and `fp24_admit_strict_spec` for the
integer window, `FP24Representable.v` `fp24_int_exact_inclusive` for FLX(17)
representability, and `R2VBTransformDP4.v` `mvp4_rows_exact` for the transform,
all with zero admits.

The CPU route's arithmetic is declared, not inherited. The scalar interpreter is
the authority and the SSE2 and SSE3 kernels are differential implementations
required to be byte-identical over randomized jobs across hostile bit patterns.
Four properties carry that contract: FMAD commits its product to binary32 before
the add, so it is two roundings rather than a fused operator; FFMA is one
rounding through `fmaf`; DP4 sums in component order seeded by the first product,
so an all-negative-zero dot keeps its sign; and arithmetic NaNs canonicalize
identically on both lanes while finites, infinities, denormals, and signed zeros
pass through verbatim. The float environment is calibrated before use and
restored on every exit path.

Those semantics are compiler-observable, and one of them was compiler-dependent.
The scalar DP4 accumulator written as `sum += a * b` is a contraction candidate,
so a target with a fused operator rounds the pair once. Measured on the dot
product whose lane 0 seeds `-1` and whose lane 1 contributes `(1 + 2^-12)^2`,
where the declared result is `2^-11`:

| Compiler | `-march=x86-64` | `-march=x86-64-v3` |
| --- | --- | --- |
| clang 22.1.8, `-O2 -std=c11` | `0x3a000000` | `0x3a000400` |
| gcc 16.2.1, `-O2 -std=c11` | `0x3a000000` | `0x3a000000` |

Every shipped r300 profile pins `-march=x86-64` or `-march=btver1`, so the
divergence was latent rather than live, and the accumulator now commits each
product through a volatile object, which is the defense FMAD already carried.
The result generalizes: a declared arithmetic semantics in C is a property of
the source plus the target baseline plus the compiler, and pinning it takes a
construct the optimizer cannot cross rather than a comment.

## Artifact profile

Measured with `scc` over `src/amd/r300/` at the recorded commit:

| Subtree | Files | Lines | Code | Comment |
| --- | --- | --- | --- | --- |
| `vulkan/` | 147 | 72,235 | 56,642 | 10,163 |
| `compiler/` | 103 | 35,659 | 26,884 | 4,897 |
| `common/` | 87 | 23,857 | 16,772 | 4,829 |
| `cpu/` | 10 | 3,642 | 2,648 | 747 |
| whole tree | 347 | 135,393 | 102,946 | 20,636 |

The `*/tests/` subtrees hold 45,042 of those 102,946 code lines, so verification
carries 0.78 lines for every line of driver, and comments carry one line for
every five of code.

`lizard` over the five spine files reports 88 functions at mean cyclomatic
complexity 8.7, with 17 above the warning threshold. The three largest are
`r3v_native_queue_submit` (320 NLOC, CCN 69), `cell_geometry_unfrozen`
(141 NLOC, CCN 63), and `r3v_native_cmd_buffer_execute_deferred_draw`
(140 NLOC, CCN 36). Each is an ordered emission or predicate matrix whose
invariant is visible only in one place, which is the shape the driver style
admits for a long function; each remains locally auditable because every exit
path is checkable in sequence.

## Threats to validity

- Host-model results are not silicon results. The drm-shim harnesses reach no
  ioctl and observe no GPU; they decide driver state transitions and refusal
  shapes only.
- The arming binds one payload. A delivery observed under one authorized vertex
  payload is one point, and the route is payload-specific by construction.
- The two oracles are serially dependent. Their agreement is one confirmation
  along one chain, sound because each carries a non-producible pre-seed.
- The vertex-job benchmark refuses to call its rows dispatch evidence on any host
  other than the qualified Turion 64 X2 TL-66, so executor cost measured on a
  development workstation is smoke output.
- The GPU-producer admission's mid-composition fault path needs an allocation
  failure inside `vkQueueSubmit`, and no harness carries an injection hook for
  it, so that leg is not run.
- `compiler/r300_vertex_job_nir.c` has no dedicated unit test; it is exercised
  only through `r3v-native-pipeline-frontend`.
- `docs/hardware/tests/test_rs482_stack_manifest_schema.py` is not registered in
  any `meson.build` and runs by hand.

## Reproduction

Build and run the registered suites from a clean isolated worktree:

```sh
repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root/build-infra"
make configure build test \
  PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache \
  HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc \
  COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba
```

`make build` puts the build directory at `$repo_root/build/mesa-$PROFILE`. The
two harnesses that witness the host-coherency and prepared-transport rules run
from there:

```sh
builddir="$repo_root/build/mesa-4_r300_full_release_x86_64v1-clang22-distcc-cache"
meson test -C "$builddir" --suite r3v r3v-native-public-surface
meson test -C "$builddir" --suite r3v r3v-native-burst-cell-admission
```

Each was calibrated by removing the mechanism it witnesses, rebuilding that
target alone, and observing the failure, then restoring it. Reproducing a
calibration repeats those steps: delete the named call, run
`ninja -C "$builddir" <harness target>`, run the test above and see it fail,
then restore the call and see it pass. The commit that introduces each witness
records which call it removed.

The kernel replays register as `r300-r2vb-public-route-replay`,
`r300-r2vb-fetched-route-replay-{f32_4,f32_3,f32_2}` (the composed fetched
route parses as two draws over four relocation entries; its known-bad arms pin
the parser's offset-blind vertex-array bound at stride times count - 1 for
the slot and source arrays and reject a source relocation past the chunk
table), and `r300-zb-depth-control-replay`, and skip with exit status 77 when
`R3V_CS_TRACK_REPLAY_TOOL` is unset. Attended silicon runs follow their own
procedure documents, which own the gate spellings, the preflight, the rollback,
and the retained-record shape.

Reproduce the call-graph capture in this document with:

```sh
cflow --omit-arguments --depth=5 --main=r3v_native_queue_submit \
  src/amd/r300/vulkan/r3v_native_queue.c \
  src/amd/r300/vulkan/r3v_native_cell.c \
  src/amd/r300/vulkan/r3v_native_arming.c
```

## Research frontier

Each item names what it would take to close and what would falsify the current
model.

- Route default. The CPU route is the default and both routes now time through
  one declared-route cell on one workload. Closing needs paired timings on RS485M
  under the same payload; a GPU route slower than the CPU route on the delivered
  workload refutes the premise that the substitution buys throughput.
- Oracle independence. A second oracle that is genuinely orthogonal to the
  carrier read-back would have to observe the delivery without reading through
  the carrier or its consumer. A GPU-side counter or a second consumer reading
  the carrier through a different fetch path are the candidates; until one
  exists, the fail-closed pre-seed argument is what carries the verdict.
- Payload generality. The route admits one payload per arming. A payload-agnostic
  admission needs a transport predicate over a payload class rather than over one
  located window, and the inertness argument above is the thing that would have
  to generalize.
- Source-format migration. F32_3 and F32_2 public routes remain open, and the
  FLOAT_4 model fetch already pins a kernel fact the widened width exposed.
  The fetched producer emits all three widths and pins their composed
  digests, the resolver opens the fetched route per width under the
  three exact gates, and each width has its retained RS485M cell
  (`r3v-native-fetched-gpu-producer-route-first-delivery-rs482`,
  `-f32-3-delivery-rs482`, `-f32-2-delivery-rs482`).
- Fetched-route closure. The fetched F32_4 route composes at submit time
  byte-identical to its offline composition (the submit-order harness's
  `gpu-fetched-composed` arm proves the digests equal through the arming
  gate), refuses atomically on an injected composition failure, replays
  through the kernel parser as `dwords=547 relocs=4 draws=2 ACCEPT`, and is
  delivered on RS485M (bundle
  `r3v-native-fetched-gpu-producer-route-first-delivery-rs482`); the
  F32_3 and F32_2 compositions pass the same harness arms
  (`gpu-fetched-composed-f32_3`, `-f32_2`) and kernel replays and are
  delivered on RS485M (bundles `-f32-3-delivery-rs482`,
  `-f32-2-delivery-rs482`, same boot and module as the F32_4 cell); the
  remaining open item is a timing cut over the fetched route against the
  measured CPU default.
- Fetch-tail enforcement. The offset-blind kernel bound leaves userspace as the
  enforcing layer. A kernel-side offset-aware bound would move that obligation;
  until then a replay `ACCEPT` is not a statement about the fetch window.
- Injection surface. The fetched route's composition boundary carries an
  injection hook (`gpu_producer_compose_inject_errno`); the immediate
  admission fault path and the read-back's stale-mapping path still lack one. A fault-injection layer over
  `radeon_drm_vk_bo_map` would make both testable and would let the coherency
  rule be witnessed negatively rather than only positively.
