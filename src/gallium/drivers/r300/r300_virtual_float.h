/*
 * SPDX-License-Identifier: MIT
 *
 * Types for the virtual IEEE FP16 machine emulated on the RS485M/r300 FP24
 * substrate (R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL).
 *
 * The 2-limb base-64 significand multiply theorem:
 *
 *   Normal FP16 significands are in [1024..2047] (11-bit, implicit-1).
 *   Split as a = a1*64 + a0 with a1 in [16..31], a0 in [0..63].
 *   Product P = a*b = c2*4096 + c1*64 + c0 where:
 *     c0 = a0*b0  <=  63*63 = 3969
 *     c1 = a0*b1 + a1*b0  <=  63*31 + 31*63 = 3906
 *     c2 = a1*b1  <=  31*31 = 961
 *   All column values are < 2^17 = 131072 (the FP24 exact integer window),
 *   so each column multiply and the carry propagation steps are FP24-exact.
 *
 *   Carry propagation (RTZ == floor for positive integers < 2^17):
 *     carry = c0 / 64; r0 = c0 % 64
 *     c1' = c1 + carry  <=  3906 + 62 = 3968 < 2^17
 *     carry = c1' / 64; r1 = c1' % 64
 *     r2 = c2 + carry  <=  961 + 62 = 1023 < 2^17
 *   All steps exact.  The final product is r2*4096 + r1*64 + r0.
 *
 *   Normalization: P >= 2^21 iff r2 >= 512 (proven: r1 <= 63 and r0 <= 63
 *   so the low 12 bits contribute at most 4095; 511*4096+4095 < 2^21).
 *
 * Retained silicon evidence: carry limbs (r0,r1,r2) 12/12 exact
 * and classification 15/15 exact on RS485M hardware (rs482_fp16_pow2_carry_exactness_20260607).
 */

#ifndef R300_VIRTUAL_FLOAT_H
#define R300_VIRTUAL_FLOAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IEEE FP16 value class.  The classifier partitions all 65536 bit patterns
 * using the (biased_exp == 0, biased_exp == 31, mantissa == 0) predicate
 * triple.  Positive and negative forms are distinguished by the sign bit. */
enum r300_fp16_class {
   R300_FP16_POS_ZERO,
   R300_FP16_NEG_ZERO,
   R300_FP16_POS_SUBNORMAL,
   R300_FP16_NEG_SUBNORMAL,
   R300_FP16_POS_NORMAL,
   R300_FP16_NEG_NORMAL,
   R300_FP16_POS_INF,
   R300_FP16_NEG_INF,
   R300_FP16_QNAN,   /* quiet NaN: exp==31, mantissa != 0, bit[9] set */
   R300_FP16_SNAN,   /* signaling NaN: exp==31, mantissa != 0, bit[9] clear */
};

/* 2-limb base-64 decomposition of an 11-bit FP16 significand.
 * For normal inputs s in [1024..2047]: a1 = s/64 in [16..31], a0 = s%64 in [0..63].
 * For subnormal inputs the significand has no implicit-1; the caller normalizes
 * by left-shifting until bit 10 is set before decomposing. */
struct r300_fp16_limb_pair {
   uint32_t a0;  /* low limb:  s % 64, range [0..63] */
   uint32_t a1;  /* high limb: s / 64, range [16..31] for normals */
};

/* Carry triple produced by 2-limb base-64 multiply carry propagation.
 * The product significand is r2*4096 + r1*64 + r0 (22 bits for normal inputs,
 * 21 bits after normalization shift when r2 < 512).
 *   r0: column-0 remainder, range [0..63]
 *   r1: column-1 remainder after carry, range [0..63]
 *   r2: column-2 plus carry, range [0..1023]
 * All three fields fit in uint32_t; the FP24 exact integer bound (2^17) is
 * never exceeded during the carry propagation steps. */
struct r300_fp16_carry_triple {
   uint32_t r0;
   uint32_t r1;
   uint32_t r2;
};

#ifdef __cplusplus
}
#endif

#endif /* R300_VIRTUAL_FLOAT_H */
