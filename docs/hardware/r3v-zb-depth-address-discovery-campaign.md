# RS485M depth address discovery campaign

The physical byte a logical depth coordinate reaches under R300-class
tiling has no model in this tree. Gallium never computes one:
`r300_transfer.c` routes a tiled map through a linear shadow texture and
lets the engine move the bytes, and `radeon_surface.c` begins at
`CHIP_R600`. The campaign recovers that mapping by measurement, one
logical pixel at a time.

## Apparatus

The discovery cell covers the 64x64 target with two triangles at window
depth 0.25 and lets `SC_SCISSORS` confine the write to one pixel. The
depth comparison is `ALWAYS` with depth writes enabled, so the depth
result is a write rather than a test outcome. The host initializes the
whole allocation to one repeated packed word, the device writes the
marker, and the host reads every byte back and reports which ones moved.

The scan carries no address resolver. A resolver asked where to look
would supply the answer the observation then reports, so the oracle takes
the declared coordinate for retention alone and scans the whole envelope
in offset order.

A constant initial image is what makes a tiled surface observable without
a transform: tiling permutes complete pixels, and a constant image is
invariant under a permutation. The scenario therefore admits a surface on
`raw_allocation_mapping` and `uniform_packed_initialization` alone.
`logical_pixel_addressing` and `logical_image_readback` stay out of the
admission, because both tiled surfaces answer false to them and requiring
either would refuse the campaign this apparatus serves.

## Allocation and the three byte classes

The allocation is 24576 bytes for every rung, held constant so the
transport, the queue predicate, and the retained artifact do not move
between tiling modes. The storage envelope does move:

| Rung | Storage rows | Envelope bytes | Layout span | Unclaimed |
|---|---|---|---|---|
| Z24 linear | 65 | 16640 | 20736 | 3840 |
| Z24 microtiled (4x2) | 66 | 16896 | 20992 | 3584 |
| Z24 microtiled + macrotiled (32x16) | 80 | 20480 | 24576 | 0 |

With a 2048-byte guard on each side, the constant allocation equals the
macrotiled span exactly and leaves slack past the smaller two. Those
bytes are neither guard nor storage. Counting them as storage would put
slack into the denominator and attribute a change in them to a discovered
depth address; counting them as guard would claim a verdict the layout
never stated. They are inspected and reported as unclaimed, and a
single-pixel observation requires zero changed bytes in them.

`r300_zb_depth_layout_is_guard_byte` classifies guard bytes. The row-major
canary the ordinary depth control uses does not apply here: a macrotiled
surface's fifth macrotile row covers logical rows 64 through 79, so the
row past the render extent shares a macrotile with rows the surface
renders, and a row-major byte offset is not a canary row in a tiled
surface.

## Executing state

The state the run requires is read out of the emitted stream through
`r300_zb_depth_discovery_check_state`, not cited from the contract table.
`r300_first_draw_state.c` pins `ZB_BW_CNTL` to zero, but
`r300_zb_depth_state_emit` writes the same register later and a later
write wins.

| Register | Value | Why |
|---|---|---|
| `ZB_FORMAT` | scenario's format, written exactly once | one write makes the effective format unambiguous by construction |
| `ZB_CNTL` | `Z_ENABLE` set, `STENCIL_ENABLE` clear, write per arm | an armed stencil test would gate the depth write on a component the run observes rather than controls |
| `ZB_ZSTENCILCNTL` | `Z_FUNC` per arm | `ALWAYS` for the measurement and writes-disabled arms, `NEVER` for the third |
| `ZB_BW_CNTL` | `0x00000000` | HiZ, fast fill, `RD_COMP`, `WR_COMP`, and `ZB_CB_CLEAR` all clear |
| `GB_Z_PEQ_CONFIG` | `0x00000000` | `Z_PEQ_SIZE_4_4`, the plane equations the R5xx acceleration guide requires while compression is disabled |
| `SC_SCREENDOOR` | `0x00ffffff` | a zero screendoor drops every sample |
| `SC_SCISSORS_TL/BR` | the declared pixel | the register that confines the write |
| `SC_CLIPRECT_TL/BR_0` | the full extent | no narrower than the scissor, so one register names the confined region |

`ZB_CB_CLEAR` matters beyond compression. The R3xx reference describes its
set state as cache-line-granular write-only operation and warns that a
partially touched microtile leaves its untouched portion unknown, and an
unknown byte inside the envelope makes a one-pixel address observation
unreadable.

The plane-equation size is a separate geometry from the two storage block
sizes and shares only a numeral with them: the Z24 packed microtile is
4x2 pixels in 32 bytes, the macrotile is 32x16 pixels in 2 KiB, and the
uncompressed plane-equation mode is 4x4.

