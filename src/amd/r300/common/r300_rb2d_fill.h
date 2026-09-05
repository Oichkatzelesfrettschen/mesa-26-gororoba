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

#include "r300_pm4_builder.h"

#include <stdbool.h>
#include <stdint.h>

/* DST_PITCH_OFFSET's two grids.  Both are the word's own packing, not a
 * driver policy, so both are checked at plan validation. */
#define R300_RB2D_PITCH_GRANULARITY 64u
#define R300_RB2D_OFFSET_GRANULARITY 1024u

/* DST_PITCH_OFFSET splits into an 8-bit pitch field at bits 22-29 above a
 * 22-bit offset field, with bits 30-31 carrying DST_TILE_MACRO and
 * DST_TILE_MICRO.  r100_reloc_pitch_offset fixes that split: it rebuilds
 * the word as (value & 0x3fc00000) | offset | tile_flags on the ordinary
 * path, so the pitch it forwards is bits 22-29 and the two tile bits come
 * from the relocation's own tiling rather than from the stream.  A ninth
 * pitch bit therefore both truncates -- 256 units reaches the kernel as
 * pitch zero -- and sets a tile bit the relocation then overwrites, so the
 * surface the engine reads is not the surface the caller named.  A caller
 * building a surface reads the bound here rather than restating the field
 * split. */
#define R300_RB2D_MAX_PITCH_UNITS 0xffu
#define R300_RB2D_MAX_OFFSET_UNITS 0x3fffffu

/* Two facts about the same number, kept apart because one is measured and
 * one is chosen.
 *
 * R300_RB2D_SCISSOR_FIELD_MAX is the encoding: DST_Y_X and
 * DST_WIDTH_HEIGHT carry 16-bit fields, so a rectangle can name a
 * coordinate to 0xffff, while the emitter opens the 2D scissor through
 * SC_BOTTOM_RIGHT and DEFAULT_SC_BOTTOM_RIGHT, whose halves are 13 bits.
 * radeon_reg.h spells that width as RADEON_DEFAULT_SC_RIGHT_MAX
 * (0x1fff << 0) and RADEON_DEFAULT_SC_BOTTOM_MAX (0x1fff << 16).  A
 * rectangle between the two widths is clipped by the scissor the same
 * stream established, so the fill lands short and every layer reports
 * success.
 *
 * R300_RB2D_SAFE_EXCLUSIVE_END is the choice: the largest exclusive far
 * edge x + width or y + height the plan admits.  Whether SC_BOTTOM_RIGHT
 * names the last written pixel or the first unwritten one decides whether
 * an exclusive end of exactly 0x2000 is legal, and radeon_reg.h gives the
 * field width without its inclusivity.  The plan takes the reading that
 * cannot over-admit and gives up at most one row and one column of a
 * full-reach surface -- 64 bytes of a 512 KiB carrier window at the
 * tightest pitch.
 *
 * One fill whose exclusive far edge is exactly 0x2000, read back against
 * a sentinel, decides it: an inclusive register writes the last column and
 * an exclusive one leaves it.  Until that runs the conservative bound
 * stands, and the two names keep the measured encoding and the chosen
 * bound from being read as one fact.
 */
#define R300_RB2D_SCISSOR_FIELD_MAX 0x1fffu
#define R300_RB2D_SAFE_EXCLUSIVE_END R300_RB2D_SCISSOR_FIELD_MAX

/* The destination formats DP_GUI_MASTER_CNTL's format field names that this
 * plan emits.  ARGB8888 is the one the retained cell exercises; a format
 * lands here with the row that fills in it. */
enum r300_rb2d_format {
   R300_RB2D_FORMAT_ARGB8888 = 0,
   /* Two bytes per pixel.  The kernel replay admits a 128-pixel RGB565 row
    * on a 256-byte pitch and refuses pixel 129, so the tracker reads the
    * stride; a fill stream on this carrier has no silicon receipt yet, and
    * the pitch-evidence registry withholds it from execution. */
   R300_RB2D_FORMAT_RGB565,
   R300_RB2D_FORMAT_COUNT,
};

