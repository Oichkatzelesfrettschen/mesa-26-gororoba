/*
 * SPDX-License-Identifier: MIT
 *
 * The attended multisample resolve cell: one shape definition the
 * arming runner emits for its digest and the attended runner records
 * through the recorder, so an arming report names the stream the
 * submission carries.  The three predicted destination dwords travel
 * with it, since the run classifies the resolve semantics rather than
 * confirming one reading of them.
 */

#ifndef R3V_NATIVE_MSAA_ARMS_H
#define R3V_NATIVE_MSAA_ARMS_H

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The resolve half's fragment constant: opaque magenta, exact in FP24
 * and in UNORM8, and a color no multisample sample holds, so the
 * destination separates a downsampled write from a fragment write.
 */
static const float r3v_native_msaa_resolve_color[4] = { 1.0f, 0.0f, 1.0f,
                                                        1.0f };

/* The clear half's fragment constant: opaque blue, exact in FP24 and
 * in UNORM8, distinct from the draw color, the resolve constant, and the
 * 0xa5a5a5a5 seed, so a destination pixel names which of the four
 * writers reached it.
 */
static const float r3v_native_msaa_clear_color[4] = { 0.0f, 0.0f, 1.0f,
                                                      1.0f };

/* Fills the reference resolve cell at a sample count: both halves take
 * the 64x64 B8G8R8A8 reference shape, so the resolve destination
 * inherits its format from color buffer 0 as the hardware does, and the
 * destination sits at the base of its own allocation.  With clear set,
 * the cover draw under the subsample set leads the stream and writes
 * r3v_native_msaa_clear_color into every sample first.
 */
static inline void
r3v_native_msaa_reference_cleared(struct r300_triangle_msaa_resolve *out,
                                  uint32_t sample_count, bool clear)
{
   memset(out, 0, sizeof(*out));
   r300_tcl_bypass_triangle_render_shape_reference(&out->render);
   r300_tcl_bypass_triangle_render_shape_reference(&out->destination);
   out->destination.target_offset = 0;
   out->sample_count = sample_count;
   out->clear = clear;
   for (unsigned i = 0; i < 4; i++) {
      memcpy(&out->resolve_color_bits[i], &r3v_native_msaa_resolve_color[i],
             sizeof(float));
      memcpy(&out->clear_color_bits[i], &r3v_native_msaa_clear_color[i],
             sizeof(float));
   }
}

static inline void
r3v_native_msaa_reference(struct r300_triangle_msaa_resolve *out,
                          uint32_t sample_count)
{
   r3v_native_msaa_reference_cleared(out, sample_count, false);
}

/* The dword a judged destination pixel holds under each reading of
 * RB3D_AARESOLVE_CTL.AARESOLVE_MODE_RESOLVE: the render half's color
 * when the mode emits the downsampled samples, the resolve half's
 * fragment constant when the fragment write reaches the destination,
 * and neither when both reach it order-dependently.  The mixture is the
 * derived case, so it takes no dword of its own: it is the reading both
 * single-dword passes judge and refuse.
 */
static inline uint32_t
r3v_native_msaa_downsample_dword(const struct r300_triangle_msaa_resolve *msaa)
{
   return r300_tcl_bypass_triangle_render_shape_draw_dword(&msaa->render);
}

static inline uint32_t
r3v_native_msaa_fragment_dword(const struct r300_triangle_msaa_resolve *msaa)
{
   return r300_tcl_bypass_triangle_pack_unorm8_dword(
      msaa->destination.lanes, r3v_native_msaa_resolve_color);
}

/* The dword every fully exterior destination pixel holds once the clear
 * half ran: the resolve averages samples that all carry the clear color.
 */
static inline uint32_t
r3v_native_msaa_clear_dword(const struct r300_triangle_msaa_resolve *msaa)
{
   return r300_tcl_bypass_triangle_pack_unorm8_dword(
      msaa->destination.lanes, r3v_native_msaa_clear_color);
}

#endif /* R3V_NATIVE_MSAA_ARMS_H */
