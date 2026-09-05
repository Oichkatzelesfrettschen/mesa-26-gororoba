# ZMASK clear protocol

The RS485M carries 5120 dwords of ZMASK SRAM (`RV3xx_ZMASK_SIZE`, 20 KB)
and no HiZ RAM at all (`hiz_ram = 0` for `CHIP_RS480` in
`r300_parse_chipset`). A fast Z clear on this part therefore composes
exactly three mechanisms on top of an ordinary depth draw: HyperZ
ownership, the ZMASK bind and clear, and the ZB_BW_CNTL compression
enables. The ladder below separates them so a verdict names one
mechanism.

No silicon run exists for any stage. Every claim here is a source-derived
model built from `src/amd/r300/common/r300_zmask_layout.c`,
`r300_zmask_clear_plan.c`, the Gallium derivation in
`r300_setup_hyperz_properties`, and the kernel's `r300_packet0_check` and
`r300_packet3_check`. The first ZMASK cell exercises the
`RADEON_INFO_WANT_HYPERZ` acquire path for the first time in this driver;
the cell runs when scheduled.

## The layout the stages consume

`r300_zmask_layout_compute` reproduces the Gallium derivation exactly.
It opens only for a depth or stencil format of 32 bits per pixel on a
microtiled level, aligns the row pitch to 16 pixels, picks an 8x8
compression block when the level is macrotiled, single-sample, and the
part is RV350-or-later (`CHIP_RS480` is), and divides the block-aligned
pixel area by the pixels one ZMASK dword covers. The result fits when
the dword count stays within `zmask_ram * pipes`, which is 5120 on one
RS480 pipe. A level that does not fit yields a zero pitch and a zero
dword count, and the bind stages refuse to build for it, matching
`r300_fast_zclear_allowed`, which returns false on a zero ZMASK dword
count.

## The prerequisite: a Z24 macrotiled control cell

The ladder measures every stage against the depth control cell, and the
cell that exists today cannot carry a ZMASK.
`r300_zb_depth_control_cell.c` selects `R300_DEPTHFORMAT_16BIT_INT_Z` at
two bytes per pixel (`R300_ZB_DEPTH_CONTROL_DEPTH_CPP`), and
`r300_zb_depth_state_emit` writes `ZB_DEPTHPITCH` with
`R300_DEPTHMACROTILE_DISABLE | R300_DEPTHMICROTILE_LINEAR`. ZMASK admits
a 32-bit depth format on a microtiled level alone, and 8x8 compression
needs macrotiling on top of that, so the existing cell's layout reports
zero dwords and stages C and D refuse it with `-EINVAL`.
`r300-zmask-clear-plan` pins exactly that, so the gap fails a test rather
than waiting on a run.

The stages therefore bind to a Z24 variant of the control cell.
`r300_zb_depth_surface` carries the two surfaces as data --
`r300_zb_depth_surface_z16_linear` restates the cell's own constants, and
`r300_zb_depth_surface_z24_macrotiled` names
`R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL` at four bytes per pixel,
microtiled and macrotiled, on the Z16 geometry so the ladder changes
format and tiling alone. `r300_zb_depth_control_params` takes the
surface, `r300_zb_depth_state_params` takes the `ZB_DEPTHPITCH` tile bits
the surface declares, and the Z16 instance emits the retained stream byte
for byte, which `r300-zb-depth-control-cell` holds.

Emitting the Z24 surface still refuses, and the refusal names the reason:
the pre-draw host fill is the comparison's other operand, and no
logical-to-physical address transform for R300-class tiling exists in
this tree to place it. Gallium never computes one -- `r300_transfer.c`
routes a tiled map through a linear shadow texture and lets the engine
move the bytes, and `radeon_surface.c` begins at `CHIP_R600` -- so
`r300_get_pixel_alignment`'s tile-dimension table is the only tiling fact
available to cross-check against, and `r300_zmask_layout.c` already
consumes that much. The transform has to come from its own authority:
either the R3xx and R5xx acceleration guides' tiling sections, or a
silicon measurement that fills a one-pixel rectangle at a logical
coordinate through a tiled `DST_PITCH_OFFSET` and reads the changed byte
out of the linear object, which is rank-1 evidence on the RB2D engine the
constant-fill route already receipts. Both are separate work with their
own evidence record; `host_addressable` on the descriptor is the fact
that closes the path until one lands.

The allocation grows with the format: four bytes per pixel doubles the
depth BO, and macrotiling imposes its own pitch alignment on top. The
depth oracle also widens from `uint16_t` to the 32-bit word Z24 stores,
and it reads through the address model rather than through
`y * pitch + x`.

Z24 stores depth in the low 24 bits, so the variant's depth sentinel is
`0x800000`, the same half-scale point `0x8000` marks in Z16: it sits
between the near depth 0.25 and the far depth 0.75, which is the only
property `R300_ZS_LESS` reads out of it. Every sentinel named below is
that 24-bit value.

## The ladder

| Stage | Mechanism added | Registers and packets the stage appends | HyperZ ownership |
| --- | --- | --- | --- |
| A | ordinary depth draw | none | not required |
| B | ownership acquire | none | required |
| C | ZMASK bind and clear | `ZB_ZMASK_OFFSET`, `ZB_ZMASK_PITCH`, `ZB_ZMASK_WRINDEX`, `ZB_ZMASK_RDINDEX`, `GB_Z_PEQ_CONFIG`, `ZB_BW_CNTL` = 0, PACKET3 `3D_CLEAR_ZMASK` | required |
| D | fast fill | stage C with `ZB_BW_CNTL` = `FAST_FILL_ENABLE` | required |

