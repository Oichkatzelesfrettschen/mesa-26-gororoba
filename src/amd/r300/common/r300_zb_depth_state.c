/*
 * SPDX-License-Identifier: MIT
 *
 * Neutral R300-class depth-buffer binding and depth-test state.
 */

#include "r300_zb_depth_state.h"

#include "r300_reg.h"

#include <errno.h>
#include <stddef.h>

/* ZB_FORMAT, ZB_DEPTHOFFSET, its relocation NOP (header plus payload),
 * ZB_DEPTHPITCH, ZB_CNTL, ZB_ZSTENCILCNTL, and ZB_BW_CNTL, each a
 * one-dword PACKET0 run of header plus value.
 */
#define DEPTH_STATE_REGISTER_WRITES 6u
#define DEPTH_STATE_RELOCATION_DWORDS 2u

/* R300_DEPTHPITCH_MASK reaches bits 2 through 13, so a pitch is a
 * multiple of four and at most 16380 pixels.
 */
#define DEPTH_PITCH_ALIGNMENT 4u
#define DEPTH_PITCH_MAX (R300_DEPTHPITCH_MASK)

/* R300_ZB_DEPTHOFFSET encodes bits 31 to 5, so the low five bits of an
 * offset have no encoding.
 */
#define DEPTH_OFFSET_ALIGNMENT 32u

/* A macrotiled surface begins on a macrotile boundary.  Two facts in the
 * R300 tree establish it.  r300_setup_cbzb_flags records the ZB unit's
 * own behavior on one path: the midpoint offset the CBZB fast clear
 * hands it returns garbage at certain surface sizes unless it is
 * 2048-aligned, and macrotiling is what supplies the alignment.  And
 * r300_setup_miptree places every macrotiled level on that grid, because
 * r300_texture_macro_switch compares u_minify(dim, level) against a
 * level-independent threshold, so macrotiled levels form a prefix and no
 * macrotiled offset ever follows a 32-byte-granular linear level.
 *
 * This bounds the surface-relative offset alone, which is what reaches
 * the register.  The address the hardware sees is that offset plus the
 * buffer object's own base, and radeon_bo_create rounds a GEM object's
 * placement alignment up to PAGE_SIZE, so the object base carries at
 * least 4096-byte granularity and the sum stays on the macrotile grid.
 */
#define DEPTH_MACROTILE_OFFSET_ALIGNMENT 2048u

/* The four encodings the emitter admits.  Exactly one of them stores four
 * bytes per pixel, which is what lets the square-microtile refusal below
 * name that encoding rather than derive a pixel width: a fifth entry at
 * four bytes per pixel would need the refusal widened with it.
 */
static bool
depth_format_supported(uint32_t format)
{
   switch (format) {
   case R300_DEPTHFORMAT_16BIT_INT_Z:
   case R300_DEPTHFORMAT_16BIT_13E3 | R300_INVERT_13E3_LEADING_ONES:
   case R300_DEPTHFORMAT_16BIT_13E3 | R300_INVERT_13E3_LEADING_ZEROS:
   case R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL:
      return true;
   default:
      return false;
   }
}

uint32_t
r300_zb_depth_state_dwords(void)
{
   return DEPTH_STATE_REGISTER_WRITES * 2u + DEPTH_STATE_RELOCATION_DWORDS;
}

