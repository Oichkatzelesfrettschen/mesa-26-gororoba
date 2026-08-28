<!--
SPDX-License-Identifier: MIT
-->

# r3v -- experimental Vulkan ICD for RS480-family Radeon

## Overview

`r3v` is an experimental Vulkan installable client driver for the AMD
RS480-family integrated graphics processors:

- Radeon Xpress 200M, RS482, PCI `1002:5974`;
- Radeon Xpress 1100/1150 mobile, RS485 marketing name, PCI `1002:5975`.

The implementation is the native Radeon DRM ICD: Vulkan objects, command
records, shader admission, R300 command-stream construction, DRM submission,
and completion are owned by r3v over `src/amd/radeon/drm_vk/`; the Gallium
r300 driver shares only the API-neutral `src/amd/r300/common/` contracts.

`docs/hardware/r3v-current-program-status.md` is the one program-status
authority: repository heads, the target deployment, the active conformance
partition, the latest target receipt, the DSO and queue-claim modes, and the
open tasks. The implementation boundaries between the native implementation
and complete Vulkan semantic/conformance coverage are documented in
`docs/hardware/r3v-implementation-boundaries.md`.

The driver is classified as experimental and nonconformant, and the
classification is a queue-claim token with three fields:

- default queue: graphics, with no conformant compute claim
  (`queue_claim_mode` `default_graphics_only`);
- gated queue: the experimental CPU/GPU compute subset behind the exact
  `R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1` opt-in (`queue_claim_mode`
  `experimental_compute_subset`);
- conformance: false (`conformanceVersion` 0.0.0.0; only the
  `conformant` mode, reached when every verb ledger row executes on both
  routes, makes a receipt `compute_claim_eligible`).

