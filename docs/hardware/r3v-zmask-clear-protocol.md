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
pixel area by the pixels one ZMASK dword covers.
`r300_zmask_layout_compute_at_block` takes the block as an argument
instead: `R300_ZCOMP_4X4` pins the smaller block whatever the level
would have chosen, and `R300_ZCOMP_8X8` asks for the larger one and
yields the level's own decision. Every stage in this ladder consumes the
4x4 form, for the reason stated under stage C. The result fits when
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
the cell writes its pre-draw depth per pixel and reads the result back as
a row-major image, so both halves need the coordinate transform, and the
descriptor answers `false` to `logical_pixel_addressing` and
`logical_image_readback`. No logical-to-physical address transform for
R300-class tiling exists in this tree. Gallium never computes one --
`r300_transfer.c` routes a tiled map through a linear shadow texture and
lets the engine move the bytes, and `radeon_surface.c` begins at
`CHIP_R600` -- so `r300_get_pixel_alignment`'s tile-dimension table is
the only tiling fact available to cross-check against, and
`r300_zmask_layout.c` already consumes that much.

The transform is derived from documentation and adjudicated on silicon,
in that order. The AMD R3xx 3D register reference fixes `ZB_DEPTHOFFSET`,
`ZB_DEPTHPITCH`, `ZB_FORMAT`, and the endian control; AMD Radeon R5xx
Acceleration revision 1.5 section 2, which states its scope as R3xx
through R5xx, fixes the 32-byte microblock, the 2 KiB macroblock, the 4x2
microtile for 32-bit pixels, and the 32x16 macro-tiled block. Those
constrain block size, alignment, and the legal combinations; they do not
determine the intra-tile address permutation, which stays a labeled
hypothesis until measured. The measurement runs through the ZB path
itself: a uniformly initialized allocation, one tightly scissored logical
pixel written at depth compare `ALWAYS` with compression and HyperZ off,
then a raw mapping read to find the changed packed word. A tiled RB2D
write establishes the RB2D engine's own address interpretation and is
kept as a separate cross-engine comparison, so it is never promoted to
`ZB_DEPTHPITCH` authority without an explicit equivalence test. The model
is keyed on engine, surface format, microtile mode, macrotile mode,
pitch, sample count, and base alignment, so no key is implicit.

Uniform initialization does not wait on that transform. An uncompressed
surface whose tiling permutes complete packed pixels carries a constant
image invariant under the permutation, so a host that writes one repeated
packed word across the whole allocation, padding included, produces the
same image whatever the permutation is. The descriptor states that as
`uniform_packed_initialization` beside `raw_allocation_mapping`, and both
hold on the Z24 surface while the two addressing capabilities stay false.

The allocation grows with the format: four bytes per pixel doubles the
depth BO, and macrotiling imposes its own pitch alignment on top. The
depth oracle also widens from `uint16_t` to the 32-bit word Z24 stores,
and it reads through the address model rather than through
`y * pitch + x`.

The sentinel is a depth code and the memory word is its packed form. R300
stores `S8_UINT_Z24_UNORM` with depth in bits 31:8 and stencil in bits
7:0, so the half-scale code `0x800000` -- the Z24 counterpart of Z16's
`0x8000`, sitting between the near depth 0.25 and the far depth 0.75,
which is the only property `R300_ZS_LESS` reads out of it -- reaches
memory as `0x80000000` under a zero stencil. `r300_zb_depth_pack` and
`r300_zb_depth_unpack` carry that conversion and refuse a value wider
than its field; every sentinel named below is the 24-bit code, never the
word.

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
block size the layout was computed at -- the 64x64 reference level clears
four dwords at 8x8 and sixteen at 4x4, so a register and a coverage taken
from different blocks describe different surfaces -- writes
`ZB_BW_CNTL` = 0 so
`FAST_FILL_ENABLE`, `RD_COMP_ENABLE`, `WR_COMP_ENABLE` and `HIZ_ENABLE`
all stay off, and issues `3D_CLEAR_ZMASK` over exactly
`layout.dwords` dwords starting at index 0 with value 0.
`SC_HYPERZ` stays unwritten: the scan converter's HiZ bit belongs to the
HiZ stage past this ladder.

The block this stage programs is `R300_ZCOMP_4X4`, whatever the level's
own decision would be. The R5xx acceleration guide requires 4x4 plane
equations while compression is disabled, so the GA and the ZB agree on
the plane-equation format, and `ZB_BW_CNTL` = 0 leaves both
`RD_COMP_ENABLE` and `WR_COMP_ENABLE` clear here and through stage D,
whose `FAST_FILL_ENABLE` enables no compression either. So both binding
stages answer `R300_ZCOMP_4X4` from
`r300_zmask_clear_stage_block`, and the 64x64 macrotiled reference level
binds sixteen dwords rather than the four its 8x8 decision would name.
`r300_zmask_clear_plan_build` refuses a layout resolved at the other
block, so the plane-equation register and the clear coverage cannot come
apart.

