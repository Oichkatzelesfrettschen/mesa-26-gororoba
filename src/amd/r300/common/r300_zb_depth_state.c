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
                         const struct r300_zb_depth_state_params *params)
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

   if (!r300_pm4_builder_reserve(builder, r300_zb_depth_state_dwords()))
      return builder->error;

   r300_pm4_reg(builder, R300_ZB_FORMAT, params->depth_format);

   /* The kernel reads the emitted word as a byte offset within the
    * buffer object and adds the object's address, so the stream carries
    * the offset and the relocation names the object.
    */
   r300_pm4_reg(builder, R300_ZB_DEPTHOFFSET, params->depth_offset_bytes);
   r300_pm4_reloc_nop(builder, params->depth_relocation_payload);

   /* Linear, unswapped: a first depth cell reads the buffer back on the
    * host, and a tiled surface would need the detiling the readback
    * oracle does not carry.
    */
   r300_pm4_reg(builder, R300_ZB_DEPTHPITCH,
                (params->pitch_pixels & R300_DEPTHPITCH_MASK) |
                   R300_DEPTHMACROTILE_DISABLE |
                   R300_DEPTHMICROTILE_LINEAR |
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
