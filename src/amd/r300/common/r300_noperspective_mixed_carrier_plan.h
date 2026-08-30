/*
 * SPDX-License-Identifier: MIT
 *
 * Mixed Smooth and NoPerspective interpolation through a shared
 * perspective-interpolated reciprocal carrier on the R300 TCL-bypass
 * triangle cell.
 *
 * The RS interpolates every texture vector with the position's
 * reciprocal clip W (VTE_CNTL.VTX_W0_FMT clear), so a Smooth payload
 * vector rides its interpolator unchanged, a NoPerspective payload
 * vector rides premultiplied by c_i = w_i / max(w) over the source
 * triangle, and one shared carrier vector rides c_i in x.  The US
 * program recovers the NoPerspective value as interp(a * c) *
 * rcp(interp(c)) = sum(l_i a_i) while reading the Smooth vector
 * directly, with GB_SELECT.W_SELECT at 0 for the whole draw.  The
 * register contract is the three-vector reciprocal carrier plan
 * (r300_noperspective_reciprocal_plan.h); this plan adds which payload
 * vectors the packing premultiplies and the US budget the baked block
 * must fit.
 *
 * Record layout, 16 dwords per vertex:
 *   dwords 0-3    position (window x, y, normalized z, reciprocal clip W)
 *   dwords 4-7    TC0 = Smooth vec4, copied verbatim
 *   dwords 8-11   TC1 = NoPerspective vec4 * c
 *   dwords 12-15  TC2 = (c, 0, 0, 1), c = w / max(w)
 * The fragment program stores (TC0.x, TC0.y, r.x, r.y) with
 * r = TC1 * rcp(TC2.x), so the target exposes two Smooth lanes and
 * two recovered NoPerspective lanes of one draw.
 */

#ifndef R300_NOPERSPECTIVE_MIXED_CARRIER_PLAN_H
#define R300_NOPERSPECTIVE_MIXED_CARRIER_PLAN_H

#include "r300_noperspective_reciprocal_plan.h"

#include <stdint.h>

/* Position plus two payload vectors, the record the vertex job stores
 * for locations 0 and 1. */
#define R300_NOPERSPECTIVE_MIXED_CARRIER_SOURCE_DWORDS 12u
/* Position, two payload vectors, the shared carrier vector. */
#define R300_NOPERSPECTIVE_MIXED_CARRIER_RECORD_DWORDS 16u
#define R300_NOPERSPECTIVE_MIXED_CARRIER_PAYLOAD_VECTORS 2u
/* The interpolator and texture-coordinate index of the shared carrier. */
#define R300_NOPERSPECTIVE_MIXED_CARRIER_VECTOR 2u
/* The R300-class US budget (radeon_code.h R300_PFS_MAX_ALU_INST,
 * R300_PFS_NUM_TEMP_REGS): the admitted ceiling for the generated
 * recovery program's ALU instructions and temporaries. */
#define R300_NOPERSPECTIVE_MIXED_CARRIER_US_ALU_MAX 64u
#define R300_NOPERSPECTIVE_MIXED_CARRIER_US_TEMP_MAX 32u

struct r300_noperspective_mixed_carrier_plan {
   /* The register contract: two payload vectors, the carrier behind
    * them. */
   struct r300_noperspective_reciprocal_plan carrier;
   /* Bit v set for each payload vector the packing premultiplies by c,
    * the NoPerspective locations; a clear bit is a Smooth vector
    * copied verbatim.  The first admitted shape is 0x2: location 0
    * Smooth, location 1 NoPerspective. */
   uint32_t noperspective_mask;
   /* The generated US program's ALU instruction and temporary counts,
    * judged against the admitted budget. */
   uint32_t us_alu_instructions;
   uint32_t us_temporaries;
};

/* The first admitted shape: TC0 Smooth, TC1 NoPerspective, TC2 carrier,
 * with the baked block's US counts. */
void r300_noperspective_mixed_carrier_plan_first(
   struct r300_noperspective_mixed_carrier_plan *out);

/* -EINVAL when the vectors plus the carrier exceed the RS budget of
 * four, the carrier is not the vector behind the two payloads, the
 * premultiplied set is not exactly vector 1, or the US counts exceed
 * the admitted budget. */
int r300_noperspective_mixed_carrier_plan_validate(
   const struct r300_noperspective_mixed_carrier_plan *plan);

/* Packs one source triangle of clip-space records (position x, y, z,
 * w, then locations 0 and 1 as vec4, twelve dwords each) into three
 * sixteen-dword carrier records: position copied, vector 0 copied,
 * vector 1 multiplied by c_i = w_i / max(w), the carrier vector (c_i,
 * 0, 0, 1).  Refuses with -EDOM ahead of any write when a lane or w is
 * not finite, w is not positive, or the w ratio or a premultiplied lane
 * exceeds the envelope (R300_NOPERSPECTIVE_CARRIER_W_RATIO_MAX,
 * _LANE_MAX); a Smooth lane is copied whatever its magnitude.  The
 * output may overlap the input: the triangle is read whole before it
 * is written. */
int r300_noperspective_mixed_carrier_pack_triangle(
   const struct r300_noperspective_mixed_carrier_plan *plan,
   const float *source_records, float *carrier_records);

/* Validates a packed stream of triangle_count triangles: every payload
 * lane finite, every premultiplied lane within the envelope, every
 * carrier vector (c in (0, 1], 0, 0, 1).  -EDOM names the first
 * violation. */
int r300_noperspective_mixed_carrier_validate_stream(
   const struct r300_noperspective_mixed_carrier_plan *plan,
   const float *carrier_records, uint32_t triangle_count);

/* Validates the clipper's expanded stream of vertex_count records,
 * skipping the padding record the clipper writes for an absent fan
 * triangle (position 0, 0, 0, 1 and every other lane 0).  Returns the
 * live record count, -EDOM on the first violation, -EINVAL on a null
 * stream. */
int r300_noperspective_mixed_carrier_validate_expanded(
   const struct r300_noperspective_mixed_carrier_plan *plan,
   const float *carrier_records, uint32_t vertex_count);

/* Walks a PM4 stream and returns the number of draw packets ahead of
 * which every carrier register -- GB_SELECT with W_SELECT clear,
 * VAP_VTX_SIZE 16, VAP_PROG_STREAM_CNTL_0/1, VAP_OUTPUT_VTX_FMT_1,
 * VAP_VSM_VTX_ASSM, RS_COUNT, RS_INST_COUNT, RS_IP_0..2, RS_INST_0..2
 * -- was written with the plan's word since the previous draw;
 * -(1 + n) names the first draw n a missing or foreign word precedes,
 * -EINVAL a malformed stream. */
int r300_noperspective_mixed_carrier_plan_stream_check(
   const struct r300_noperspective_mixed_carrier_plan *plan,
   uint32_t gb_select_base, const uint32_t *ib, uint32_t ib_dwords);

/* Host model of one recovered NoPerspective lane, b * rcp(c) on the
 * s1e7m16 grid (r300_noperspective_reciprocal_recover). */
float r300_noperspective_mixed_carrier_recover(float b, float c);

#endif /* R300_NOPERSPECTIVE_MIXED_CARRIER_PLAN_H */
