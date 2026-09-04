/*
 * SPDX-License-Identifier: MIT
 *
 * Route selection for Vulkan interpolation qualifiers on the R3V native
 * CPU delivery route.  A Flat varying reaches the fragment stage either
 * through host provoking-value replication (r3v_post_vs_lowering.h) or
 * through the RS485M GA's own provoking-vertex selection over the color
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
 * A NoPerspective location has one route, direct GB W_SELECT; the
 * refused conjunction is UNSUPPORTED, and the draw refuses at record
 * time (r3v_native_draw.c), so a NoPerspective varying is either
 * interpolated affine on silicon or refused ahead of submission.
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

#include "amd/r300/common/r300_rs_tex_adj_probe.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

struct r3v_shader_interface_link;

enum r3v_interpolation_route {
   R3V_INTERPOLATION_ROUTE_REPLICATE = 0,
   R3V_INTERPOLATION_ROUTE_DIRECT_GA_COLOR0,
   /* NoPerspective through GB_SELECT.W_SELECT = 1: the GB hands the
    * rasterizer 1.0 as the outgoing 1/W (AMD R3xx 3D Registers,
    * GB_SELECT), so every interpolant in the draw is linear in window
    * space.  On RS485M the two-pass census classifies that word affine
    * on every judged pixel (882/882 within one UNORM8 quantum) with the
    * same stream classifying perspective under W_SELECT = 0, while
    * RS_INST.TEX_ADJ leaves the target unchanged
    * (r300_rs_tex_adj_probe.h).  The word is per draw, so the route
    * admits an interface whose one varying is NoPerspective and refuses
    * a Smooth location beside it. */
   R3V_INTERPOLATION_ROUTE_DIRECT_GB_W_SELECT,
   /* A NoPerspective interface outside the W_SELECT conjunction: a
    * Flat or Smooth location beside it, a narrower or non-float
    * varying, a delivery route other than CPU, or a primitive other
    * than the triangle list.  The (a * w, w) reciprocal carrier that
    * would serve those shapes is not built, and replication would hand
    * the varying perspective interpolation, so the draw refuses at
    * record time with R3V_NATIVE_REFUSAL_RESULT. */
   R3V_INTERPOLATION_ROUTE_UNSUPPORTED,
   /* NoPerspective through the perspective-interpolated reciprocal
    * carrier (r300_noperspective_reciprocal_plan.h): the post-VS stage
    * packs a * w into TEX0 and the triangle-normalized w into TEX1.x,
    * and the US recovers a as TEX0 * rcp(TEX1.x) with GB_SELECT.W_SELECT
    * at 0.  The route is per varying, so it serves the shapes W_SELECT
    * cannot; until each shape's silicon receipt lands the selector
    * takes it for the W_SELECT conjunction alone under the exact gate
    * R3V_NATIVE_NOPERSPECTIVE_CARRIER_FORCE=1, the forced-carrier rung. */
   R3V_INTERPOLATION_ROUTE_RECIPROCAL_CARRIER,
   /* NoPerspective through the q lane of the varying's own vector
    * (r300_noperspective_q_lane_plan.h): a float, vec2, or vec3
    * NoPerspective varying at location 0 whose components start at x
    * and run contiguously leaves TEX0.w free, so the post-VS stage
    * packs a.xyz * c into the leading lanes, 0 into the lanes past the
    * width, and c = w / max(w) into w, and the US recovers xyz *
    * rcp(w) with alpha 1.0 under GB_SELECT.W_SELECT 0.  The record and
    * every register word stay the varying cell's.  The fragment
    * program must be the narrow pass-through (the varying's lanes,
    * zero fill, alpha 1), the shape that binary executes.  The stage
    * packs ahead of the clipper, so every clipping class is admitted
    * and the expanded stream is validated ahead of publication. */
   R3V_INTERPOLATION_ROUTE_RECIPROCAL_Q_LANE,
   /* Mixed Smooth and NoPerspective through the shared reciprocal
    * carrier (r300_noperspective_mixed_carrier_plan.h): location 0 a
    * Smooth float vec4 riding TC0 verbatim, location 1 a NoPerspective
    * float vec4 riding TC1 premultiplied by c = w / max(w), and c in
    * TC2.x, three of the four RS vectors at VAP_VTX_SIZE 16 under
    * GB_SELECT.W_SELECT 0.  The fragment program must be the mixed
    * carrier lane program (loc0.x, loc0.y, loc1.x, loc1.y), the shape
    * the cell's binary executes as (TC0.xy, (TC1 * rcp(TC2.x)).xy).
    * The post-VS stage packs the twelve-dword two-location records
    * into sixteen-dword records ahead of the clipper, so every
    * clipping class is admitted and the expanded stream is validated
    * ahead of publication.  Location 0 may be Flat instead of Smooth:
    * the post-VS replication writes the provoking vertex's vector to
    * every record of the triangle ahead of the packing, and the cell's
    * bytes are unchanged.  Every other mixed shape is UNSUPPORTED. */
   R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER,
   /* The public full-vec4 NoPerspective conjunction ahead of the clip
    * judgment: one float vec4 NoPerspective varying at location 0, CPU
    * delivery, a triangle list, every probe and force gate closed.
    * The record installs the direct GB W_SELECT cell and retains the
    * TC1 reciprocal carrier cell beside it; at submission, after the
    * CPU vertex execution, r3v_interpolation_route_resolve_clip judges
    * the draw's triangles and selects the concrete route: every source
    * triangle ACCEPT keeps the direct cell, any PARTIAL triangle
    * splices the carrier cell in and packs the carrier stream, and a
    * carrier envelope refusal fails the draw ahead of publication.
    * The selection precedes the arming digest, so the armed bytes are
    * the concrete cell's. */
   R3V_INTERPOLATION_ROUTE_W_SELECT_OR_RECIPROCAL_CARRIER,
};

