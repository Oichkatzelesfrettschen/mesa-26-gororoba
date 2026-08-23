/*
 * SPDX-License-Identifier: MIT
 *
 * Registry implementation for the RS482/r300 typed numeric domain model.
 *
 * The per-domain descriptor table defines numeric traits (rounding model,
 * exact bound kind and value, significand width, special-value policy, and
 * evidence tier).  The legacy virtual-op catalog records operation names,
 * theorems, status, and descriptive implementation labels.
 *
 * Build-time assertions keep the domain table and the r300_numeric_domain enum
 * in sync: adding a domain to the enum without a table row fails the build.
 */

#include "amd/r300/common/r300_numeric_domain.h"

#include "util/macros.h"

/* One row per r300_numeric_domain value, in enum order.  The build assert
 * below enforces that the table length equals R300_NUM_DOMAIN_COUNT, so
 * a new enum value without a corresponding row is a build error. */
static const struct r300_numeric_domain_info r300_numeric_domain_table[] = {
   {
      .domain            = R300_NUM_DOMAIN_FP24_RTZ,
      .name              = "fp24-rtz",
      .rounding          = R300_ROUND_TRUNCATE,
      .exact_bound_kind  = R300_BOUND_MAX_ABS_INCLUSIVE,
      .exact_int_bound   = R300_FP24_EXACT_INT_CEILING,
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
      .exact_bound_kind  = R300_BOUND_NONE,
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
      .exact_bound_kind  = R300_BOUND_NONE,
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
      .exact_bound_kind  = R300_BOUND_MAX_ABS_INCLUSIVE,
      .exact_int_bound   = 131071,  /* 2^17-1: (2^16-1)+(2^16-1)+1 for two-limb add */
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = true,
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = "(2^16-1)+(2^16-1)+1 = 2^17-1 < 2^17 for Q16_16 add; "
                           "(2^6-1)^2 = 3969, 4*3969 = 15876 < 2^17 per 6-bit limb column; "
                           "limb arithmetic verified on RS482 (rs482_fp16_pow2_carry_exactness_20260607)",
   },
   {
      .domain            = R300_NUM_DOMAIN_U7_DOT,
      .name              = "u7-dot",
      .rounding          = R300_ROUND_EXACT,
      .exact_bound_kind  = R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
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
      .domain            = R300_NUM_DOMAIN_U7_CONV5,
      .name              = "u7-conv5",
      .rounding          = R300_ROUND_EXACT,
      .exact_bound_kind  = R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
      .exact_int_bound   = 80645,  /* 5*(2^7-1)^2 = 5*127^2 = 80645 */
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = true,
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = "5*(2^7-1)^2 = 80645 < 2^17 = 131072",
   },
   {
      .domain            = R300_NUM_DOMAIN_I8_MAG_DOT,
      .name              = "i8-mag-dot",
      .rounding          = R300_ROUND_EXACT,
      .exact_bound_kind  = R300_BOUND_MAX_ABS_INCLUSIVE,
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
      .exact_bound_kind  = R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
      .exact_int_bound   = R300_FP24_EXACT_INT_CEILING,
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
      .exact_bound_kind  = R300_BOUND_NONE,
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
      .exact_bound_kind  = R300_BOUND_NONE,
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
      .exact_bound_kind  = R300_BOUND_NONE,
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
      .exact_bound_kind  = R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
      .exact_int_bound   = 255,
      .significand_bits  = 0,
      .has_nan           = false,
      .has_inf           = false,
      .has_subnormal     = false,
      .is_native_compute = false,  /* per-pixel state machine, not PFS ALU */
      /* INCR/INVERT confirmed; DECR/WRAP pending. */
      .evidence          = R300_EVIDENCE_HW_CONFIRMED,
      .theorem           = "INCR and INVERT confirmed; DECR/WRAP need targeted probes",
   },
   {
      .domain            = R300_NUM_DOMAIN_ZPASS_COUNT,
      .name              = "zpass-count",
      .rounding          = R300_ROUND_EXACT,
      .exact_bound_kind  = R300_BOUND_UNBOUNDED_BY_DOMAIN,
      .exact_int_bound   = 0,
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
      .exact_bound_kind  = R300_BOUND_NONE,
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
      .exact_bound_kind  = R300_BOUND_NONE,
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

bool
r300_vop_status_is_carrier_pending(enum r300_vop_status status)
{
   return status == R300_VOP_CARRIER_PENDING ||
          status == R300_VOP_HW_CONFIRMED_CARRIER_PENDING;
}

/* Virtual op catalog for the RS482 compute-as-raster substrate.
 *
 * Each row records one named virtual op: domain, status, theorem, and an
 * optional descriptive implementation label.  External evidence paths belong
 * in retained bundles and
 * findings, not compiled Mesa metadata.  The catalog is the C-side
 * representation of the TSV substrate table; the two must be kept in sync when
 * a new op is confirmed.
 *
 * Terminated by a row with op_name == NULL. */
#define OP(id)                                                                \
   .operation_id = R300_OPERATION_ID_##id, .op_name = #id

const struct r300_virtual_op_info r300_virtual_op_catalog[] = {
   {
      OP(IDENTITY_MAP),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "out[gid] = in[gid]: passthrough via fullscreen TEX + RB3D export",
      .implementation_label = "r300_nir_detect_identity_map",
   },
   {
      OP(BINARY_MAP),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "out[gid] = f(a[gid], b[gid]) for admitted binary FP24 ops",
      .implementation_label = "r300_nir_detect_binary_map",
   },
   {
      OP(BLEND_ACC_REDUCTION),
      .domain          = R300_NUM_DOMAIN_RB3D_BLEND,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "histogram add via RB3D COMB_FCN_ADD blend accumulation",
      .implementation_label = "r300_nir_detect_blend_acc_reduction",
   },
   {
      OP(ZPASS_COVERAGE_COUNT),
      .domain          = R300_NUM_DOMAIN_ZPASS_COUNT,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "predicated fragment count via ZB_ZPASS_DATA occlusion-query path",
      .implementation_label = "r300_nir_detect_zpass_reduction",
   },
   {
      OP(MULTIPASS_PING_PONG_SCAN),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "self-iterated doubling via dependent FBO ping-pong passes",
      .implementation_label = "r300_nir_detect_multipass_scan_pattern",
   },
   {
      OP(PREDICATED_MASKED_STORE),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "per-element conditional store via per-pixel KILL_IF discard",
      .implementation_label = "r300_nir_detect_predicated_store_pattern",
   },
   {
      OP(MULTITAP_GATHER),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "N-tap neighborhood sum via multi-TEX fragment draw; "
                         "integer-exact for per-tap UNORM8 sum <= 255",
      .implementation_label = "r300_nir_detect_multitap_gather_pattern",
   },
   {
      OP(DP4_UINT7_EXACT),
      .domain          = R300_NUM_DOMAIN_U7_DOT,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "4*(2^7-1)^2 = 64516 < 2^17: byte-exact for U7-magnitude operands",
      .implementation_label = "r300_nir_detect_dp4_pattern",
   },
   {
      OP(DP4_INT8_SIGNED_CARRIER_PENDING),
      .domain          = R300_NUM_DOMAIN_I8_MAG_DOT,
      .status          = R300_VOP_CARRIER_PENDING,
      .theorem         = "|a_i|,|b_i| <= 127: |sum| <= 64516 < 2^17; signed DP4 arithmetic is exact, "
                         "but Mesa must add a signed byte carrier before dispatch readback can route it",
      .implementation_label = NULL,
   },
   {
      OP(DP4_UINT8_OFFGRID_ROUNDS),
      .domain          = R300_NUM_DOMAIN_U8_OFFGRID,
      .status          = R300_VOP_BOUNDARY,
      .theorem         = "4*(2^8-1)^2 = 260100 > 2^17; above-window results are FP24-approximate",
      .implementation_label = "r300_nir_detect_dp4_pattern",
   },
   {
      OP(QUADRATIC_DISCRIMINANT_OFFGRID),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_BOUNDARY,
      .theorem         = "two-circle gradient t = (B +/- sqrt(B^2 - A*C))/A: the intermediates "
                         "B^2, A*C, B^2-A*C reach ~10^8 (180x-1600x the 2^17 = 131072 exact "
                         "window), so FP24 RTZ rounds them off-grid and perturbs t by ~1e-4, "
                         "enough to cross the [0,1) repeat boundary",
      /* Glamor emits this quadratic directly; Mesa has no NIR detector. */
      .implementation_label = NULL,
   },
   {
      OP(Q16_16_ADD),
      .domain          = R300_NUM_DOMAIN_Q16_16,
      .status          = R300_VOP_CARRIER_PENDING,
      .theorem         = "(2^16-1)+(2^16-1)+1 = 2^17-1 < 2^17; limb carry exact.  "
                         "The NIR detector classifies the shape; no production carrier "
                         "or dispatch path consumes it",
      /* The detector is test-only; production classification does not invoke it. */
      .implementation_label = NULL,
   },
   {
      OP(Q16_16_MUL),
      .domain          = R300_NUM_DOMAIN_Q16_16,
      .status          = R300_VOP_CARRIER_PENDING,
      .theorem         = "(2^6-1)^2 = 3969 per limb; 4-column sum <= 15876 < 2^17 "
                         "per column.  The production detector, carrier, and dispatch "
                         "path are absent",
      /* A 4x6-bit limb-multiply detector remains absent. */
      .implementation_label = NULL,
   },
   {
      OP(Q16_16_MAC),
      .domain          = R300_NUM_DOMAIN_Q16_16,
      .status          = R300_VOP_HW_CONFIRMED_CARRIER_PENDING,
      .theorem         = "out = a*b + c in Q16.16, carried multi-limb at base 2^4 / 8 limbs.  "
                         "Base 2^4 (not the Q16_16_MUL entry's base 2^6) makes the Q16.16 "
                         "recovery from the Q32.32 product a >>16 = drop-exactly-4-limbs "
                         "limb-boundary realign (16 = 4*4); the partial product is (2^4-1)^2 = "
                         "225 and a convolution column sums at most 8 partials = 1800 << 2^17, "
                         "FP24-exact with wide margin.  The multi-limb MUL principle is "
                         "HW-confirmed at five 7-bit limbs (MULTILIMB7_U32_MUL).  HW-confirmed "
                         "10/10 bit-exact on RS482 (R300_R2VB_QMAC, 31 r300 ALU, boot-stable) "
                         "across the edge-case set (zero, +-1.0, mixed sign, fractional carry, "
                         "near-overflow +/-, accumulate): the conv + >>16 truncation-carry + "
                         "add c run on the fragment ALU, the inherent limb->integer recombine on "
                         "the host.  Two FP24 constraints made explicit by silicon: the >>16 "
                         "truncation carry must be a PER-LIMB base-2^4 chain (a one-shot "
                         "floor((p0+16p1+256p2+4096p3)/65536) overflows 2^17); and signed Q16.16 "
                         "needs the two's-complement sign correction (unsigned limb product "
                         "matches signed in its low 48 bits, but >>16 pulls in bits 32-47 "
                         "differing by (a*sb + b*sa)*2^32 -- subtract the other operand's low "
                         "limbs when negative).  Host oracle r300_q16_16_mac_multilimb_oracle.c "
                         "bit-exact (tol 0) over 2,000,000 random triples.  Production "
                         "NIR admission, carrier, and dispatch remain absent",
      /* The limb-MAC NIR detector remains pending. */
      .implementation_label = NULL,
   },
   {
      OP(IEEE16_CLASSIFY_LUT),
      .domain          = R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "FP16 bit[15]=sign, bits[14:10]=exp(0..31), bits[9:0]=mantissa; "
                         "class determined by (exp==0, exp==31, mantissa==0) partition; "
                         "15/15 bit patterns exact on RS482 (rs482_fp16_pow2_carry_exactness_20260607)",
      .implementation_label = "r300_nir_lower_ieee16_classify",
   },
   {
      OP(IEEE16_MUL_RNE),
      .domain          = R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "2-limb base-64: c1=a0*b1+a1*b0 <= 2*63*31=3906 < 2^17; "
                         "carry limbs (r0,r1,r2) 12/12 exact on RS482; "
                         "RNE round from guard/sticky/lsb (rs482_fp16_pow2_carry_exactness_20260607)",
      .implementation_label = "r300_nir_lower_ieee16_mul_normal_rne",
   },
   {
      OP(QMUL_HAMILTON),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "Hamilton product = Cayley-Dickson multiplication at dim 4 = four "
                         "sign-permuted DP4s; sign-for-sign the machine-verified quat_mul; "
                         "integer self-check (1,2,3,4)*(5,6,7,8) = (-60,12,30,24) exact on RS482",
      /* QMUL is four sign-permuted DP4s. */
      .implementation_label = "r300_nir_detect_qmul_pattern",
   },
   {
      OP(QDIV),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion right division out = a/b = a*inv(b), inv(b) = "
                         "conj(b)/|b|^2.  H is an ASSOCIATIVE division algebra: every nonzero "
                         "quaternion is invertible (|b|^2 > 0 unless b=0) and the scalar "
                         "reciprocal r = 1/|b|^2 (US RCP) scaling conj(b) is a true two-sided "
                         "inverse -- ROCQ ground open_gororoba CDInverse.v quat_mul_inv_r and "
                         "quat_mul_inv_l (quat_norm_sq q <> 0).  ONE pass, unlike the octonion "
                         "ODIV: a single Hamilton product a*inv(b) (four DP4s plus the "
                         "reciprocal) stays well under the 64-ALU R300 fragment limit "
                         "(R300_PFS_MAX_ALU_INST), so no MRT split is needed.  Admitted by "
                         "r300_nir_detect_qdiv_pattern (matches the single-self-dot reciprocal "
                         "1/dot(b,b), conj(b)*r, and the qmul_match Hamilton product a*inv(b)), "
                         "synthesized by r3v_build_qdiv_fs_nir, dispatched on the QMUL "
                         "two-in/one-out replay core; HW-confirmed 4/4 on RS480 by qdiv_vk_probe "
                         "(a/1=a, 1/(2i)=-0.5i, 4i/2i=2 bit-exact; x/x=1 within FP16 RT tol), the "
                         "FS compiling to 23 fragment ALU ops -- far under the 64-ALU limit",
      .implementation_label = "r300_nir_detect_qdiv_pattern",
   },
   {
      OP(QROTATE_SANDWICH),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "vertex rotation q*v*conj(q) = two Hamilton products = eight DP4s.  "
                         "The sandwich equals the rotation matrix R(q) v for a unit q: "
                         "MACHINE-VERIFIED in open_gororoba -- cd_quat_rotate_eq_matrix "
                         "(CDQuatRotationMatrix.v, on the substrate's CDQuat type) and "
                         "C876_quat_rotation_eq_matrix (C876_QuaternionRotation.v, on the Quat "
                         "type).  HW-confirmed 4/4 by qrotate_vk_probe vs a CPU sandwich on "
                         "RS482",
      /* QROTATE is a nested two-Hamilton sandwich. */
      .implementation_label = "r300_nir_detect_qrotate_pattern",
   },
   {
      OP(MAT4VEC),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "general 4x4 vertex transform out[j] = M * p[j], component i = "
                         "dot(row_i, p_j) -- four DP4s per vertex, one fullscreen invocation, "
                         "the absent vertex FPU's core operation run on the PRESENT FP24 "
                         "fragment ALU.  RS480 has num_vert_fpus = 0 (r300_chipset.c never sets "
                         "it for CHIP_RS480, so has_hardware_tcl = num_vert_fpus>0 = false): the "
                         "vertex engine is a breadboard, FPUs ABSENT not gated.  But the MVP "
                         "transform IS a vec4 of four DP4s, and DP4 is HW-confirmed on the "
                         "fragment ALU, so the transform routes through the compute-as-raster "
                         "substrate.  The scalar decomposition is ALREADY HW-confirmed 16/16 "
                         "byte-exact (dp4_mvp_probe: invocation j*4+i = dot(row_i, p_j) via the "
                         "existing DP4 op, integer operands keep the FP24 dot exact, each "
                         "4-term dot < 2^17).  MAT4VEC promotes that to a first-class op: matrix "
                         "broadcast at sampler stage 0 (a 4-texel constant), vertices per-element "
                         "at stage 1, the synthesized FS sampling row_i at the four fixed texel "
                         "centres and emitting vec4(dot(row_i, p)) to the FP16 RT / FP32 readback "
                         "(QMUL two-in/one-out replay core, matrix rows in place of the Hamilton "
                         "permutations).  This is the bridge: the vertex transform wired into the "
                         "FP24 ALU through the breadboard hole.  Position precision is the FP24 "
                         "snapped-coordinate budget; per-element matrix is the skinning extension.  "
                         "The first-class op is HW-confirmed 4/4 byte-exact on RS482 (mat4vec_vk_"
                         "probe: matrix {2,0,0,5; 0,3,0,7; 0,0,4,9; 0,0,0,1} times four vertices, "
                         "GPU == CPU oracle to maxabs 0.0, QMUL control unregressed)",
      /* The descriptive five-load shape is four broadcast matrix rows plus
       * one per-element vertex. */
      .implementation_label = "r300_nir_detect_mat4vec_pattern",
   },
   {
      OP(OMUL_OCTONION),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion product (a,b)*(c,d) = (a*c - conj(d)*b, d*a + b*conj(c)) "
                         "= Cayley-Dickson doubling = four Hamilton products = sixteen DP4s; "
                         "norm multiplicative |xy|^2 = |x|^2 |y|^2 (Hurwitz at dim 8).  "
                         "r300_nir_detect_omul_pattern admits the eight-wide kernel (four "
                         "quaternion inputs, two output halves), the two synthesized FS "
                         "passes (r3v_build_omul_lo/hi_fs_nir) emit the halves, and the "
                         "two-pass dispatch fills the result -- HW-confirmed 4/4 exact on "
                         "RS482 by omul_vk_probe, the Hurwitz norm holding exactly",
      .implementation_label = "r300_nir_detect_omul_pattern",
   },
   {
      OP(OADD),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion addition (a,b)+(c,d) = (a+c, b+d), componentwise vec8 "
                         "add over two output halves, zero DP4.  Admitted by "
                         "r300_nir_detect_oaddsub_pattern (is_sub=false) and filled in one "
                         "MRT pass; HW-confirmed 4/4 on RS482 by oct_alg_vk_probe oadd",
      .implementation_label = "r300_nir_detect_oaddsub_pattern",
   },
   {
      OP(OSUB),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion subtraction (a,b)-(c,d) = (a-c, b-d), componentwise vec8 "
                         "sub, zero DP4.  The is_sub=true form of the oaddsub detector, same "
                         "single MRT pass; HW-confirmed 4/4 on RS482 by oct_alg_vk_probe osub",
      .implementation_label = "r300_nir_detect_oaddsub_pattern",
   },
   {
      OP(OCONJ),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion conjugate conj((a,b)) = (conj(a), -b) = Cayley-Dickson "
                         "involution: the lower half is the quaternion conjugate of a "
                         "(scalar lane kept, vector lanes negated), the upper half the full "
                         "negation of b; zero DP4.  Admitted by r300_nir_detect_oconj_pattern, "
                         "filled in one MRT pass; HW-confirmed 4/4 on RS482 by oct_alg_vk_probe",
      .implementation_label = "r300_nir_detect_oconj_pattern",
   },
   {
      OP(ONORM),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion squared norm |(a,b)|^2 = dot(a,a) + dot(b,b), broadcast "
                         "to four lanes; two DP4s.  The norm whose multiplicativity "
                         "|xy|^2=|x|^2|y|^2 OMUL confirms (Hurwitz at dim 8).  Admitted by "
                         "r300_nir_detect_onorm_pattern and dispatched on the 2-in/1-out "
                         "core; HW-confirmed 4/4 on RS482 by oct_alg_vk_probe onorm",
      .implementation_label = "r300_nir_detect_onorm_pattern",
   },
   {
      OP(ODIV),
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
      .implementation_label = "r300_nir_detect_odiv_pattern",
   },
   {
      OP(ODIV_L),
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
      .implementation_label = "r300_nir_detect_odiv_pattern",
   },
   {
      OP(OTRANS),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "octonion sandwich transform out = x*v*conj(x), the octonion "
                         "analog of the quaternion rotation sandwich.  Two chained "
                         "octonion products t = x*v then out = t*conj(x) (32 DP4s).  "
                         "Parenthesis-safe by the flexible law (x*y)*x = x*(y*x) since "
                         "conj(x) in R[x] -- ROCQ ground oct_flexible CDPowerAssociative.v, "
                         "schafer1954_octonion_flexibility Schafer1954.v, Brown Thm 3.3(iv) "
                         "Brown1972ChapterIII.v.  Norm scales as |out| = |x|^2 |v| "
                         "(oct_norm_mul + conj-norm invariance); for a unit x the sandwich "
                         "is norm-preserving.  Admitted by r300_nir_detect_otrans_pattern "
                         "(finds the OMUL(x,v) halves tl,th, then verifies the stores are "
                         "OMUL((tl,th),conj(x)) with conj(x) = (conj(xlo),-xhi) folded into "
                         "the rotation rows).  Dispatched as four single-output passes "
                         "through a scratch intermediate t (32 DP4s far exceed the 64-ALU "
                         "R300 fragment limit): pass 1 t=x*v, pass 2 t*conj(x).  HW-confirmed "
                         "4/4 on RS482 by otrans_vk_probe vs a CPU sandwich",
      .implementation_label = "r300_nir_detect_otrans_pattern",
   },
   {
      OP(SED_DIV_DOWNCAST_ADMIT),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_NUMERIC_DERIVED,
      .theorem         = "dim-16 sedenion division is admissible IFF both operands lie in the "
                         "octonion downcast lane (hi half = components 8..15 all zero); "
                         "otherwise it must reject.  A hi-half-zero sedenion is exactly an "
                         "octonion downcast and the lane is CLOSED under the product (the "
                         "product's hi half stays zero) -- ROCQ ground open_gororoba "
                         "C1630_SedenionOctonionDowncast -- and has NO zero divisors "
                         "(C1638_OctonionDowncastNoZeroDivisors), so on the lane conj/N is a "
                         "true inverse and division reduces to the HW-confirmed octonion ODIV "
                         "of the low halves.  Off the lane dim-16 has zero divisors: "
                         "C1635_SedenionDriverSemantics proves a nonzero zero-divisor pair "
                         "cannot both be hi-half-zero, and the R300 witness "
                         "C1637_R300SedenionZeroDivisor proves (e1+e10)(e5+e14)=0 with both "
                         "operands nonzero and NOT downcast (HW-confirmed reading exact zero "
                         "on RS482, r2vb_sed_q0_verify).  So the admission predicate is a "
                         "two-operand hi-half-zero test feeding the octonion ODIV lane; the "
                         "general dim-16 division stays REJECTED (Hurwitz wall, see ODIV)",
      /* The admitted downcast lane reuses the octonion ODIV shape. */
      .implementation_label = NULL,
   },
   {
      OP(QADD),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion addition a+b = componentwise vec4 add, zero DP4; "
                         "served by the binary-map detector (nir_op_fadd of two load_ssbo "
                         "vec4s).  A value_is_float binary map now dispatches in the FP "
                         "domain (FP32 sampler, FP16 RT, FP32 readback) instead of UNORM8, "
                         "HW-confirmed 4/4 on RS482 by qadd_vk_probe",
      .implementation_label = "r300_nir_detect_binary_map",
   },
   {
      OP(QSUB),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion subtraction a-b = componentwise vec4 sub, zero DP4; "
                         "binary-map(nir_op_fsub) on the same FP-domain dispatch as QADD, "
                         "HW-confirmed 4/4 on RS482 by qsub_vk_probe",
      .implementation_label = "r300_nir_detect_binary_map",
   },
   {
      OP(QDOT),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion inner product <a,b> = dot(a,b) = one DP4; served by "
                         "the dp4 detector (f2u32(fdot4) store) and HW-confirmed 4/4 by "
                         "reuse of the dp4 dispatch",
      .implementation_label = "r300_nir_detect_dp4_pattern",
   },
   {
      OP(QCONJ),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion conjugate conj(a) = (a.x,-a.y,-a.z,-a.w), zero DP4; "
                         "involution conj(conj a)=a and antimorphism conj(p*q)=conj(q)*"
                         "conj(p) machine-verified (open_gororoba CayleyDicksonAlgebra.v "
                         "quat_conj_involution:68, quat_conj_antimorphism:150).  The "
                         "single-load vec4 sign flip is admitted by r300_nir_detect_qconj_"
                         "pattern and dispatched on the 1-in/1-out FP16-RT core, HW-"
                         "confirmed 4/4 (exact) on RS482 by qconj_vk_probe",
      .implementation_label = "r300_nir_detect_qconj_pattern",
   },
   {
      OP(QNORM),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion squared norm |a|^2 = dot(a,a) = one DP4; "
                         "a*conj(a) = (|a|^2,0,0,0) machine-verified (open_gororoba "
                         "CayleyDicksonAlgebra.v quat_norm_conjugate:84).  Admitted as the "
                         "single-load self-dot splat vec4(dot(a,a)) by r300_nir_detect_"
                         "qnorm_pattern and dispatched on the 1-in/1-out FP16-RT core, HW-"
                         "confirmed 4/4 on RS482 by qnorm_vk_probe (the kernel reads lane 0)",
      .implementation_label = "r300_nir_detect_qnorm_pattern",
   },
   {
      OP(QNORMALIZE),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion normalize a/|a| = a * rsqrt(|a|^2); one DP4 (QNORM) "
                         "plus the US RSQ (the substrate's reciprocal-square-root, new to "
                         "this op) and a vec4 scale -- the unit-quaternion form QROTATE "
                         "requires.  ROCQ ground: quat_normalize_unit (open_gororoba "
                         "QuatNormalize.v) proves |normalize(a)|^2 = 1 for nonzero a, via "
                         "quat_norm_sq_scale + sqrt_sqrt (the corpus previously had no "
                         "normalization lemma -- this op closed that gap).  Admitted as the "
                         "single-load fmul(a, frsq(dot(a,a)).xxxx) by r300_nir_detect_"
                         "qnormalize_pattern and dispatched on the 1-in/1-out FP16-RT core, "
                         "HW-confirmed 4/4 on RS482 by qnormalize_vk_probe (|out| = 1)",
      .implementation_label = "r300_nir_detect_qnormalize_pattern",
   },
   {
      OP(QFMADD),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion fused multiply-add out = a*b + c: the Hamilton "
                         "product (four DP4s) plus the quaternion c, one pass.  Three "
                         "input SSBOs a,b,c, one output.  Admitted by r300_nir_detect_"
                         "qfmadd_pattern (the fadd of a qmul(a,b) vec4 and the identity-"
                         "loaded c) and dispatched on the three-in/one-out FP16-RT core.  "
                         "The +c could ride the RB3D COMB_FCN_ADD blend over a c-preloaded "
                         "target (a substrate-native FMA, the blend-acc path already drives "
                         "it); here it is a straightforward ALU add.  HW-confirmed 4/4 on "
                         "RS482 by qfmadd_vk_probe vs a CPU Hamilton-product-plus-add",
      .implementation_label = "r300_nir_detect_qfmadd_pattern",
   },
   {
      OP(QFMMUL),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion fused triple product out = a*b*c = (a*b)*c: two "
                         "chained Hamilton products (eight DP4s) in one pass, well under "
                         "the 64-ALU R300 fragment limit.  Associativity in H makes the "
                         "parenthesization irrelevant (quat_mul_assoc, open_gororoba "
                         "CayleyDicksonAlgebra.v) -- unlike the octonion OTRANS, no scratch "
                         "intermediate is needed.  Three input SSBOs a,b,c, one output.  "
                         "Admitted by r300_nir_detect_qfmmul_pattern (find t = qmul(a,b), "
                         "verify the store is qmul(t,c)) and dispatched on the three-in/"
                         "one-out core.  HW-confirmed 4/4 on RS482 by qfmmul_vk_probe",
      .implementation_label = "r300_nir_detect_qfmmul_pattern",
   },
   {
      /* QF scalar tier (quaternion x real scalar), the broadcast-operand lever. */
      OP(QFMUL),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "quaternion-scalar product out[gid] = a[gid] * s: a per-element vec4 "
                         "quaternion times ONE uniform scalar broadcast across all four lanes, "
                         "0 DP4 -- a single vec4 MUL.  The lever is where s lives: a uniform "
                         "broadcast belongs in the fragment constant file (CONST[0].x), not a "
                         "per-element texture, exactly as MAT4VEC parks its broadcast matrix in "
                         "CONST[0..3].  The synthesized FS is one TEX (the quaternion at sampler "
                         "stage 0) and one MUL against CONST[0].x -- no second sampler, no "
                         "per-fragment fetch for the scalar.  This is NOT the FP binary_map path: "
                         "binary_map wraps BOTH operands as per-element samplers spanning the whole "
                         "raster extent, so the one-float scalar buffer would be sampled past its "
                         "end; the binary_map detector now declines the 4-vs-1 component-width "
                         "asymmetry (width-equality guard) and hands the kernel to the dedicated "
                         "QFMUL detector.  The vec4*scalar fmul lowers OpVectorTimesScalar to a "
                         "4-component fmul whose scalar operand is a 1-component splat; the detector "
                         "keys on that identity-swizzled-vec4 + splat-swizzled-scalar shape.  "
                         "HW-confirmed 4/4 byte-exact on RS482 (qfmul_vk_probe: s = 2.5 times four "
                         "quaternions, GPU == CPU oracle to maxabs 0.0).  QFADD/QFSUB are masked "
                         "adds on the real part, QFDIV = a*rcp(s), QFTRANS = s*a + t*(1,0,0,0) a "
                         "MAD -- the remaining scalar-tier forms, 0 DP4 componentwise",
      /* Broadcast scalar: CONST[0].x, one TEX, and one MUL; the binary-map
       * width guard keeps the classes disjoint. */
      .implementation_label = "r300_nir_detect_qfmul_pattern",
   },
   {
      /* Degenerate constant-fill: out[gid] = C for every element.  The
       * stored value is a compile-time constant, so no fragment ALU is
       * needed -- the entire output is the same four bytes.  The hardware
       * primitive is an RB3D color-buffer clear (C rides the RGBA8 clear
       * color channels) followed by the standard RT-to-buffer copy that the
       * identity-map readback path provides.  Theorem: "degenerate store is
       * a clear."  Not a compute domain -- no FP24 ALU participates; this is
       * a degenerate SSBO-store verb realized entirely by the RB3D clear unit
       * and the identity-map readback copy. */
      OP(CONSTFILL),
      .domain          = R300_NUM_DOMAIN_RB3D_BLEND,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "out[gid] = C for all gid: degenerate store is a clear; "
                         "C rides the RGBA8 RB3D clear color, the RT-to-buffer "
                         "identity-map readback copy delivers C to every element; "
                         "no per-element fragment ALU needed",
      .implementation_label = "r300_nir_detect_const_fill_pattern",
   },
   {
      /* First verb that materializes the work-item index as an FP24 VALUE:
       * a single oversized triangle carries a texel-unit varying, the
       * fragment program snaps each interpolated coordinate with FLR
       * (absorbing sub-texel plane-equation error), reconstructs the linear
       * index, evaluates the affine, and byte-decomposes little-endian into
       * the RGBA8 export.  One-dimensional dispatches only; the dispatch
       * gate bounds stride * (total - 1) + offset by 2^17. */
      OP(AFFINE_IOTA),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "out[gid] = stride * gid + offset, gid from per-axis "
                         "FLR-snapped texel-unit varying; exact for "
                         "stride * (total - 1) + offset <= 2^17 (probe: 131072 "
                         "elements byte-exact at the ceiling, explicit no-op above)",
      .implementation_label = "r300_nir_detect_affine_iota_pattern",
   },
   {
      /* 32x32 -> 64-bit integer multiply on the FP24 ALU: each factor splits
       * into five 7-bit limbs and the product's nine convolution columns
       * c_k = sum_{i+j=k} a_i * b_j each stay <= 5 * 127^2 = 80645 < 2^17,
       * an exact FP24 integer (the U7_CONV5 property).
       * Carry propagation over the byte-decomposed columns is integer
       * bookkeeping outside the FP24 ALU in the single-pass catalog entry. */
      OP(MULTILIMB7_U32_MUL),
      .domain          = R300_NUM_DOMAIN_U7_CONV5,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "a * b for 32-bit a, b via five 7-bit limbs: every "
                         "convolution column <= 5 * 127^2 < 2^17 is FP24-exact; "
                         "GL probe: 0xFFFFFFFF^2 assembled bit-exact; Vulkan "
                         "replay verb: 1024/1024 elements exact end-to-end "
                         "(low-32 wraparound), sampled bytes snapped with "
                         "floor(v * 255 + 0.5) before limb extraction",
      .implementation_label = "r300_nir_detect_multilimb_mul_pattern",
   },
   {
      /* One log4 tree level per LINEAR tap at a 2x2 texel corner: the 6-bit
       * filter weights put 0.25 on-grid, so the tap is the exact quarter sum.
       * The recorded LIMIT is the UNORM8 render-target quantization between
       * levels: a level is byte-exact iff its 2x2 sum is divisible by 4. */
      OP(LOG4_BILINEAR_REDUCE),
      .domain          = R300_NUM_DOMAIN_TX_INT6_WEIGHT,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "sum/4 per 2x2 via one LINEAR corner tap, exact iff "
                         "sum mod 4 == 0 (UNORM8 inter-level carrier); off-grid "
                         "sums quantize within one byte; UNORM8 payloads only "
                         "(float payloads point-sample on RS482)",
      .implementation_label = "r300_nir_detect_log4_pool_pattern",
   },
   {
      /* Versioned CAS: GL exposes ONE stencil ref, so REPLACE writes the
       * value the EQUAL test compared against -- the arbitrary-value swap
       * "test expect, write new" is impossible by API algebra, not by
       * silicon.  The expressible primitive advances exactly the cells at
       * version v, where v < 255, to v + 1 and returns the success count from
       * the same draw's ZPASS query.  Version 255 is a saturating terminal
       * state for INCR, not an advance. */
      OP(STENCIL_VERSIONED_CAS),
      .domain          = R300_NUM_DOMAIN_U8_STENCIL,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "func EQUAL v + op INCR + SAMPLES_PASSED for "
                         "0 <= v < 255: cells at version v advance to v + 1 "
                         "and the query returns the swap-success count in one "
                         "draw; v == 255 is the saturating terminal state; "
                         "probe ladder "
                         "advance(0) = all, advance(0) = none, advance(1) = all, "
                         "end state EQUAL 2 = all",
      /* The mechanism is stencil state rather than a NIR kernel. */
      .implementation_label = NULL,
   },
   {
      /* CAS ROUTE A: per-element constant-operand compare-and-swap on the
       * US select path -- per-cell exclusivity is structural (one fragment
       * per guard), so no ZB involvement; the bytewise SEQ compare has no
       * 2^17 ceiling.  The contended multi-pass form rides the separate
       * stencil-backed STENCIL_VERSIONED_CAS mechanism. */
      OP(CAS_CONST_U32),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "old = comp_swap(guard[gid], C_expect, C_new): "
                         "snapped guard bytes, one vec4 SEQ vs the expect "
                         "bytes, three predicate multiplies, per-channel "
                         "select; pre-image copy returns old; probe 10/10 "
                         "patterns x 4096 elements exact incl one-bit deltas "
                         "and the operand-order discriminator",
      .implementation_label = "r300_nir_detect_cas_pattern",
   },
   {
      /* QFM subtract shares the admitted Hamilton-product shape with QFMADD. */
      OP(QFMSUB),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_NUMERIC_DERIVED,
      .theorem         = "fused multiply-sub a*b - c uses four Hamilton-product DP4s and "
                         "one vec4 SUB.  The QFMADD detector records the non-commutative "
                         "fsub form and pipeline synthesis preserves it in the fragment NIR",
      /* is_sub selects the subtracting fragment shader. */
      .implementation_label = "r300_nir_detect_qfmadd_pattern",
   },
   {
      /* COMB_FCN_MIN: result = min(src_factor * src, dst_factor * dst).
       * With factors ONE/ONE this is per-element min over UNORM8 carriers.
       * Vulkan spec: blend factors are ignored for VK_BLEND_OP_MIN/MAX.
       * R300_COMB_FCN_MIN = (4 << 12); r300_translate_blend_function selects it
       * (rg --fixed-strings R300_COMB_FCN_MIN src/).
       * RS482 probe (r300_substrate_probe.sh): 6/6 byte-exact
       * (min(96,160)=96, min(192,64)=64 for both RGBA channels). */
      OP(REDUCE_MIN),
      .domain          = R300_NUM_DOMAIN_RB3D_BLEND,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "out[gid] = min(a[gid], b[gid]) via R300_COMB_FCN_MIN "
                         "(VK_BLEND_OP_MIN, factors ONE/ONE ignored per spec); "
                         "byte-exact over UNORM8 carrier: 6/6 cases RS482 silicon",
      /* The mechanism is pipeline blend-op state. */
      .implementation_label = NULL,
   },
   {
      /* COMB_FCN_MAX: result = max(src_factor * src, dst_factor * dst).
       * R300_COMB_FCN_MAX = (5 << 12); r300_translate_blend_function selects it
       * (rg --fixed-strings R300_COMB_FCN_MAX src/).
       * RS482 probe: 6/6 byte-exact (max(96,160)=160, max(192,64)=192). */
      OP(REDUCE_MAX),
      .domain          = R300_NUM_DOMAIN_RB3D_BLEND,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "out[gid] = max(a[gid], b[gid]) via R300_COMB_FCN_MAX "
                         "(VK_BLEND_OP_MAX, factors ONE/ONE ignored per spec); "
                         "byte-exact over UNORM8 carrier: 6/6 cases RS482 silicon",
      /* The mechanism is pipeline blend-op state. */
      .implementation_label = NULL,
   },
   {
      /* COMB_FCN_SUB_CLAMP: result = clamp(src - dst, 0, 1) over UNORM8.
       * VK_BLEND_OP_SUBTRACT with UNORM8 render target clamps to [0,1].
       * R300_COMB_FCN_SUB_CLAMP = (2 << 12); r300_translate_blend_function
       * selects it (rg --fixed-strings R300_COMB_FCN_SUB_CLAMP src/).
       * Equivalent to saturating subtract sat_sub(a, b) = max(a - b, 0).
       * RS482 probe: 6/6 byte-exact (sat(96-160)=0, 192-64=128). */
      OP(SATURATING_DIFF),
      .domain          = R300_NUM_DOMAIN_RB3D_BLEND,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "out[gid] = max(a[gid] - b[gid], 0) via R300_COMB_FCN_SUB_CLAMP "
                         "(VK_BLEND_OP_SUBTRACT on UNORM8 target); clamp is UNORM8 format "
                         "saturation; byte-exact: 6/6 cases RS482 silicon",
      /* The mechanism is pipeline blend-op state. */
      .implementation_label = NULL,
   },
   {
      /* Four independent RGBA8_UNORM render targets written in parallel by
       * a single fragment shader with layout(location=0..3) outputs.
       * r3v exposes maxColorAttachments=4 in r3v_physical_device_init_limits
       * (rg --fixed-strings r3v_physical_device_init_limits src/amd/r300/vulkan/;
       *  rg --fixed-strings maxColorAttachments src/amd/r300/vulkan/).
       * independentBlend=false: all 4 attachments share RB3D_CBLEND state,
       * but distinct FS output locations route to distinct color buffers.
       * RS482 probe: 4-attachment framebuffer, FS writes 0x01020304 /
       * 0x05060708 / 0x090a0b0c / 0x0d0e0f10 -- all 4 readback byte-exact. */
      OP(PARALLEL_4OUT_MAP),
      .domain          = R300_NUM_DOMAIN_FP24_RTZ,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "out_k[gid] = f_k(gid) for k in {0,1,2,3}: four "
                         "parallel independent scatter writes via MRT; "
                         "byte-exact 4/4 attachments on RS482 silicon "
                         "(r300_substrate_probe.sh PROBE_MRT4)",
      /* The mechanism is a four-attachment render pass. */
      .implementation_label = NULL,
   },
   {
      /* VK_STENCIL_OP_INVERT flips all 8 bits of the stencil plane per
       * fragment.  r300_translate_stencil_op maps PIPE_STENCIL_OP_INVERT to
       * R300_ZS_INVERT, and r3v_stencil_op_to_pipe maps the Vulkan operation
       * to the Gallium operation (rg --fixed-strings r3v_stencil_op_to_pipe
       * src/amd/r300/vulkan/; rg --fixed-strings PIPE_STENCIL_OP_INVERT src/).
       * Bitwise contract: INVERT(x) = ~x for all x in [0, 255].
       * RS482 probe: fill 0xA5, INVERT once, readback 0x5A -- exact.
       * Enables bitwise NOT on U8 stencil payloads without the ALU. */
      OP(STENCIL_INVERT_NOT),
      .domain          = R300_NUM_DOMAIN_U8_STENCIL,
      .status          = R300_VOP_HW_CONFIRMED,
      .theorem         = "INVERT(x) = ~x for x in [0,255]: VK_STENCIL_OP_INVERT "
                         "flips all 8 stencil bits per fragment; 0xA5 -> 0x5A "
                         "bit-exact on RS482 silicon (r300_substrate_probe.sh "
                         "PROBE_STENCIL_INVERT)",
      /* The mechanism is stencil-op state rather than a NIR pattern. */
      .implementation_label = NULL,
   },
   /* NULL sentinel -- keep last */
   { .operation_id = R300_OPERATION_ID_NONE, .op_name = NULL },
};

#undef OP

unsigned
r300_virtual_op_count(void)
{
   STATIC_ASSERT(ARRAY_SIZE(r300_virtual_op_catalog) ==
                 R300_OPERATION_ID_COUNT);
   /* Count entries up to the NULL sentinel. */
   unsigned n = 0;
   while (r300_virtual_op_catalog[n].op_name)
      n++;
   return n;
}

const struct r300_virtual_op_info *
r300_virtual_op_info_for_id(enum r300_operation_id operation_id)
{
   STATIC_ASSERT(ARRAY_SIZE(r300_virtual_op_catalog) ==
                 R300_OPERATION_ID_COUNT);
   if (operation_id <= R300_OPERATION_ID_NONE ||
       operation_id >= R300_OPERATION_ID_COUNT)
      return NULL;
   const struct r300_virtual_op_info *info =
      &r300_virtual_op_catalog[(unsigned)operation_id - 1];
   return info->operation_id == operation_id && info->op_name != NULL ? info
                                                                      : NULL;
}
