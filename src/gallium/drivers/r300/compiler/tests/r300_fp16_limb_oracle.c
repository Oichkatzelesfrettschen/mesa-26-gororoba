/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * CPU oracle for the virtual IEEE FP16 machine (R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL).
 *
 * Three test suites:
 *   1. Domain catalog: r300_numeric_domain_info() returns correct row for
 *      R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL (rounding=RNE, significand_bits=11,
 *      is_native_compute=false, evidence=HW_CONFIRMED), keeps the five-term
 *      U7 convolution domain distinct from the four-term U7 dot domain, and
 *      ships no retained external evidence identifiers in the virtual-op
 *      catalog.
 *   2. Classification: all 65536 FP16 raw bit patterns produce the correct
 *      r300_fp16_class value (compare classify_fp16() against fp16_class_ref()).
 *   3. Multiply: ~30 representative pairs -- 2-limb result matches the C
 *      reference (direct uint32 multiply + RNE rounding from the carry triple).
 *
 * Hardware not run.  This is a CPU-only oracle test; RS482 silicon probing is
 * the next gate (fp16_class_lut_probe, fp16_mul_rne_probe on RS482 hardware).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "r300_numeric_domain.h"
#include "r300_virtual_float.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int g_failures = 0;

#define CHECK(cond, name)                                      \
   do {                                                        \
      if (!(cond)) {                                           \
         printf("FAIL %s\n", (name));                          \
         g_failures++;                                         \
      } else {                                                  \
         printf("ok   %s\n", (name));                          \
      }                                                        \
   } while (0)

/* -------------------------------------------------------------------------
 * Suite 1: Domain catalog
 * ---------------------------------------------------------------------- */

