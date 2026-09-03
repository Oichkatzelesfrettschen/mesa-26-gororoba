/*
 * SPDX-License-Identifier: MIT
 *
 * RB2D solid-fill plan: a linear destination surface, a list of rectangles,
 * and the register sequence the 2D engine executes them through.  The plan
 * carries no render geometry of its own -- no triangle, no vertex, no 3D
 * state -- so a caller names a surface and a rectangle and the emitter
 * produces the same stream the fixed direct-write control cell produces.
 *
 * The surface contract is the packing of DST_PITCH_OFFSET, which
 * r100_copy_blit writes as (pitch << 22) | (offset >> 10): the pitch field
 * counts 64-byte units and the offset field counts 1 KiB units.  A pitch
 * outside the 64-byte grid or an offset outside the 1 KiB grid has no
 * representation in that word, so the plan refuses it rather than
 * truncating a value the hardware would then read as a different surface.
 *
 * The register contract and its kernel-source derivation live in
 * docs/hardware/r300-direct-write-2d-fill-authority.md.
 */

#ifndef R300_RB2D_FILL_H
#define R300_RB2D_FILL_H

#include <stdbool.h>
#include <stdint.h>

/* DST_PITCH_OFFSET's two grids.  Both are the word's own packing, not a
 * driver policy, so both are checked at plan validation. */
#define R300_RB2D_PITCH_GRANULARITY 64u
#define R300_RB2D_OFFSET_GRANULARITY 1024u

/* The far edge a rectangle may reach on either axis.  DST_Y_X and
 * DST_WIDTH_HEIGHT carry 16-bit fields, but the emitter opens the 2D
 * scissor through SC_BOTTOM_RIGHT and DEFAULT_SC_BOTTOM_RIGHT, whose halves
 * are 13 bits: radeon_reg.h spells the ceiling as
 * RADEON_DEFAULT_SC_RIGHT_MAX (0x1fff << 0) and
 * RADEON_DEFAULT_SC_BOTTOM_MAX (0x1fff << 16).  A rectangle reaching past
 * that is clipped by the scissor the same stream established, so the fill
 * lands short with no error anywhere, and the plan refuses it instead.
 *
 * The bound is the scissor value rather than one past it: whether
 * SC_BOTTOM_RIGHT names the last written pixel or the first unwritten one
 * is not settled by a primary source here, and the tighter reading is the
 * one that cannot over-admit.
 */
#define R300_RB2D_MAX_COORD_REACH 0x1fffu

/* The destination formats DP_GUI_MASTER_CNTL's format field names that this
 * plan emits.  ARGB8888 is the one the retained cell exercises; a format
 * lands here with the row that fills in it. */
enum r300_rb2d_format {
   R300_RB2D_FORMAT_ARGB8888 = 0,
   R300_RB2D_FORMAT_COUNT,
};

/* A linear destination the 2D engine writes into.  base_offset_bytes is the
 * distance from the relocated buffer base; pitch_bytes is the row stride;
 * width and height bound the rectangles the plan admits. */
struct r300_rb2d_surface {
   uint32_t base_offset_bytes;
   uint32_t pitch_bytes;
   uint32_t width_pixels;
   uint32_t height_pixels;
   enum r300_rb2d_format format;
};

/* One solid-color rectangle.  DST_Y_X carries the origin and
 * DST_WIDTH_HEIGHT both launches the fill and carries its extent, so a
 * zero-extent rectangle names no work and the plan refuses it. */
struct r300_rb2d_fill_rect {
   uint32_t x;
   uint32_t y;
   uint32_t width;
   uint32_t height;
   uint32_t value;
};

/* The plan's relocation slot vocabulary: the destination surface is the one
 * buffer the stream names, through DST_PITCH_OFFSET's payload. */
enum r300_rb2d_fill_slot {
   R300_RB2D_FILL_SLOT_DST = 0,
   R300_RB2D_FILL_SLOT_COUNT,
};