/* Bytes one pixel of a format occupies, or zero outside the table. */
uint32_t r300_rb2d_format_bytes_per_pixel(enum r300_rb2d_format format);

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

/* The emitter as a state machine.  The 2D engine consumes registers in a
 * fixed dependency order, and the kernel tracker refuses a launch that
 * precedes its destination, format, or origin:
 *
 *    DESTINATION  DST_PITCH_OFFSET + relocation      dst_epoch
 *    FORMAT       DP_GUI_MASTER_CNTL                 format_epoch
 *    COMMON       scissor, DP_CNTL, DP_WRITE_MSK
 *    ORIGIN       DST_Y_X                            origin_epoch
 *    LAUNCH       DST_WIDTH_HEIGHT                   consumes all three
 *    EPILOGUE     DSTCACHE flush, WAIT_UNTIL
 *
 * Each epoch counts the writes of its register since init.  A new
 * destination clears the format and origin epochs, so a launch after a
 * surface rebind requires the format and origin re-established on the new
 * surface, and a launch with any epoch at zero records -EINVAL in the
 * builder.  The four r300_rb2d_emit_* helpers are the only writers of these
 * registers; r300_rb2d_fill_emit_into is their canonical sequence.
 */
struct r300_rb2d_emitter {
   struct r300_pm4_builder builder;
   struct r300_rb2d_fill_ib *out;
   /* The surface the current destination epoch bound; the common state
    * reads its format from here, so the format written is the bound
    * surface's and never a caller-supplied other one. */
   struct r300_rb2d_surface bound;
   uint32_t dst_epoch;
   uint32_t format_epoch;
   uint32_t origin_epoch;
};

/* A typed destination binding: the relocation slot the surface's
 * DST_PITCH_OFFSET payload names.  DST_PITCH_OFFSET has no emission path
 * without one, so a stream that names a destination always carries its
 * relocation site. */
struct r300_rb2d_relocation {
   uint32_t slot;
};

void r300_rb2d_emitter_init(struct r300_rb2d_emitter *e, uint32_t *words,
                            uint32_t capacity,
                            struct r300_rb2d_fill_ib *out);

/* DESTINATION: DST_PITCH_OFFSET bound to a relocation.  Refuses a surface
 * off the pitch or offset grid, a slot outside the plan vocabulary, or a
 * second site when the output has no room for it. */
void r300_rb2d_emit_surface_state(struct r300_rb2d_emitter *e,
                                  const struct r300_rb2d_surface *surface,
                                  struct r300_rb2d_relocation relocation);

/* FORMAT and COMMON: the 2D scissor opened at its field maximum, the
 * solid-brush master control carrying the bound surface's format, the
 * raster direction, and the write mask.  Refuses before a destination is
 * bound. */
void r300_rb2d_emit_common_state(struct r300_rb2d_emitter *e,
                                 uint32_t write_mask);

/* ORIGIN and LAUNCH for one rectangle: brush color, DST_Y_X, then the
 * DST_WIDTH_HEIGHT write that launches the fill.  Refuses while any epoch
 * is zero. */
void r300_rb2d_emit_rect(struct r300_rb2d_emitter *e,
                         const struct r300_rb2d_fill_rect *rect);

/* EPILOGUE: 2D destination-cache flush and the engine-idle wait. */
void r300_rb2d_emit_epilogue(struct r300_rb2d_emitter *e);

/* Closes the stream: -ENOSPC or -EINVAL from the builder, else the dword
 * count in out->ib_size_dwords. */
int r300_rb2d_emitter_finish(struct r300_rb2d_emitter *e);

/* Checks emitted relocation sites against the stream: one site, inside the
 * stream, naming the destination slot at the recorded index, with the
 * PACKET3 NOP header one dword before its payload. */
int r300_rb2d_fill_validate_reloc_sites(const struct r300_rb2d_fill_ib *ib);

#endif /* R300_RB2D_FILL_H */
