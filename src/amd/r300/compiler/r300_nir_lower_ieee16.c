/*
 * SPDX-License-Identifier: MIT
 *
 * NIR lowering passes for the emulated virtual IEEE FP16 machine
 * (R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL).
 *
 * Implements:
 *   1. r300_nir_lower_ieee16_classify: lowers the raw bit extraction and
 *      class partition using floor/modulo math in the positive domain.
 *   2. r300_nir_lower_ieee16_mul: lowers significand multiplication to
 *      2-limb base-64 RNE math.
 *
 * Emits only exact multipliers (no division/modulo) and flat selects (bcsel)
 * to avoid dynamic branching, keeping the math exact within the FP24 window.
 */

#include <stdbool.h>
#include "nir_builder.h"
#include "r300_nir.h"

static bool
lower_ieee16_classify_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if (alu->op != nir_op_ldexp)
      return false;

   /* ldexp is used as a 4-component vector placeholder for classification */
   if (alu->def.num_components != 4)
      return false;

   /* Retrieve bits from the red channel of the first source */
   b->cursor = nir_before_instr(&alu->instr);
   nir_def *bits = nir_channel(b, alu->src[0].src.ssa, 0);

   /* float sign = floor(bits / 32768.0); */
   nir_def *sign = nir_ffloor(b, nir_fmul_imm(b, bits, 1.0 / 32768.0));

   /* float exp5 = mod(floor(bits / 1024.0), 32.0); */
   nir_def *temp_exp = nir_ffloor(b, nir_fmul_imm(b, bits, 1.0 / 1024.0));
   nir_def *exp5 = nir_fsub(b, temp_exp,
      nir_fmul_imm(b, nir_ffloor(b, nir_fmul_imm(b, temp_exp, 1.0 / 32.0)), 32.0));

   /* float mant = bits - floor(bits / 1024.0) * 1024.0; */
   nir_def *mant = nir_fsub(b, bits, nir_fmul_imm(b, temp_exp, 1024.0));

   /* float qbit = floor(mant / 512.0); */
   nir_def *qbit = nir_ffloor(b, nir_fmul_imm(b, mant, 1.0 / 512.0));

   /* Conditionals */
   nir_def *exp_eq_31 = nir_feq_imm(b, exp5, 31.0);
   nir_def *exp_eq_0  = nir_feq_imm(b, exp5, 0.0);
   nir_def *mant_gt_0 = nir_fgt_imm(b, mant, 0.0);
   nir_def *mant_eq_0 = nir_feq_imm(b, mant, 0.0);
   nir_def *sign_gt_0 = nir_fgt_imm(b, sign, 0.0);
   nir_def *qbit_gt_0 = nir_fgt_imm(b, qbit, 0.0);

   /* Leaf outputs */
   nir_def *cls_nan    = nir_bcsel(b, qbit_gt_0, nir_imm_float(b, 8.0), nir_imm_float(b, 9.0));
   nir_def *cls_inf    = nir_bcsel(b, sign_gt_0, nir_imm_float(b, 7.0), nir_imm_float(b, 6.0));
   nir_def *cls_zero   = nir_bcsel(b, sign_gt_0, nir_imm_float(b, 1.0), nir_imm_float(b, 0.0));
   nir_def *cls_sub    = nir_bcsel(b, sign_gt_0, nir_imm_float(b, 3.0), nir_imm_float(b, 2.0));
   nir_def *cls_normal = nir_bcsel(b, sign_gt_0, nir_imm_float(b, 5.0), nir_imm_float(b, 4.0));

   /* Nested selects */
   nir_def *cond_zero = nir_iand(b, exp_eq_0, mant_eq_0);
   nir_def *cond_sub  = exp_eq_0;
   nir_def *cond_nan  = nir_iand(b, exp_eq_31, mant_gt_0);
   nir_def *cond_inf  = exp_eq_31;

   nir_def *cls = nir_bcsel(b, cond_nan, cls_nan,
                     nir_bcsel(b, cond_inf, cls_inf,
                        nir_bcsel(b, cond_zero, cls_zero,
                           nir_bcsel(b, cond_sub, cls_sub, cls_normal))));

   /* float mant_hi = floor(mant / 4.0); */
   nir_def *mant_hi = nir_ffloor(b, nir_fmul_imm(b, mant, 0.25));

   /* float mant_lo = mant - mant_hi * 4.0; */
   nir_def *mant_lo = nir_fsub(b, mant, nir_fmul_imm(b, mant_hi, 4.0));

   /* gl_FragColor = vec4(cls / 255.0, exp5 / 255.0, mant_hi / 255.0, mant_lo / 3.0); */
   nir_def *out_val = nir_vec4(b,
      nir_fmul_imm(b, cls, 1.0 / 255.0),
      nir_fmul_imm(b, exp5, 1.0 / 255.0),
      nir_fmul_imm(b, mant_hi, 1.0 / 255.0),
      nir_fmul_imm(b, mant_lo, 1.0 / 3.0));

   nir_def_replace(&alu->def, out_val);
   return true;
}

