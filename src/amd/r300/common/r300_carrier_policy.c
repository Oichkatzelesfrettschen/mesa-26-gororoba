/*
 * SPDX-License-Identifier: MIT
 *
 * Carrier-policy contracts for candidate RS482 compute-as-raster paths.
 *
 * These static instances define API-neutral buffer formats, strides, and
 * result bounds.  They remain contract inventory until a consumer explicitly
 * selects one; compilation and registration do not make a route executable.
 */

#include "amd/r300/common/r300_carrier_policy.h"

#include <stddef.h>

/* Candidate identity-map carrier: RGBA8 input and output with equal strides.
 * This is not the executing R2VB F32_4 identity-carrier route. */
const struct r300_carrier_policy r300_carrier_identity = {
   .name                = "identity",
   .operation_id        = R300_OPERATION_ID_IDENTITY_MAP,
   .domain              = R300_NUM_DOMAIN_FP24_RTZ,
   .encoding            = R300_CARRIER_ENC_IDENTITY,
   .value_format        = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .bit_format          = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .input_stride        = 4,
   .output_stride       = 4,
   .max_exact_result_kind = R300_BOUND_NONE,
   .max_exact_result    = 0,
   .pack_alpha_byte     = false,
   .requires_fp32_rt    = false,
};

/* DP4 with U7-magnitude operands: byte-exact for the full admitted domain.
 * Input is R32G32B32A32_FLOAT (one vec4 operand per element, stride 16).
 * Output is R8G8B8A8_UNORM (RGBA8 integer-encoded result, stride 4).
 * The result fits in three bytes (64516 < 2^17 < 2^24), so the A channel
 * is zero for every exact-domain result; pack_alpha_byte is false. */
const struct r300_carrier_policy r300_carrier_dp4_u7 = {
   .name                = "dp4-u7-exact",
   .operation_id        = R300_OPERATION_ID_DP4_UINT7_EXACT,
   .domain              = R300_NUM_DOMAIN_U7_DOT,
   .encoding            = R300_CARRIER_ENC_RGBA8_UINT,
   .value_format        = R300_CARRIER_FORMAT_R32G32B32A32_FLOAT,
   .bit_format          = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .input_stride        = 16,
   .output_stride       = 4,
   .max_exact_result_kind = R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
   .max_exact_result    = 64516,  /* 4*(2^7-1)^2, hardware-confirmed */
   .pack_alpha_byte     = false,
   .requires_fp32_rt    = false,
};

/* DP4 with U8 operands: same formats as dp4_u7, but only results inside the
 * FP24 exact-integer window are exact.  The full U8 dot range is
 * 4*(2^8-1)^2 = 260100, so values above 2^17 remain the hardware-confirmed
 * DP4_UINT8_OFFGRID_ROUNDS boundary behavior. */
const struct r300_carrier_policy r300_carrier_dp4_u8_boundary = {
   .name                = "dp4-u8-boundary",
   .operation_id        = R300_OPERATION_ID_DP4_UINT8_OFFGRID_ROUNDS,
   .domain              = R300_NUM_DOMAIN_U8_OFFGRID,
   .encoding            = R300_CARRIER_ENC_RGBA8_UINT,
   .value_format        = R300_CARRIER_FORMAT_R32G32B32A32_FLOAT,
   .bit_format          = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .input_stride        = 16,
   .output_stride       = 4,
   .max_exact_result_kind = R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
   .max_exact_result    = R300_FP24_EXACT_INT_CEILING,
   .pack_alpha_byte     = false,
   .requires_fp32_rt    = false,
};

/* Blend-add accumulation: the output is a 1xN RB3D render target holding per-bin
 * normalized float sums accumulated by the RB3D COMB_FCN_ADD blend equation.
 * RGBA8_UNORM is the candidate output carrier because blend hardware
 * accumulates normalized float channels. */
const struct r300_carrier_policy r300_carrier_blend_acc = {
   .name                = "blend-acc-reduction",
   .operation_id        = R300_OPERATION_ID_BLEND_ACC_REDUCTION,
   .domain              = R300_NUM_DOMAIN_RB3D_BLEND,
   .encoding            = R300_CARRIER_ENC_RGBA8_UNORM,
   .value_format        = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .bit_format          = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .input_stride        = 4,
   .output_stride       = 4,
   .max_exact_result_kind = R300_BOUND_INPUT_DEPENDENT,
   .max_exact_result    = 0,
   .pack_alpha_byte     = false,
   .requires_fp32_rt    = false,
};

