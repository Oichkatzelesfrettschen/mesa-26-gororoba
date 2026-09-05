# RB2D legalizer design

The RB2D legalizer is the compiler in front of the 2D packet emitter. A
linear Vulkan buffer has no rows, so the registers the 2D engine consumes --
`DST_PITCH_OFFSET`, `DST_Y_X`, `DST_WIDTH_HEIGHT` -- are an address-generator
instruction set the driver targets, and the hardware grids are that target's
constraints. The legalizer lowers an arbitrary dword-aligned byte interval
into surface windows that satisfy every constraint, the way a register
allocator lowers an unbounded virtual register file onto a fixed one. The
kernel's raw-stream grammar stays deliberately narrow; the legalizer never
widens it, it makes the semantic operation reach it.

## Pipeline

```text
byte interval [offset, offset + size)
  -> carrier choice        pitch and format from the evidence registry
  -> window sequence       each rebased on the 1 KiB base grid, y small
  -> rectangle tiling      first-row remainder, whole rows, tail
  -> window check          every invariant, per window
  -> coverage check        exact byte-set equality across windows
  -> emission              DAG-ordered state, one typed relocation per window
  -> kernel                r100_cs_track_2d_dst_check bounds each launch
```

Source: `src/amd/r300/common/r300_rb2d_legalize.{c,h}` (request, window,
checker, coverage, chooser, emission), `r300_rb2d_pitch_evidence.{c,h}`
(carrier registry), `r300_rb2d_linear_span.{c,h}` (the cut), and
`r300_rb2d_fill.{c,h}` (plan checker and the emitter state machine).

## Three acceptances

| Acceptance | Question | Target |
| --- | --- | --- |
| Semantic | Can this valid Vulkan operation execute at all? | broad |
| Legalized plan | Does it decompose into hardware-valid windows? | broad |
| Raw stream | Is this exact packet stream safe? | narrow, the kernel's |

`x = 65` on a 64-pixel row is semantically accepted, legalized to `x = 1,
y + 1`, and the raw stream naming `x = 65` keeps rejecting in the kernel.
The legalization differential holds all three at once for every row of the
transformation table.

## Transformation table

| Raw rejection (kernel) | Legalization |
| --- | --- |
| x past pitch | carry `x / row_pixels` into y through the 1 KiB base grid |
| width overruns pitch | first-row remainder, whole rows, tail |
| y too large, height too large | rebase the surface every 0x1fff rows |
| x + width field overflow | whole-row split before packing |
| address arithmetic overflow | nearer base per window; the 32-bit surface bound refuses the rest |
| missing relocation | `r300_rb2d_emit_surface_state` takes a typed relocation; `DST_PITCH_OFFSET` has no other emission path |
| unsupported datatype | carrier format from the registry: ARGB8888 for a 32-bit pattern, RGB565 only for a pattern whose halves are equal |
| geometry before destination or format, launch before origin | emitter epochs: a launch with any of dst, format, origin at zero records `-EINVAL` |
| DEFAULT pitch source | every surface emits explicit `DST_PITCH_OFFSET` with `GMC_DST_PITCH_OFFSET_CNTL` |
| pitch zero | the carrier is chosen, never zero; an empty request is eliminated |
| width zero | a partial row cuts no empty rectangle; the checker refuses one |
| undersized object | the span refuses outside the buffer; the window checker bounds the kernel's own `end_byte` |
| completion object as destination | stays a kernel rejection; the bound is the object the relocation consumed |
| past scissor | rebase and split at `R300_RB2D_SAFE_EXCLUSIVE_END` |
| missing final wait | the epilogue is appended per window; the kernel accepts a stream without it |
| RGB565 row overflow | the same row split with `cpp = 2` |

## Window invariants

`r300_rb2d_window_check` holds every window to, in order: a format in the
carrier table; cpp equal to the format's; base on the 1 KiB grid and inside
the 22-bit offset field; pitch nonzero, on the 64-byte grid, inside the
10-bit field; at least one rectangle and none empty; `x < row_pixels`;
`x + width <= row_pixels`; `x + width` and `y + height` at or under the safe
scissor end; the declared height equal to the rows the rectangles reach;
the last written byte -- the kernel's `offset + (y + height - 1) * pitch +
(x + width) * cpp` -- inside the 32-bit surface and inside the object; and
the fill plan checker's own admission. The footprint bound is the kernel's,
so a dense carrier ends inside a buffer narrower than its pitch, which is
what makes a wide virtual pitch usable on a small buffer at all.

Coverage is proven twice: the legalizer walks every rectangle row in order
and requires each to start where the previous ended and the sum of areas to
equal the interval; the test replays every window onto a per-byte count and
requires exactly one touch inside the interval and none outside.

## Carrier evidence and the chooser

`r300_rb2d_pitch_evidence.c` is the one table that admits a carrier. Each
row names pitch, format, usage, the highest evidence class that exercised
it, and the retained artifact. Classes ascend PLANNED, HOST_MODEL,
KERNEL_REPLAY, SILICON_RECEIPT; execution admits SILICON_RECEIPT alone,
which today is the 256-byte ARGB8888 fill carrier under the sealed attended
receipt. The dense candidates 1024 through 32704 bytes are PLANNED, and a
pitch-only silicon qualification promotes one by editing its row.

The chooser legalizes the request on every admitted row and ranks by
`cost = 64 * windows + 16 * relocation_sites + 8 * rectangles + ib_dwords`,
the smaller pitch winning a tie. Exact division into whole rows is one
rectangle, so a pitch that divides the interval beats a wider one that needs
a tail, and the witnessed pitch stays cheapest for any interval one of its
windows holds.

## Contracts

`R300_RB2D_CONTRACT_CONST_FILL_V1` is the qualified public fill: the
256-byte carrier, one window, at most three rectangles, and the 38-dword
stream the receipt retains. `r300_rb2d_legalize_test` asserts the V1
legalization of the attended cell emits those bytes unchanged.
`R300_RB2D_CONTRACT_CONST_FILL_V2` admits any registry-admitted carrier,
several rebased windows, and one relocation site per window; the route
selects it only under its own receipt. The public route in
`r3v_native_fill_route.c` keeps consuming the V1 shape until then.

## Verification

* `r300-rb2d-legalize`: transformation table, V1 byte identity, V2
  multi-window emission, every window invariant broken alone, coverage
  oracle known-bads, registry self-check, chooser ranking, emitter epochs.
* `r300-rb2d-legalization-differential`: assembles each raw rejecting
  stream from register words, replays it through `replay_r300_cs_track`
  built from the linux-radeon-gororoba tree (`R3V_CS_TRACK_REPLAY_TOOL`),
  requires REJECT, legalizes the same interval, requires ACCEPT, and holds
  the touched-byte union to the interval. Without the tool the test is
  recorded as skipped, never as passed.
* `r300-rb2d-linear-span` and `r300-rb2d-fill-plan` keep their arms; the
  span's footprint rule is now the kernel's `end_byte`.

## Deferred, with the evidence each waits on

* Pitch-only qualification of a dense carrier on the RS485M specimen: one
  attended fill on the candidate pitch, sealed as a receipt, then the
  registry row moves to SILICON_RECEIPT.
* V2 selection by the public route: its own receipt with several windows.
* Replacing the per-window epilogue with a single end-of-stream wait: a
  silicon discriminator that a fence cannot retire ahead of RB2D completion.
* Mapping a resource wider than the 32-bit surface through a low GART
  window: a linux-radeon-gororoba mechanism, not needed at the Vostro
  aperture sizes.
