/*
 * SPDX-License-Identifier: MIT
 *
 * NoPerspective through the q lane of one perspective-interpolated
 * texture vector on the R300 TCL-bypass triangle cell.
 *
 * The RS interpolates every lane of a texture vector with the
 * position's reciprocal clip W (VTE_CNTL.VTX_W0_FMT clear), so a
 * vector whose xyz lanes carry a_i * c_i and whose q lane carries c_i
 * = w_i / max(w) over the source triangle yields interp(a * c) /
 * interp(c) = sum(l_i a_i), the window-linear value the Vulkan
 * NoPerspective qualifier requires.  A varying of width one to three
 * leaves the q lane free, so the carrier rides the varying's own
 * vector: the record keeps the eight-dword position-plus-TEX0 shape of
 * the varying cell, every register word is the varying cell's, and the
 * US program recovers xyz * rcp(q) with alpha 1.0 while GB_SELECT keeps
 * W_SELECT at 0.
 *
 * Record layout, 8 dwords per vertex:
 *   dwords 0-3   position (window x, y, normalized z, reciprocal clip W)
 *   dwords 4-6   TEX0.xyz = a.xyz * c, lanes at or past the width 0
 *   dword  7     TEX0.w = c = w / max(w)
 */

#ifndef R300_NOPERSPECTIVE_Q_LANE_PLAN_H
#define R300_NOPERSPECTIVE_Q_LANE_PLAN_H

#include "r300_noperspective_reciprocal_plan.h"

#include <stdint.h>

#define R300_NOPERSPECTIVE_Q_LANE_RECORD_DWORDS 8u
#define R300_NOPERSPECTIVE_Q_LANE_WIDTH_MAX 3u
/* The lane the normalized w rides. */
#define R300_NOPERSPECTIVE_Q_LANE_CARRIER_LANE 7u

struct r300_noperspective_q_lane_plan {
   /* Logical varying width, 1..3: lanes 0..width-1 of TEX0 carry the
    * premultiplied payload. */
   uint32_t width;
};

void r300_noperspective_q_lane_plan_init(
   struct r300_noperspective_q_lane_plan *out, uint32_t width);
/* -EINVAL when the width is outside 1..3. */
int r300_noperspective_q_lane_plan_validate(
   const struct r300_noperspective_q_lane_plan *plan);

/* The varying cell's register words, restated so the stream check and
 * the emitter share one authority: VAP_PROG_STREAM_CNTL_0 (FLOAT_4
 * into vector 0, FLOAT_4 into vector 6 with LAST_VEC),
 * VAP_OUTPUT_VTX_FMT_1 (TEX0 four components), VAP_VSM_VTX_ASSM
 * (position plus TC0), RS_COUNT (four components, no colors, HIRES),
 * RS_INST_COUNT 0, RS_IP_0 (texture pointer 0, identity selects),
 * RS_INST_0 (texture 0 to US input 0). */
uint32_t r300_noperspective_q_lane_plan_prog_stream_cntl_0(void);
uint32_t r300_noperspective_q_lane_plan_vtx_fmt_1(void);
uint32_t r300_noperspective_q_lane_plan_vsm_vtx_assm(void);
uint32_t r300_noperspective_q_lane_plan_rs_count(void);
uint32_t r300_noperspective_q_lane_plan_rs_inst_count(void);
uint32_t r300_noperspective_q_lane_plan_rs_ip_0(void);
uint32_t r300_noperspective_q_lane_plan_rs_inst_0(void);

/* Packs one source triangle of clip-space records (position x, y, z,
 * w then the varying's four lanes, eight dwords each) into three
 * q-lane records of eight dwords: position copied, lanes 0..width-1
 * multiplied by c_i = w_i / max(w), lanes width..2 written 0, lane 3
 * written c_i.  Refuses with -EDOM ahead of any write when a position
 * lane, a payload lane, or w is not finite, w is not positive, or the
 * w ratio or a premultiplied lane exceeds the admission envelope
 * (R300_NOPERSPECTIVE_CARRIER_W_RATIO_MAX, _LANE_MAX).  The output may
 * be the input: the triangle is read whole before it is written. */
int r300_noperspective_q_lane_pack_triangle(
   const struct r300_noperspective_q_lane_plan *plan,
   const float *source_records, float *q_lane_records);

/* Validates a packed stream of triangle_count triangles: every payload
 * lane finite and within the envelope, every lane at or past the width
 * exactly 0, every q lane finite and in (0, 1].  -EDOM names the first
 * violation. */
int r300_noperspective_q_lane_validate_stream(
   const struct r300_noperspective_q_lane_plan *plan,
   const float *q_lane_records, uint32_t triangle_count);

/* Validates the clipper's expanded stream of vertex_count records,
 * skipping the padding record the clipper writes for an absent fan
 * triangle (position 0, 0, 0, 1 and every other lane 0).  A clipped
 * record is a convex combination of source records, so its q lane
 * stays inside (0, 1], its zero lanes stay 0, and its payload inside
 * the envelope.  Returns the live record count, -EDOM on the first
 * violation, -EINVAL on a null stream. */
int r300_noperspective_q_lane_validate_expanded(
   const struct r300_noperspective_q_lane_plan *plan,
   const float *q_lane_records, uint32_t vertex_count);

/* Walks a PM4 stream and returns the number of draw packets ahead of
 * which every q-lane register -- GB_SELECT at gb_select_base with
 * W_SELECT clear, VAP_VTX_SIZE 8, VAP_PROG_STREAM_CNTL_0,
 * VAP_OUTPUT_VTX_FMT_1, VAP_VSM_VTX_ASSM, RS_COUNT, RS_INST_COUNT,
 * RS_IP_0, RS_INST_0 -- was written with the plan's word since the
 * previous draw; -(1 + n) names the first draw n a missing or foreign
 * word precedes, -EINVAL a malformed stream. */
int r300_noperspective_q_lane_plan_stream_check(
   const struct r300_noperspective_q_lane_plan *plan,
   uint32_t gb_select_base, const uint32_t *ib, uint32_t ib_dwords);

/* Host model of the fragment recovery of one payload lane: b * rcp(c)
 * with the RCP and the MUL each rounded to the s1e7m16 grid, the model
 * r300_noperspective_reciprocal_recover states. */
float r300_noperspective_q_lane_recover(float b, float c);

#endif /* R300_NOPERSPECTIVE_Q_LANE_PLAN_H */