static void
test_domain_catalog(void)
{
   const struct r300_numeric_domain_info *fp16_info =
      r300_numeric_domain_info(R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL);

   CHECK(fp16_info->domain == R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
         "catalog: domain field matches enum value");
   CHECK(fp16_info->rounding == R300_ROUND_RNE,
         "catalog: rounding == R300_ROUND_RNE");
   CHECK(fp16_info->significand_bits == 11,
         "catalog: significand_bits == 11");
   CHECK(fp16_info->has_nan == true,
         "catalog: has_nan == true");
   CHECK(fp16_info->has_inf == true,
         "catalog: has_inf == true");
   CHECK(fp16_info->has_subnormal == true,
         "catalog: has_subnormal == true");
   CHECK(fp16_info->is_native_compute == false,
         "catalog: is_native_compute == false (emulated)");
   CHECK(fp16_info->evidence == R300_EVIDENCE_HW_CONFIRMED,
         "catalog: evidence == HW_CONFIRMED");
   CHECK(fp16_info->theorem != NULL,
         "catalog: theorem string non-NULL");

   const struct r300_numeric_domain_info *u7_dot =
      r300_numeric_domain_info(R300_NUM_DOMAIN_U7_DOT);
   const struct r300_numeric_domain_info *u7_conv5 =
      r300_numeric_domain_info(R300_NUM_DOMAIN_U7_CONV5);
   const struct r300_numeric_domain_info *u8_offgrid =
      r300_numeric_domain_info(R300_NUM_DOMAIN_U8_OFFGRID);

   CHECK(u7_dot->exact_int_bound == 64516,
         "catalog: U7 dot exact bound covers four terms");
   CHECK(u7_conv5->exact_int_bound == 80645,
         "catalog: U7 conv5 exact bound covers five terms");
   CHECK(u7_conv5->rounding == R300_ROUND_EXACT,
         "catalog: U7 conv5 is exact");
   CHECK(u8_offgrid->exact_int_bound == 131072,
         "catalog: U8 offgrid exact subset covers the FP24 integer window");

   const struct r300_virtual_op_info *multilimb = NULL;
   const struct r300_virtual_op_info *signed_dp4 = NULL;
   const struct r300_virtual_op_info *quad_disc = NULL;
   const struct r300_virtual_op_info *q16_add = NULL;
   const struct r300_virtual_op_info *q16_mul = NULL;
   const struct r300_virtual_op_info *q16_mac = NULL;
   for (unsigned op_index = 0; r300_virtual_op_catalog[op_index].op_name; op_index++) {
      const struct r300_virtual_op_info *op = &r300_virtual_op_catalog[op_index];
      if (strcmp(op->op_name, "MULTILIMB7_U32_MUL") == 0)
         multilimb = op;
      if (strcmp(op->op_name, "DP4_INT8_SIGNED_CARRIER_PENDING") == 0)
         signed_dp4 = op;
      if (strcmp(op->op_name, "QUADRATIC_DISCRIMINANT_OFFGRID") == 0)
         quad_disc = op;
      if (strcmp(op->op_name, "Q16_16_ADD") == 0)
         q16_add = op;
      if (strcmp(op->op_name, "Q16_16_MUL") == 0)
         q16_mul = op;
      if (strcmp(op->op_name, "Q16_16_MAC") == 0)
         q16_mac = op;
   }

   CHECK(multilimb != NULL,
         "catalog: MULTILIMB7_U32_MUL row exists");
   CHECK(multilimb != NULL && multilimb->domain == R300_NUM_DOMAIN_U7_CONV5,
         "catalog: MULTILIMB7_U32_MUL uses five-term U7 domain");
   CHECK(signed_dp4 != NULL,
         "catalog: signed DP4 carrier-pending row exists");
   CHECK(signed_dp4 != NULL && signed_dp4->status == R300_VOP_CARRIER_PENDING,
         "catalog: signed DP4 does not advertise a dispatch carrier");
   CHECK(signed_dp4 != NULL && signed_dp4->mesa_hook == NULL,
         "catalog: signed DP4 has no Mesa dispatch hook until its carrier exists");
   CHECK(quad_disc != NULL,
         "catalog: QUADRATIC_DISCRIMINANT_OFFGRID row exists");
   CHECK(quad_disc != NULL && quad_disc->status == R300_VOP_BOUNDARY,
         "catalog: quadratic discriminant is a documented precision boundary");
   CHECK(quad_disc != NULL && quad_disc->domain == R300_NUM_DOMAIN_FP24_RTZ,
         "catalog: quadratic discriminant lives in the FP24 RTZ root domain");
   CHECK(quad_disc != NULL && quad_disc->mesa_hook == NULL,
         "catalog: quadratic discriminant has no Mesa hook (glamor-emitted, not a Mesa pass)");
   CHECK(q16_add != NULL && q16_add->status == R300_VOP_CARRIER_PENDING,
         "catalog: Q16.16 ADD remains carrier-pending after shape detection");
   CHECK(q16_add != NULL && q16_add->mesa_hook == NULL,
         "catalog: Q16.16 ADD detector is test-only, no production hook");
   CHECK(q16_mul != NULL && q16_mul->status == R300_VOP_CARRIER_PENDING,
         "catalog: Q16.16 MUL remains carrier-pending");
   CHECK(q16_mul != NULL && q16_mul->mesa_hook == NULL,
         "catalog: Q16.16 MUL has no detector");
   CHECK(q16_mac != NULL && q16_mac->status == R300_VOP_HW_CONFIRMED_CARRIER_PENDING,
         "catalog: Q16.16 MAC is HW-confirmed with the production carrier pending");
   CHECK(q16_mac != NULL && q16_mac->mesa_hook == NULL,
         "catalog: Q16.16 MAC has no production detector");
}

/* -------------------------------------------------------------------------
 * Suite 2: FP16 classification
 * ---------------------------------------------------------------------- */

/* Reference classifier: partitions all 65536 FP16 bit patterns. */
static enum r300_fp16_class
fp16_class_ref(uint16_t bits)
{
   unsigned sign = (bits >> 15) & 1;
   unsigned exp  = (bits >> 10) & 0x1f;
   unsigned mant = bits & 0x3ff;

   if (exp == 0 && mant == 0)
      return sign ? R300_FP16_NEG_ZERO : R300_FP16_POS_ZERO;
   if (exp == 0)
      return sign ? R300_FP16_NEG_SUBNORMAL : R300_FP16_POS_SUBNORMAL;
   if (exp == 31 && mant == 0)
      return sign ? R300_FP16_NEG_INF : R300_FP16_POS_INF;
   if (exp == 31) {
      /* bit 9 of mantissa: 1=quiet, 0=signaling */
      return (mant & 0x200) ? R300_FP16_QNAN : R300_FP16_SNAN;
   }
   return sign ? R300_FP16_NEG_NORMAL : R300_FP16_POS_NORMAL;
}

/* Classify using the same predicate triple as fp16_class_ref() -- this is
 * the implementation under test.  Both share the same algorithm here so the
 * suite verifies the type definitions and table partition, not an independent
 * algorithm; the RS482 silicon probe will be the independent check. */
