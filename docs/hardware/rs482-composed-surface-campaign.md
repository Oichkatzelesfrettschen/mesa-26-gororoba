# RS482 composed surfaces: 4096 images and the 4-sample count

`dEQP-VK.info.device_properties` fails the R3V ICD on three limit
families below the Vulkan 1.0 core minimums, and each has a driver
route to a pass on the RS482 die.  This document records the
mechanism, the evidence each rung rests on, the falsifier, and the
receipt that lets the advertised limit widen.  The limits and the
minimums come from the CTS limit table (`vktApiFeatureInfo.cpp`,
`featureLimitTable`, "Based on 1.0.28 Vulkan spec"); the die facts from
`src/amd/r300/common/r300_chip_identity.c` (`r300_rs480_die_facts`).

## The three families

| limit | advertised | minimum | die fact | class |
|---|---|---|---|---|
| `maxImageDimension2D` | 2048 | 4096 | sampler and tiled-row cap 2048 | composed surface |
| `maxFramebufferWidth/Height`, `maxViewportDimensions` | 64 | 4096 | render span 2560 | executed cell ceiling, then composed |
| seven sample-count masks | `1` | `1\|4` | `GB_AA_CONFIG`, `GB_MSPOS0/1`, `RB3D_AARESOLVE_*` present; 2x/4x FBO MSAA executes 8/8 | unbuilt native MSAA path |

The sample-count row corrects an earlier assumption: RS482 has
multisample hardware.  steinmarder-r300 finding
`2026-05-26-r300-default-fb-msaa-readback-not-driver-specific` records
FBO MSAA depth/stencil at samples 2 and 4 passing 8/8 on the hardware
X11 session (renderer `ATI RS480`, zero GPU resets), and the RS482
register database carries `R300_GB_AA_CONFIG` (0x4020),
`R300_GB_MSPOS0/1` (0x4010/0x4014), and `R300_RB3D_AARESOLVE_OFFSET/
PITCH/CTL` (0x4E80/0x4E84/0x4E88) as present with at-rest live reads.
The hardware executes 4x MSAA; the native lane has no path for it yet.

## Composition rules the corpus already proves

`docs/hardware/rs482-2048-4096-virtualization.md` records three
mechanisms sharing one radix, `i = q * 2048 + r`, and no unified law:

- Partition for render and copy: a logical image up to 4096 per axis
  as a 2x2 grid of 2048 tiles with per-tile viewport and scissor
  translation.  Silicon-proven for create, clear, and tile-boundary
  copy on the retired Gallium-backed lane; the primitive-crossing-seam
  probe is open.
- Partition lowered into the shader for NEAREST sampling: one fetch
  becomes up to four per-tile fetches plus a branchless tile select.
  Silicon-proven pixel-exact at 4096x64 across the seam (the
  steinmarder-r300 finding of 2026-06-13 on the experimental nearest
  tile-stitch sampler, Mesa `57c6c912f59`).
- Cover for LINEAR sampling: overlapped halo charts so a bilinear
  footprint near the seam stays in one chart; exact to 4094 per axis,
  4095 and 4096 refused pending a three-chart cover.  Source-implemented
  on the retired lane, never silicon-proven.

The native lane retains only the gate scaffolding of the first two
(`r3v_private.h`, `R3V_NEAREST_STITCH_*`, zero consumers); the
implementations left with the Gallium-backed lane.

## Rungs, in evidence order

Each rung carries its observation, constraint, hypothesis, falsifier,
validation, and the receipt that widens the limit.  A rung opens its
limit only after the attended cell is retained in steinmarder-r300.

### R1 render extent 64 to 2560

