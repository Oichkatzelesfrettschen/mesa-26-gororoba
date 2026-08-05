/*
 * SPDX-License-Identifier: MIT
 *
 * Fixed TCL-bypass triangle cell: the first native hardware witness.
 */

#ifndef R300_TCL_BYPASS_TRIANGLE_H
#define R300_TCL_BYPASS_TRIANGLE_H

#include <stdint.h>

struct r300_fragment_binary;

/* BO slots the cell references; the transport binds slot order to the
 * relocation-list order at submission.
 */
enum r300_tcl_bypass_triangle_slot {
   R300_TRIANGLE_SLOT_VERTEX = 0,
   R300_TRIANGLE_SLOT_COLOR = 1,
   R300_TRIANGLE_SLOT_COUNT = 2,
};

struct r300_tcl_bypass_triangle_params {
   /* Byte offset of the first vertex inside the vertex BO. */
   uint32_t vertex_offset;
   /* RB3D_COLORPITCH0 value: pitch in pixels plus format and endian
    * fields, chosen by the caller from the color BO's layout.
    */
   uint32_t color_pitch_format;
   const struct r300_fragment_binary *fragment_binary;
};

/* One IB position whose payload names a relocation slot. */
struct r300_tcl_bypass_triangle_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

struct r300_tcl_bypass_triangle_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_tcl_bypass_triangle_reloc_site reloc_sites[4];
   uint32_t reloc_site_count;
};

/* Emits the complete fixed cell: TCL bypass, one FLOAT_4 position stream
 * with identity PSC selectors, VAP_VTX_SIZE = 4, position-only VAP output,
 * the owned fragment binary verbatim, one color target, depth disabled,
 * destination-cache publication, one vertex-list triangle draw.  Returns 0
 * or a negative errno; the caller owns the returned IB allocation.
 */
int r300_tcl_bypass_triangle_emit(
   const struct r300_tcl_bypass_triangle_params *params,
   struct r300_tcl_bypass_triangle_ib *out);

void r300_tcl_bypass_triangle_release(struct r300_tcl_bypass_triangle_ib *ib);

#endif /* R300_TCL_BYPASS_TRIANGLE_H */