## Arms

Three arms come off one emitter, so the run that finds an address and the
runs that must find none are the same apparatus at two parameter values.

| Arm | Comparison | Depth write | Required result |
|---|---|---|---|
| `measure` | `ALWAYS` | enabled | exactly one changed depth slot; the declared pixel colored and no other |
| `writes_disabled` | `ALWAYS` | disabled | the declared pixel colored; no depth code changed |
| `never` | `NEVER` | enabled | nothing changed at all |

A stencil-only change is retained in every arm and fails none of them.
Whether a depth write preserves the packed stencil byte is what the seed
pair measures, not a condition on the address. A change in a guard range
or in the unclaimed slack is a containment failure whatever else passed.

## Experiment identity

The stencil seed reaches the device through the host fill alone, so the
three linear scenarios emit byte-identical IBs and one stream digest
admits all three. The retained artifact therefore carries a BLAKE3 over
the initial depth image, and the arming runner reports both digests so an
operator arms a stream and records an experiment. The attended program
reads the initial image back from the allocation the recorder filled
rather than reconstructing it, so the observation compares against the
state that reached the device.

## Execution order

Each execution runs through the existing authorized procedure, one
attempt per cell, with its prediction sealed before arming. The ordering
is a dependency chain, not a schedule: each rung's result is what makes
the next one readable.

1. **Frozen Z24-linear depth control.** The unchanged ordinary depth
   cell. Requires correct near, far, and exterior behavior, the retained
   near-depth distribution, and the stencil observation. This is the
   first hardware execution of the depth campaign and it uses the
   discovery apparatus not at all.
2. **Discovery in linear mode, `measure` arm.** Requires exactly one
   changed depth slot. Its offset is checked against
   `r300_zb_depth_address_linear`, which is an independently known
   answer: the apparatus must recover an address it did not compute.
3. **Linear controls.** `writes_disabled` and `never`, each its own
   attempt.
4. **Nonzero stencil seeds.** `z24_linear_seed_5a` and
   `z24_linear_seed_a5` on the `measure` arm, which separate stencil
   preservation from replacement. The ordinary depth write and the fast
   clear are distinct paths here: the R3xx register reference states that
   `ZB_DEPTHCLEARVALUE` updates stencil in 24-bit mode regardless of
   whether stencil is enabled, and this cell issues no fast clear.
5. **Microtiled discovery.** The first rung where a logical coordinate
   and a physical byte separate. Requires a recovered offset with no
   resolver consulted.
6. **Macrotiled discovery.** Offsets across microtile, macrotile, and row
   transitions.
7. **Held-out coordinates, another pitch, another aligned base.** Tests a
   derived mapping outside its fitting samples.

The coordinate-mask campaign that recovers all 4096 labels at once is
downstream of step 5 passing its known-linear-address test and is not a
prerequisite for the apparatus. Its acceptance conditions are that every
label appears exactly once, no two labels resolve to one physical
address, no logical pixel is unmapped, and no guard byte moves.

## What a resolver may be derived from

The observed mapping table is retained first, keyed independently of any
resolver's implementation, so a formula cannot generate its own expected
answers. A resolver is derived from that table afterwards and validated
against retained observations it did not produce. Only then does it
connect to the ordinary depth control, with nonuniform initial depth and
overlapping near and far draws in both orders.

Address discovery, depth-conversion behavior, and stencil semantics stay
separate acceptance dimensions throughout.

## Gallium plane-equation disagreement

`r300_update_hyperz` builds `gb_z_peq_config` from `tex.zcomp8x8[level]`
whenever HyperZ is permitted, before it decides which `ZB_BW_CNTL`
enables the state carries. That constructs an 8x8 plane-equation state
for a surface selecting 8x8 even on paths that enable no compression.

This is a question about the effective register state at a draw, not
about the order of C assignments: `r300_emit_hyperz_state` emits the
state table afterwards, and permission to use HyperZ is not active
compression. Whether the uncompressed-with-8x8 combination reaches a
relevant draw on RS485M requires a caller and state-transition trace or a
retained submission, and that investigation is outside this campaign.

The native ladder holds 4x4 while compression is disabled and does not
imitate a path whose complete conditions have not been reproduced. The
discovery cell's constant-depth plane is also the wrong instrument for
that question: a constant plane removes the gradients through which a
plane-equation disagreement would become observable, so a separately
calibrated depth ramp owns it.

## Status

Every row above is `not run`. The apparatus, its oracles, its controls,
its arming runner, and its qualification tests are built and registered;
no discovery cell has reached silicon.
