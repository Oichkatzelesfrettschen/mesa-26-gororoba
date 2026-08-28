/*
 * SPDX-License-Identifier: MIT
 *
 * The attended two-pass cell: one definition the arming runner emits
 * for its digest and the attended runner records through the recorder,
 * so an arming report names the concatenated stream the submission
 * carries.  Both passes take the 64x64 B8G8R8A8 reference shape; the
 * second pass brings its own vertex page and color target and a
 * fragment constant distinct from the first pass's, so each target
 * names the pass that wrote it.
 */

#ifndef R3V_NATIVE_MULTI_PASS_ARMS_H
#define R3V_NATIVE_MULTI_PASS_ARMS_H

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <stdint.h>
#include <string.h>

/* The second pass's fragment constant: opaque green, exact in FP24 and
 * in UNORM8, distinct from the reference draw color and the sentinel.
 */
static const float r3v_native_multi_pass_second_color[4] = { 0.0f, 1.0f,
                                                             0.0f, 1.0f };

static inline void
r3v_native_multi_pass_reference(struct r300_triangle_multi_pass *out)
{
   memset(out, 0, sizeof(*out));
   r300_tcl_bypass_triangle_render_shape_reference(&out->pass[0]);
   r300_tcl_bypass_triangle_render_shape_reference(&out->pass[1]);
   for (unsigned i = 0; i < 4; i++)
      memcpy(&out->pass[1].color_bits[i],
             &r3v_native_multi_pass_second_color[i], sizeof(float));
   out->second_vertex_index = 2;
   out->second_color_index = 3;
}

#endif /* R3V_NATIVE_MULTI_PASS_ARMS_H */