`r3v_private.h` owns the canonical `R3V_CONFORMANCE_STATUS` string, and the
queue-claim receipt (`r3v_native_queue_claim_report`, run by the
conformance runner's `--queue-report`) is the authority for the mode a
given run advertised; the driver refuses when the advertised compute bit,
the verb ledger's claim, and the gate state disagree, so the static
classification here restates the receipt and never contradicts it. R3xx
silicon has no documented native compute-dispatch packet.

## Hardware target

| Field | Value | Primary source |
|---|---|---|
| Vendor | ATI / AMD | PCI vendor ID `0x1002` |
| RS482 device | `0x5974` | `include/pci_ids/r300_pci_ids.h` |
| RS485-marketed device | `0x5975` | `include/pci_ids/r300_pci_ids.h` |
| Mesa family | `CHIP_RS480` | `r300_parse_chipset()` |
| Generation | R3xx | AMD R3xx Register Reference Guide |
| Mesa-classified vertex FPUs | 0 | `r300_parse_chipset()` |
| Vulkan compute queue | behind the exact `R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1` opt-in (CPU compute route) | `r3v_native_compute.c` |
| Kernel driver | `radeon` | Linux `drivers/gpu/drm/radeon/` |
| Renderer string | `ATI RS480` | r300g `r300_get_renderer()` |

Mesa's PCI table uses the `RS482_` prefix for both device IDs. The second device
is marketed as RS485. Source paths and PCI identities control technical claims;
marketing names remain descriptive only.

The current r300g path routes RS480-family vertex-stage execution through
Gallium Draw software TCL because Mesa classifies the family with
`num_vert_fpus == 0`. This is a current driver-path fact, not proof that the
silicon can never execute a hardware vertex program. The RS, TX, US, CB, and ZB
graphics blocks remain hardware-backed.

## Execution architecture

Vulkan commands are recorded into command-buffer-owned state and executed at
queue submission over the Radeon DRM transport.

```text
Vulkan command recording (r3v_native_recording.c, r3v_native_cmd.c)
-> admitted draw or dispatch (r3v_native_draw.c, r3v_native_compute.c)
-> vertex gather through the CPU executor (src/amd/r300/cpu/) into the
   command-buffer-owned GTT carrier, one stream per attribute slot the
   job reads over its bound per-vertex or per-instance binding, linearly
   from the first vertex or through the three indices an indexed draw
   reads from the bound index buffer at execution, once per instance
   from firstInstance into the cell family's 3 * instanceCount vertex
   list (VertexIndex and InstanceIndex carry the draw's base values, the
   Vulkan semantics; a vertex job that stores
   the location-0 varying writes eight-dword records, and the draw
   records the varying triangle cell whose RS routes that second FLOAT_4
   to the pass-through fragment program), or the R2VB producer route
   under its exact opt-in (immediate producer, or the fetched producer
   reading the bound vertex BO under a further exact opt-in; both keep
   one source relocation role, so they admit the slot-0 identity job
   alone); compute kernels execute on the CPU route for the one verb the
   ledger `common/r300_compute_verb.h` marks executing (identity map),
   or on its GPU route -- the compute identity carrier, the fetched
   producer over the input and output storage buffers -- under the verb's
   exact gate with the CPU bit copy as the read-back oracle; the
   remaining verbs are precommitted rows
-> fixed-cell PM4 lowering over the common contracts
   (src/amd/r300/common/)
-> prepared submission: relocation list, completion BO, arming evaluation,
   DRM_RADEON_CS through src/amd/radeon/drm_vk/
-> finite completion (write-domain BO plus bounded GEM_WAIT_IDLE)
```

`struct r3v_native_device` owns the DRM transport, the GEM-backed
`VkDeviceMemory` objects (one BO per allocation), the descriptor, pipeline,
image, and queue objects, and the prepared submission; no Gallium screen,
context, or resource takes part.

## Native ICD status

`libvulkan_r3v.so` (manifest `r3v_icd.<cpu>.json`, `driverName` `r3v`)
owns its Radeon DRM transport through `src/amd/radeon/drm_vk/` and links no
Gallium runtime library; a separation-audit test enforces that boundary. The native library owns
GEM-backed `VkDeviceMemory` (one BO per allocation, bound to buffers and to
the admitted image family through `r3v_BindImageMemory`), a queue
whose submission path builds the three-chunk `DRM_RADEON_CS` object, and
command-carrier objects. Fragment binaries are deep-copied into R3V-owned
`r300_fragment_binary` storage with a content hash and structural validator.

Real submission sits behind a conjunction, evaluated by
`r3v_native_arming_evaluate`: the exact-value gate
`R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`, an operator-declared bundle
digest matching the BLAKE3 of the IB about to travel, the authorized
RS482 PCI identity, the declared kernel release, the declared radeon
module srcversion, and an evidence directory that exists and carries no
attempt token. Reaching the ioctl writes that token by exclusive
creation, so the directory admits one attempt, and an armed submit
carries one command buffer. Any closed factor fails closed with
`VK_ERROR_DEVICE_LOST` and names itself. The closed gate retains the semantic
cell under `R3V_NATIVE_MANIFEST_DIR`; an open gate additionally retains the
exact submit object after completion relocation is folded into the CS. Each
retained artifact is bound by digest in its manifest, and a retention failure
refuses before the ioctl.
`r3v_native_arming_runner` reports every factor and stops at the
authorization boundary without creating a device; the attended run it
precedes follows
`docs/hardware/r3v-native-attended-cell-procedure.md`.
The public draw surface in `r3v_native_draw.c` lowers the qualified
render-pass begin/end, pipeline and vertex-buffer binds, and draw into
the fixed TCL-bypass cell; `r3v_native_recording.c` fail-closes every
other core 1.0 `vkCmd*` entrypoint by poisoning the command buffer. The
native surface outside that draw decomposes as follows:

- images: bounded render and transfer families implemented
  (`r3v_native_image.c` admits the executed 2D linear family for
  color-attachment usage at the cell extent and for transfer usage to
  the transfer bound; every other `VkImageCreateInfo` shape refuses);
- descriptors: core object model implemented (layouts, pools, sets,
  buffer views in `r3v_native_compute.c` and `r3v_native_object.c`);
  the executing subsets are narrower -- the compute
  route admits set 0 storage-buffer bindings alone, and the graphics
  recording surface binds no descriptor outside the fixed cell;
- transfers: synchronous host-mapped route implemented
  (`r3v_native_transfer.c` fills, updates, and copies through the
  mapped BO at submission);
- WSI: the surface and query denominator is present
  (`docs/hardware/r3v-wsi-denominator.md`); native swapchain and
  presentation are incomplete, and `R3V_WSI_SW=1` selects the xcb-shm
  CPU-copy present path.
The drm-shim harness and offline kernel-parser replay carry the
pre-hardware evidence; the attended-cell runner has carried one armed
`DRM_RADEON_CS` submission on RS482 that the kernel accepted and retired
clean while the color target retained its sentinel fill. The cause of
that unwritten target is underdetermined -- the run retained no
predecessor register values, and
`docs/hardware/r3v-native-attended-cell-procedure.md` carries the
canonical classification -- while a later RS482 silicon matrix proved
that an unestablished `US_OUT_FMT_0`, `RB3D_COLOR_CHANNEL_MASK`, or
`SC_SCREENDOOR` each alone suppresses every color write, and the
original cell owned none of the three. The recorded
cell therefore now opens with the neutral first-draw state contract
(`src/amd/r300/common/r300_first_draw_state.c`): the contract's clauses
are emitted in pipeline order ahead of the cell, the poison-model checker
proves the stream establishes every clause itself, and the recorder,
manifest tool, and harness reference all build the one byte-identical
successor IB. The successor cell has rendered its predicted interior on
RS482 with the exterior and canary rows clean (retained bundle
`results/rs482_native_triangle_first_correct_pixel_witness_20260808T070427Z/`
in the steinmarder-r300 evidence tree;
`docs/hardware/r3v-implementation-boundaries.md` carries the
classification).

The first native hardware witness and the ordered migration path are specified
in `docs/hardware/r3v-implementation-boundaries.md`.

## Compute status

Behind the exact `R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1` opt-in the one queue
family advertises `VK_QUEUE_COMPUTE_BIT`; `r3v_CreateComputePipelines` admits
the identity-map kernel from SPIR-V words directly into the common compute
job (`src/amd/r300/common/r300_compute_spirv.c`), storage-buffer
descriptors bind on set 0, one dispatch records per command buffer, and
`r300_cpu_compute_job_execute` runs it at submission.  Every module outside
the admitted subset refuses at pipeline creation, so no admitted pipeline
reaches an unmatched no-op.  With the opt-in unset, compute pipeline creation
refuses.  The exposure remains experimental and nonconformant; widening it
means more admitted kernel shapes and a GPU raster-carrier route.

The design authority for those raster-carrier routes is
`docs/hardware/rs482-gpu-compute-substrate-atlas.md`.  It names, for every unit
from the PM4 command stream through the color backend, the arithmetic the unit
performs, its numeric domains and exactness bounds, and the evidence class
behind each statement, and it binds every row of the compute verb ledger
(`src/amd/r300/common/r300_compute_verb.c`) to the unit whose arithmetic
realizes it plus the probe that row still owes.  A GPU verb route is designed
against a named unit and a named bound there; the CPU direct-SPIR-V route stays
the oracle every GPU result is measured against.

## R2VB source-format work

r300g's live R2VB producer admits `R32G32B32_FLOAT` and
`R32G32B32A32_FLOAT` source records, routed through the neutral
vertex-format contract with a byte-identity test pin. Final delivery remains
FP32x4.

The `FLOAT_2` source transaction is gated behind the exact-value
`R300_R2VB_FLOAT2_SOURCE` experiment and captured without submit; every
production admission path holds it out of the automatic route. Its exact
source contract and next integration step live in:

`docs/hardware/r300-r2vb-float2-source-contract.md`

Narrow source fetch support never implies narrow final-delivery support.

## Repository layout

### Retired lane

A Gallium-backed Vulkan lane preceded the ICD and is deleted; the
retirement section of `docs/hardware/r3v-implementation-boundaries.md`
maps each of its former capabilities to the mechanism that carries it and
names the retained manifests, the common-IR parity test, and the rebound
fixtures that are the differential reference.

```text
src/amd/radeon/drm_vk/     Gallium-free Radeon DRM transport (BO, PRIME,
                           relocation, CS build/submit, finite completion)

src/amd/r300/
  common/                  R300-neutral format, source-contract, fragment-
                           binary, and triangle-cell vocabulary
  vulkan/                  the r3v ICD: Vulkan objects over the Radeon DRM
                           transport (r3v_native_*.c)

src/gallium/drivers/r300/
  r300_public.h            r300 extraction interface
  r300_r2vb.c              live Gallium R2VB implementation
  compiler/                NIR/RC lowering and R300 shader compiler

docs/hardware/
  r3v-implementation-boundaries.md
  r300-r2vb-float2-source-contract.md
```

Important r3v sources include:

- `r3v_instance.c`: instance and ICD entry points;
- `r3v_physical_device.c`: DRM probe, properties, queues, memory, and limits;
- `r3v_native_device.c`, `r3v_native_queue.c`: device and the prepared
  submission over the DRM transport;
- `r3v_native_memory.c`, `r3v_native_object.c`, `r3v_native_image.c`: GEM-backed
  memory and objects;
- `r3v_native_recording.c`, `r3v_native_cmd.c`, `r3v_native_draw.c`: command
  recording and the admitted draw surface;
- `r3v_native_pipeline.c`, `r3v_native_compute.c`: direct SPIR-V admission
  into the common job IR;
- `r3v_cpu_sync.c`: CPU-timeline fence implementation;
- `meson.build`: ICD and test build graph.

## Build

The ICD builds with no Gallium dependency:

```sh
meson setup builddir-r3v \
    -Dvulkan-drivers=ati_r300 \
    -Dgallium-drivers= \
    -Dopengl=false -Dgles1=false -Dgles2=false \
    -Dglx=disabled -Degl=disabled \
    -Dplatforms=x11 -Dllvm=disabled -Dlibunwind=disabled \
    -Dbuildtype=debug

ninja -C builddir-r3v src/amd/r300/vulkan/libvulkan_r3v.so
```

Adding `drm-shim` to `-Dtools` builds the render-node model the cell
harnesses preload; adding `r300` to `-Dgallium-drivers` builds the GL driver
beside it and registers the r300g-owned planner oracles.

To inspect the in-tree ICD:

```sh
export VK_DRIVER_FILES="$PWD/builddir-r3v/src/amd/r300/vulkan/r3v_devenv_icd.x86_64.json"
export R3V_DEBUG=startup
vulkaninfo --summary
```

## Validation boundaries

Build, loader, runtime, silicon, conformance, and deployment are separate
results.

At minimum, a loader/identity check records:

| Surface | Required observation |
|---|---|
| Instance | `vkCreateInstance` succeeds |
| Physical device | one supported RS480-family PCI identity is enumerated |
| Queue families | graphics and transfer only; no compute |
| Mapped ICD | the process maps the intended `libvulkan_r3v.so` |
| Kernel window | no unexplained CS validation, reset, hang, or lockup event |
| Conformance status | experimental/nonconformant classification retained |

Native direct execution requires R3V-owned memory and PM4 plus offline
parser acceptance before an attended silicon submission; every drawing,
R2VB, and compute result carries its own positive and negative controls.

The Radeon drm-shim is a host model. Kernel parser acceptance is not execution.
Fence retirement is not output correctness. An old image hash is not evidence
for the current source head.

## Conformance ladder

The Vulkan 1.0 conformance surface is finite and machine-checked.
`tests/r3v_vulkan10_requirement_inventory.py` generates the registry
inventory and closes the advertised extension dependencies at core 1.0;
`tests/r3v_advertised_surface_deqp_binding.tsv` binds every advertised
extension, granted feature bit, receipt-bound limit, and format-feature
row to the registered test that exercises its executing route and to the
exact dEQP-VK mustpass group that judges it, so a bit without a binding
row fails the build.  `apiVersion` stays 1.0 and advertises exactly what
those bindings prove; 1.1 and later are new campaigns with their own
evidence, never a string edit.

`tests/r3v_conformance_runner.py` runs a caselist against the ICD and
seals an identity receipt: source SHA against the declared SHA and tree
cleanliness, Meson options, ICD manifest and DSO digest, the dEQP
binary digest and the release name the binary writes into its own log,
kernel release and radeon module srcversion, packages, GPU PCI identity
and the render node resolving to it, boot id, the driver-visible
environment, the case list, per-case status, dmesg delta, the deadline,
digests of every raw artifact, and the finite verdict; `verify-receipt`
recomputes the seal and the artifact digests.  The evidence class is
derived from the run (a preloaded drm-shim is host-model; only an
unpreloaded run of libvulkan_r3v.so on a host whose render node resolves
to an RS4xx device is silicon), a run is valid for qualification only with a
clean tree at the declared SHA, NotSupported never counts as a pass, a
process the deadline kills is never a pass, and every non-pass
classifies against `tests/r3v_conformance_nonpass_ledger.tsv`,
most-specific row first over status, case name, and result text; a
status the ledger does not name blocks the run as unclassified, and
each row carries a witness case that `check-ledgers` classifies through
the real path so no row is unreachable.  The runner calibrates on
fake-dEQP fixtures (pass, mixed, truncated, timeout, hang after session,
crash, device loss with a terminated case, multi-line result, late
abort, framework abort, wrong ICD, dmesg hazard, duplicate caselist,
tampered receipt) and on a replay of a real dEQP log.

`tests/r3v_conformance_slices.tsv` orders the mustpass groups by hazard:
a hazard-free slice runs on the host model or on silicon, and a slice
carrying a submission or display hazard requires silicon.  dEQP's
default context requires a GRAPHICS|COMPUTE queue and aborts on the
graphics-only family; the operator passes the exact compute-queue opt-in
through `--env` and the receipt records it.

The runner's evidence boundary: the dEQP process inherits an
allowlisted environment (PATH, HOME, user, locale, terminal) plus the
values declared through `--env` (the drm-shim preload, its library
path, and the compute-queue framework gate are declared values), and
the receipt records that whole environment; a declared submission,
authorization, experimental-route, plan, or evidence-directory value
is admitted on a submission-hazard slice alone and refuses elsewhere
as `gate_contamination`, with `check-ledgers` holding the pattern to
every compute verb gate the ledger names; a run takes a fresh output
directory and a caselist inside the shard ceiling; qualification validity
requires the declared source SHA, DSO, dEQP, caselist, and partition
digests, the runtime event digest on silicon, the queue-claim report,
and a caselist bound to a partition shard; the kernel log delta comes
from a journal cursor when journalctl serves the kernel log, or from
dmesg with the before stream held as a prefix of the after stream
(`kernel_log_continuity_broken` otherwise); the runner writes dEQP's
stdout and stderr to files and kills the process group when neither
the log (dEQP flushes it after every write) nor stdout grows within
`--case-timeout` (`case_deadline`, ranked below a kernel hazard and
folded into `runner_deadline` after the session closed) or the shard
passes `--timeout`; the receipt keeps the exit code that named the
deadline; a runner `--max-cases` other than the manifest's shard
ceiling refuses (`shard_ceiling_mismatch`); `--runtime-event` joins the
target-side capture by digest and retains it beside the receipt.  The
declared caselist digest is the shard file's bytes, the value the
partition manifest publishes per shard and `sha256sum` prints; a
classified-nonpass verdict exits nonzero, so a strace witness wrapped
around the runner (`r3v_cs_ioctl_trace.py trace --allow-tracee-failure`)
declares that exit as expected or refuses every shard carrying a fail;
and `dEQP-VK.api.device_init.create_instance_device_intentional_alloc_fail.basic`
injects one allocation failure per allocation index of instance and
device creation, 604 s on the RS482 host's K8 against the real device,
so the `api-version-init` slice runs under a case ceiling above that
(1800 s) while the other hazard-free slices finish every case inside
120 s.  The receipt records declared values verbatim and inherited values as
digests, so the operator's paths stay out of a retained receipt; a
display-slice receipt therefore names `R3V_WSI_SW=1`, the xcb-shm
CPU-copy present path, whenever that value was declared.  `--queue-report` runs
`r3v_native_queue_claim_report` through the Vulkan loader under the
run's environment with the pinned `VK_DRIVER_FILES` and records the
queue flags the ICD advertises there, the claim mode behind the
compute bit (`default_graphics_only`, `experimental_compute_subset`
under the exact framework gate over the delivered CPU route, or
`conformant` once every verb ledger row executes on both routes), the
digest of every field of the verb ledger the report was built from,
and the report binary's digest; the report refuses when the advertised
bit, the ledger's claim, and the gate state disagree.  Only the
conformant mode makes a receipt `compute_claim_eligible`, a statement
about the compute queue alone, so a gate-assisted run reads as its
mode, and qualification validity requires the report.

A submission-bearing slice runs under a plan (`R3V_NATIVE_PLAN_FILE`
and `R3V_NATIVE_PLAN_NONCE`): the planning pass under the drm-shim
captures the shard's ordered submissions, `r3v_native_plan_tool
compose` seals them with the run identities, and the device replays
the plan alone, binding at the first submission to the DSO digest, the
built source SHA prefix, the kernel and module identity, the RS482 PCI
identity, the nonce, an empty evidence directory, and closed gates,
admitting each submission's whole entry before any device-visible
effect, holding the IB at the ioctl boundary to the admitted digest,
retaining IBs content-addressed with a hash chain, and recording the
session's bound, terminal, incomplete, or complete state.  The driver
binds the binary; the clean-tree-at-declared-SHA proof lives outside
it, in the runner receipt's source identity and the qualification
worktree record, and the plan's dEQP, partition, and caselist digests
are the runner's declarations verified against that receipt.

A planning pass captures transcripts on the host model and proves
nothing about conformance.  The runner admits it as the host-planning
disposition, evidence class `host-planning` with `qualification_valid` false,
on exact conditions alone: the slice hazard is `submission`, the radeon
drm-shim `libradeon_noop_drm_shim.so` is in the preload path so it
interposes ioctl (the same basename the driver's capture admission
resolves the ioctl symbol to), `R3V_NATIVE_PLAN_CAPTURE_FILE` is
declared, `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED` is closed (unset, empty,
or `0`), no attended evidence directory (`R3V_NATIVE_MANIFEST_DIR`) and
no replay plan (`R3V_NATIVE_PLAN_FILE`, `R3V_NATIVE_PLAN_NONCE`) is
declared, the shard runs `--process-per-case`, and the per-process
strace witness (`--strace-binary`, or the strace `r3v_cs_ioctl_trace`
proves can attach) counts zero kernel-entering `DRM_IOCTL_RADEON_CS`
over the shard with every case traced and every line parsed.  A failed
condition refuses the run by name (`planning_disposition_refused` with
the condition listed under `planning.refused_conditions`,
`planning_witness_unavailable`, `planning_cs_witnessed`,
`planning_unwitnessed`), and the evidence class then stays
`host-model`.  The sealed receipt's `planning` object records the
conditions, the tracer, the shard's ioctl census, each case's outcome
(`transcript` with the digest of every transcript the device wrote at
the declared path and its `.N` ordinals; `no_nonempty_ib` when the
device logged that no executable submission ran and wrote nothing;
`unresolved` otherwise), and each case's `ioctl.strace` among the
digested artifacts.  The disposition changes nothing about the slice:
its required evidence stays silicon, and the receipt's verdict keeps
its ordinary vocabulary while standing as a statement about capture
alone.  The same declaration on a silicon run refuses as
`capture_on_silicon`, since a capture session opens the CS ioctl with
the hazard gate closed and only the host model answers it, and on a
hazard-free slice it stays gate contamination.  dEQP runs a
whole caselist in one process and creates devices freely -- one dEQP
case may create several, ahead of the one it drives (its own robustness
cases do this) -- so capture assigns each device in a process its own
ordinal: the first lands at the declared path and the Nth (N >= 2)
lands at `<path>.N-1`, and a device that never submits writes no
transcript at all.  Replay still binds one session to one evidence
directory, so a capturing or replaying shard runs `--process-per-case`,
which gives each case its own transcript family, plan, and nonce
through the `{case}`, `{index}`, and `{nonce}` tokens in a declared
`--env` value.
`tests/r3v_native_plan_compose_shard.py` composes one sealed plan per
case from that case's transcript family, each with its own nonce and
evidence directory; a family carrying entries in more than one member
refuses that case as `multiple_submitting_devices` without refusing the
rest of the shard.

The `command` slice captures nothing on the host model today: over all
851 cases under the drm-shim, zero transcripts carry an entry, because
every submitting case refuses at image creation before the driver emits
an IB and `r3v_native_plan_capture_record` sees only command buffers
whose `ib_size_dwords` the cell emitters set.  A target run of that
slice therefore submits nothing under closed gates.

The `transfer`, `draw`, `synchronization`, and `robustness` slices
capture nothing either: over all 416,370 cases in their 23 shards, run
one process per case under the drm-shim with capture declared, zero
transcripts carry an entry, and a closed-gate target run of the same
shards on RS482 reproduces the host model's status on every case
(0 of 24,206 witnessed ioctls are `DRM_IOCTL_RADEON_CS`).  Two recipe
facts from those passes: the transcript path is bounded by
`R3V_NATIVE_PLAN_PATH_MAX` (255 bytes) including the case name a
`{case}` template appends and the `.N` ordinal a second device adds, so
the capture root stays short (a 268-byte path refuses at device
creation as `VK_ERROR_INITIALIZATION_FAILED` from the first case whose
name crosses the ceiling); and a per-case pass forks one dEQP process
per case, so several passes side by side on one host exhaust the
process budget and leave cases `not_run` with `Resource temporarily
unavailable` in stderr, a host artifact the rerun of those cases alone
resolves.

`tests/r3v_conformance_partition.tsv` is the exhaustive partition of
the pinned mustpass corpus (3,251,483 cases in 21 slices, all executable
once every group's CTS source resolved to a `none` or `submission`
hazard):
`tests/r3v_conformance_partition.py` reads every corpus file once, assigns each
case to the one slice whose group prefixes it, and refuses an uncovered
case, a case two slices claim, a group claiming nothing, and a case a
corpus file repeats; it generates every slice's sorted caselist with its
count and SHA-256 beside the corpus and table digests in
`partition_manifest.json`, splits every slice into consecutive shards
of at most `--shard-max-cases` (20,000) cases, each with its own
caselist and digest, so the recovery unit one dEQP process runs and the
identity unit the receipt binds are one object; the slice counts sum
to the corpus count by construction, and
`tests/r3v_conformance_corpus.pin` binds the corpus
digest, case count, and CTS revision so another mustpass directory is
another denominator and refuses.  The eleven-row `r3v_conformance_slices.tsv` is the
pilot ladder: the same tool proves its disjointness with `--kind pilot`
and records what it leaves uncovered, so a run over it is a pilot slice
run and never the corpus.  A slice whose hazard is `unknown` is blocked:
its caselist is generated for the record and the runner refuses it
(`blocked_slice`), so an unclassified case rides only a slice named for
its hazard.  `run --partition-manifest` binds the caselist to its slice
by digest, refuses a silicon-required slice under any other evidence
class (`evidence_below_required`), and records kind, slice, hazard,
caselist digest, corpus digest, and manifest digest in the receipt.
Hazard is assigned from the CTS sources: a family whose tests reach
`submitCommandsAndWait` (api.smoke, api.buffer_view.access,
api.descriptor_set, api.get_memory_commitment, memory.binding) is a
submission slice whatever its namespace.

The dEQP-VK binary reaches the target as a bundle
`tests/r3v_deqp_provision.py bundle` writes: the binary, its data
directory, the pinned mustpass corpus, and `provenance.json` (source
commit and cleanliness, CMake pins, compiler and binutils, digest,
dynamic inventory, embedded release name held equal to the source
commit, the GNU x86 ISA-needed property, the functions a finite
above-K8 mnemonic screen flags with the allow pattern that admits each,
tree digests, build-host glibc); `verify` recomputes every digest on
the target and re-derives the four refusals.  The RS482 host's loader
refuses a binary whose ISA-needed note exceeds the baseline, and that
note is the admission test the tool keeps: a build host whose crt and
libgcc objects carry x86-64-v3 stamps every link with it even at
`-march=x86-64`, so the K8 build compiles with `-march=x86-64
-mtune=k8` and links with `gcc -B<dir>` over the target's own baseline
`Scrt1.o`, `crti.o`, `crtn.o`, `crtbeginS.o`, `crtendS.o`, `libgcc.a`,
and `libgcc_eh.a`, which the bundle records under `startfiles`.
`--strip-isa-property` exists for a host whose CPU satisfies every
level the note carried; `verify` on any other CPU refuses the stripped
bundle by `/proc/cpuinfo`.

`tests/r3v_cs_ioctl_trace.py` is the independent witness that a slice
run issued no command submission.  `trace -- <argv>` runs the command
under `strace -f -qq -e raw=ioctl -e trace=ioctl` and counts ioctls by
request number, sealing `{cs_ioctls, gem_info_ioctls, total_ioctls,
strace_sha256, argv}` into a JSON summary beside the tracee's own exit
status, the complete `ioctls_by_request` census, and the four-request
`gem_info_requests` allowlist the named GEM and INFO counter keys on.
The request number derives from the headers rather than from a name
strace happens to print -- `_IOWR('d', DRM_COMMAND_BASE 0x40 +
DRM_RADEON_CS 0x26, struct drm_radeon_cs)` is `0xc0206466`, and
`derivation` prints the same construction for GEM_CREATE, GEM_MMAP,
GEM_WAIT_IDLE, and INFO -- because strace resolves a request several
drivers share into an ambiguous alternation and `raw=ioctl` is what
makes the bare number the thing counted.

A count is a verdict only when the run behind it happened, so the tool
refuses a `cs_ioctls` other than exactly `--expect-cs`, a tracee that
exited nonzero, a trace holding no ioctl at all, an ioctl line the
parser could not read, and an `LD_PRELOAD` interposer that answers in
the tracee's address space; `--allow-tracee-failure`, `--allow-empty`,
and `--allow-preload` open each of the last three by name; an
incomplete census stays closed, since a count read off lines the parser
could not finish reading is no count at all.  The summary lists every
refusal it raised beside the `gates_opened` the caller passed, so a
witnessed verdict carries whether the run was clean or a gate stood open
over it.  A run that never executed therefore refuses on both its exit
status and its empty log rather than sealing the digest of an empty file
as a witnessed zero.

The witness stops at the syscall boundary.  The radeon drm-shim answers
the whole radeon request set inside the tracee's address space, so a
shim run performs no DRM ioctl syscall and this tool reports zero for a
submitting arm whose own transport-table counter reports one; every
syscall-boundary instrument shares that blindness, and a counter ahead
of the shim in the preload order is what witnesses an absorbed call.
`selftest` holds the parser to its contract over a written log (a whole
call, a call the tracer split across a context switch counting once, and
a line it rejects), calibrates it on emitters issuing a known count of
real CS-numbered and GEM-numbered syscalls, drives each refusal against
an input that trips exactly it, and pins both shim arms of
`r3v_native_submit_order_harness` at a syscall-visible zero, which is
the property that holds the scope of the verdict.  On the host model the
zero states that nothing reached the kernel; on silicon,
where every submission is a real syscall, the same zero states that no
submission happened, which is what the hazard-free slices must show.

## Conformance contract

The ICD must not be advertised as conformant Vulkan until:

- every exposed command has implemented semantics;
- memory mapping, visibility, aliasing, and external-memory behavior are
  complete;
- queue ordering and synchronization are resource-correct;
- image, render-pass, dynamic-rendering, query, transfer, and WSI behavior are
  complete for advertised surfaces;
- unsupported work returns a documented error rather than succeeding as a
  no-op;
- feature and extension tables are generated from implemented behavior;
- the relevant CTS and dEQP suites pass on current source and target identities.

The current empty or bounded feature exposure does not convert the driver into a
conformant Vulkan implementation.

## Project and evidence boundary

Mesa source, build rules, tests, and driver documentation live in
`mesa-26-gororoba`.

Target probes, retained silicon bundles, findings, falsifiers, and cross-repo
manifests live in the evidence repository. Linux Radeon kernel changes live in
the kernel source repository; package and deployment policy live in their
packaging repository.

A Mesa behavior change cites external evidence but lands in Mesa.

## Primary sources

- AMD R3xx Register Reference Guide;
- Mesa r300g source under `src/gallium/drivers/r300/`;
- r3v source under `src/amd/r300/vulkan/`;
- Linux Radeon DRM source under `drivers/gpu/drm/radeon/`;
- Vulkan specification, especially devices/queues, memory, synchronization,
  command execution, WSI, limits, and conformance requirements;
- Khronos Vulkan loader ICD interface documentation and `vulkan/vk_icd.h`.
