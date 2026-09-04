/*
 * SPDX-License-Identifier: MIT
 *
 * Direct hardware Flat interpolation on RS485M (Radeon Xpress 200M,
 * CHIP_RS480, R300-class US/PFS fixed VLIW): the varying rides the
 * TCL-bypass color 0 vector, the GA selects the provoking vertex's
 * color per primitive, and the RS routes color 0 into the US input
 * register the pass-through fragment binary reads.
 */

#ifndef R300_FLAT_COLOR0_PLAN_H
#define R300_FLAT_COLOR0_PLAN_H

#include <stdbool.h>
#include <stdint.h>

struct r300_first_draw_contract;

/* The register plan for one Flat varying through color 0.  The
 * canonical direct plan is the one the route validator admits; every
 * other field combination is a calibration mutation the emitter still
 * realizes so the byte deviation and the refusal are both observable
 * ahead of silicon.
 *
 * Field           Register                 Canonical direct value
 * rgb0_shading    GA_COLOR_CONTROL[1:0]    RGB0_SHADING_FLAT
 * alpha0_shading  GA_COLOR_CONTROL[3:2]    ALPHA0_SHADING_FLAT
 * provoking       GA_COLOR_CONTROL[17:16]  PROVOKING_VERTEX_FIRST
 * rs_source       RS_IP_0                  color pointer 0, RGBA
 * us_input        RS_INST_0.COL_ADDR       0, the pass-through input
 */
enum r300_flat_color0_rs_source {
   /* RS_IP_0 reads color pointer 0 in RGBA order; RS_INST_0 writes it
    * through COL_CN_WRITE. */
   R300_FLAT_COLOR0_RS_SOURCE_COLOR0 = 0,
   /* RS_IP_0 reads texture pointer 0 with the identity channel
    * selects; RS_INST_0 writes it through TEX_CN_WRITE.  The carrier
    * still lands in the color vector, so the interpolator reads the
    * unwritten texture vector: a calibration mutation. */
   R300_FLAT_COLOR0_RS_SOURCE_TEX0,
};

struct r300_flat_color0_plan {
   /* R300_GA_COLOR_CONTROL_RGB0_SHADING_* field value, bits 1:0. */
   uint32_t rgb0_shading;
   /* R300_GA_COLOR_CONTROL_ALPHA0_SHADING_* field value, bits 3:2. */
   uint32_t alpha0_shading;
   /* R300_GA_COLOR_CONTROL_PROVOKING_VERTEX_* field value, bits 17:16. */
   uint32_t provoking;
   enum r300_flat_color0_rs_source rs_source;
   /* RS_INST_0 destination: the US input register the interpolated
    * color lands in. */
   uint32_t us_input;
};

/* The canonical direct plan: RGB and alpha both FLAT, provoking vertex
 * FIRST, color pointer 0 into US input 0. */
void r300_flat_color0_plan_direct_first(struct r300_flat_color0_plan *out);

/* Admits the canonical direct plan alone, so the selector that routes a
 * Flat varying to hardware selection only ever emits the qualified
 * state.  Returns 0, or -EINVAL naming a deviation: a Gouraud or solid
 * shading field, a provoking vertex other than FIRST, an RS source
 * other than color 0, or a US input other than 0, the register the
 * pass-through fragment binary (r300_tcl_bypass_triangle_varying_fs)
 * reads. */
int r300_flat_color0_plan_validate(const struct r300_flat_color0_plan *plan);

/* Register words the plan realizes.  ga_color_control keeps every
 * field of base outside RGB0, ALPHA0, and PROVOKING_VERTEX, so the
 * remaining shading lanes stay the contract's. */
uint32_t r300_flat_color0_plan_ga_color_control(
   const struct r300_flat_color0_plan *plan, uint32_t base);
uint32_t r300_flat_color0_plan_rs_count(
   const struct r300_flat_color0_plan *plan);
uint32_t r300_flat_color0_plan_rs_ip_0(
   const struct r300_flat_color0_plan *plan);
uint32_t r300_flat_color0_plan_rs_inst_0(
   const struct r300_flat_color0_plan *plan);
/* VAP_VSM_VTX_ASSM: position plus the vector the RS source reads. */
uint32_t r300_flat_color0_plan_vsm_vtx_assm(
   const struct r300_flat_color0_plan *plan);

/* Programs the plan into a resolved first-draw contract: GA_COLOR_CONTROL,
 * RS_COUNT, RS_INST_COUNT, RS_IP_0, RS_INST_0, and VAP_VSM_VTX_ASSM take
 * the plan's words, so every draw's own contract prefix establishes the
 * interpolation state and a second pass inherits nothing from a first.
 * Returns 0, or -EINVAL when the contract lacks one of those clauses. */
int r300_flat_color0_plan_apply_contract(
   const struct r300_flat_color0_plan *plan,
   struct r300_first_draw_contract *contract);

/* The per-pass state check over an emitted stream: ahead of every draw
 * packet, each register the plan programs holds the plan's word.  A
 * pass begins with the first plan register a cell writes after an
 * earlier draw, and that boundary drops the words the previous pass
 * established, so a pass that writes some of its words and inherits
 * the rest fails at its own draw, while the segment draws one cell
 * emits behind one contract prefix share the prefix.  ga_base is the
 * contract's GA_COLOR_CONTROL word the plan overlays.  Returns the
 * number of draw packets checked, 0 when the stream carries no draw, a
 * negative value -(1 + index) naming the first draw whose state
 * deviates, or -EINVAL for a NULL argument or a packet claiming payload
 * past the end. */
int r300_flat_color0_plan_stream_check(
   const struct r300_flat_color0_plan *plan, uint32_t ga_base,
   const uint32_t *ib, uint32_t ib_dwords);

#endif /* R300_FLAT_COLOR0_PLAN_H */