### A: ordinary depth, HyperZ absent

The append is empty and the stream is the depth control cell alone. Its
observation is the depth control cell's own dual-oracle verdict: the near
half colored over a depth value below the sentinel, the far half
untouched. Every later stage is measured against this image, so a stage
that changes it changed something the depth test already established.

### B: ownership acquired, no HyperZ register written

The append is still empty, so the stream admits with or without
ownership, yet the plan sets `requires_hyperz_ownership`. The
asymmetry is the point: a failure at B is the
`RADEON_INFO_WANT_HYPERZ` ioctl returning 0, and a failure at C is the
register path. Separating them keeps an acquire failure from being read
as a ZMASK defect. Expected observation against A: identical color and
depth images, since no state changed.

### C: bind and clear with compression off

The bind places the level at the base of the ZMASK RAM
(`ZB_ZMASK_OFFSET` = 0) at the layout's pitch, zeroes both
autoincrementing RAM access indices, writes `GB_Z_PEQ_CONFIG` with the
tile size the layout decided -- the 64x64 reference level clears four
dwords at 8x8 and sixteen at 4x4, so a retained setting would describe a
different surface than the clear covers -- writes `ZB_BW_CNTL` = 0 so
`FAST_FILL_ENABLE`, `RD_COMP_ENABLE`, `WR_COMP_ENABLE` and `HIZ_ENABLE`
all stay off, and issues `3D_CLEAR_ZMASK` over exactly
`layout.dwords` dwords starting at index 0 with value 0.
`SC_HYPERZ` stays unwritten: the scan converter's HiZ bit belongs to the
HiZ stage past this ladder.

With every compression enable off, the depth pipe reads and writes
depth memory as it did in A, so the expected observation against A is an
identical color and depth image. The stage measures that the bind and
the clear packet traverse the kernel and the ring without disturbing the
draw. `ZB_ZMASK_PITCH` is nonzero and `3D_CLEAR_ZMASK` is a gated
packet3, so this stream admits under ownership and refuses without it --
`r300_zb_hyperz_admit_stream` reports `REFUSE_OWNERSHIP` at the pitch
write, which precedes the tile-size write. `GB_Z_PEQ_CONFIG` is gated
the same way, and its 8x8 value refuses on its own while its 4x4 value
of zero admits, so a 4x4 stream's refusal rests on the pitch and the
clear packet.

### D: fast fill

`ZB_BW_CNTL` gains `FAST_FILL_ENABLE` alone. FASTFILL performs no fill:
it tells the depth pipe to consult the ZMASK RAM before fetching from
depth memory, and a tile whose ZMASK bits are zero is in the cleared
state, for which the pipe returns `ZB_DEPTHCLEARVALUE` in place of the
value stored in memory. Stage C cleared the whole ZMASK to zeros, so
every tile of the surface reads back as `ZB_DEPTHCLEARVALUE` under D.

The prediction follows from that substitution. `R300_ZS_LESS` now
compares both triangles against `ZB_DEPTHCLEARVALUE` rather than against
the sentinel the host wrote, so D reproduces A's color image exactly
when the cell has established `ZB_DEPTHCLEARVALUE` equal to the depth
sentinel `0x800000`, which sits between the near depth 0.25 and the far
depth 0.75. With any other clear value the two halves move together:
a clear value above the far depth colors both halves and one below the
near depth colors neither. `ZB_DEPTHCLEARVALUE` at 0x4f28 carries no row
in the kernel's HyperZ table, so the cell writes it outside this append.

The depth oracle reads the surface through the host, which the ZMASK
resolve does not intercept: `WR_COMP_ENABLE` is off, so passing
fragments still store uncompressed depth, and the depth image stays A's.
The color image is the fast-fill resolve's verdict.

Rule 8 of the fast-clear notes in `r300_blit.c` forbids FASTFILL with
`RD_COMP_ENABLE` off while `WR_COMP_ENABLE` is on. Both are off in C
and D, so the ladder stays inside that rule.

## Order after D

`r300_update_hyperz` sets the enables in two groups, which is what makes
the next steps separable rather than arbitrary: the decompression path
sets `FAST_FILL_ENABLE | RD_COMP_ENABLE`, and the in-use path sets
`FAST_FILL_ENABLE | RD_COMP_ENABLE | WR_COMP_ENABLE`. The order after D
follows those groups:

1. `RD_COMP_ENABLE` -- the depth pipe decompresses tiles whose ZMASK
   bits say compressed, which is the step past reading the clear value
   for a zeroed tile. Gallium's decompression path pairs it with
   FAST_FILL and nothing else, so it stands alone as a stage.
2. `WR_COMP_ENABLE` -- passing fragments write compressed tiles back and
   the ZMASK stops being read-only. A defect here changes depth memory,
   which the depth oracle reads directly.
3. HiZ -- `hiz_ram` is 0 on `CHIP_RS480`, so HiZ has no RAM on this part
   and no HiZ stage is reachable here. The step exists for the discrete
   R3xx and R5xx parts that carry HiZ RAM.

Each step keeps the ownership requirement stages C and D establish, and
each is measured against A by the same two oracles.
