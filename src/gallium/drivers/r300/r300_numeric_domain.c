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
   {
      .op_name         = "QMUL_HAMILTON",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "Hamilton product = Cayley-Dickson multiplication at dim 4 = four "
                         "sign-permuted DP4s; sign-for-sign the machine-verified quat_mul; "
                         "integer self-check (1,2,3,4)*(5,6,7,8) = (-60,12,30,24) exact on RS482",
      .mesa_hook       = "r300_nir_detect_qmul_pattern",  /* QMUL = 4 sign-permuted DP4s; canonical 4-dot detector */
      .retained_bundle = NULL,  /* RS482 surfaceless-EGL probe; fork evidence paths stay out of Mesa metadata */
   },
   {
      .op_name         = "QROTATE_SANDWICH",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "vertex rotation q*v*conj(q) = two Hamilton products = eight DP4s.  "
                         "The sandwich (quat_rotate) and the rotation matrix R(q) "
                         "(matrix_rotate) are both defined in open_gororoba but their "
                         "equivalence is not yet machine-verified (ROCQ gap); HW-confirmed "
                         "4/4 by qrotate_vk_probe vs a CPU sandwich on RS482",
      .mesa_hook       = "r300_nir_detect_qrotate_pattern",  /* QROTATE = nested 2-Hamilton sandwich detector */
      .retained_bundle = NULL,  /* RS482 surfaceless-EGL probe; fork evidence paths stay out of Mesa metadata */
   },
   {
      .op_name         = "OMUL_OCTONION",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion product (a,b)*(c,d) = (a*c - conj(d)*b, d*a + b*conj(c)) "
                         "= Cayley-Dickson doubling = four Hamilton products = sixteen DP4s; "
                         "norm multiplicative |xy|^2 = |x|^2 |y|^2 (Hurwitz at dim 8).  "
                         "r300_nir_detect_omul_pattern admits the eight-wide kernel (four "
                         "quaternion inputs, two output halves), the two synthesized FS "
                         "passes (r300vk_build_omul_lo/hi_fs_nir) emit the halves, and the "
                         "two-pass dispatch fills the result -- HW-confirmed 4/4 exact on "
                         "RS482 by omul_vk_probe, the Hurwitz norm holding exactly",
      .mesa_hook       = "r300_nir_detect_omul_pattern",
      .retained_bundle = NULL,  /* RS482 surfaceless-EGL probe; fork evidence paths stay out of Mesa metadata */
   },
   {
      .op_name         = "OADD",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion addition (a,b)+(c,d) = (a+c, b+d), componentwise vec8 "
                         "add over two output halves, zero DP4.  Admitted by "
                         "r300_nir_detect_oaddsub_pattern (is_sub=false) and filled in one "
                         "MRT pass; HW-confirmed 4/4 on RS482 by oct_alg_vk_probe oadd",
      .mesa_hook       = "r300_nir_detect_oaddsub_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "OSUB",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion subtraction (a,b)-(c,d) = (a-c, b-d), componentwise vec8 "
                         "sub, zero DP4.  The is_sub=true form of the oaddsub detector, same "
                         "single MRT pass; HW-confirmed 4/4 on RS482 by oct_alg_vk_probe osub",
      .mesa_hook       = "r300_nir_detect_oaddsub_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "OCONJ",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion conjugate conj((a,b)) = (conj(a), -b) = Cayley-Dickson "
                         "involution: the lower half is the quaternion conjugate of a "
                         "(scalar lane kept, vector lanes negated), the upper half the full "
                         "negation of b; zero DP4.  Admitted by r300_nir_detect_oconj_pattern, "
                         "filled in one MRT pass; HW-confirmed 4/4 on RS482 by oct_alg_vk_probe",
      .mesa_hook       = "r300_nir_detect_oconj_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "ONORM",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion squared norm |(a,b)|^2 = dot(a,a) + dot(b,b), broadcast "
                         "to four lanes; two DP4s.  The norm whose multiplicativity "
                         "|xy|^2=|x|^2|y|^2 OMUL confirms (Hurwitz at dim 8).  Admitted by "
                         "r300_nir_detect_onorm_pattern and dispatched on the 2-in/1-out "
                         "core; HW-confirmed 4/4 on RS482 by oct_alg_vk_probe onorm",
      .mesa_hook       = "r300_nir_detect_onorm_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "ODIV",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion right division out = x/y = x*inv(y), inv(y) = "
                         "conj(y)/|y|^2.  Valid for EVERY nonzero octonion: dim 8 is a "
                         "division algebra (no zero divisors, |y|^2 > 0 unless y=0), so the "
                         "scalar reciprocal r = 1/|y|^2 (US RCP) and the eight-wide product "
                         "x*inv(y) (OMUL fold) compose a true inverse -- ROCQ ground "
                         "open_gororoba Brown1972ChapterV.v brown1972_oct_inv_mul_left/right "
                         "(N(y)<>0).  Admitted by r300_nir_detect_odiv_pattern (matches the "
                         "reciprocal of the norm + the OMUL of x against conj(y)*r) and "
                         "dispatched in two single-output passes -- the combined MRT form is "
                         "73 ALU ops, over the 64-ALU R300 fragment limit (R300_PFS_MAX_ALU_INST), "
                         "so each pass recomputes inv(y) and emits one half; HW-confirmed 4/4 on "
                         "RS482 by odiv_vk_probe.  Left division inv(y)*x is the ODIV_L "
                         "sibling (same detector, is_left).  Division stays DIM-8-ONLY: at dim "
                         "16 conj/N is only a pseudo-inverse (sedenion zero divisors, Moreno G2 / "
                         "de Marrais box-kites; oct_norm_mul holds, sed_norm_fails)",
      .mesa_hook       = "r300_nir_detect_odiv_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "ODIV_L",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion left division out = y\\x = inv(y)*x, inv(y) = "
                         "conj(y)/|y|^2.  Differs from right division x*inv(y) because "
                         "octonions are non-commutative AND non-associative, but each side is "
                         "parenthesis-safe (Artin: x,y generate an associative subalgebra "
                         "containing inv(y) -- ROCQ ground oct_flexible CDPowerAssociative.v, "
                         "brown1972_oct_inv_mul_left/right Brown1972ChapterV.v).  Same detector "
                         "r300_nir_detect_odiv_pattern (sets is_left when the stores fold as "
                         "OMUL(inv(y),x) -- operands swapped vs right) and same two-pass split "
                         "under the 64-ALU limit; the synthesize step picks odiv_l_lo/hi.  The "
                         "identity is y*out == x (left), vs out*y == x for right.  HW-confirmed "
                         "4/4 on RS482 by odiv_l_vk_probe.  DIM-8-ONLY (same Hurwitz wall)",
      .mesa_hook       = "r300_nir_detect_odiv_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "QADD",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion addition a+b = componentwise vec4 add, zero DP4; "
                         "served by the binary-map detector (nir_op_fadd of two load_ssbo "
                         "vec4s).  A value_is_float binary map now dispatches in the FP "
                         "domain (FP32 sampler, FP16 RT, FP32 readback) instead of UNORM8, "
                         "HW-confirmed 4/4 on RS482 by qadd_vk_probe",
      .mesa_hook       = "r300_nir_detect_binary_map",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "QSUB",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion subtraction a-b = componentwise vec4 sub, zero DP4; "
                         "binary-map(nir_op_fsub) on the same FP-domain dispatch as QADD, "
                         "HW-confirmed 4/4 on RS482 by qsub_vk_probe",
      .mesa_hook       = "r300_nir_detect_binary_map",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "QDOT",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion inner product <a,b> = dot(a,b) = one DP4; served by "
                         "the dp4 detector (f2u32(fdot4) store) and HW-confirmed 4/4 by "
                         "reuse of the dp4 dispatch",
      .mesa_hook       = "r300_nir_detect_dp4_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "QCONJ",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion conjugate conj(a) = (a.x,-a.y,-a.z,-a.w), zero DP4; "
                         "involution conj(conj a)=a and antimorphism conj(p*q)=conj(q)*"
                         "conj(p) machine-verified (open_gororoba CayleyDicksonAlgebra.v "
                         "quat_conj_involution:68, quat_conj_antimorphism:150).  The "
                         "single-load vec4 sign flip is admitted by r300_nir_detect_qconj_"
                         "pattern and dispatched on the 1-in/1-out FP16-RT core, HW-"
                         "confirmed 4/4 (exact) on RS482 by qconj_vk_probe",
      .mesa_hook       = "r300_nir_detect_qconj_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "QNORM",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion squared norm |a|^2 = dot(a,a) = one DP4; "
                         "a*conj(a) = (|a|^2,0,0,0) machine-verified (open_gororoba "
                         "CayleyDicksonAlgebra.v quat_norm_conjugate:84).  Admitted as the "
                         "single-load self-dot splat vec4(dot(a,a)) by r300_nir_detect_"
                         "qnorm_pattern and dispatched on the 1-in/1-out FP16-RT core, HW-"
                         "confirmed 4/4 on RS482 by qnorm_vk_probe (the kernel reads lane 0)",
      .mesa_hook       = "r300_nir_detect_qnorm_pattern",
      .retained_bundle = NULL,
   },
   {
      .op_name         = "QNORMALIZE",
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_NUMERIC_DERIVED,
      .theorem         = "quaternion normalize a/|a| = a * rsqrt(|a|^2); one DP4 (QNORM) "
                         "plus scalar RSQ and a vec4 scale -- the unit-quaternion form "
                         "QROTATE requires.  open_gororoba has no normalization lemma yet "
                         "(ROCQ gap recorded in the quaternion-ISA design finding)",
      .mesa_hook       = NULL,  /* QNORM + RSQ + scale; NIR detector pending */
      .retained_bundle = NULL,
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