int
r300_zb_depth_state_emit(struct r300_pm4_builder *builder,
                         const struct r300_zb_depth_state_params *params,
                         uint32_t *out_reloc_ib_index)
{
   if (builder == NULL || params == NULL)
      return -EINVAL;

   /* Validation precedes the first dword, so a refused call leaves the
    * builder exactly as it was and a caller that ignores the status
    * cannot submit a half-written depth binding.
    */
   if (!depth_format_supported(params->depth_format) ||
       params->depth_function > R300_ZS_MASK)
      return -EINVAL;
   if (params->pitch_pixels == 0 ||
       params->pitch_pixels % DEPTH_PITCH_ALIGNMENT != 0 ||
       params->pitch_pixels > DEPTH_PITCH_MAX)
      return -EINVAL;
   if (params->depth_offset_bytes % DEPTH_OFFSET_ALIGNMENT != 0)
      return -EINVAL;
   /* The tile bits occupy 16 and 17 alone, and microtile 3 is reserved,
    * so a value outside the two fields or naming the reserved encoding
    * would land in the pitch word as a mode the hardware does not have. */
   if ((params->pitch_tile_bits & ~(uint32_t)(R300_DEPTHMACROTILE(1u) |
                                              R300_DEPTHMICROTILE(3u))) != 0 ||
       (params->pitch_tile_bits & R300_DEPTHMICROTILE(3u)) ==
          R300_DEPTHMICROTILE(3u))
      return -EINVAL;
   /* Square microtiling has a tile shape at 16 bits per pixel alone --
    * r300_get_pixel_alignment reports {0, 0} at every other width and
    * asserts on it -- so packed Z24/S8 selecting that mode names a
    * layout with no alignment the pitch rounds to.  The descriptor path
    * refuses the pair in r300_zb_depth_surface_check; this closes the
    * same shape reaching the emitter as hand-built params. */
   if ((params->pitch_tile_bits & R300_DEPTHMICROTILE(3u)) ==
          R300_DEPTHMICROTILE(2u) &&
       params->depth_format == R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL)
      return -EINVAL;
   /* The tile word is well formed by here, so the macrotile bit reads as
    * the mode it names. */
   if ((params->pitch_tile_bits & R300_DEPTHMACROTILE(1u)) != 0 &&
       params->depth_offset_bytes % DEPTH_MACROTILE_OFFSET_ALIGNMENT != 0)
      return -EINVAL;

   if (!r300_pm4_builder_reserve(builder, r300_zb_depth_state_dwords()))
      return builder->error;

   r300_pm4_reg(builder, R300_ZB_FORMAT, params->depth_format);

   /* The kernel reads the emitted word as a byte offset within the
    * buffer object and adds the object's address, so the stream carries
    * the offset and the relocation names the object.
    */
   r300_pm4_reg(builder, R300_ZB_DEPTHOFFSET, params->depth_offset_bytes);
   const uint32_t reloc_index =
      r300_pm4_reloc_nop(builder, params->depth_relocation_payload);
   if (out_reloc_ib_index != NULL)
      *out_reloc_ib_index = reloc_index;

   /* Unswapped, with the tile modes the caller's surface declares.  A
    * linear surface passes zero and the word matches the depth cell's
    * retained stream; a tiled surface carries its bits here because the
    * submission keeps its own tiling flags.
    */
   r300_pm4_reg(builder, R300_ZB_DEPTHPITCH,
                (params->pitch_pixels & R300_DEPTHPITCH_MASK) |
                   params->pitch_tile_bits |
                   R300_DEPTHENDIAN(R300_SURF_NO_SWAP));

   r300_pm4_reg(builder, R300_ZB_CNTL,
                R300_Z_ENABLE |
                   (params->depth_write ? R300_Z_WRITE_ENABLE : 0u));

   /* Stencil stays disabled through ZB_CNTL, so the stencil fields of
    * this word select nothing and the depth comparison is its content.
    */
   r300_pm4_reg(builder, R300_ZB_ZSTENCILCNTL,
                params->depth_function << R300_Z_FUNC_SHIFT);

   /* HiZ and fast fill read a hierarchical buffer this cell does not
    * establish, and a depth readback oracle reads the depth surface, so
    * both stay off and every fragment resolves against that surface.
    */
   r300_pm4_reg(builder, R300_ZB_BW_CNTL,
                R300_HIZ_DISABLE | R300_FAST_FILL_DISABLE);

   return 0;
}
