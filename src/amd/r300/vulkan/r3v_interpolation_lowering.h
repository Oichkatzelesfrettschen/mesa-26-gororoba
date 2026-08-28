/*
 * SPDX-License-Identifier: MIT
 *
 * Route selection for Vulkan interpolation qualifiers on the R3V native
 * CPU delivery route.  A Flat varying reaches the fragment stage either
 * through host provoking-value replication (r3v_post_vs_lowering.h) or
 * through the RS482 GA's own provoking-vertex selection over the color
 * 0 vector (r300_flat_color0_plan.h).  The selector admits the direct
 * hardware route on the conjunction below and falls back to
 * replication otherwise, so every refused predicate lands on the
 * qualified route rather than on an unverified state.
 *
 *   direct GA Flat allowed iff
 *     delivery route is CPU
 *     and the primitive is emitted as a triangle list
 *     and clipping class is ACCEPT
 *     and the Flat location maps completely to an admitted color lane
 *     and the required RS destination is available
 *     and the fragment program consumes that destination
 *     and provoking FIRST is representable
 *   otherwise use provoking-value replication
 *
 * The clipping class is a per-triangle execution-time fact: hardware
 * provoking selection acts on each emitted fan triangle and cannot
 * recover the source primitive's provoking value once clipping changes
 * the vertices, so a partially clipped triangle keeps replication ahead
 * of the clipper while the GA state, harmless over equal endpoints,
 * stays as recorded.
 */

#ifndef R3V_INTERPOLATION_LOWERING_H
#define R3V_INTERPOLATION_LOWERING_H

#include <stdbool.h>
#include <stdint.h>

struct r3v_shader_interface_link;

enum r3v_interpolation_route {
   R3V_INTERPOLATION_ROUTE_REPLICATE = 0,
   R3V_INTERPOLATION_ROUTE_DIRECT_GA_COLOR0,
};

enum r3v_interpolation_clip_class {
   /* Every vertex inside the clip volume: the clipper emits the
    * triangle unchanged. */
   R3V_INTERPOLATION_CLIP_ACCEPT = 0,
   /* At least one vertex outside: the clipper emits a fan whose
    * vertices differ from the source's. */
   R3V_INTERPOLATION_CLIP_PARTIAL,
};

struct r3v_interpolation_query {
   bool cpu_delivery;
   bool triangle_list;
   enum r3v_interpolation_clip_class clip_class;
   const struct r3v_shader_interface_link *link;
   /* The RS can write US input 0, the register the pass-through
    * fragment binary reads. */
   bool rs_destination_available;
   /* The fragment program reads that input. */
   bool fragment_consumes_destination;
   /* GA_COLOR_CONTROL.PROVOKING_VERTEX_FIRST exists on the target. */
   bool provoking_first_representable;
};

/* The record-time form of the query: everything but the clipping class,
 * which the execution-time classification of each triangle supplies.
 * A link with no Flat location selects replication (the identity
 * lowering) with a naming reason. */
enum r3v_interpolation_route
r3v_interpolation_route_select(const struct r3v_interpolation_query *query,
                               const char **reason);

/* Classifies one clip-space triangle: three records of record_dwords
 * each, position first as x, y, z, w.  ACCEPT requires every vertex to
 * satisfy w > 0, -w <= x <= w, -w <= y <= w, and 0 <= z <= w, the
 * Vulkan clip volume; any other vertex makes the class PARTIAL. */
enum r3v_interpolation_clip_class
r3v_interpolation_clip_class_of_triangle(const uint32_t *records,
                                         uint32_t record_dwords);

#endif /* R3V_INTERPOLATION_LOWERING_H */
