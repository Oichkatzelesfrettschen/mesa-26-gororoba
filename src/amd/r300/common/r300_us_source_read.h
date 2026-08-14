/*
 * SPDX-License-Identifier: MIT
 *
 * RS48x US source-operand read model over the FP24 value lattice.
 *
 * The FP24 representation model (R300_NUM_DOMAIN_FP24_RTZ,
 * r300_numeric_domain.h) describes which values the US carries: s1e7m16,
 * bias 62, normal range [2^-61, 2^65], round-toward-zero, flush-to-zero,
 * saturate.  This header models a separate execution stage: what the US
 * delivers when an instruction reads a stored value as a source operand.
 *
 * Measured behavior (RS482 silicon, sign-flip mov discriminator on a cold
 * first-contact boot, byte-for-bit against precommitted models): a negative
 * nonzero source operand read from an input or temporary register arrives
 * one FP24 ULP smaller in magnitude -- the immediately preceding lattice
 * value toward zero -- before the ABS/NEG source modifiers apply.  Positive
 * operands, ALU computation, register writes, and exports are exact, and a
 * negative created by the NEG modifier on an exact positive source exports
 * exactly.  Negative zero reads back as positive zero; the discriminator
 * cannot separate read-side from write-side canonicalization, so the model
 * canonicalizes at the read.
 *
 * Evidence scope: input and temporary register reads of FP24-exact normal
 * values.  Constant-file reads, texture results, presubtract results, the
 * RGB-versus-alpha read ports, and non-lattice FP32 sources are unmeasured;
 * the model applies the predecessor only to the measured files and keeps
 * identity elsewhere, failing closed on conjecture.
 *
 * On lattice values the predecessor is FP32 magnitude minus 0x80: clearing
 * one FP24 ULP borrows through the exponent at a binade boundary and lands
 * on the all-ones-mantissa value of the lower binade, which is itself on
 * the lattice.  The min-normal boundary (predecessor would be subnormal,
 * which FP24 lacks) and values below min normal flush to positive zero;
 * both boundaries are model choices awaiting silicon measurement.
 */

#ifndef R300_US_SOURCE_READ_H
#define R300_US_SOURCE_READ_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Execution-stage model for the US source-operand read.  IDENTITY is the
 * ideal read (the stored value arrives unchanged, bit for bit);
 * RS48X_NEG_PREDECESSOR is the measured RS482 behavior described above.
 * Storage conversion is a separate stage: producers land values on the
 * lattice through r300_fp24_store_quantize_f32, and the read model then
 * operates on stored lattice values.  The pipeline is
 * store-quantize -> stored FP24 value -> source read -> ABS/NEG -> ALU. */
enum r300_source_read_model {
   R300_SOURCE_READ_IDENTITY,
   R300_SOURCE_READ_RS48X_NEG_PREDECESSOR,
   /* Evaluation-refusing sentinel for source classes silicon has not
    * measured (constant, texture, presubtract).  A consumer treats a class
    * marked UNMODELED as indeterminate when it delivers a negative value;
    * r300_us_source_read_f32() accepts only the two measured models. */
   R300_SOURCE_READ_UNMODELED,
};

/* FP32 bit patterns of the FP24 lattice boundaries.  Min normal 2^-61 and
 * max finite (2 - 2^-16) * 2^64 follow from s1e7m16 with bias 62 and the
 * top exponent field reserved.  RS482 delivers every magnitude through
 * (2 - 2^-16) * 2^64 byte-exact on the R2VB identity route and delivers
 * 2^65-bin magnitudes with the exponent field decremented, so the finite
 * ceiling sits one exponent bin below an all-fields-usable reading of the
 * format. */
#define R300_FP24_MIN_NORMAL_F32_BITS 0x21000000u
#define R300_FP24_MAX_FINITE_F32_BITS 0x5FFFFF80u

static inline uint32_t
r300_f32_to_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

static inline float
r300_bits_to_f32(uint32_t bits)
{
   float value;
   memcpy(&value, &bits, sizeof(value));
   return value;
}

/* Quantize an FP32 value onto the FP24 lattice: truncate the low 7 mantissa
 * bits (round toward zero), flush magnitudes below min normal to positive
 * zero, and saturate NaN/Inf/overflow magnitudes to max finite.  Values
 * already on the lattice pass through unchanged. */
static inline uint32_t
r300_fp24_quantize_bits(uint32_t f32_bits)
{
   const uint32_t sign = f32_bits & 0x80000000u;
   uint32_t magnitude = f32_bits & 0x7FFFFFFFu;
   if (magnitude < R300_FP24_MIN_NORMAL_F32_BITS)
      return 0;
   magnitude &= ~0x7Fu;
   if (magnitude > R300_FP24_MAX_FINITE_F32_BITS)
      magnitude = R300_FP24_MAX_FINITE_F32_BITS;
   return sign | magnitude;
}

/* Immediately preceding FP24 lattice magnitude toward zero.  The caller
 * passes a lattice magnitude (sign bit clear); min normal steps to zero
 * because FP24 has no subnormals. */
static inline uint32_t
r300_fp24_pred_magnitude_bits(uint32_t lattice_magnitude)
{
   if (lattice_magnitude <= R300_FP24_MIN_NORMAL_F32_BITS)
      return 0;
   return lattice_magnitude - 0x80u;
}

/* Storage-conversion stage: the value a register write lands on the FP24
 * lattice.  Producers and evaluators call this once at the write, keeping
 * ingestion quantization separate from the source-read transform so an
 * off-lattice measurement can attribute a deviation to the right stage. */
static inline float
r300_fp24_store_quantize_f32(float value)
{
   return r300_bits_to_f32(r300_fp24_quantize_bits(r300_f32_to_bits(value)));
}

/* The modeled US source-operand read over a stored value.  IDENTITY returns
 * the stored value unchanged.  RS48X_NEG_PREDECESSOR quantizes to the
 * lattice (a stored lattice value passes through), steps a negative nonzero
 * value to the preceding lattice magnitude keeping its sign, and
 * canonicalizes negative zero to positive zero; ABS/NEG source modifiers
 * apply after this transform, on the caller's side. */
static inline float
r300_us_source_read_f32(float stored, enum r300_source_read_model model)
{
   if (model == R300_SOURCE_READ_IDENTITY)
      return stored;
   uint32_t bits = r300_fp24_quantize_bits(r300_f32_to_bits(stored));
   if (bits & 0x80000000u) {
      const uint32_t pred =
         r300_fp24_pred_magnitude_bits(bits & 0x7FFFFFFFu);
      bits = pred ? (0x80000000u | pred) : 0;
   }
   return r300_bits_to_f32(bits);
}

#ifdef __cplusplus
}
#endif

#endif /* R300_US_SOURCE_READ_H */
