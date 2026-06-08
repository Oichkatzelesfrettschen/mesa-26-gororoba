/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Registry implementation for the RS482/r300 typed numeric domain model.
 *
 * The per-domain descriptor table and virtual op catalog defined here are the
 * single source of truth for domain properties (rounding model, exact integer
 * bound, significand width, special-value policy, evidence tier) and for the
 * substrate virtual op inventory (name, theorem, status, Mesa hook).
 *
 * Build-time assertions keep the domain table and the r300_numeric_domain enum
 * in sync: adding a domain to the enum without a table row fails the build.
 */

#include "r300_numeric_domain.h"

#include "util/macros.h"

/* One row per r300_numeric_domain value, in enum order.  The build assert
 * below enforces that the table length equals R300_NUM_DOMAIN_COUNT, so
 * a new enum value without a corresponding row is a build error. */
static const struct r300_numeric_domain_info r300_numeric_domain_table[] = {
   {
      .domain            = R300_NUM_DOMAIN_FP24_RTZ,
      .name              = "fp24-rtz",
      .rounding          = R300_ROUND_TRUNCATE,
      .exact_int_bound   = 131072,  /* 2^17: FP24 exact integer window */
      .significand_bits  = 17,      /* 1 implicit + 16 stored mantissa bits */
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = true,
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = "|n| <= 2^17 = 131072 exactly representable in FP24",
   },
   {
      .domain            = R300_NUM_DOMAIN_FP16_STORAGE,
      .name              = "fp16-storage",
      .rounding          = R300_ROUND_TRUNCATE,  /* FP24 RTZ on the compute side */
      .exact_int_bound   = 0,
      .significand_bits  = 11,                   /* 1 implicit + 10 stored */
      .has_nan           = true,                 /* IEEE FP16 has NaN */
      .has_inf           = true,                 /* IEEE FP16 has Inf */
      .has_subnormal     = true,                 /* IEEE FP16 has subnormals */
      .is_native_compute = false,                /* storage/transport only */
      .evidence          = R300_EVIDENCE_SPEC_GROUNDED,
      .theorem           = "finite FP16 values fit FP24 (11-bit significand < 17-bit window)",
   },
   {
      .domain            = R300_NUM_DOMAIN_FP32_STORAGE,
      .name              = "fp32-storage",
      .rounding          = R300_ROUND_TRUNCATE,  /* FP24 RTZ on the compute side */
      .exact_int_bound   = 0,
      .significand_bits  = 24,                   /* 1 implicit + 23 stored */
      .has_nan           = true,
      .has_inf           = true,
      .has_subnormal     = true,
      .is_native_compute = false,                /* input texture only; no FP32 RT */
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = "R32G32B32A32_FLOAT texture accepted; FP32 color FBO incomplete (0x8cdd)",
   },
   {
      .domain            = R300_NUM_DOMAIN_Q16_16,
      .name              = "q16.16",
      .rounding          = R300_ROUND_EXACT,
      .exact_int_bound   = 131071,  /* 2^17-1: (2^16-1)+(2^16-1)+1 for two-limb add */
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = true,
      .evidence          = R300_EVIDENCE_NUMERIC_DERIVED,
      .theorem           = "(2^16-1)+(2^16-1)+1 = 2^17-1 < 2^17 for Q16_16 add; "
                           "(2^6-1)^2 = 3969, 4*3969 = 15876 < 2^17 per 6-bit limb column",
   },
   {
      .domain            = R300_NUM_DOMAIN_U7_DOT,
      .name              = "u7-dot",
      .rounding          = R300_ROUND_EXACT,
      .exact_int_bound   = 64516,  /* 4*(2^7-1)^2 = 4*127^2 = 64516 */
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = true,
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = "4*(2^7-1)^2 = 64516 < 2^17 = 131072",
   },
   {
      .domain            = R300_NUM_DOMAIN_I8_MAG_DOT,
      .name              = "i8-mag-dot",
      .rounding          = R300_ROUND_EXACT,
      .exact_int_bound   = 64516,  /* |sum| <= 4*127^2 = 64516 */
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = true,
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = "|a_i|, |b_i| <= 127: |sum| <= 4*(2^7-1)^2 = 64516 < 2^17",
   },
   {
      .domain            = R300_NUM_DOMAIN_U8_OFFGRID,
      .name              = "u8-offgrid",
      .rounding          = R300_ROUND_TRUNCATE,  /* FP24 RTZ for out-of-window values */
      .exact_int_bound   = 64516,   /* exact up to here; deviates above */
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = true,
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = "4*(2^8-1)^2 = 260100 > 2^17; results above 2^17 are approximate",
   },
   {
      .domain            = R300_NUM_DOMAIN_TX_INT6_WEIGHT,
      .name              = "tx-int6-weight",
      .rounding          = R300_ROUND_BIASED,
      .exact_int_bound   = 0,
      .significand_bits  = 6,  /* 6-bit bilinear weight */
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = false,  /* sampler interpolation, not PFS ALU */
      .evidence          = R300_EVIDENCE_SPEC_GROUNDED,
      .theorem           = NULL,
   },
   {
      .domain            = R300_NUM_DOMAIN_RB3D_BLEND,
      .name              = "rb3d-blend",
      .rounding          = R300_ROUND_CLAMP,
      .exact_int_bound   = 0,
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = false,  /* RT output reduction, not PFS ALU */
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = NULL,
   },
   {
      .domain            = R300_NUM_DOMAIN_ROP_BOOL,
      .name              = "rop-bool",
      .rounding          = R300_ROUND_EXACT,
      .exact_int_bound   = 0,
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = false,  /* bitplane unit, not PFS ALU */
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,  /* XOR confirmed; full family pending probes */
      .theorem           = "XOR hardware-confirmed; AND/OR/NOT need targeted truth-table probes",
   },
   {
      .domain            = R300_NUM_DOMAIN_U8_STENCIL,
      .name              = "u8-stencil",
      .rounding          = R300_ROUND_CLAMP,
      .exact_int_bound   = 255,
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = false,  /* per-pixel state machine, not PFS ALU */
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,  /* INCR confirmed; DECR/INVERT/WRAP pending */
      .theorem           = "INCR confirmed; DECR/INVERT/WRAP need targeted probes",
   },
   {
      .domain            = R300_NUM_DOMAIN_ZPASS_COUNT,
      .name              = "zpass-count",
      .rounding          = R300_ROUND_EXACT,
      .exact_int_bound   = 0,      /* count is unbounded within the RT extent */
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = false,  /* reduction unit via ZB_ZPASS_DATA/ZB_ZPASS_ADDR */
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = "surviving-fragment count exact via ZB_ZPASS_DATA occlusion-query path",
   },
   {
      .domain            = R300_NUM_DOMAIN_VAP_FORMAT_INGEST,
      .name              = "vap-format-ingest",
      .rounding          = R300_ROUND_EXACT,   /* format decode is lossless for supported formats */
      .exact_int_bound   = 0,
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = false,   /* source transform, not compute */
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,  /* FLOAT_4 observed; FLOAT_8/FLT16 pending */
      .theorem           = "FLOAT_4 vertex format observed; FLOAT_8/FLT16 remain probe targets",
   },
   {
      .domain            = R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
      .name              = "ieee-fp16-virtual",
      .rounding          = R300_ROUND_RNE,
      .exact_int_bound   = 0,
      .significand_bits  = 11,    /* 1 implicit + 10 stored; normals in [1024..2047] */
      .has_nan           = true,
      .has_inf           = true,
      .has_subnormal     = true,
      .is_native_compute = false, /* emulated via integer limb arithmetic on FP24 substrate */
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = "2-limb base-64: c0=a0*b0<=3969, c1=a0*b1+a1*b0<=3906, "
                           "c2=a1*b1<=961; all < 2^17; carry limbs (r0,r1,r2) 12/12 exact on RS482 "
                           "(rs482_fp16_pow2_carry_exactness_20260607); classification 15/15 exact",
   },
};

