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
#include "r3v_native_reference_spirv.h"

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

/* The public two-draw form: the constants are the two admitted fragment
 * modules' -- the reference module's green in the first pass, the blue
 * module's in the second -- so the stream this reference emits is the
 * one a command buffer recording two render passes through the public
 * surface, one pipeline over each module, installs and appends.  The
 * binding is the same (2, 3): each pass's deferred draw owns its own
 * carrier, so the four references are carrier, target, carrier, target
 * in record order under the winsys first-add rule.
 */
static const uint32_t r3v_native_multi_pass_public_first_bits[4] =
   R3V_REFERENCE_FRAGMENT_COLOR_BITS;
static const uint32_t r3v_native_multi_pass_public_second_bits[4] =
   R3V_REFERENCE_FRAGMENT_BLUE_COLOR_BITS;

static inline void
r3v_native_multi_pass_public_reference(struct r300_triangle_multi_pass *out)
{
   memset(out, 0, sizeof(*out));
   r300_tcl_bypass_triangle_render_shape_reference(&out->pass[0]);
   r300_tcl_bypass_triangle_render_shape_reference(&out->pass[1]);
   for (unsigned i = 0; i < 4; i++) {
      out->pass[0].color_bits[i] = r3v_native_multi_pass_public_first_bits[i];
      out->pass[1].color_bits[i] = r3v_native_multi_pass_public_second_bits[i];
   }
   out->second_vertex_index = 2;
   out->second_color_index = 3;
}

/* The public Flat two-draw form: both passes carry the TEX0 varying
 * the Flat module pair declares, so each pass emits through the cell
 * family's varying record shape rather than a fragment constant.  The
 * binding stays (2, 3).  The provoking values ride the vertex stream,
 * not the stream bytes, so one emitted stream covers every vertex
 * order the runner draws.
 */
static inline void
r3v_native_multi_pass_public_flat_reference(
   struct r300_triangle_multi_pass *out)
{
   memset(out, 0, sizeof(*out));
   r300_tcl_bypass_triangle_render_shape_reference(&out->pass[0]);
   r300_tcl_bypass_triangle_render_shape_reference(&out->pass[1]);
   out->pass[0].varying = true;
   out->pass[1].varying = true;
   out->second_vertex_index = 2;
   out->second_color_index = 3;
}

/* The public direct GA Flat two-draw form: both passes carry the
 * varying through color 0 under the canonical direct plan
 * (r300_flat_color0_plan_direct_first), so the GA's provoking-vertex
 * selection, not host replication, chooses each pass's value.  The
 * binding stays (2, 3).
 */
static inline void
r3v_native_multi_pass_public_flat_color0_reference(
   struct r300_triangle_multi_pass *out)
{
   r3v_native_multi_pass_public_flat_reference(out);
   out->pass[0].flat_color0 = true;
   out->pass[1].flat_color0 = true;
}

#endif /* R3V_NATIVE_MULTI_PASS_ARMS_H */
