/*
 * SPDX-License-Identifier: MIT
 *
 * Neutral R300-class depth-buffer binding and depth-test state.
 */

#ifndef R300_ZB_DEPTH_STATE_H
#define R300_ZB_DEPTH_STATE_H

#include "r300_pm4_builder.h"

#include <stdbool.h>
#include <stdint.h>

/* Binds a depth buffer and arms the depth test.  The first-draw
 * contract writes every ZB function disabled and carries the depth
 * resource words as reference artifacts for a buffer no draw binds, so
 * a cell whose output depends on the depth test establishes these
 * registers itself.
 *
 * ZB_DEPTHOFFSET carries a byte offset the kernel relocates, the same
 * NOP-form relocation the color target's RB3D_COLOROFFSET0 takes:
 * the emitted word is the offset within the buffer object and the
 * kernel adds the object's address.  R300_ZB_DEPTHOFFSET encodes bits
 * 31 to 5, so an offset the low five bits reach has no encoding and the
 * resolver refuses it rather than emitting an address the hardware
 * reads elsewhere.
 */
struct r300_zb_depth_state_params {
   /* Depth surface pitch in pixels.  R300_DEPTHPITCH_MASK reaches bits
    * 2 through 13, so the pitch is a multiple of four and at most 16380.
    */
   uint32_t pitch_pixels;
   /* One complete ZB_FORMAT depth encoding: integer Z16, either 13E3
    * inversion, or packed Z24/S8.
    */
   uint32_t depth_format;
   /* Depth binding: byte offset within the buffer object and the
    * caller's relocation payload for the NOP-form relocation that
    * follows the offset write.
    */
   uint32_t depth_offset_bytes;
   uint32_t depth_relocation_payload;
   /* Depth comparison, one of R300_ZS_NEVER through R300_ZS_ALWAYS. */
   uint32_t depth_function;
   /* Z_WRITE_ENABLE: a passing fragment updates depth memory.  A cell
    * proving the write needs this set and a readback of the buffer.
    */
   bool depth_write;
};

/* Dwords r300_zb_depth_state_emit reserves, so a caller sizes its
 * packet before building.
 */
uint32_t r300_zb_depth_state_dwords(void);

/* Emits the depth binding and test state.  Every parameter is validated
 * before the first dword, so a refused call leaves the builder
 * untouched: -EINVAL for a null builder or params, a reserved or malformed
 * depth format, a depth function outside R300_ZS_MASK, a pitch that is not a
 * multiple of four or exceeds the DEPTHPITCH field, or an offset the low five
 * bits reach.  A builder that already carries an error returns that first
 * error; insufficient capacity returns -ENOSPC.  Returns 0 on success.
 */
int r300_zb_depth_state_emit(
   struct r300_pm4_builder *builder,
   const struct r300_zb_depth_state_params *params);

#endif /* R300_ZB_DEPTH_STATE_H */