Observation: `R3V_MAX_RENDER_EXTENT 64` is the qualified cell's shape,
pinned by receipt `r3v-native-public-surface`; the die's render span
is 2560.  Constraint: the retained cell IBs fix `RB3D_COLORPITCH`, the
scissor, and the viewport at 64.  Hypothesis: the cell family emitter
parameterized by extent (pitch in 64-byte units, scissor and viewport
words, `VAP_VF_MAX_VTX_INDX` unchanged) delivers the reference triangle
byte-exact at 256, 1024, and 2560 with the interior/exterior oracle
scaled.  Falsifier: any extent whose target differs from the CPU
oracle, or a CS-track replay refusal at the wider pitch.  Validation:
`r300-*-cs-track-replay` against linux-radeon-gororoba, then an
attended cell per extent.  Receipt: a new `LIMIT_RECEIPTS` entry naming
the widest retained cell; framebuffer and viewport limits rise to it.
Conformance cost recorded: 2560 stays below 4096 until R2.

### R2 composed 4096 render and copy surfaces

Observation: 4096 per axis exceeds both the sampler cap and the render
span.  Constraint: the 2D engine addresses one tile per
`DST_PITCH_OFFSET`; four 2048 tiles cover 4096.  Hypothesis: a
composed image is four transfer- or render-family tiles with a
per-tile offset table; copies split regions at tile edges; a render
pass over a composed target issues one cell per touched tile with the
scissor translated, and a primitive crossing a seam is re-issued per
tile under the same window-space vertices (the raster clips to the
scissor, so the seam pixels are each rasterized exactly once).
Falsifier: a seam pixel written twice or missed, detectable by a
checker-pattern oracle across the seam.  Validation: host-model
composition digests, CS-track replay of the multi-tile IB, attended
seam cell.  Receipt: `maxImageDimension2D` and the framebuffer limits
rise to 4096 with the seam cell named.

### R3 native NEAREST and LINEAR sampling over composed images

Observation: the native lane has no sampled-image path; the retired
lane proved NEAREST stitch and implemented the LINEAR halo cover.
Constraint: the fragment program budget (64 ALU, 32 TEX) bounds the
per-tile fetch expansion.  Hypothesis: re-derive the stitch in the
direct SPIR-V admitter as a job-IR lowering (four per-tile samplers
plus the piecewise-affine select) and the halo cover as a
composition-time chart builder.  Falsifier: a seam sample differing
from the single-tile oracle.  Validation: host oracle, then attended
sampling cells.  Receipt: sampled-image format features and the
sampler limits advertise after the cells.

### R4 native 4x multisample through GB_AA_CONFIG and AARESOLVE

Observation: the hardware executes 4x MSAA on FBOs; the native lane
emits single-sample cells only.  Constraint: `GB_AA_CONFIG.AA` with
`NUM_AA_SUBSAMPLES`, the `GB_MSPOS0/1` sample positions, a
multisampled color buffer at 4x the footprint, and the
`RB3D_AARESOLVE_*` resolve into a single-sample target.  Hypothesis:
the cell family gains a multisample member (AA config, sample
positions, 4x pitch) and a resolve pass member; an edge-crossing
triangle's resolved coverage equals the box-filtered 4x oracle on the
CPU executor.  Falsifier: resolved edge pixels differing from the 4x
box filter beyond the FP24 tolerance, or a CS-track replay refusal of
the AARESOLVE words.  Validation: kernel replay, then an attended
resolve cell.  Receipt: the seven sample-count masks advertise `1|4`
with the cell named, and `VK_SAMPLE_COUNT_4_BIT` enters image creation
for the render family.

An alternative for R4, a 2x2 supersample render folded by a box filter
on the host, needs no AA registers and gives an exact oracle for the
hardware resolve; it is the calibration arm, not the advertised route.

## Order and gates

R1 first (one emitter parameter, smallest cell), then R4 (its resolve
cell is independent of composition and closes seven limits at once),
then R2 (composition, the largest surface), then R3 (sampling, which
needs R2's composed images).  Every rung's attended cell is an
operator-armed run on the RS482; the host-model receipts and the
CS-track replay precede it.  The non-pass ledger row
`limit_below_core_minimum` stays until all four rungs retain their
cells, and `dEQP-VK.info.device_properties` is the judge.