static bool
lower_ieee16_mul_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if (alu->op != nir_op_fpow)
      return false;

   /* fpow is used as a 4-component vector placeholder for normal significand multiplication */
   if (alu->def.num_components != 4)
      return false;

   /* Retrieve significands from channel 0 of both inputs */
   b->cursor = nir_before_instr(&alu->instr);
   nir_def *ua = nir_channel(b, alu->src[0].src.ssa, 0);
   nir_def *ub = nir_channel(b, alu->src[1].src.ssa, 0);

   /* split a */
   nir_def *a1 = nir_ffloor(b, nir_fmul_imm(b, ua, 1.0 / 64.0));
   nir_def *a0 = nir_fsub(b, ua, nir_fmul_imm(b, a1, 64.0));

   /* split b */
   nir_def *b1 = nir_ffloor(b, nir_fmul_imm(b, ub, 1.0 / 64.0));
   nir_def *b0 = nir_fsub(b, ub, nir_fmul_imm(b, b1, 64.0));

   /* column products */
   nir_def *c0 = nir_fmul(b, a0, b0);
   nir_def *c1 = nir_fadd(b, nir_fmul(b, a0, b1), nir_fmul(b, a1, b0));
   nir_def *c2 = nir_fmul(b, a1, b1);

   /* carry from c0 */
   nir_def *carry0 = nir_ffloor(b, nir_fmul_imm(b, c0, 1.0 / 64.0));
   nir_def *r0 = nir_fsub(b, c0, nir_fmul_imm(b, carry0, 64.0));

   /* carry from c1+carry0 */
   nir_def *c1c = nir_fadd(b, c1, carry0);
   nir_def *carry1 = nir_ffloor(b, nir_fmul_imm(b, c1c, 1.0 / 64.0));
   nir_def *r1 = nir_fsub(b, c1c, nir_fmul_imm(b, carry1, 64.0));

   /* top limb, split to avoid clamping */
   nir_def *r2 = nir_fadd(b, c2, carry1);
   nir_def *r2_hi = nir_ffloor(b, nir_fmul_imm(b, r2, 0.25));
   nir_def *r2_lo = nir_fsub(b, r2, nir_fmul_imm(b, r2_hi, 4.0));

   /* gl_FragColor = vec4(r0 / 255.0, r1 / 255.0, r2_hi / 255.0, r2_lo / 3.0); */
   nir_def *out_val = nir_vec4(b,
      nir_fmul_imm(b, r0, 1.0 / 255.0),
      nir_fmul_imm(b, r1, 1.0 / 255.0),
      nir_fmul_imm(b, r2_hi, 1.0 / 255.0),
      nir_fmul_imm(b, r2_lo, 1.0 / 3.0));

   nir_def_replace(&alu->def, out_val);
   return true;
}

