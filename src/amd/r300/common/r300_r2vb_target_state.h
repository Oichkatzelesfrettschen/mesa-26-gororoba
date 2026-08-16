/*
 * SPDX-License-Identifier: MIT
 *
 * Neutral R2VB producer render-target prologue and cache-publication
 * tail for R300-class silicon.
 */

#ifndef R300_R2VB_TARGET_STATE_H
#define R300_R2VB_TARGET_STATE_H

#include "r300_pm4_builder.h"
#include "r300_r2vb_fetch_pass.h"

#include <stdint.h>

/* One prepacked four-dword constant write, e.g. an identity wpos
 * override into a PFS_PARAM quad.  The caller packs the values (FP24 on
 * R300-class parts); the emitter writes them verbatim.
 */
struct r300_r2vb_target_const_write {
   uint32_t reg;
   uint32_t value[4];
};

/* The producer output-target contract: retarget the color buffer to the
 * carrier behind the destination-cache flush barrier, then pin the
 * one-target, one-sample, full-write raster state the producer draw
 * depends on, independent of application blend, alpha-test, logic-op,
 * MRT, and sample state.
 */
struct r300_r2vb_target_state_params {
   /* Scissor extent in pixels; the emitter applies the non-R500 1440
    * coordinate bias.  Extents past 2656 have no biased encoding.
    */
   uint32_t width;
   uint32_t height;
   /* RB3D_COLORPITCH0: pitch in pixels, or'd with the color format
    * (e.g. R300_COLOR_FORMAT_ARGB32323232).
    */
   uint32_t pitch_pixels;
   uint32_t color_format;
   /* The complete US_OUT_FMT_0 word: format plus channel selects.  The
    * producer drives one output, so FMT_1..3 are written UNUSED.
    */
   uint32_t us_out_fmt0;
   /* RB3D_CCTL is chip-derived (independent color format enable on
    * R500), so the caller supplies the packed word.
    */
   uint32_t rb3d_cctl;
   /* Carrier binding: byte offset within the BO and the caller's
    * relocation payload for the NOP-form relocation after
    * RB3D_COLOROFFSET0.
    */
   uint32_t color_offset_bytes;
   uint32_t color_relocation_payload;
   /* GA point size in sixths of a pixel; 6 is one pixel.  The minmax
    * word takes 6 as its floor.
    */
   uint32_t point_size_sixths;
   /* Prepacked constant writes appended after the fixed state. */
   const struct r300_r2vb_target_const_write *const_writes;
   uint32_t const_write_count;
};

/* The fixed prologue is 55 dwords; each constant write adds five. */
#define R300_R2VB_TARGET_STATE_FIXED_DWORDS 55u
#define R300_R2VB_TARGET_CONST_WRITE_DWORDS 5u

static inline uint32_t
r300_r2vb_target_state_dwords(uint32_t const_write_count)
{
   return R300_R2VB_TARGET_STATE_FIXED_DWORDS +
          const_write_count * R300_R2VB_TARGET_CONST_WRITE_DWORDS;
}

/* Emits the prologue.  Rejects a zero or biased-unencodable extent, a
 * zero point size, and a null constant list with a nonzero count, all
 * before writing any dword.  Reports the color relocation payload's IB
 * index through *color_reloc_index with role R300_R2VB_BO_CARRIER
 * semantics.  Returns 0 or a negative errno.
 */
int r300_r2vb_target_state_emit(
   struct r300_pm4_builder *b,
   const struct r300_r2vb_target_state_params *params,
   uint32_t *color_reloc_index);

/* Producer cache-publication tail: push the color write out of the
 * ZB/RB3D caches, wait 3D idle-clean, then write zero to
 * VAP_PVS_STATE_FLUSH_REG.  The cache flushes leave the vertex cache
 * untouched, and a later vertex fetch of this same BO can return stale
 * vertices the vertex cache kept from an earlier fetch of the recycled
 * GART page; the VAP flush clears that state, so the producer-to-fetch
 * route stays deterministic.  This is the production-safe sequence; a
 * diagnostic variant that weakens any member lives outside production
 * code as an explicitly unsafe test harness.
 */
#define R300_R2VB_PUBLICATION_TAIL_DWORDS 8u

int r300_r2vb_publication_tail_emit(struct r300_pm4_builder *b);

#endif /* R300_R2VB_TARGET_STATE_H */
