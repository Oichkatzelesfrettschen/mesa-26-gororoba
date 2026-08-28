/*
 * SPDX-License-Identifier: MIT
 *
 * Post-vertex-shader lowering of Vulkan interpolation semantics on the
 * CPU delivery route.  The vertex job writes one record per vertex --
 * the clip-space position, then each varying as a vec4 -- and the
 * triangle list assembles three consecutive records per triangle.
 * Between that assembly and the clip-space delivery carrier this stage
 * realizes the qualifiers the shader-interface record linked: a Flat
 * varying takes the provoking vertex's value at all three vertices,
 * so the rasterizer's perspective interpolation of equal endpoints
 * reproduces the Vulkan flat result exactly, and the homogeneous
 * clipper that follows interpolates equal values to the same value.
 * Smooth varyings pass untouched.
 */

#ifndef R3V_POST_VS_LOWERING_H
#define R3V_POST_VS_LOWERING_H

#include "r3v_shader_interface.h"

#include <stdbool.h>
#include <stdint.h>

/* The Vulkan core provoking vertex: the first vertex of each triangle
 * in the list (VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT is the default
 * and the only mode a driver without VK_EXT_provoking_vertex offers). */
#define R3V_POST_VS_PROVOKING_VERTEX_FIRST 0u

struct r3v_post_vs_lowering {
   /* Bit l set for each varying location the fragment stage reads
    * Flat; the record carries location l at dwords 4 + 4l .. 7 + 4l. */
   uint32_t flat_mask;
   /* The vertex (0..2) of each triangle whose varyings the Flat
    * locations replicate. */
   uint8_t provoking_vertex;
};

/* Derives the lowering from a linked interface: flat_mask is the
 * link's, the provoking vertex is the Vulkan core default. */
void r3v_post_vs_lowering_from_interface(
   const struct r3v_shader_interface_link *link,
   struct r3v_post_vs_lowering *out);

/* Rewrites a triangle list in place: for every triangle and every
 * Flat location, the provoking vertex's four dwords replace the other
 * two vertices'.  record_dwords is the per-vertex record length, four
 * position dwords plus four per varying.  Refuses with -EINVAL, ahead
 * of any write, when record_dwords is below four or not a multiple of
 * four, when a Flat location lies past the varyings the record
 * carries, when the provoking vertex is outside 0..2, or when records
 * is NULL with a nonzero triangle count.  A lowering with no Flat
 * location leaves the list untouched and returns 0.
 */
int r3v_post_vs_lower_triangles(const struct r3v_post_vs_lowering *lowering,
                                uint32_t *records, uint32_t triangle_count,
                                uint32_t record_dwords);

#endif /* R3V_POST_VS_LOWERING_H */
