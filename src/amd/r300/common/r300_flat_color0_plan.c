/*
 * SPDX-License-Identifier: MIT
 *
 * Direct hardware Flat interpolation plan through color 0 on RS482.
 */

#include "r300_flat_color0_plan.h"

#include "r300_first_draw_state.h"
#include "r300_reg.h"

#include <errno.h>
#include <string.h>

#define GA_RGB0_MASK 0x3u
#define GA_ALPHA0_MASK 0xcu
#define GA_PROVOKING_MASK 0x30000u

void
r300_flat_color0_plan_direct_first(struct r300_flat_color0_plan *out)
{
   memset(out, 0, sizeof(*out));
   out->rgb0_shading = R300_GA_COLOR_CONTROL_RGB0_SHADING_FLAT;
   out->alpha0_shading = R300_GA_COLOR_CONTROL_ALPHA0_SHADING_FLAT;
   out->provoking = R300_GA_COLOR_CONTROL_PROVOKING_VERTEX_FIRST;
   out->rs_source = R300_FLAT_COLOR0_RS_SOURCE_COLOR0;
   out->us_input = 0;
}

int
r300_flat_color0_plan_validate(const struct r300_flat_color0_plan *plan)
{
   if (plan == NULL)
      return -EINVAL;
   if (plan->rgb0_shading != R300_GA_COLOR_CONTROL_RGB0_SHADING_FLAT ||
       plan->alpha0_shading != R300_GA_COLOR_CONTROL_ALPHA0_SHADING_FLAT ||
       plan->provoking != R300_GA_COLOR_CONTROL_PROVOKING_VERTEX_FIRST ||
       plan->rs_source != R300_FLAT_COLOR0_RS_SOURCE_COLOR0 ||
       plan->us_input != 0)
      return -EINVAL;
   return 0;
}

uint32_t
r300_flat_color0_plan_ga_color_control(
   const struct r300_flat_color0_plan *plan, uint32_t base)
{
   return (base & ~(GA_RGB0_MASK | GA_ALPHA0_MASK | GA_PROVOKING_MASK)) |
          (plan->rgb0_shading & GA_RGB0_MASK) |
          (plan->alpha0_shading & GA_ALPHA0_MASK) |
          (plan->provoking & GA_PROVOKING_MASK);
}

uint32_t
r300_flat_color0_plan_rs_count(const struct r300_flat_color0_plan *plan)
{
   /* One rasterized color and no texture components on the color
    * source; the texture-source mutation declares the TEX0 form the
    * varying cell uses. */
   if (plan->rs_source == R300_FLAT_COLOR0_RS_SOURCE_TEX0)
      return R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN;
   return R300_IT_COUNT(0) | R300_IC_COUNT(1) | R300_HIRES_EN;
}

uint32_t
r300_flat_color0_plan_rs_ip_0(const struct r300_flat_color0_plan *plan)
{
   if (plan->rs_source == R300_FLAT_COLOR0_RS_SOURCE_TEX0)
      return R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
             R300_RS_SEL_T(R300_RS_SEL_C1) | R300_RS_SEL_R(R300_RS_SEL_C2) |
             R300_RS_SEL_Q(R300_RS_SEL_C3);
   return R300_RS_COL_PTR(0) | R300_RS_COL_FMT(R300_RS_COL_FMT_RGBA);
}

uint32_t
r300_flat_color0_plan_rs_inst_0(const struct r300_flat_color0_plan *plan)
{
   if (plan->rs_source == R300_FLAT_COLOR0_RS_SOURCE_TEX0)
      return R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
             R300_RS_INST_TEX_ADDR(plan->us_input);
   return R300_RS_INST_COL_ID(0) | R300_RS_INST_COL_CN_WRITE |
          R300_RS_INST_COL_ADDR(plan->us_input);
}

uint32_t
r300_flat_color0_plan_vsm_vtx_assm(const struct r300_flat_color0_plan *plan)
{
   if (plan->rs_source == R300_FLAT_COLOR0_RS_SOURCE_TEX0)
      return R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0;
   return R300_INPUT_CNTL_POS | R300_INPUT_CNTL_COLOR;
}

