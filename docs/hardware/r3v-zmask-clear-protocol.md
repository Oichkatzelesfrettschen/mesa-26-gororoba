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

## The ladder

| Stage | Mechanism added | Registers and packets the stage appends | HyperZ ownership |
| --- | --- | --- | --- |
| A | ordinary depth draw | none | not required |
| B | ownership acquire | none | required |
| C | ZMASK bind and clear | `ZB_ZMASK_OFFSET`, `ZB_ZMASK_PITCH`, `ZB_ZMASK_WRINDEX`, `ZB_ZMASK_RDINDEX`, `ZB_BW_CNTL` = 0, PACKET3 `3D_CLEAR_ZMASK` | required |
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
autoincrementing RAM access indices, writes `ZB_BW_CNTL` = 0 so
`FAST_FILL_ENABLE`, `RD_COMP_ENABLE`, `WR_COMP_ENABLE` and `HIZ_ENABLE`
all stay off, and issues `3D_CLEAR_ZMASK` over exactly
`layout.dwords` dwords starting at index 0 with value 0.
`SC_HYPERZ` and `GB_Z_PEQ_CONFIG` stay unwritten: the scan converter's
HiZ bit and the ZMASK tile size belong to stages past this ladder.

With every compression enable off, the depth pipe reads and writes
depth memory as it did in A, so the expected observation against A is an
identical color and depth image. The stage measures that the bind and
the clear packet traverse the kernel and the ring without disturbing the
draw. `ZB_ZMASK_PITCH` is nonzero and `3D_CLEAR_ZMASK` is a gated
packet3, so this stream admits under ownership and refuses without it --
`r300_zb_hyperz_admit_stream` reports `REFUSE_OWNERSHIP` at the pitch
write.

### D: fast fill

`ZB_BW_CNTL` gains `FAST_FILL_ENABLE` alone. The cleared ZMASK now
answers depth reads for tiles whose mask says cleared, so the expected
observation against A is again an identical image -- this time because a
zeroed ZMASK means "cleared" and the depth pipe must resolve the same
values through the compression path that A read out of memory. A
deviation at D with A, B and C all matching isolates the fast-fill
resolve.

## Order after D

`r300_update_hyperz` sets the enables in two groups, which is what makes
the next steps separable rather than arbitrary: the decompression path
sets `FAST_FILL_ENABLE | RD_COMP_ENABLE`, and the in-use path sets
`FAST_FILL_ENABLE | RD_COMP_ENABLE | WR_COMP_ENABLE`. The order after D
follows those groups:

1. `RD_COMP_ENABLE` -- the depth pipe reads compressed tiles through the
   ZMASK. This is the decompression pair the kernel's own path uses, so
   it stands alone as a stage.
2. `WR_COMP_ENABLE` -- passing fragments write compressed tiles back and
   the ZMASK stops being read-only. A defect here changes depth memory,
   which the depth oracle reads directly.
3. HiZ -- `hiz_ram` is 0 on `CHIP_RS480`, so HiZ has no RAM on this part
   and no HiZ stage is reachable here. The step exists for the discrete
   R3xx and R5xx parts that carry HiZ RAM.

Each step keeps the ownership requirement stages C and D establish, and
each is measured against A by the same two oracles.