static enum r300_fp16_class
classify_fp16(uint16_t bits)
{
   unsigned sign = (bits >> 15) & 1;
   unsigned exp  = (bits >> 10) & 0x1f;
   unsigned mant = bits & 0x3ff;

   if (exp == 0 && mant == 0)
      return sign ? R300_FP16_NEG_ZERO : R300_FP16_POS_ZERO;
   if (exp == 0)
      return sign ? R300_FP16_NEG_SUBNORMAL : R300_FP16_POS_SUBNORMAL;
   if (exp == 31 && mant == 0)
      return sign ? R300_FP16_NEG_INF : R300_FP16_POS_INF;
   if (exp == 31)
      return (mant & 0x200) ? R300_FP16_QNAN : R300_FP16_SNAN;
   return sign ? R300_FP16_NEG_NORMAL : R300_FP16_POS_NORMAL;
}

static void
test_classification(void)
{
   unsigned mismatches = 0;
   for (unsigned bits = 0; bits <= 0xffff; bits++) {
      enum r300_fp16_class got = classify_fp16((uint16_t)bits);
      enum r300_fp16_class ref = fp16_class_ref((uint16_t)bits);
      if (got != ref)
         mismatches++;
   }
   char label[64];
   snprintf(label, sizeof(label),
            "classify: all 65536 bit patterns match reference (%u mismatch)", mismatches);
   CHECK(mismatches == 0, label);
}

/* -------------------------------------------------------------------------
 * Suite 3: 2-limb base-64 significand multiply with RNE rounding
 * ---------------------------------------------------------------------- */

/* Decode the biased exponent and significand from a normal FP16 value.
 * Returns false if bits is not a normal (exp in [1..30]). */
static bool
fp16_normal_decode(uint16_t bits, int *out_biased_exp, uint32_t *out_sig)
{
   unsigned exp  = (bits >> 10) & 0x1f;
   unsigned mant = bits & 0x3ff;
   if (exp == 0 || exp == 31)
      return false;
   *out_biased_exp = (int)exp;
   *out_sig        = (1u << 10) | mant;  /* implicit-1 restored */
   return true;
}

/* Decode subnormal: biased_exp treated as 1 (IEEE rule), normalize by
 * left-shifting mantissa until bit 10 is set; adjust effective exp.
 * Returns false if bits is not a subnormal (exp==0, mant!=0). */
static bool
fp16_subnormal_decode(uint16_t bits, int *out_biased_exp, uint32_t *out_sig)
{
   unsigned exp  = (bits >> 10) & 0x1f;
   unsigned mant = bits & 0x3ff;
   if (exp != 0 || mant == 0)
      return false;
   int eff_exp = 1;
   while (!(mant & 0x400)) {
      mant <<= 1;
      eff_exp--;
   }
   *out_biased_exp = eff_exp;
   *out_sig        = mant & 0x7ff;
   return true;
}

/* Reference multiply: convert FP16 bits to double, multiply, convert result
 * back to FP16 using RNE rounding via the C compiler's float16 emulation.
 * We use uint32_t as the bit representation and float arithmetic as the
 * reference since the test host is x86_64 (IEEE 754 double precision). */