/* The per-vertex dword count the route publishes to the cell: the
 * reciprocal carrier widens the eight-dword varying record to twelve,
 * the mixed carrier the twelve-dword two-location record to sixteen,
 * the adaptive route reserves the carrier's twelve so either concrete
 * cell fits the carrier memory, and every other route publishes the
 * vertex job's own record. */
uint32_t r3v_interpolation_published_record_dwords(
   enum r3v_interpolation_route route, uint32_t job_record_dwords);

enum r3v_interpolation_clip_class {
   /* Every vertex inside the clip volume: the clipper emits the
    * triangle unchanged. */
   R3V_INTERPOLATION_CLIP_ACCEPT = 0,
   /* At least one vertex outside: the clipper emits a fan whose
    * vertices differ from the source's. */
   R3V_INTERPOLATION_CLIP_PARTIAL,
   /* The record-time class: the draw's triangles are judged after the
    * CPU vertex execution at submission, so the selector returns the
    * route that holds both concrete cells where the class decides. */
   R3V_INTERPOLATION_CLIP_DEFERRED,
};

/* Resolves the adaptive route against the judged class of a draw:
 * ACCEPT (every source triangle) selects the direct GB W_SELECT cell,
 * PARTIAL (any source triangle) the TC1 reciprocal carrier cell.  Every
 * other route resolves to itself; the adaptive route against DEFERRED
 * stays adaptive. */
enum r3v_interpolation_route
r3v_interpolation_route_resolve_clip(enum r3v_interpolation_route route,
                                     enum r3v_interpolation_clip_class clip);

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
   /* The forced-carrier gate is open: the W_SELECT conjunction selects
    * the reciprocal carrier instead. */
   bool carrier_forced;
   /* The fragment module is the narrow pass-through of this width
    * (1..3, r3v_fragment_narrow_passthrough_from_spirv); 0 for every
    * other fragment shape.  The q-lane fragment binary executes that
    * program alone, so a narrow pass-through outside the q-lane
    * conjunction is UNSUPPORTED. */
   uint32_t narrow_passthrough_width;
   /* The fragment module is the mixed carrier lane program
    * (r3v_fragment_mixed_carrier_from_spirv).  The mixed carrier
    * binary executes that program alone, so the program outside the
    * mixed conjunction is UNSUPPORTED. */
   bool mixed_carrier_fragment;
};

/* The record-time form of the query: everything but the clipping class,
 * which the execution-time classification of each triangle supplies.
 * A link with no Flat location selects replication (the identity
 * lowering) with a naming reason. */
/* The rasterizer probe candidate a NoPerspective interface takes.
 * Bit 22 of RS_INST (TEX_ADJ) and GB_SELECT.W_SELECT carry no retained
 * silicon classification on RS485M, so neither is a NoPerspective
 * route; a candidate marks the pass whose stream differs from the
 * control varying cell in that one word, and the census classifies
 * the bit (r300_rs_tex_adj_probe.h).  The candidate opens on exactly
 * one probe gate at the exact value 1, and a probe pipeline is a
 * NoPerspective interface: one float vec4 varying at location 0, CPU
 * delivery, a triangle list, and the pass-through fragment program
 * reading the RS destination. */
enum r3v_rs_probe_candidate {
   R3V_RS_PROBE_NONE = 0,
   R3V_RS_PROBE_TEX_ADJ,
   R3V_RS_PROBE_W_SELECT_ONE,
};
/* The route enum travels into the cell as the common probe candidate
 * through one uint8_t, so the two numberings coincide. */
static_assert((int)R3V_RS_PROBE_NONE == (int)R300_RS_TEX_ADJ_PROBE_CONTROL,
              "probe candidate numbering");
static_assert((int)R3V_RS_PROBE_TEX_ADJ == (int)R300_RS_TEX_ADJ_PROBE_TEX_ADJ,
              "probe candidate numbering");
static_assert((int)R3V_RS_PROBE_W_SELECT_ONE ==
                 (int)R300_RS_TEX_ADJ_PROBE_W_SELECT_ONE,
              "probe candidate numbering");

struct r3v_rs_probe_query {
   /* R3V_NATIVE_RS_TEX_ADJ_PROBE=1 and R3V_NATIVE_RS_W_SELECT_PROBE=1;
    * both open refuses, since one cell carries one candidate. */
   bool tex_adj_gate;
   bool w_select_gate;
   bool cpu_delivery;
   bool triangle_list;
   const struct r3v_shader_interface_link *link;
   bool rs_destination_available;
   bool fragment_consumes_destination;
};

enum r3v_rs_probe_candidate
r3v_rs_probe_candidate_select(const struct r3v_rs_probe_query *query,
                              const char **reason);

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