struct plan_register {
   uint32_t reg;
   uint32_t value;
};

/* The six registers the plan owns, in one place for the contract
 * application and the stream check. */
static unsigned
plan_registers(const struct r300_flat_color0_plan *plan, uint32_t ga_base,
               struct plan_register out[6])
{
   out[0].reg = R300_GA_COLOR_CONTROL;
   out[0].value = r300_flat_color0_plan_ga_color_control(plan, ga_base);
   out[1].reg = R300_RS_COUNT;
   out[1].value = r300_flat_color0_plan_rs_count(plan);
   out[2].reg = R300_RS_INST_COUNT;
   out[2].value = 0;
   out[3].reg = R300_RS_IP_0;
   out[3].value = r300_flat_color0_plan_rs_ip_0(plan);
   out[4].reg = R300_RS_INST_0;
   out[4].value = r300_flat_color0_plan_rs_inst_0(plan);
   out[5].reg = R300_VAP_VSM_VTX_ASSM;
   out[5].value = r300_flat_color0_plan_vsm_vtx_assm(plan);
   return 6;
}

int
r300_flat_color0_plan_apply_contract(
   const struct r300_flat_color0_plan *plan,
   struct r300_first_draw_contract *contract)
{
   if (plan == NULL || contract == NULL)
      return -EINVAL;
   uint32_t ga_base = 0;
   bool ga_found = false;
   for (uint32_t i = 0; i < contract->count; i++) {
      if (contract->entries[i].reg == R300_GA_COLOR_CONTROL) {
         ga_base = contract->entries[i].value;
         ga_found = true;
         break;
      }
   }
   if (!ga_found)
      return -EINVAL;
   struct plan_register regs[6];
   const unsigned n = plan_registers(plan, ga_base, regs);
   for (unsigned i = 0; i < n; i++) {
      const int rc = r300_first_draw_contract_set_entry(
         contract, regs[i].reg, regs[i].value);
      if (rc != 0)
         return rc;
   }
   return 0;
}

int
r300_flat_color0_plan_stream_check(
   const struct r300_flat_color0_plan *plan, uint32_t ga_base,
   const uint32_t *ib, uint32_t ib_dwords)
{
   if (plan == NULL || ib == NULL)
      return -EINVAL;
   struct plan_register regs[6];
   const unsigned n = plan_registers(plan, ga_base, regs);
   bool written[6] = { false };
   uint32_t state[6] = { 0 };
   int draws = 0;
   /* Set once a draw has consumed the current words; the next plan
    * register write then opens a new pass and drops the rest. */
   bool pass_consumed = false;
   uint32_t i = 0;
   while (i < ib_dwords) {
      const uint32_t header = ib[i];
      const uint32_t kind = header >> 30;
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      if (kind == 0) {
         if (i + count >= ib_dwords)
            return -EINVAL;
         const uint32_t base = (header & 0x3FFF) * 4;
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0;
         for (uint32_t k = 0; k < count; k++) {
            const uint32_t reg = base + (one_reg ? 0 : 4 * k);
            for (unsigned e = 0; e < n; e++) {
               if (regs[e].reg != reg)
                  continue;
               /* The first plan register a cell writes after an
                * earlier draw opens that cell's pass: the words the
                * previous pass established count for nothing, so a
                * pass that writes some of its words and inherits the
                * rest fails at its draw, while the segment draws one
                * cell emits behind one prefix share the prefix. */
               if (pass_consumed) {
                  memset(written, 0, sizeof(written));
                  pass_consumed = false;
               }
               state[e] = ib[i + 1 + k];
               written[e] = true;
            }
         }
         i += 1 + count;
      } else if (kind == 3) {
         if (i + count >= ib_dwords)
            return -EINVAL;
         if (r300_first_draw_is_draw_packet(header)) {
            for (unsigned e = 0; e < n; e++) {
               if (!written[e] || state[e] != regs[e].value)
                  return -(1 + draws);
            }
            draws++;
            pass_consumed = true;
         }
         i += 1 + count;
      } else {
         i += 1;
      }
   }
   return draws;
}