/* ZPASS fragment-count: the output is a single-element uint32 written by the
 * occlusion-query chain (ZB_ZPASS_DATA / ZB_ZPASS_ADDR).  The predicate input
 * is read as RGBA8_UNORM (the default identity-map carrier format); the output
 * is UINT32_COUNTER, representing the surviving-fragment count returned by
 * PIPE_QUERY_OCCLUSION_COUNTER and written to the first uint32 of the output
 * SSBO. */
const struct r300_carrier_policy r300_carrier_zpass = {
   .name                = "zpass-count",
   .operation_id        = R300_OPERATION_ID_ZPASS_COVERAGE_COUNT,
   .domain              = R300_NUM_DOMAIN_ZPASS_COUNT,
   .encoding            = R300_CARRIER_ENC_UINT32_COUNTER,
   .value_format        = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .bit_format          = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .input_stride        = 4,
   .output_stride       = 4,
   .max_exact_result_kind = R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
   .max_exact_result    = UINT32_MAX,
   .pack_alpha_byte     = false,
   .requires_fp32_rt    = false,
};

const struct r300_carrier_policy r300_carrier_ieee16_classify = {
   .name                = "ieee16-classify",
   .operation_id        = R300_OPERATION_ID_IEEE16_CLASSIFY_LUT,
   .domain              = R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
   .encoding            = R300_CARRIER_ENC_FP16_RAWBITS_RGBA8,
   .value_format        = R300_CARRIER_FORMAT_R32_FLOAT,
   .bit_format          = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .input_stride        = 4,
   .output_stride       = 4,
   .max_exact_result_kind = R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
   .max_exact_result    = 65535,  /* all 16-bit patterns */
   .pack_alpha_byte     = false,
   .requires_fp32_rt    = false,
};

const struct r300_carrier_policy r300_carrier_ieee16_mul = {
   .name                = "ieee16-mul",
   .operation_id        = R300_OPERATION_ID_IEEE16_MUL_RNE,
   .domain              = R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
   .encoding            = R300_CARRIER_ENC_RGBA8_U24,
   .value_format        = R300_CARRIER_FORMAT_R32G32_FLOAT,
   .bit_format          = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .input_stride        = 8,
   .output_stride       = 4,
   .max_exact_result_kind = R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
   .max_exact_result    = 4190209, /* 2047 * 2047 */
   .pack_alpha_byte     = false,
   .requires_fp32_rt    = false,
};

const struct r300_carrier_policy r300_carrier_ieee16_result = {
   .name                = "ieee16-result",
   .operation_id        = R300_OPERATION_ID_NONE,
   .domain              = R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
   .encoding            = R300_CARRIER_ENC_RGBA8_U16,
   .value_format        = R300_CARRIER_FORMAT_R32_FLOAT,
   .bit_format          = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .input_stride        = 4,
   .output_stride       = 4,
   .max_exact_result_kind = R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
   .max_exact_result    = 65535,
   .pack_alpha_byte     = false,
   .requires_fp32_rt    = false,
};

const struct r300_carrier_policy r300_carrier_ieee16_debug = {
   .name                = "ieee16-debug",
   .operation_id        = R300_OPERATION_ID_NONE,
   .domain              = R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
   .encoding            = R300_CARRIER_ENC_RGBA8_UINT,
   .value_format        = R300_CARRIER_FORMAT_R32_FLOAT,
   .bit_format          = R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   .input_stride        = 4,
   .output_stride       = 4,
   .max_exact_result_kind = R300_BOUND_NONE,
   .max_exact_result    = 0,
   .pack_alpha_byte     = true,
   .requires_fp32_rt    = false,
};

static const struct r300_carrier_policy *const policies[] = {
   &r300_carrier_identity,
   &r300_carrier_dp4_u7,
   &r300_carrier_dp4_u8_boundary,
   &r300_carrier_blend_acc,
   &r300_carrier_zpass,
   &r300_carrier_ieee16_classify,
   &r300_carrier_ieee16_mul,
   &r300_carrier_ieee16_result,
   &r300_carrier_ieee16_debug,
};

const struct r300_carrier_policy *const *
r300_carrier_policies(unsigned *count)
{
   if (count != NULL)
      *count = sizeof(policies) / sizeof(policies[0]);
   return policies;
}

const struct r300_carrier_policy *
r300_carrier_dp4_select(unsigned max_operand_magnitude)
{
   /* The U8 carrier proves exactness only for operands in 0..255.  An operand
    * magnitude above 255 leaves the U8 domain, so neither carrier's exactness
    * bound holds -- return NULL to reject rather than hand back a policy that
    * over-promises exactness. */
   if (max_operand_magnitude > 255)
      return NULL;
   return (max_operand_magnitude <= 127) ? &r300_carrier_dp4_u7
                                         : &r300_carrier_dp4_u8_boundary;
}