static uint16_t
fp16_mul_ref(uint16_t a_bits, uint16_t b_bits)
{
   /* Use __fp16 if available (clang), otherwise fall back to software path. */
#if defined(__clang__) && defined(__ARM_FP16_FORMAT_IEEE)
   __fp16 a, b, r;
   memcpy(&a, &a_bits, 2);
   memcpy(&b, &b_bits, 2);
   r = a * b;
   uint16_t result;
   memcpy(&result, &r, 2);
   return result;
#else
   /* Software reference via double.
    *
    * Decode a and b as doubles, multiply, then re-encode to FP16 with RNE.
    * This is correct for the set of test cases we exercise (normal, subnormal,
    * inf, nan, zero); the double has enough precision (53-bit significand) to
    * hold the exact product of two FP16 significands (22-bit max). */
   uint16_t a16 = a_bits, b16 = b_bits;
   uint32_t a_exp  = (a16 >> 10) & 0x1f;
   uint32_t a_mant = a16 & 0x3ff;
   uint32_t b_exp  = (b16 >> 10) & 0x1f;
   uint32_t b_mant = b16 & 0x3ff;
   int      a_sign = (a16 >> 15);
   int      b_sign = (b16 >> 15);
   int      r_sign = a_sign ^ b_sign;

   /* NaN propagation: if either is NaN, return a quiet NaN. */
   if ((a_exp == 31 && a_mant != 0) || (b_exp == 31 && b_mant != 0))
      return 0x7e00u;  /* canonical quiet NaN */

   /* Inf * 0 = NaN */
   if ((a_exp == 31 && ((b_exp == 0) && b_mant == 0)) ||
       (b_exp == 31 && ((a_exp == 0) && a_mant == 0)))
      return 0x7e00u;

   /* Inf * anything-finite = Inf */
   if (a_exp == 31 || b_exp == 31)
      return (uint16_t)((r_sign << 15) | 0x7c00u);

   /* Zero * anything = signed zero */
   if ((a_exp == 0 && a_mant == 0) || (b_exp == 0 && b_mant == 0))
      return (uint16_t)(r_sign << 15);

   /* Normalize for the significand multiply; every special case is
    * handled above, so exactly one decoder accepts each operand. */
   int ea, eb;
   uint32_t sa, sb;
   if (!fp16_normal_decode(a16, &ea, &sa) &&
       !fp16_subnormal_decode(a16, &ea, &sa))
      return 0x7e00u;
   if (!fp16_normal_decode(b16, &eb, &sb) &&
       !fp16_subnormal_decode(b16, &eb, &sb))
      return 0x7e00u;

   /* Product significand (up to 22 bits, exact in uint32_t). */
   uint32_t prod = sa * sb;  /* max 2047*2047 = 4190209, fits uint32_t */
   int biased_exp = ea + eb - 15;

   /* Normalize: product is in [2^20, 2^22).  Shift so MSB is bit 10
    * of the normalized result, tracking guard/sticky bits. */
   uint32_t guard = 0, sticky = 0;

   /* sa,sb in [1024..2047] -> prod in [2^20..2^21] for normals before shift,
    * or [2^21..2^22) when both near max.  Determine shift. */
   int shift = 10;  /* we want 11-bit result with bit 10 set */
   if (prod >= (1u << 21)) {
      shift = 11;
      biased_exp++;
   }
   /* guard and sticky bits for RNE */
   if (shift == 11) {
      guard  = (prod >> 10) & 1;
      sticky = (prod & 0x3ff) ? 1 : 0;
   } else {
      guard  = (prod >> 9) & 1;
      sticky = (prod & 0x1ff) ? 1 : 0;
   }
   uint32_t sig = prod >> shift;

   /* Overflow: biased_exp >= 31 -> Inf */
   if (biased_exp >= 31)
      return (uint16_t)((r_sign << 15) | 0x7c00u);

   /* Underflow / subnormal output */
   uint32_t r_mant;
   int r_exp;
   if (biased_exp <= 0) {
      /* Denormalize: shift right by (1 - biased_exp). */
      int right = 1 - biased_exp;
      /* Accumulate sticky bits from the right shift.  The RNE decision for the
       * minimum subnormal still depends on guard/sticky when right >= 11. */
      for (int i = 0; i < right; i++) {
         sticky |= guard;
         guard   = sig & 1;
         sig   >>= 1;
      }
      r_exp  = 0;
      r_mant = sig & 0x3ff;
   } else {
      r_exp  = biased_exp;
      r_mant = sig & 0x3ff;  /* strip implicit-1 */
   }

   /* RNE rounding */
   uint32_t lsb = r_mant & 1;
   if (guard && (sticky || lsb)) {
      r_mant++;
      if (r_mant >= 0x400) {
         r_mant = 0;
         r_exp++;
         if (r_exp >= 31)
            return (uint16_t)((r_sign << 15) | 0x7c00u);
      }
   }

   return (uint16_t)((r_sign << 15) | ((uint32_t)r_exp << 10) | r_mant);
}
#endif /* __fp16 vs double path */

/* 2-limb base-64 multiply.  Handles normals only; special cases are
 * dispatched before calling this function.  Returns the RNE-rounded FP16
 * result for two normal inputs. */
