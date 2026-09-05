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
which two ARGB8888 fill carriers hold: the 256-byte carrier under the
sealed attended receipt of the public route, and the 16320-byte carrier
under the attended `dense_16320_carrier` qualification. The dense
candidates 1024 through 8192 bytes are PLANNED, and a pitch-only silicon
qualification promotes one by editing its row. 16320 is the widest carrier
the word can name: `r100_reloc_pitch_offset` rebuilds
DST_PITCH_OFFSET as `(value & 0x3fc00000) | offset | tile_flags`, so the
pitch is bits 22-29 and reaches 255 of the 64-byte grid, while bits 30-31
carry DST_TILE_MACRO and DST_TILE_MICRO and are taken from the relocation.
A 256-unit pitch reaches the kernel as pitch zero and a 511-unit pitch as
255 units with the macro-tile bit set; the differential replays both raw
streams to REJECT.

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
`r3v_native_fill_route.c` lowers through the legalizer and dispatches the
contract from the selected route's own `gpu_route_contract_id`, so the V1
row pins the 256-byte carrier and the V2 row runs the chooser. V2's
contract-evidence row receipts no window, so a selected V2 route declines
until a receipt lands.

## Contract evidence

Two authorities govern one legalization and answer different questions.
`r300_rb2d_pitch_evidence.c` holds carrier evidence, one row per (pitch,
format, usage); `r300_rb2d_contract_evidence.c` holds contract evidence, one
row per route contract, naming the highest class that exercised the
contract's own stream shape together with the window and relocation-site
counts that class reached. Admission requires both: a request clears the
carrier table on `minimum_evidence` and the contract table on
`minimum_contract_evidence`, and failing either refuses.

The split is the receipt's own scope. The sealed attended run receipted a
256-byte ARGB8888 carrier and, on it, the stream shapes that ran: one
window through one relocation site under V1, and two windows through two
relocation sites under V2, the latter from the attended `v2_multiwindow_256`
CONTROL_PASS. Carrier evidence carries the first fact and says nothing about
how often a stream rebases the destination, so a legalization wider than the
receipted shape refuses on `REFUSE_CONTRACT_EVIDENCE` even where the carrier
holds SILICON_RECEIPT. The structural window cap runs first: a V1 stream
that rebases twice refuses on `REFUSE_CONTRACT_WINDOWS` before this
authority reads it.

| Contract | State | Windows receipted | Sites receipted | Artifact |
| --- | --- | --- | --- | --- |
| `CONST_FILL_V1` | SILICON_RECEIPT | 1 | 1 | the sealed attended public-route receipt |
| `CONST_FILL_V2` | SILICON_RECEIPT | 2 | 2 | the attended `v2_multiwindow_256` receipt |

A request leaving `minimum_contract_evidence` at PLANNED reads no contract
row, which is what lets the cost model and the geometry tests rank shapes
nothing has run.

## V2 route identity

| Field | Value |
| --- | --- |
| Route | `R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2`, `rb2d_const_fill_v2` |
| Operation | `R300_OPERATION_ID_CONSTFILL` |
| Executor | GPU |
| Unit | `R300_EXECUTION_UNIT_RB2D_FILL` |
| Use | `R300_ROUTE_USE_TRANSFER_BUFFER` |
| State | EXECUTING |
| Implementation | `R300_OPERATION_IMPLEMENTATION_RB2D_LINEAR_SOLID_FILL` |
| Contract | `R300_GPU_ROUTE_CONTRACT_RB2D_LINEAR_SOLID_FILL_V2` |
| Admission | `R300_ROUTE_ADMISSION_RB2D_WINDOWED_LINEAR_SURFACE` |
| Exactness | BIT_EXACT |
| Evidence | SILICON_RETAINED at NATIVE_GPU_ROUTE_CELL |
| Gate | `R3V_NATIVE_ROUTE_RB2D_CONST_FILL_V2_EXPERIMENTAL` |

The implementation is shared with V1 -- the same RB2D solid brush and the
same legalizer -- and the contract and admission identities are the route's
own. Both gates open name two executors for one transfer destination, so
device creation refuses that pair and the route policy refuses it again at
every request rather than ranking the two or falling to the host.

Three attended CONTROL_PASS runs on the Dell Vostro 1000 (RS485M,
`1002:5974`) under the strict-2d parser epoch carry the row to EXECUTING,
each with its own retained bundle in `steinmarder-r300 src/re/r300/results`:

* `r3v-native-rb2d-const-fill-v2-multiwindow-receipt-vostro1000_rs485m_5974-strict-2d-cs`
  -- two rebased windows through two relocation sites on the 256-byte
  carrier, which is the stream shape the V2 contract row receipts.
* `r3v-native-rb2d-dense-16320-carrier-receipt-vostro1000_rs485m_5974-strict-2d-cs`
  -- one window of five rows on the widest pitch `DST_PITCH_OFFSET`
  encodes, which is the carrier the 16320 pitch row receipts.
* `r3v-native-rb2d-const-fill-v2-chooser-receipt-vostro1000_rs485m_5974-strict-2d-cs`
  -- the public route reaching 16320 through its own chooser, one window of
  129 rows, 38 dwords, which is the cost model's verdict receipted as the
  carrier the run exercises.

The gate stays and AUTO keeps the host path. Automatic selection is
withheld until the host is measured against V2 at 4 B, 64 B, 256 B, 4 KiB,
64 KiB, 512 KiB, 2 MiB, and 8 MiB; the crossover that measurement finds is
the threshold any AUTO admission would name, so until it runs the route
answers an operator who opens its gate or a `GPU_ONLY` caller.

`R3V_NATIVE_RB2D_V2_EXPECTED_PITCH_BYTES` is an assertion over the chooser,
read once at device creation beside the route gates: when it is set, the
chosen carrier pitch equals it or the route declines. It never selects a
carrier, so a value that disagrees with the chooser is a refusal rather than
a different stream. The accepted form is a decimal byte count on the 64-byte
grid inside the `DST_PITCH_OFFSET` pitch field; a declaration of any other
form (empty, non-numeric, zero, off-grid, or past the field) is recorded as
malformed at device creation and declines every windowed-route request,
so a typo closes the route rather than disabling the assertion.

## Designed cells

The multi-window V2 cell, computed from the legalizer and carrying the
attended CONTROL_PASS that receipts the V2 contract row above:

* carrier 256-byte ARGB8888, allocation 2 MiB (2097152 bytes), offset 12,
  size 2097012, coverage exactly [12, 2097024).
* window 0: base 0, height_rows 8191, rectangles (x 3, y 0, w 61, h 1) and
  (x 0, y 1, w 64, h 8190). The row limit is the safe scissor end 0x1fff, so
  the window stops with the interval unexhausted.
* window 1: base 2096128, height_rows 4, rectangle (x 0, y 3, w 32, h 1).
  8191 * 256 = 2096896 is 768 bytes past a 1 KiB boundary, so the rebase
  leaves a local y of 3 and the tail is one rectangle.
* window_count 2, relocation_sites 2, one buffer object.

The dense carrier cell, pinned in `r300_rb2d_legalize_test` at
`minimum_evidence` SILICON_RECEIPT with pitch 16320, the evidence its own
attended run receipted:

* 64 KiB object, offset 12, size 65428, rectangles (3, 0, 4077, 1),
  (0, 1, 4080, 3), (0, 4, 40, 1), height_rows 5.
* The footprint is the kernel's `end_byte`, 65440 bytes, so a carrier whose
  pitch is a quarter of the object fits inside it.

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