#define R300_RB2D_FILL_MAX_RECTS 8u

struct r300_rb2d_fill_plan {
   struct r300_rb2d_surface surface;
   /* DP_WRITE_MSK; the lanes the fill is allowed to change. */
   uint32_t write_mask;
   const struct r300_rb2d_fill_rect *rects;
   uint32_t rect_count;
};

/* One IB position whose payload names a relocation slot. */
struct r300_rb2d_fill_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

struct r300_rb2d_fill_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_rb2d_fill_reloc_site reloc_sites[R300_RB2D_FILL_SLOT_COUNT];
   uint32_t reloc_site_count;
};

/* Dwords the emitter writes for a plan: the fixed prologue and epilogue plus
 * three registers per rectangle, each register costing a PACKET0 header and
 * its value, and the relocation NOP costing a header and a payload. */
#define R300_RB2D_FILL_DWORDS(rects) (20u + 6u * (uint32_t)(rects))

/* Every rule the plan holds a caller to, in the order the validator tests
 * them, so a refusal names one fact.  A caller reads the reason; the
 * enumerated form lets a test name the arm it exercises. */
enum r300_rb2d_fill_refusal {
   R300_RB2D_FILL_OK = 0,
   R300_RB2D_FILL_REFUSE_NO_RECTS,
   R300_RB2D_FILL_REFUSE_TOO_MANY_RECTS,
   R300_RB2D_FILL_REFUSE_FORMAT,
   R300_RB2D_FILL_REFUSE_PITCH_GRID,
   R300_RB2D_FILL_REFUSE_PITCH_ZERO,
   R300_RB2D_FILL_REFUSE_PITCH_FIELD,
   R300_RB2D_FILL_REFUSE_OFFSET_GRID,
   R300_RB2D_FILL_REFUSE_OFFSET_FIELD,
   R300_RB2D_FILL_REFUSE_EXTENT_ZERO,
   R300_RB2D_FILL_REFUSE_PITCH_BELOW_WIDTH,
   R300_RB2D_FILL_REFUSE_RECT_EMPTY,
   R300_RB2D_FILL_REFUSE_RECT_FIELD,
   R300_RB2D_FILL_REFUSE_RECT_BEYOND_SCISSOR,
   R300_RB2D_FILL_REFUSE_RECT_OUTSIDE,
   R300_RB2D_FILL_REFUSAL_COUNT,
};

const char *r300_rb2d_fill_refusal_name(enum r300_rb2d_fill_refusal r);

/* Holds a plan to the surface contract above and to the field widths
 * DST_Y_X and DST_WIDTH_HEIGHT carry.  Returns R300_RB2D_FILL_OK, or the
 * first rule the plan violates. */
enum r300_rb2d_fill_refusal
r300_rb2d_fill_plan_check(const struct r300_rb2d_fill_plan *plan);

/* Emits a validated plan into caller storage of exactly capacity dwords.
 * Refuses with -ENOSPC before any half-written stream is reported, and with
 * -EINVAL when the plan does not pass r300_rb2d_fill_plan_check.  The
 * emission order is fixed: destination pitch and offset bound by
 * relocation, the 2D scissor established rather than inherited,
 * solid-brush raster state, the rectangles in plan order, then the 2D
 * destination-cache flush and the engine-idle wait.
 */
int r300_rb2d_fill_emit_into(const struct r300_rb2d_fill_plan *plan,
                             uint32_t *words, uint32_t capacity,
                             struct r300_rb2d_fill_ib *out);

/* Checks emitted relocation sites against the stream: one site, inside the
 * stream, naming the destination slot at the recorded index, with the
 * PACKET3 NOP header one dword before its payload. */
int r300_rb2d_fill_validate_reloc_sites(const struct r300_rb2d_fill_ib *ib);

#endif /* R300_RB2D_FILL_H */
