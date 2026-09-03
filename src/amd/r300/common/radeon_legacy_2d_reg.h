/*
 * SPDX-License-Identifier: MIT
 *
 * The legacy Radeon 2D/GUI register block: direct MMIO byte offsets into the
 * register aperture the radeon driver maps from PCI BAR 2 on every ASIC below
 * CHIP_BONAIRE (radeon_device_init, rdev->rmmio_base), together with the field
 * and value codes those words carry.  A command stream reaches the same words
 * through PACKET0, whose header carries the offset as a dword index -- the
 * kernel's PACKET0 macro spells that as (register) >> 2 -- so the byte offset
 * is the durable identity and the packet encoding is derived from it.
 *
 * The vocabulary belongs to the 2D engine rather than to one plan, so solid
 * fill, copy, and raw linear transfer name the same offsets from here.
 *
 * Eleven of the twelve offsets sit on the R300 command-stream safe list
 * drivers/gpu/drm/radeon/reg_srcs/r300 and reach the ring unchecked.
 * DST_PITCH_OFFSET is absent from that list because its word names a buffer:
 * r300_packet0_check routes it to r100_reloc_pitch_offset, which consumes one
 * relocation and rebuilds the offset field from the validated base, so a
 * stream writing it carries a relocation for it.  Both halves of that claim
 * are checkable at the kernel tree: rg -e 0x1438 -e 0x146C -e 0x147C
 * -e 0x1598 -e 0x16C0 -e 0x16CC -e 0x16E8 -e 0x16EC -e 0x16F0 -e 0x1714
 * -e 0x1720 drivers/gpu/drm/radeon/reg_srcs/r300 returns one line per
 * admitted register and none for 0x142C, and rg --fixed-strings
 * r100_reloc_pitch_offset drivers/gpu/drm/radeon/r300.c returns its case.
 *
 * Two offsets answer to a second kernel name.  reg_srcs/r300 spells 0x16CC
 * DP_WRITE_MSK while radeon_reg.h spells the same word DP_WRITE_MASK, and
 * radeon_reg.h gives 0x1714 both DSTCACHE_CTLSTAT and FLUSH_5.  The safe-list
 * spelling is the one used here, because the safe list is what admits the
 * write.
 *
 * WAIT_UNTIL and its idle bits are spelled token for token as r300_reg.h
 * spells them, so a translation unit including both headers redefines them
 * identically instead of failing -Wmacro-redefined.
 */

#ifndef RADEON_LEGACY_2D_REG_H
#define RADEON_LEGACY_2D_REG_H

/* Destination geometry: DST_PITCH_OFFSET names the surface, DST_Y_X its
 * origin, and the write to DST_WIDTH_HEIGHT launches the operation.  The
 * grids DST_PITCH_OFFSET packs a surface onto live at
 * R300_RB2D_PITCH_GRANULARITY in r300_rb2d_fill.h, beside the plan that
 * measures a surface against them. */
#define RADEON_DST_PITCH_OFFSET 0x142C
#define RADEON_DST_Y_X 0x1438
#define RADEON_DST_WIDTH_HEIGHT 0x1598

/* GUI master control and its companions: DP_GUI_MASTER_CNTL selects brush,
 * destination datatype, and raster op; DP_BRUSH_FRGD_CLR carries the solid
 * brush's color in destination byte order; DP_CNTL carries the walk
 * direction; DP_WRITE_MSK carries the destination lanes the operation may
 * change. */
#define RADEON_DP_GUI_MASTER_CNTL 0x146C
#define RADEON_DP_BRUSH_FRGD_CLR 0x147C
#define RADEON_DP_CNTL 0x16C0
#define RADEON_DP_WRITE_MSK 0x16CC

/* The 2D scissor, each word packing right or bottom in the low half and the
 * other axis in the high half: radeon_reg.h fixes that split at
 * DEFAULT_SC_RIGHT_MAX (0x1fff << 0) beside DEFAULT_SC_BOTTOM_MAX
 * (0x1fff << 16), and r100_copy_blit writes both scissors as
 * (0x1fff) | (0x1fff << 16).  That 0x1fff is the field maximum
 * R300_RB2D_SCISSOR_FIELD_MAX states in r300_rb2d_fill.h. */
#define RADEON_DEFAULT_SC_BOTTOM_RIGHT 0x16E8
#define RADEON_SC_TOP_LEFT 0x16EC
#define RADEON_SC_BOTTOM_RIGHT 0x16F0

/* Completion, in the order r100_copy_blit ends a blit with: flush the 2D
 * destination cache through DSTCACHE_CTLSTAT, then hold the stream at
 * WAIT_UNTIL until the named engines drain. */
#define RADEON_DSTCACHE_CTLSTAT 0x1714
#define RADEON_WAIT_UNTIL 0x1720

/* DP_GUI_MASTER_CNTL fields.  The destination surface comes from
 * DST_PITCH_OFFSET, brush datatype 13 is the solid color DP_BRUSH_FRGD_CLR
 * holds, and ROP3 P writes that brush through unmodified.  r100_copy_blit
 * sets CLR_CMP_CNTL_DIS and WR_MSK_DIS together, so the operation runs with
 * the color compare and the GMC write mask both retired. */
#define RADEON_GMC_DST_PITCH_OFFSET_CNTL (1u << 1)
#define RADEON_GMC_BRUSH_SOLID_COLOR (13u << 4)
#define RADEON_ROP3_P 0x00f00000u
#define RADEON_GMC_CLR_CMP_CNTL_DIS (1u << 28)
#define RADEON_GMC_WR_MSK_DIS (1u << 30)

/* The destination datatype code DP_GUI_MASTER_CNTL carries at bit 8, which
 * r100_copy_blit writes as (RADEON_COLOR_FORMAT_ARGB8888 << 8). */
#define RADEON_COLOR_FORMAT_ARGB8888 6u

/* DP_CNTL: the walk order across the destination rectangle. */
#define RADEON_DST_X_LEFT_TO_RIGHT (1u << 0)
#define RADEON_DST_Y_TOP_TO_BOTTOM (1u << 1)

/* DSTCACHE_CTLSTAT's flush-all pattern, which radeon_reg.h decomposes at the
 * RB2D block's own copy of the word into DC_FLUSH (3 << 0) beside DC_FREE
 * (3 << 2): the cache flushes and then frees its lines. */
#define RADEON_RB2D_DC_FLUSH_ALL 0xfu

/* WAIT_UNTIL bits: the 2D engine and the host path each drain clean, and the
 * GUI DMA engine goes idle. */
#define RADEON_WAIT_DMA_GUI_IDLE (1 << 9)
#define RADEON_WAIT_2D_IDLECLEAN (1 << 16)
#define RADEON_WAIT_HOST_IDLECLEAN (1 << 18)

#endif /* RADEON_LEGACY_2D_REG_H */