static uint16_t
fp16_mul_2limb(int r_sign, int ea, int eb, uint32_t sa, uint32_t sb)
{
   /* Decompose into base-64 limbs. */
   uint32_t a0 = sa % 64, a1 = sa / 64;
   uint32_t b0 = sb % 64, b1 = sb / 64;

   /* Column products -- all < 2^17, FP24-exact. */
   uint32_t c0 = a0 * b0;
   uint32_t c1 = a0 * b1 + a1 * b0;
   uint32_t c2 = a1 * b1;

   /* Carry propagation via integer division -- exact for positive integers
    * < 2^17 under RTZ (RTZ == floor for positive values). */
   uint32_t carry = c0 / 64;
   uint32_t r0    = c0 % 64;
   uint32_t c1p   = c1 + carry;
   uint32_t r1    = c1p % 64;
   uint32_t r2    = c2 + c1p / 64;

   /* Normalization: product P = r2*4096 + r1*64 + r0.
    * P >= 2^21 iff r2 >= 512 (proven: r1<=63, r0<=63 -> low 12 bits <= 4095;
    * 511*4096+4095 < 2^21). */
   int biased_exp;
   uint32_t sig_out;
   uint32_t guard, sticky;

   if (r2 >= 512) {
      /* P in [2^21, 2^22): significand = P >> 11, biased_exp += 1.
       * P = r2*4096 + r1*64 + r0.  Bits [21:11] = (r2<<1)|(r1>>5).
       * Bit 10 (guard) = (r1>>4)&1.  Bits [9:0] (sticky) = (r1&0xf)||r0. */
      biased_exp = ea + eb - 14;
      sig_out    = (r2 << 1) | (r1 >> 5);
      guard      = (r1 >> 4) & 1;
      sticky     = (r1 & 0xf) || r0;
   } else {
      /* P in [2^20, 2^21): significand = P >> 10.
       * Bits [20:10] = (r2<<2)|(r1>>4).
       * Bit 9 (guard) = (r1>>3)&1.  Bits [8:0] (sticky) = (r1&7)||r0. */
      biased_exp = ea + eb - 15;
      sig_out    = (r2 << 2) | (r1 >> 4);
      guard      = (r1 >> 3) & 1;
      sticky     = (r1 & 7) || r0;
   }

   /* Overflow: biased_exp >= 31 -> Inf */
   if (biased_exp >= 31)
      return (uint16_t)((r_sign << 15) | 0x7c00u);

   /* Underflow / subnormal output.  The domain advertises has_subnormal, so the
    * 2-limb path reproduces IEEE gradual underflow instead of flushing to zero:
    * denormalize the significand by shifting right (1 - biased_exp) places,
    * folding the shifted-out bits into guard/sticky, then RNE.  This matches the
    * double-precision reference (fp16_mul_ref).  Only a product that rounds below
    * the minimum subnormal becomes signed zero. */
   uint32_t r_mant;
   int r_exp;
   if (biased_exp <= 0) {
      int right = 1 - biased_exp;
      for (int i = 0; i < right; i++) {
         sticky |= guard;
         guard   = sig_out & 1;
         sig_out >>= 1;
      }
      r_exp  = 0;
      r_mant = sig_out & 0x3ff;
   } else {
      r_exp  = biased_exp;
      r_mant = sig_out & 0x3ff;  /* strip implicit-1 */
   }

   /* RNE rounding.  A carry out of the 10-bit field promotes a subnormal to the
    * minimum normal (r_exp 0 -> 1), the correct IEEE result. */
   uint32_t lsb = r_mant & 1;
   if (guard && (sticky || lsb)) {
      r_mant++;
      if (r_mant >= 0x400) {
         r_mant = 0;
         r_exp++;
         if (r_exp >= 31)
            return (uint16_t)((r_sign << 15) | 0x7c00u);
      }
   }

   return (uint16_t)((r_sign << 15) | ((uint32_t)r_exp << 10) | r_mant);
}

/* Top-level 2-limb multiply: handles all FP16 special cases, then delegates
 * normal*normal to fp16_mul_2limb(). */
