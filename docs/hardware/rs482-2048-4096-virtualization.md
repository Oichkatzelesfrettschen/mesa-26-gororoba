# RS485M 2048-to-4096 virtualization capability matrix

One home for the family of representation-preserving transformations that
present 2048-physical surfaces and rasters as 4096-scale logical objects on
R300-class hardware (RS485M, `1002:5974`).  Each domain uses the quotient/
remainder decomposition `i = q * 2048 + r` in the form its access semantics
require; the domains share the radix, not one mechanism.

Physical facts the matrix rests on: the R3xx texture axis cap is 2048; the
R3xx color-render axis cap is 2560; the Vulkan 1.0 logical floor
`maxImageDimension2D` is 4096.  A surface that must be both rendered and
sampled uses 2048, the smaller cap (`R300_RASTER_AXIS_CAP`,
`r300_grid_fold.h`).

## Lane status

The R3V render/copy tiling, NEAREST stitch, and LINEAR halo atlas rows
below were implemented on the retired Gallium-backed Vulkan lane; the
silicon evidence they cite stands, and the direct-DRM ICD carries none
of the three implementations yet.  `rs482-composed-surface-campaign.md`
records the re-derivation rungs for the native lane and the receipts
that widen each advertised limit.

## Matrix

Representation classes: partition = disjoint tiles, each logical element in
exactly one; cover = overlapping charts with duplicated seam texels; fold =
mixed-radix coordinate isomorphism preserving the flat byte stream.

- R3V render/copy tiling (partition, `r3v_image.c` tile split +
  `r3v_queue.c` replay): logical optimal images up to 4096 per axis as a
  2x2 grid of 2048 tiles with per-tile viewport/scissor translation.
  Evidence: silicon -- 4096 image create, render-pass clear, and
  tile-boundary copies proven on RS485M; a general primitive crossing the
  seam under replay is source-supported with the geometry-seam probe still
  open.
- R3V NEAREST sampling (partition lowered into the shader,
  `fs_nearest_stitch` NIR pass): one logical sample becomes up to four
  physical tile samples plus branchless affine selection; exact for
  nearest because a point footprint lies in one tile.  Evidence: silicon
  -- 4096x64 seam sampling exact under the experimental gate.
- R3V LINEAR sampling (cover, `r3v_image_build_sampler_atlas`,
  `r3v_image.c`): lazy overlapped halo atlas; a bilinear footprint near
  the logical seam stays inside one chart.  Two charts span an axis only
  while the overlap keeps one seam texel per side: admissible up to
  `2 * cap - 2` (4094 on r3xx); 4095 and 4096 are refused by the
  eligibility predicate and wait on a three-chart cover.  Evidence:
  source-implemented; a retained silicon LINEAR seam proof and a
  host-test extraction of the eligibility predicate (the `W + 2 > 2 * cap`
  refusal in `r3v_image_build_sampler_atlas`) are the named follow-ups.
- Compute grid fold (fold, `r300_grid_fold.h`): position-addressed kernels
  keep per-axis coordinates at or below 2048 and never materialize the
  linear index, so 2048x2048 = 2^22 invocations stay honest; a kernel that
  materializes `gid` binds to the FP24 exact-integer window 2^17.
  Evidence: admission classes calibrated on host; window proof in the
  FP24 exactness theorem.
- R2VB slot fold (fold, `r300_r2vb_slot_grid` helper + BO-fetch
  producer): slot `s` at pixel `(s % W, s / W)` with `pitch == W`
  preserves `(y * pitch + x) * 16 == s * 16`, so the producer raster is
  2D while re-ingest reads one flat record stream.  Evidence: silicon --
  the one-row frontier is exact through 2049 records (two cold
  first-contact boots, precommitted BLAKE3 oracle, byte-perfect under the
  negative-ULP allowance), so 2048 is not a one-row producer bound; the
  historical 4096 single-row ceiling and the first 2048-wide two-row fold
  are the next unprobed cells.

## Boundary distinctions the matrix keeps

- 2048 names four different quantities: the texture axis cap, the common
  render+sample tile width, the grid-fold radix, and the R2VB row width
  under test.  The stale-frame "2048 pixels" of the 64x64 clear family
  was none of these -- it was the CBZB half-surface under the stale
  US-program law -- and the producer raster shares no 2048 boundary.
- 4096 names the Vulkan logical floor, the historical R2VB single-row
  ceiling, and twice the tile width.  Only the first is a contract; the
  other two are measurements to probe.
- Linear-tiling and exported/scanout images stay single-resource and bind
  to the physical caps; mip chains split only when the whole image fits
  one tile.

## Value-exactness allowance

The deviation stage is localized: the RS485M US source-operand read
delivers a negative nonzero input- or temporary-register value one FP24
ULP smaller in magnitude, before the ABS/NEG source modifiers apply;
positive operands, ALU computation, register writes, and exports are
exact, and negative zero reads back as positive zero (finding
`rs482-us-source-read-negative-ulp-law`, steinmarder-r300; software
model `r300_us_source_read.h`).

Value oracles therefore compare against the emitted-program source-read
model, bit for bit -- expected value = ideal evaluation with the
predecessor applied at each negative register read in the scheduled
pair program -- rather than against a generic one-ULP epsilon.  An
epsilon band would accept single-read deviations on multi-read lanes
and reject exact lanes recomposed from two reads, so the schedule-model
identity is both stricter and correct.  Width and layout acceptance
cells use nonnegative payloads when arithmetic is not the cell's
subject, keeping the layout verdict independent of the read law.
Constant-file, texture, and presubtract source classes are unmeasured;
oracles over schedules that read those files as negative sources record
the class as unmodeled instead of assuming either behavior.