In-tree `r300_update_hyperz` disagrees: it sets `Z_PEQ_SIZE_8_8` from
`tex.zcomp8x8[level]` whenever HyperZ is enabled, before deciding which
enables `ZB_BW_CNTL` carries, so the Gallium path programs 8x8 plane
equations in configurations where the guide asks for 4x4. The guide is
the higher-ranked authority and decides the value the ladder emits. No
retained silicon observation of either configuration exists, so the
disagreement is a recorded conflict rather than a settled question, and
a stage that enables compression is where 8x8 first becomes admissible.
That stage is not in this ladder.

With every compression enable off, the depth pipe reads and writes
depth memory as it did in A, so the expected observation against A is an
identical color and depth image. The stage measures that the bind and
the clear packet traverse the kernel and the ring without disturbing the
draw. `ZB_ZMASK_PITCH` is nonzero and `3D_CLEAR_ZMASK` is a gated
packet3, so this stream admits under ownership and refuses without it --
`r300_zb_hyperz_admit_stream` reports `REFUSE_OWNERSHIP` at the pitch
write, which precedes the tile-size write. `GB_Z_PEQ_CONFIG` is gated
the same way, and its 8x8 value refuses on its own while its 4x4 value
of zero admits, so this ladder's refusal rests on the pitch and the
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

## Plan aspect obligations

Stage D's substitution is what bounds which operations a ZMASK plan may
carry. FASTFILL returns `ZB_DEPTHCLEARVALUE` in place of the value stored in
depth memory for every tile whose ZMASK bits are zero, and that value is one
packed D24S8 word, so the substitution replaces the depth aspect and the
stencil aspect of every pixel of the surface together. An operation whose
own semantics leave either aspect, or any pixel outside its region, holding
its previous value cannot be expressed as that substitution, and resolves
through materialization onto the ordinary depth path.

The Vulkan side of the boundary rests on three clauses of the 1.0 core
specification. Vulkan 1.0, Resource Creation, Image Layouts stores an
optimal-layout image in an implementation-dependent opaque layout and makes
image layout per-image subresource, so the ZMASK representation is a legal
storage form and both aspects of one subresource share it. Vulkan 1.0,
Resource Creation, Image Views defines `VkImageSubresourceRange` and
`VkImageSubresourceLayers` `aspectMask`, through which an operation names
the depth aspect, the stencil aspect, or both. Vulkan 1.0, Render Pass,
Render Pass Store Operations states that a store updates values within the
render area alone and that, for a depth/stencil image, a write to one aspect
may result in a read-modify-write of the other -- which the packed D24S8
word makes unconditional on this part. Vulkan 1.0, Clear Commands, Clearing
Images Outside a Render Pass Instance supplies the one shape that meets the
substitution: `vkCmdClearDepthStencilImage` writes a constant over whole mip
levels of the aspects its ranges name.

`struct r3v_native_zmask_plan_obligations` declares the seven facts an
operation carries, `r3v_native_zmask_plan_obligations_init` derives them
from the operation kind, aspect mask, logical region, and representation,
and `r3v_native_zmask_plan_admit` returns the route.

| Operation | Aspects | Region | Route |
| --- | --- | --- | --- |
| clear | depth + stencil | whole surface | ZMASK fast clear |
| clear | depth | any | ordinary through materialization |
| clear | stencil | any | ordinary through materialization |
| clear | depth + stencil | subrectangle | ordinary through materialization |
| store | any | any | ordinary through materialization |
| transfer read | any | any | ordinary through materialization |

`requires_materialization` stands apart from the route and names a
metadata-backed representation -- `ZMASK_FAST_CLEAR` or `ZMASK_COMPRESSED`
-- whose ordinary path resolves the metadata into depth memory first. It
holds for a fast-clear-admitted operation that the caller records through
the ordinary route, which is how a full both-aspect clear of a
metadata-backed image reaches the RB2D fill.
`r3v_native_cmd_buffer_require_ordinary_depth_backing` is the single point
that applies it: a retired tiled image passes through, a `ZMASK_FAST_CLEAR`
image materializes, and a `ZMASK_COMPRESSED` image refuses with
`VK_ERROR_FEATURE_NOT_PRESENT` because no compressed resolve route exists.

### Operations that stay on the ordinary path

Each row below stays ordinary until a separate demonstration moves it, and
each names what that demonstration would have to establish.

- Depth-only and stencil-only clears. The substitution writes both halves of
  the packed word, so a single-aspect plan needs a ZMASK-side mechanism that
  leaves the other half alone. None is in this tree.
- Partial clears. `3D_CLEAR_ZMASK` covers a dword range of the ZMASK RAM,
  and each dword covers a compression block, so a rectangle that splits a
  block has no exact ZMASK expression. A demonstration would need the
  block-aligned subrectangle case measured on silicon.
- Partial stores and attachment access. A ZB draw reads and rewrites the
  packed word, and store operations leave every location outside the render
  area untouched, so the surface owes ordinary depth bytes for the whole
  logical extent before the pass records.
- Transfers that read one aspect of a metadata-backed image. The host reads
  depth memory directly and the ZMASK resolve does not intercept that read,
  so the metadata resolves into memory first.
- Compressed representations. `RD_COMP_ENABLE` and `WR_COMP_ENABLE` are off
  through stages C and D, so no compressed tile is ever produced and no
  resolve for one exists. The refusal is explicit rather than silent.