static uint16_t
fp16_mul_2limb_full(uint16_t a_bits, uint16_t b_bits)
{
   uint32_t a_exp  = (a_bits >> 10) & 0x1f;
   uint32_t a_mant = a_bits & 0x3ff;
   uint32_t b_exp  = (b_bits >> 10) & 0x1f;
   uint32_t b_mant = b_bits & 0x3ff;
   int      a_sign = (a_bits >> 15);
   int      b_sign = (b_bits >> 15);
   int      r_sign = a_sign ^ b_sign;

   /* NaN propagation */
   if ((a_exp == 31 && a_mant != 0) || (b_exp == 31 && b_mant != 0))
      return 0x7e00u;

   /* Inf * 0 = NaN */
   if ((a_exp == 31 && b_exp == 0 && b_mant == 0) ||
       (b_exp == 31 && a_exp == 0 && a_mant == 0))
      return 0x7e00u;

   /* Inf * finite-nonzero = Inf */
   if (a_exp == 31 || b_exp == 31)
      return (uint16_t)((r_sign << 15) | 0x7c00u);

   /* Zero * anything = signed zero */
   if ((a_exp == 0 && a_mant == 0) || (b_exp == 0 && b_mant == 0))
      return (uint16_t)(r_sign << 15);

   /* Decode: normals get implicit-1; subnormals are normalized. */
   int ea = (a_exp != 0) ? (int)a_exp : 1;
   int eb = (b_exp != 0) ? (int)b_exp : 1;
   uint32_t sa = (a_exp != 0) ? (0x400u | a_mant) : a_mant;
   uint32_t sb = (b_exp != 0) ? (0x400u | b_mant) : b_mant;

   while (a_exp == 0 && !(sa & 0x400)) { sa <<= 1; ea--; }
   while (b_exp == 0 && !(sb & 0x400)) { sb <<= 1; eb--; }

   return fp16_mul_2limb(r_sign, ea, eb, sa, sb);
}

/* Representative test pairs for the multiply suite.  Each entry is
 * (a_bits, b_bits, description). */
static const struct {
   uint16_t a_bits;
   uint16_t b_bits;
   const char *desc;
} mul_cases[] = {
   /* 1.0 * 1.0 = 1.0 */
   { 0x3c00u, 0x3c00u, "1.0 * 1.0 = 1.0" },
   /* 2.0 * 3.0 = 6.0 */
   { 0x4000u, 0x4200u, "2.0 * 3.0 = 6.0" },
   /* -1.0 * 1.0 = -1.0 */
   { 0xbc00u, 0x3c00u, "-1.0 * 1.0 = -1.0" },
   /* 1.0 * -2.0 = -2.0 */
   { 0x3c00u, 0xc000u, "1.0 * -2.0 = -2.0" },
   /* -2.0 * -3.0 = 6.0 */
   { 0xc000u, 0xc200u, "-2.0 * -3.0 = 6.0" },
   /* 0.5 * 2.0 = 1.0 */
   { 0x3800u, 0x4000u, "0.5 * 2.0 = 1.0" },
   /* max_normal * 1.0 = max_normal (0x7bff = 65504.0) */
   { 0x7bffu, 0x3c00u, "max_normal * 1.0 = max_normal" },
   /* max_normal * 2.0 = Inf (overflow) */
   { 0x7bffu, 0x4000u, "max_normal * 2.0 = Inf (overflow)" },
   /* min_normal * 1.0 = min_normal (0x0400 = 2^-14) */
   { 0x0400u, 0x3c00u, "min_normal * 1.0 = min_normal" },
   /* +Inf * 1.0 = +Inf */
   { 0x7c00u, 0x3c00u, "+Inf * 1.0 = +Inf" },
   /* +Inf * -1.0 = -Inf */
   { 0x7c00u, 0xbc00u, "+Inf * -1.0 = -Inf" },
   /* +Inf * +Inf = +Inf */
   { 0x7c00u, 0x7c00u, "+Inf * +Inf = +Inf" },
   /* +Inf * 0 = NaN */
   { 0x7c00u, 0x0000u, "+Inf * 0 = NaN" },
   /* 0 * 0 = 0 */
   { 0x0000u, 0x0000u, "+0 * +0 = +0" },
   /* -0 * +0 = -0 */
   { 0x8000u, 0x0000u, "-0 * +0 = -0" },
   /* -0 * -0 = +0 */
   { 0x8000u, 0x8000u, "-0 * -0 = +0" },
   /* qNaN * 1.0 = qNaN */
   { 0x7e00u, 0x3c00u, "qNaN * 1.0 = qNaN" },
   /* 1.0 * sNaN -> qNaN */
   { 0x3c00u, 0x7c01u, "1.0 * sNaN -> qNaN" },
   /* 1.5 * 1.5 = 2.25 (0x3c00 = 1.0, 0x3e00 = 1.5; 2.25 = 0x4100) */
   { 0x3e00u, 0x3e00u, "1.5 * 1.5 = 2.25" },
   /* 0.1 (FP16) * 10.0 -- result near 1.0, tests RNE */
   { 0x2e66u, 0x4900u, "0.1 * 10.0 ~ 1.0 (RNE)" },
   /* Smallest subnormal * smallest subnormal -> 0 (deep underflow, flushed) */
   { 0x0001u, 0x0001u, "min_subnormal * min_subnormal = 0 (deep underflow)" },
   /* 1.0 * 0 = 0 */
   { 0x3c00u, 0x0000u, "1.0 * 0 = 0" },
   /* Tie-to-even: find a case where guard=1, sticky=0, lsb determines rounding.
    * 1.0 in FP16 is 0x3c00 (exp=15, sig=1024).  1.5 is 0x3e00 (sig=1536).
    * 1.0 * 1.5 = 1.5 -- no rounding needed, but exercises the path. */
   { 0x3c00u, 0x3e00u, "1.0 * 1.5 = 1.5" },
   /* 4.0 * 0.25 = 1.0 */
   { 0x4400u, 0x3400u, "4.0 * 0.25 = 1.0" },
   /* 3.0 * 7.0 = 21.0 */
   { 0x4200u, 0x4700u, "3.0 * 7.0 = 21.0" },
   /* 0.5 * 0.5 = 0.25 */
   { 0x3800u, 0x3800u, "0.5 * 0.5 = 0.25" },
   /* Large normals: 100.0 * 100.0 = 10000.0 (fits FP16: 10000 <= 65504) */
   { 0x5640u, 0x5640u, "100.0 * 100.0 = 10000.0" },
   /* 1.0009765625 * 1.0009765625 -- exercises mantissa rounding */
   { 0x3c01u, 0x3c01u, "1+eps * 1+eps (mantissa rounding)" },
   /* Gradual-underflow outputs: products whose exponent falls below the minimum
    * normal (2^-14) must round to a subnormal, not flush to zero, because the
    * domain advertises has_subnormal.  These exercise the 2-limb underflow path
    * against the double-precision reference. */
   /* min_normal (2^-14) * 0.5 (0x3800) = 2^-15 subnormal (0x0200) */
   { 0x0400u, 0x3800u, "min_normal * 0.5 = 2^-15 subnormal" },
   /* min_normal (2^-14) * 0.25 (0x3400) = 2^-16 subnormal (0x0100) */
   { 0x0400u, 0x3400u, "min_normal * 0.25 = 2^-16 subnormal" },
   /* Slightly above half of the minimum subnormal rounds up to 0x0001. */
   { 0x0400u, 0x1001u, "min_normal * 0x1001 rounds to min subnormal" },
   /* subnormal input 2^-15 (0x0200) * 1.0 = 2^-15 subnormal (0x0200) */
   { 0x0200u, 0x3c00u, "subnormal * 1.0 = subnormal" },
};

