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

#include "amd/r300/common/r300_noperspective_mixed_carrier_plan.h"
#include "amd/r300/common/r300_noperspective_q_lane_plan.h"
#include "amd/r300/common/r300_noperspective_reciprocal_plan.h"

#include <stdbool.h>
#include <stdint.h>

/* The Vulkan core provoking vertex: the first vertex of each triangle
 * in the list (VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT is the default
 * and the only mode a driver without VK_EXT_provoking_vertex offers). */
#define R3V_POST_VS_PROVOKING_VERTEX_FIRST 0u

struct r3v_post_vs_lowering {
   /* Bit l set for each varying location the fragment stage reads
    * Flat; the record carries location l at dwords 4 + 4l .. 7 + 4l.
    * The mixed carrier packing admits flat_mask 0 (Smooth at location
    * 0) or 0x1 (Flat at location 0, replicated ahead of the packing);
    * every other mask refuses at r3v_post_vs_pack_noperspective_mixed_carrier. */
   uint32_t flat_mask;
   /* The vertex (0..2) of each triangle whose varyings the Flat
    * locations replicate. */
   uint8_t provoking_vertex;
   /* Bit l set for each location the fragment stage reads
    * NoPerspective. */
   uint32_t noperspective_mask;
   /* Set when the pipeline routes NoPerspective through the reciprocal
    * carrier: after replication and ahead of clipping the stage packs
    * each triangle into the TC1 carrier shape
    * (r3v_post_vs_pack_noperspective_carrier). */
   bool reciprocal_carrier;
   /* Nonzero (1..3) when the pipeline routes NoPerspective through the
    * q lane of the location-0 vector: after replication and ahead of
    * clipping the stage packs each triangle in place
    * (r3v_post_vs_pack_noperspective_q_lane). */
   uint8_t q_lane_width;
   /* Set when the pipeline routes the Smooth location 0 plus
    * NoPerspective location 1 interface through the mixed carrier:
    * ahead of clipping the stage packs each twelve-dword triangle into
    * the sixteen-dword mixed shape
    * (r3v_post_vs_pack_noperspective_mixed_carrier). */
   bool mixed_carrier;
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

/* Packs a triangle list into the reciprocal carrier shape: source
 * records of record_dwords (position plus one full varying at
 * location 0, the one shape the TC1 plan serves), carrier records of
 * R300_NOPERSPECTIVE_CARRIER_RECORD_DWORDS each, per
 * r300_noperspective_reciprocal_pack_triangle.  The carrier buffer may
 * be the source buffer: the stage walks triangles from the last to the
 * first and each triangle's three records are read whole before its
 * wider output is written, and no later triangle's source lies inside
 * an earlier triangle's output.  Refuses with -EINVAL when the lowering
 * does not select the carrier, the noperspective mask is not exactly
 * location 0, or record_dwords is not eight; -EDOM is the packer's
 * envelope refusal, reported ahead of any write.
 */
int r3v_post_vs_pack_noperspective_carrier(
   const struct r3v_post_vs_lowering *lowering, const uint32_t *records,
   uint32_t triangle_count, uint32_t record_dwords, uint32_t *carrier);

/* Packs a triangle list in place into the q-lane shape: eight-dword
 * records whose location-0 vector becomes (a.xyz * c, 0 past the
 * width, c) per r300_noperspective_q_lane_pack_triangle.  Refuses with
 * -EINVAL when the lowering selects no q-lane width, the noperspective
 * mask is not exactly location 0, or record_dwords is not eight; -EDOM
 * is the packer's envelope refusal, judged over every triangle ahead
 * of the first write. */
int r3v_post_vs_pack_noperspective_q_lane(
   const struct r3v_post_vs_lowering *lowering, uint32_t *records,
   uint32_t triangle_count, uint32_t record_dwords);

/* Packs a triangle list into the mixed carrier shape: source records
 * of R300_NOPERSPECTIVE_MIXED_CARRIER_SOURCE_DWORDS (position, location
 * 0, location 1), carrier records of
 * R300_NOPERSPECTIVE_MIXED_CARRIER_RECORD_DWORDS each, per
 * r300_noperspective_mixed_carrier_pack_triangle.  The carrier buffer
 * may be the source buffer: the stage walks triangles from the last to
 * the first, each triangle's three records are read whole before its
 * wider output is written, and no later triangle's source lies inside
 * an earlier triangle's output.  Every triangle is judged against the
 * envelope ahead of the first write, so a refusal leaves the source
 * list intact.  Refuses with -EINVAL when the lowering does not select
 * the mixed carrier, the noperspective mask is not exactly location 1,
 * the flat mask is not empty, or record_dwords is not twelve; -EDOM is
 * the packer's envelope refusal. */
int r3v_post_vs_pack_noperspective_mixed_carrier(
   const struct r3v_post_vs_lowering *lowering, const uint32_t *records,
   uint32_t triangle_count, uint32_t record_dwords, uint32_t *carrier);

#endif /* R3V_POST_VS_LOWERING_H */