static bool
lower_ieee16_mul_normal_rne_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if (alu->op != nir_op_fsin)
      return false;

   /* fsin used as placeholder for virtual normal-normal multiply.  fsin has
    * exactly one source, so both operands ride in the one vec4. */
   if (alu->def.num_components != 4)
      return false;

   b->cursor = nir_before_instr(&alu->instr);

   /* Source layout: [significand_a, biased_exp_a, significand_b, biased_exp_b] */
   nir_def *sa = nir_channel(b, alu->src[0].src.ssa, 0);
   nir_def *ea = nir_channel(b, alu->src[0].src.ssa, 1);
   nir_def *sb = nir_channel(b, alu->src[0].src.ssa, 2);
   nir_def *eb = nir_channel(b, alu->src[0].src.ssa, 3);

   /* 2-limb significand multiply */
   nir_def *a1 = nir_ffloor(b, nir_fmul_imm(b, sa, 1.0 / 64.0));
   nir_def *a0 = nir_fsub(b, sa, nir_fmul_imm(b, a1, 64.0));
   nir_def *b1 = nir_ffloor(b, nir_fmul_imm(b, sb, 1.0 / 64.0));
   nir_def *b0 = nir_fsub(b, sb, nir_fmul_imm(b, b1, 64.0));

   nir_def *c0 = nir_fmul(b, a0, b0);
   nir_def *c1 = nir_fadd(b, nir_fmul(b, a0, b1), nir_fmul(b, a1, b0));
   nir_def *c2 = nir_fmul(b, a1, b1);

   /* Carry propagation */
   nir_def *carry0 = nir_ffloor(b, nir_fmul_imm(b, c0, 1.0 / 64.0));
   nir_def *r0 = nir_fsub(b, c0, nir_fmul_imm(b, carry0, 64.0));
   nir_def *c1c = nir_fadd(b, c1, carry0);
   nir_def *carry1 = nir_ffloor(b, nir_fmul_imm(b, c1c, 1.0 / 64.0));
   nir_def *r1 = nir_fsub(b, c1c, nir_fmul_imm(b, carry1, 64.0));
   nir_def *r2 = nir_fadd(b, c2, carry1);

   /* Normalization and RNE rounding logic */
   nir_def *r2_ge_512 = nir_fge_imm(b, r2, 512.0);

   /* Exponent adjustment */
   nir_def *exp_norm = nir_bcsel(b, r2_ge_512, nir_imm_float(b, -14.0), nir_imm_float(b, -15.0));
   nir_def *biased_exp = nir_fadd(b, nir_fadd(b, ea, eb), exp_norm);

   /* significand extraction */
   nir_def *sig_hi = nir_bcsel(b, r2_ge_512, nir_fmul_imm(b, r2, 2.0), nir_fmul_imm(b, r2, 4.0));
   nir_def *r1_sh = nir_bcsel(b, r2_ge_512, nir_imm_float(b, 1.0 / 32.0), nir_imm_float(b, 1.0 / 16.0));
   nir_def *sig_lo = nir_ffloor(b, nir_fmul(b, r1, r1_sh));
   nir_def *sig = nir_fadd(b, sig_hi, sig_lo);

   /* guard and sticky */
   nir_def *g_sh = nir_bcsel(b, r2_ge_512, nir_imm_float(b, 1.0 / 16.0), nir_imm_float(b, 1.0 / 8.0));
   nir_def *gv = nir_ffloor(b, nir_fmul(b, r1, g_sh));
   nir_def *guard = nir_fsub(b, gv, nir_fmul_imm(b, nir_ffloor(b, nir_fmul_imm(b, gv, 0.5)), 2.0));

   nir_def *r1_low = nir_fsub(b, r1, nir_fmul(b, nir_ffloor(b, nir_fmul(b, r1, g_sh)),
                                              nir_bcsel(b, r2_ge_512, nir_imm_float(b, 16.0), nir_imm_float(b, 8.0))));
   nir_def *sticky = nir_bcsel(b, nir_fgt_imm(b, nir_fadd(b, r1_low, r0), 0.0), nir_imm_float(b, 1.0), nir_imm_float(b, 0.0));

   /* RNE condition: guard && (sticky || (sig & 1)) */
   nir_def *lsb = nir_fsub(b, sig, nir_fmul_imm(b, nir_ffloor(b, nir_fmul_imm(b, sig, 0.5)), 2.0));
   nir_def *round_up = nir_iand(b, nir_fgt_imm(b, guard, 0.0),
                                  nir_ior(b, nir_fgt_imm(b, sticky, 0.0), nir_fgt_imm(b, lsb, 0.0)));

   /* Apply rounding */
   nir_def *sig_round = nir_bcsel(b, round_up, nir_fadd_imm(b, sig, 1.0), sig);
   nir_def *sig_ovf = nir_fge_imm(b, sig_round, 2048.0);
   nir_def *sig_final = nir_bcsel(b, sig_ovf, nir_imm_float(b, 1024.0), sig_round);
   nir_def *exp_final = nir_bcsel(b, sig_ovf, nir_fadd_imm(b, biased_exp, 1.0), biased_exp);

   /* Result carrier: bits [15:10] = exp, bits [9:0] = mantissa (sig & 0x3ff) */
   nir_def *mantissa = nir_fsub(b, sig_final, nir_imm_float(b, 1024.0));
   nir_def *res_bits = nir_fadd(b, nir_fmul_imm(b, exp_final, 1024.0), mantissa);

   /* Encode to RGBA8_U16 carrier: R = bits & 0xff, G = bits >> 8 */
   nir_def *res_g = nir_ffloor(b, nir_fmul_imm(b, res_bits, 1.0 / 256.0));
   nir_def *res_r = nir_fsub(b, res_bits, nir_fmul_imm(b, res_g, 256.0));

   nir_def *out_val = nir_vec4(b,
      nir_fmul_imm(b, res_r, 1.0 / 255.0),
      nir_fmul_imm(b, res_g, 1.0 / 255.0),
      nir_imm_float(b, 0.0),
      nir_imm_float(b, 1.0)); /* A=1.0 as "normal result" flag */

   nir_def_replace(&alu->def, out_val);
   return true;
}

bool
r300_nir_lower_ieee16_classify(struct nir_shader *shader)
{
   return nir_shader_alu_pass(shader, lower_ieee16_classify_instr, nir_metadata_control_flow, NULL);
}

bool
r300_nir_lower_ieee16_mul(struct nir_shader *shader)
{
   return nir_shader_alu_pass(shader, lower_ieee16_mul_instr, nir_metadata_control_flow, NULL);
}

bool
r300_nir_lower_ieee16_mul_normal_rne(struct nir_shader *shader)
{
   return nir_shader_alu_pass(shader, lower_ieee16_mul_normal_rne_instr, nir_metadata_control_flow, NULL);
}