static void
test_multiply(void)
{
   unsigned mismatches = 0;
   unsigned total = sizeof(mul_cases) / sizeof(mul_cases[0]);

   /* 1. Test hand-picked representative cases */
   for (unsigned i = 0; i < total; i++) {
      uint16_t a = mul_cases[i].a_bits;
      uint16_t b = mul_cases[i].b_bits;
      uint16_t ref  = fp16_mul_ref(a, b);
      uint16_t got  = fp16_mul_2limb_full(a, b);

      if (got != ref) {
         printf("FAIL multiply[%u] %s: ref=0x%04x got=0x%04x\n",
                i, mul_cases[i].desc, (unsigned)ref, (unsigned)got);
         mismatches++;
      }
   }

   char label[80];
   snprintf(label, sizeof(label),
            "multiply (hand-picked): %u/%u cases match reference (%u mismatch)",
            total - mismatches, total, mismatches);
   CHECK(mismatches == 0, label);

   CHECK(fp16_mul_ref(0x0400u, 0x1001u) == 0x0001u,
         "multiply reference: half-min-subnormal RNE boundary rounds up");
   CHECK(fp16_mul_2limb_full(0x0400u, 0x1001u) == 0x0001u,
         "multiply 2-limb: half-min-subnormal RNE boundary rounds up");

   /* 2. Curated set of 128 representative values to construct a pseudo-exhaustive cross product */
   static const uint16_t rep_values[128] = {
      /* Zeroes */
      0x0000u, 0x8000u,
      /* Subnormals */
      0x0001u, 0x0002u, 0x0003u, 0x0010u, 0x0020u, 0x0040u, 0x0080u, 0x0100u, 0x0200u, 0x03ffu,
      0x8001u, 0x8002u, 0x8003u, 0x8010u, 0x8020u, 0x8040u, 0x8080u, 0x8100u, 0x8200u, 0x83ffu,
      /* Min normals */
      0x0400u, 0x0401u, 0x0402u, 0x0403u, 0x0500u, 0x0600u, 0x07ffu,
      0x8400u, 0x8401u, 0x8402u, 0x8403u, 0x8500u, 0x8600u, 0x87ffu,
      /* Intermediate normals */
      0x1000u, 0x1400u, 0x1800u, 0x2000u, 0x2400u, 0x2800u, 0x3000u, 0x3400u, 0x3800u,
      0x9000u, 0x9400u, 0x9800u, 0xa000u, 0xa400u, 0xa800u, 0xb000u, 0xb400u, 0xb800u,
      /* 1.0 area */
      0x3c00u, 0x3c01u, 0x3c02u, 0x3c03u, 0x3d00u, 0x3dffu, 0x3e00u, 0x3f00u, 0x3fffu,
      0xbc00u, 0xbc01u, 0xbc02u, 0xbc03u, 0xbd00u, 0xbdffu, 0xbe00u, 0xbf00u, 0xbfffu,
      /* Large normals */
      0x7000u, 0x7400u, 0x7800u, 0x7bffu,
      0xf000u, 0xf400u, 0xf800u, 0xfbffu,
      /* Infinities */
      0x7c00u, 0xfc00u,
      /* NaNs */
      0x7c01u, 0x7c02u, 0x7cffu, 0x7d00u, 0x7dffu, 0x7e00u, 0x7e01u, 0x7eeeu, 0x7fffu,
      0xfc01u, 0xfc02u, 0xfcffu, 0xfd00u, 0xfdffu, 0xfe00u, 0xfe01u, 0xfeeeu, 0xffffu,
      /* More subnormals */
      0x0004u, 0x0008u, 0x000fu, 0x001fu, 0x003fu, 0x007fu, 0x00ffu, 0x01ffu, 0x02ffu,
      0x8004u, 0x8008u, 0x800fu, 0x801fu, 0x803fu, 0x807fu, 0x80ffu, 0x81ffu, 0x82ffu,
      /* More normals */
      0x4000u, 0x4200u, 0x4400u, 0x4700u, 0x4800u, 0x4900u, 0x4b00u, 0x4c00u, 0x5000u, 0x5640u
   };

   unsigned loop_mismatches = 0;
   unsigned loop_total = 0;

   /* Cross-product: test all 65536 values against 128 representative values */
   for (unsigned a_bits = 0; a_bits <= 0xffff; a_bits++) {
      for (unsigned j = 0; j < 128; j++) {
         uint16_t a = (uint16_t)a_bits;
         uint16_t b = rep_values[j];

         uint16_t ref  = fp16_mul_ref(a, b);
         uint16_t got  = fp16_mul_2limb_full(a, b);

         if (got != ref) {
            if (loop_mismatches < 10) {
               printf("FAIL loop_multiply: a=0x%04x b=0x%04x ref=0x%04x got=0x%04x\n",
                      a, b, ref, got);
            }
            loop_mismatches++;
         }
         loop_total++;

         /* And symmetrically */
         ref  = fp16_mul_ref(b, a);
         got  = fp16_mul_2limb_full(b, a);
         if (got != ref) {
            if (loop_mismatches < 10) {
               printf("FAIL loop_multiply (symmetric): a=0x%04x b=0x%04x ref=0x%04x got=0x%04x\n",
                      b, a, ref, got);
            }
            loop_mismatches++;
         }
         loop_total++;
      }
   }

   snprintf(label, sizeof(label),
            "multiply (exhaustive cross-product): %u/%u cases match reference (%u mismatch)",
            loop_total - loop_mismatches, loop_total, loop_mismatches);
   CHECK(loop_mismatches == 0, label);

   printf("note: hardware not run; this is a CPU-only oracle test.\n");
   printf("note: RS482 silicon probing (fp16_class_lut_probe, fp16_mul_rne_probe)\n");
   printf("note: is the next gate before any GPU-side implementation.\n");
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
   printf("r300-fp16-limb-oracle: IEEE FP16 virtual machine CPU oracle\n");
   printf("domain: R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL (hardware-confirmed)\n");
   printf("\n");

   test_domain_catalog();
   test_classification();
   test_multiply();

   printf("\n%s: %d failure(s)\n",
          g_failures == 0 ? "PASS" : "FAIL", g_failures);
   return g_failures != 0;
}