const struct r300_numeric_domain_info *
r300_numeric_domain_info(enum r300_numeric_domain domain)
{
   /* One row per enum value: fails the build if a domain is added to the enum
    * without a table row -- the same divergence guard as r300_compute_admission.c. */
   STATIC_ASSERT(ARRAY_SIZE(r300_numeric_domain_table) == R300_NUM_DOMAIN_COUNT);
   if ((unsigned)domain >= R300_NUM_DOMAIN_COUNT)
      return &r300_numeric_domain_table[0];
   return &r300_numeric_domain_table[(unsigned)domain];
}

/* Virtual op catalog for the RS482 compute-as-raster substrate.
 *
 * Each row records one named virtual op: domain, status, theorem, Mesa
 * detection hook, and (when available) the sibling steinmarder retained
 * bundle path.  The catalog is the C-side representation of the TSV substrate
 * table; the two must be kept in sync when a new op is confirmed.
 *
 * Terminated by a row with op_name == NULL. */
const struct r300_virtual_op_info r300_virtual_op_catalog[] = {
   {
      .op_name         = "IDENTITY_MAP",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "out[gid] = in[gid]: passthrough via fullscreen TEX + RB3D export",
      .mesa_hook       = "r300_nir_detect_identity_map",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "BINARY_MAP",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "out[gid] = f(a[gid], b[gid]) for admitted binary FP24 ops",
      .mesa_hook       = "r300_nir_detect_binary_map",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "BLEND_ACC_REDUCTION",
      .domain          = R300_NUM_DOMAIN_RB3D_BLEND,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "histogram add via RB3D COMB_FCN_ADD blend accumulation",
      .mesa_hook       = "r300_nir_detect_blend_acc_reduction",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "ZPASS_COVERAGE_COUNT",
      .domain          = R300_NUM_DOMAIN_ZPASS_COUNT,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "predicated fragment count via ZB_ZPASS_DATA occlusion-query path",
      .mesa_hook       = "r300_nir_detect_zpass_reduction",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "MULTIPASS_PING_PONG_SCAN",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "self-iterated doubling via dependent FBO ping-pong passes",
      .mesa_hook       = "r300_nir_detect_multipass_scan_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "PREDICATED_MASKED_STORE",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "per-element conditional store via per-pixel KILL_IF discard",
      .mesa_hook       = "r300_nir_detect_predicated_store_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "MULTITAP_GATHER",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "N-tap neighborhood sum via multi-TEX fragment draw; "
                         "integer-exact for per-tap UNORM8 sum <= 255",
      .mesa_hook       = "r300_nir_detect_multitap_gather_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "DP4_UINT7_EXACT",
      .domain          = R300_NUM_DOMAIN_U7_DOT,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "4*(2^7-1)^2 = 64516 < 2^17: byte-exact for U7-magnitude operands",
      .mesa_hook       = "r300_nir_detect_dp4_pattern",
      .retained_bundle = NULL,  /* RS482 dp4_fp32 probe: 6/6 exact; r300vk DP4: 4/4 byte-exact */
   },
   {
      .op_name         = "DP4_INT8_SIGNED_EXACT",
      .domain          = R300_NUM_DOMAIN_I8_MAG_DOT,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "|a_i|,|b_i| <= 127: |sum| <= 64516 < 2^17; signed cases confirmed",
      .mesa_hook       = "r300_nir_detect_dp4_pattern",
      .retained_bundle = NULL,  /* RS482 dp4 probe signed-cancellation and negative-total cases */
   },
   {
      .op_name         = "DP4_UINT8_OFFGRID_ROUNDS",
      .domain          = R300_NUM_DOMAIN_U8_OFFGRID,
      .status          = R300_VOP_BOUNDARY,
      .theorem         = "4*(2^8-1)^2 = 260100 > 2^17; above-window results are FP24-approximate",
      .mesa_hook       = "r300_nir_detect_dp4_pattern",
      .retained_bundle = NULL,  /* RS482 dp4_fp32 probe: off-grid cases confirmed as rounding */
   },
   {
      .op_name         = "Q16_16_ADD",
      .domain          = R300_NUM_DOMAIN_Q16_16,
      .status          = R300_VOP_NUMERIC_DERIVED,
      .theorem         = "(2^16-1)+(2^16-1)+1 = 2^17-1 < 2^17; limb carry exact",
      .mesa_hook       = NULL,  /* no detector yet; requires limb-add NIR pattern */
      .retained_bundle = NULL,
   },
   {
      .op_name         = "Q16_16_MUL",
      .domain          = R300_NUM_DOMAIN_Q16_16,
      .status          = R300_VOP_NUMERIC_DERIVED,
      .theorem         = "(2^6-1)^2 = 3969 per limb; 4-column sum <= 15876 < 2^17 per column",
      .mesa_hook       = NULL,  /* no detector yet; requires 4x6-bit limb-multiply NIR pattern */
      .retained_bundle = NULL,
   },
   {
      .op_name         = "IEEE16_CLASSIFY_LUT",
      .domain          = R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "FP16 bit[15]=sign, bits[14:10]=exp(0..31), bits[9:0]=mantissa; "
                         "class determined by (exp==0, exp==31, mantissa==0) partition; "
                         "15/15 bit patterns exact on RS482 (rs482_fp16_pow2_carry_exactness_20260607)",
      .mesa_hook       = NULL,
      .retained_bundle = NULL,  /* bundle named in .theorem; fork evidence paths stay out of Mesa metadata */
   },
   {
      .op_name         = "IEEE16_MUL_RNE",
      .domain          = R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "2-limb base-64: c1=a0*b1+a1*b0 <= 2*63*31=3906 < 2^17; "
                         "carry limbs (r0,r1,r2) 12/12 exact on RS482; "
                         "RNE round from guard/sticky/lsb (rs482_fp16_pow2_carry_exactness_20260607)",
      .mesa_hook       = NULL,
      .retained_bundle = NULL,  /* bundle named in .theorem; fork evidence paths stay out of Mesa metadata */
   },
   /* NULL sentinel -- keep last */
   { .op_name = NULL },
};

unsigned
r300_virtual_op_count(void)
{
   /* Count entries up to the NULL sentinel. */
   unsigned n = 0;
   while (r300_virtual_op_catalog[n].op_name)
      n++;
   return n;
}
