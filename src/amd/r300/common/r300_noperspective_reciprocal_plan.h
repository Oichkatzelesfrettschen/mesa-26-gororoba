/*
 * SPDX-License-Identifier: MIT
 *
 * NoPerspective through a perspective-interpolated reciprocal carrier on
 * the R300 TCL-bypass triangle cell.
 *
 * The RS interpolates every varying lane a with the position's reciprocal
 * clip W (VTE_CNTL.VTX_W0_FMT clear): interp(a) = sum(l_i a_i W_i) /
 * sum(l_i W_i).  A record that carries b_i = a_i w_i in the payload vector
 * and c_i = w_i in one shared carrier lane therefore yields interp(b) /
 * interp(c) = sum(l_i a_i), the window-linear value the Vulkan
 * NoPerspective qualifier requires, so the US program recovers it with one
 * RCP of the carrier lane and one MUL per payload vector while GB_SELECT
 * keeps W_SELECT at 0 and every Smooth lane in the draw keeps perspective.
 *
 * Record layout of the TC1 carrier shape, 12 dwords per vertex:
 *   dwords 0-3   position (window x, y, normalized z, reciprocal clip W)
 *   dwords 4-7   TC0 = a * s * w, the payload vector
 *   dwords 8-11  TC1 = (s * w, 0, 0, 1), the shared carrier vector
 * s = 1 / max_i(w_i) over the source triangle; the common scale cancels in
 * b / c, pins the largest carrier lane of the triangle at 1.0, and bounds
 * the payload lanes by max|a_i|.
 */

#ifndef R300_NOPERSPECTIVE_RECIPROCAL_PLAN_H
#define R300_NOPERSPECTIVE_RECIPROCAL_PLAN_H

#include <stdbool.h>
#include <stdint.h>

#define R300_NOPERSPECTIVE_CARRIER_POSITION_DWORDS 4u
#define R300_NOPERSPECTIVE_CARRIER_VECTOR_DWORDS 4u
/* Position, one payload vector, the shared carrier vector. */
#define R300_NOPERSPECTIVE_CARRIER_RECORD_DWORDS 12u
/* The R300 RS exposes RS_IP_0..3: four interpolated vectors per draw, the
 * budget the payload vectors plus the shared carrier vector share. */
#define R300_NOPERSPECTIVE_CARRIER_RS_VECTOR_BUDGET 4u
/* Carrier admission envelope in the s1e7m16 US ALU: max(w) / min(w) at or
 * below 2^16 keeps the interpolated carrier lane at or above 2^-16 after
 * normalization, sixteen binades above the FP24 denormal floor, and a
 * premultiplied lane magnitude at or below 2^32 stays exact to FP24's
 * relative precision on every RCP * MUL product. */
#define R300_NOPERSPECTIVE_CARRIER_W_RATIO_MAX 65536.0
#define R300_NOPERSPECTIVE_CARRIER_LANE_MAX 4294967296.0

struct r300_noperspective_reciprocal_plan {
   /* Payload vectors ahead of the carrier: 1 for the TC1 shape. */
   uint32_t payload_vectors;
   /* The RS interpolator (and VAP texture-coordinate index) that carries
    * the shared w lane: payload_vectors for the TC1 shape. */
   uint32_t carrier_vector;
};

/* The TC1 carrier shape: one payload vector in TC0, the carrier in TC1. */
void r300_noperspective_reciprocal_plan_tc1(
   struct r300_noperspective_reciprocal_plan *out);
/* -EINVAL when the vector count exceeds the RS budget or the carrier
 * index is not the vector behind the payload. */
int r300_noperspective_reciprocal_plan_validate(
   const struct r300_noperspective_reciprocal_plan *plan);

uint32_t r300_noperspective_reciprocal_plan_record_dwords(
   const struct r300_noperspective_reciprocal_plan *plan);
/* VAP_PROG_STREAM_CNTL_0 and _1: FLOAT_4 elements into vectors 0, 6, 7,
 * LAST_VEC on the carrier element. */
uint32_t r300_noperspective_reciprocal_plan_prog_stream_cntl(
   const struct r300_noperspective_reciprocal_plan *plan, uint32_t index);
/* VAP_OUTPUT_VTX_FMT_1: four components for every texture vector. */
uint32_t r300_noperspective_reciprocal_plan_vtx_fmt_1(
   const struct r300_noperspective_reciprocal_plan *plan);
/* VAP_VSM_VTX_ASSM: position plus every texture vector. */
uint32_t r300_noperspective_reciprocal_plan_vsm_vtx_assm(
   const struct r300_noperspective_reciprocal_plan *plan);
/* RS_COUNT: four interpolated components per vector, no colors, HIRES. */
uint32_t r300_noperspective_reciprocal_plan_rs_count(
   const struct r300_noperspective_reciprocal_plan *plan);
/* RS_INST_COUNT: the last executed RS instruction index. */
uint32_t r300_noperspective_reciprocal_plan_rs_inst_count(
   const struct r300_noperspective_reciprocal_plan *plan);
/* RS_IP_i: texture pointer 4i with identity channel selects. */
uint32_t r300_noperspective_reciprocal_plan_rs_ip(
   const struct r300_noperspective_reciprocal_plan *plan, uint32_t index);
/* RS_INST_i: texture i written to US input i. */
uint32_t r300_noperspective_reciprocal_plan_rs_inst(
   const struct r300_noperspective_reciprocal_plan *plan, uint32_t index);

/* Packs one source triangle of clip-space records (position x, y, z, w
 * then payload_vectors varyings, 4 + 4 * payload_vectors dwords each) into
 * three carrier records of record_dwords each: position copied, every
 * payload lane multiplied by s * w_i, the carrier vector (s * w_i, 0, 0,
 * 1).  Refuses with -EDOM ahead of any write when a lane or w is not
 * finite, w is not positive, the w ratio or a premultiplied lane exceeds
 * the admission envelope.  The output may alias nothing in the input.
 */
int r300_noperspective_reciprocal_pack_triangle(
   const struct r300_noperspective_reciprocal_plan *plan,
   const float *source_records, float *carrier_records);

/* Validates a packed carrier stream of triangle_count triangles: every
 * carrier lane finite and in (0, 1], every payload lane finite and within
 * the envelope, every carrier vector's y, z, w at 0, 0, 1.  -EDOM names the
 * first violation. */
int r300_noperspective_reciprocal_validate_stream(
   const struct r300_noperspective_reciprocal_plan *plan,
   const float *carrier_records, uint32_t triangle_count);

/* Walks a PM4 stream and returns the number of draw packets ahead of
 * which every carrier register -- GB_SELECT at gb_select_base (W_SELECT
 * clear), VAP_VTX_SIZE, VAP_PROG_STREAM_CNTL_0/1, VAP_OUTPUT_VTX_FMT_1,
 * VAP_VSM_VTX_ASSM, RS_COUNT, RS_INST_COUNT, and each vector's RS_IP and
 * RS_INST -- was written with the plan's word since the previous draw;
 * -(1 + n) names the first draw n that a missing or foreign word
 * precedes, -EINVAL a malformed stream. */
int r300_noperspective_reciprocal_plan_stream_check(
   const struct r300_noperspective_reciprocal_plan *plan,
   uint32_t gb_select_base, const uint32_t *ib, uint32_t ib_dwords);

/* Host model of the fragment recovery: b * rcp(c) with each of the RCP and
 * the MUL rounded to the s1e7m16 grid the US ALU computes on. */
float r300_noperspective_reciprocal_recover(float b, float c);

#endif /* R300_NOPERSPECTIVE_RECIPROCAL_PLAN_H */
