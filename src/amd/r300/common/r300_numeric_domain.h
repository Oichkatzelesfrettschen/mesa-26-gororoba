/*
 * SPDX-License-Identifier: MIT
 *
 * Typed numeric domain model for the RS482/r300 compute-as-raster substrate.
 *
 * The root type for the PFS fragment ALU is F_{24}^{RTZ}: a finite FP24 value
 * set (s1e7m16, exponent bias 62, normal range [2^-61, 2^65]) with
 * round-toward-zero arithmetic and an exact integer window of |n| <= 2^17.
 * No NaN, no Inf, no subnormals; underflow flushes to zero; overflow saturates.
 *
 * Each hardware block on RS482 implements a distinct algebra over a specific
 * carrier type.  Naming those carriers explicitly lets admission classifiers,
 * pattern detectors, and orchestrator dispatch logic state which domain a
 * virtual op operates in instead of collapsing everything to "float" or
 * "integer", which loses the actual precision and exactness contract.
 *
 * The DP4 arc established the template: theorem (4*(2^7-1)^2 < 2^17) ->
 * silicon probe -> pattern detector -> carrier policy -> Vulkan readback.
 * Every entry in the virtual op catalog should carry the same chain.
 */

#ifndef R300_NUMERIC_DOMAIN_H
#define R300_NUMERIC_DOMAIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest integer N such that every integer in [0, N] is exact in FP24.
 * This is a silicon numeric-domain constant, not an API or compiler policy. */
#define R300_FP24_EXACT_INT_CEILING ((uint32_t)1 << 17)

/* Rounding contract for an arithmetic domain.  EXACT captures the
 * FP24 integer window (result is the mathematical value when bounded).
 * TRUNCATE is the FP24 RTZ model for out-of-window floating-point ops.
 * CLAMP is the UNORM/stencil saturation model.
 * BIASED is the 6-bit bilinear weight model whose rounding is
 * implementation-dependent within the TX hardware.
 * RNE is round-to-nearest-even (IEEE 754 default mode), emulated in FP24
 * via integer limb arithmetic for domains such as IEEE_FP16_VIRTUAL. */
enum r300_rounding_model {
   R300_ROUND_EXACT,     /* no rounding: result equals the mathematical value */
   R300_ROUND_TRUNCATE,  /* truncate toward zero: FP24 RTZ out-of-window ops */
   R300_ROUND_CLAMP,     /* saturate on overflow: U8, stencil, UNORM paths */
   R300_ROUND_BIASED,    /* hardware-defined: TX 6-bit bilinear interpolation */
   R300_ROUND_RNE,       /* round-to-nearest-even: IEEE 754 default, emulated */
};

/* Meaning of an exact numeric bound.  The kind, rather than the numeric
 * value, distinguishes an inapplicable bound from a domain whose operation
 * count is not bounded by the numeric representation. */
enum r300_bound_kind {
   R300_BOUND_NONE = 0,
   R300_BOUND_MAX_ABS_INCLUSIVE,
   R300_BOUND_MAX_UNSIGNED_INCLUSIVE,
   R300_BOUND_INPUT_DEPENDENT,
   R300_BOUND_UNBOUNDED_BY_DOMAIN,
};

/* Status of a virtual operation in the substrate catalog. */
enum r300_vop_status {
   R300_VOP_NUMERIC_DERIVED,  /* theorem proven; silicon not yet measured */
   R300_VOP_HW_CONFIRMED,     /* silicon measurement matches theorem */
   /* silicon measurement matches theorem; production carrier absent */
   R300_VOP_HW_CONFIRMED_CARRIER_PENDING,
   R300_VOP_BOUNDARY,         /* confirmed with documented precision limit */
   R300_VOP_CARRIER_PENDING,  /* arithmetic known; production admission,
                               * carrier, and dispatch work remain pending */
   R300_VOP_REJECTED,         /* falsified by silicon measurement */
};

/* Numeric carrier domains realized by RS482 hardware blocks.  Ordered by
 * hardware block: PFS ALU first, then sampler, then output and reduction
 * units.  The VAP format-ingestion domain is listed last as a source
 * transform, not a compute or reduction domain. */
enum r300_numeric_domain {
   /* US/PFS fragment ALU: s1e7m16, bias 62, normal range [2^-61, 2^65].
    * Round-toward-zero.  Exact integer window: |n| <= 2^17 = 131072.
    * No NaN, no Inf, no subnormals.  Flush-to-zero on underflow; saturate
    * on overflow.  Native ops: MAD, DP3, DP4, ADD, MUL, MIN, MAX, CMP, CND,
    * FRC, MOV_SAT, alpha EX2/LG2/RCP/RSQ, RGB and alpha co-issue. */
   R300_NUM_DOMAIN_FP24_RTZ,

   /* FP16 as storage and transport, not native compute.  Finite FP16 values
    * fit inside FP24 (11-bit significand fits the 17-bit exact window;
    * exponent range [-14, 15] fits FP24 bias 62).  Load through 16F texture
    * paths; compute in FP24_RTZ; export to FP16 or UNORM carrier.  The RTZ
    * vs round-to-nearest-even divergence and flush-to-zero vs subnormal
    * diverge for edge-case inputs, so claiming full IEEE FP16 compute
    * semantics through native FP24 alone is not supportable. */
   R300_NUM_DOMAIN_FP16_STORAGE,

   /* FP32 as texture input only.  R32G32B32A32_FLOAT textures are accepted
    * as DP4 input (hardware-confirmed: dp4_fp32 probe).  FP32 render targets
    * are absent: FP32 color FBO is incomplete (0x8cdd) and
    * EXT_color_buffer_float is not exposed.  Output must use a byte-encoded
    * carrier (RGBA8 integer encoding).  FP32 native compute arithmetic is
    * outside the FP24 envelope: FP32 has a 24-bit significand vs FP24's 17. */
   R300_NUM_DOMAIN_FP32_STORAGE,

   /* Q16.16 fixed-point via base-B integer limbs.  Two-limb add (B=2^16):
    * (2^16-1)+(2^16-1)+1 = 2^17-1 < 2^17, numeric-derived exact.
    * Four-limb multiply (B=2^6, eager carry): each 6-bit limb product
    * (2^6-1)^2 = 3969 and column sums <= 4*3969 = 15876 < 2^17,
    * numeric-derived exact per column.  The base-16 Q16.16 MAC path is
    * silicon-confirmed through the R300_R2VB_QMAC diagnostic.  ADD, MUL, and
    * MAC remain without a production carrier and dispatch path. */
   R300_NUM_DOMAIN_Q16_16,

   /* Unsigned 7-bit dot product: 0 <= a_i, b_i <= 127.
    * Theorem: 4*(2^7-1)^2 = 64516 < 2^17 = 131072.
    * Hardware-confirmed: RS482 surfaceless-EGL dp4 probe (6/6 exact,
    * including 64516, signed cancellation, and random cases) and
    * end-to-end r3v Vulkan DP4 readback (4/4 byte-exact). */
   R300_NUM_DOMAIN_U7_DOT,

   /* Unsigned 7-bit five-term convolution column: 0 <= a_i, b_i <= 127,
    * with at most five terms in any output column.
    * Theorem: 5*(2^7-1)^2 = 80645 < 2^17 = 131072.
    * This is the exactness domain for 32x32 -> 64-bit multiply split into
    * five 7-bit limbs; it is wider than the four-term DP4 domain. */
   R300_NUM_DOMAIN_U7_CONV5,

   /* Signed 8-bit magnitude dot product: |a_i|, |b_i| <= 127.
    * Theorem: |sum| <= 4*(2^7-1)^2 = 64516 < 2^17.
    * Hardware-confirmed as part of the dp4 probe (signed cancellation and
    * signed-negative-total cases in the 6/6 set). */
   R300_NUM_DOMAIN_I8_MAG_DOT,

   /* Unsigned 8-bit dot product, boundary domain: 0 <= a_i, b_i <= 255.
    * 4*(2^8-1)^2 = 260100 > 2^17 = 131072.  Not all-input exact.
    * Hardware-confirmed as DP4_UINT8_OFFGRID_ROUNDS: results above the
    * 2^17 window deviate from the CPU exact value.  Admitted as a
    * precision-boundary op, not an exact op. */
   R300_NUM_DOMAIN_U8_OFFGRID,

   /* TX sampler: 6-bit bilinear interpolation weight for UNORM payloads.
    * UNORM textures interpolate with 6-bit fixed-point weights (Evergreen
    * ISA / TeraScale-2 hardware specification).  Float payloads (FP16/FP32
    * textures) are point-sampled, not bilinearly filtered, on RS482.
    * This domain names the sampler interpolation carrier; it is not a PFS
    * compute domain. */
   R300_NUM_DOMAIN_TX_INT6_WEIGHT,

   /* RB3D blend: render-target-format monoid.  ADD, SUB, MIN, MAX in the
    * blend unit act on the RT format's value range, clamped/wrapped per
    * format.  This is not PFS ALU arithmetic; it is a separate reduction
    * unit downstream of the ALU. */
   R300_NUM_DOMAIN_RB3D_BLEND,

   /* RB3D ROP Boolean bitplane algebra.  AND, OR, and XOR are
    * hardware-confirmed bit-exact on RS482; ROP NOT still needs a targeted
    * truth-table probe.  Operates on raw color-target bits, not FP24 values. */
   R300_NUM_DOMAIN_ROP_BOOL,

   /* Stencil U8 per-pixel state machine.  Per-pixel value in Z/256 or a
    * saturating/replace form depending on the stencil op.
    * INCR and INVERT are hardware-confirmed on RS482; DECR/WRAP need probes. */
   R300_NUM_DOMAIN_U8_STENCIL,

   /* ZPASS fragment-count reduction.  N = sum_{p in Omega} [predicate(p)].
    * UINT-style popcount unit driven by ZB_ZPASS_DATA / ZB_ZPASS_ADDR.
    * Hardware-confirmed via the occlusion-query chain (r300_query.c
    * PIPE_QUERY_OCCLUSION_COUNTER path). */
   R300_NUM_DOMAIN_ZPASS_COUNT,

   /* VAP/PSC format-ingestion morphism.  Packed vertex formats decoded to
    * the float stream the PVS/PFS consumes.  FLOAT_4 observed; FLOAT_8 and
    * FLT16 remain probe targets.  Not a compute domain; a source transform
    * that feeds the PFS. */
   R300_NUM_DOMAIN_VAP_FORMAT_INGEST,

   /* Virtual IEEE FP16 arithmetic emulated on the FP24 substrate.
    * Significand multiply uses 2-limb base-64 column arithmetic: for normal
    * FP16 inputs (a,b in [1024..2047]), split a=a1*64+a0, b=b1*64+b0;
    * columns c0=a0*b0<=3969, c1=a0*b1+a1*b0<=3906, c2=a1*b1<=961 are all
    * < 2^17 = 131072 (FP24 exact integer window), so each column and carry
    * step is FP24-exact.  Rounding is RNE, extracted from the carry triple.
    * Subnormals, NaN, Inf, and signed zero handled by an integer classifier.
    * is_native_compute=false: FP24 hardware does not natively implement IEEE
    * FP16 semantics (RTZ vs RNE, flush-to-zero vs subnormal diverge). */
   R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,

   /* Sentinel: count of domain values.  Keep last. */
   R300_NUM_DOMAIN_COUNT,
};

/* Per-domain descriptor.  One row per r300_numeric_domain value in the
 * r300_numeric_domain_table[] array.  r300_numeric_domain_info() returns a
 * pointer to the row for a given domain; the pointer is static and never NULL
 * for a valid enum value. */
struct r300_numeric_domain_info {
   enum r300_numeric_domain    domain;
   const char                 *name;              /* short stable token */
   enum r300_rounding_model    rounding;
   enum r300_bound_kind        exact_bound_kind;
   uint64_t                    exact_int_bound;
   unsigned                    significand_bits;  /* for float domains (with implicit 1); 0 = N/A */
   bool                        has_nan;
   bool                        has_inf;
   bool                        has_subnormal;
   bool                        is_native_compute; /* false = storage or reduction only */
   const char                 *theorem;           /* formal bound string or NULL */
};

/* Return the descriptor for a domain.  Returns the FP24_RTZ row (index 0)
 * as a safe fallback when domain is out of range; callers need not guard the
 * return value. */
const struct r300_numeric_domain_info *
r300_numeric_domain_info(enum r300_numeric_domain domain);

/* Return true when production admission, carrier, and dispatch work remains
 * pending. */
bool r300_vop_status_is_carrier_pending(enum r300_vop_status status);

/* Stable API-neutral identities for the legacy virtual-operation inventory.
 * NONE is used by registries that have no matching catalog operation.  Values
 * are append-only: retain every assigned number when adding or retiring an
 * operation.  Strings are diagnostic labels only and must not be used as
 * joins. */
enum r300_operation_id {
   R300_OPERATION_ID_NONE = 0,
   R300_OPERATION_ID_IDENTITY_MAP = 1,
   R300_OPERATION_ID_BINARY_MAP = 2,
   R300_OPERATION_ID_BLEND_ACC_REDUCTION = 3,
   R300_OPERATION_ID_ZPASS_COVERAGE_COUNT = 4,
   R300_OPERATION_ID_MULTIPASS_PING_PONG_SCAN = 5,
   R300_OPERATION_ID_PREDICATED_MASKED_STORE = 6,
   R300_OPERATION_ID_MULTITAP_GATHER = 7,
   R300_OPERATION_ID_DP4_UINT7_EXACT = 8,
   R300_OPERATION_ID_DP4_INT8_SIGNED_CARRIER_PENDING = 9,
   R300_OPERATION_ID_DP4_UINT8_OFFGRID_ROUNDS = 10,
   R300_OPERATION_ID_QUADRATIC_DISCRIMINANT_OFFGRID = 11,
   R300_OPERATION_ID_Q16_16_ADD = 12,
   R300_OPERATION_ID_Q16_16_MUL = 13,
   R300_OPERATION_ID_Q16_16_MAC = 14,
   R300_OPERATION_ID_IEEE16_CLASSIFY_LUT = 15,
   R300_OPERATION_ID_IEEE16_MUL_RNE = 16,
   R300_OPERATION_ID_QMUL_HAMILTON = 17,
   R300_OPERATION_ID_QDIV = 18,
   R300_OPERATION_ID_QROTATE_SANDWICH = 19,
   R300_OPERATION_ID_MAT4VEC = 20,
   R300_OPERATION_ID_OMUL_OCTONION = 21,
   R300_OPERATION_ID_OADD = 22,
   R300_OPERATION_ID_OSUB = 23,
   R300_OPERATION_ID_OCONJ = 24,
   R300_OPERATION_ID_ONORM = 25,
   R300_OPERATION_ID_ODIV = 26,
   R300_OPERATION_ID_ODIV_L = 27,
   R300_OPERATION_ID_OTRANS = 28,
   R300_OPERATION_ID_SED_DIV_DOWNCAST_ADMIT = 29,
   R300_OPERATION_ID_QADD = 30,
   R300_OPERATION_ID_QSUB = 31,
   R300_OPERATION_ID_QDOT = 32,
   R300_OPERATION_ID_QCONJ = 33,
   R300_OPERATION_ID_QNORM = 34,
   R300_OPERATION_ID_QNORMALIZE = 35,
   R300_OPERATION_ID_QFMADD = 36,
   R300_OPERATION_ID_QFMMUL = 37,
   R300_OPERATION_ID_QFMUL = 38,
   R300_OPERATION_ID_CONSTFILL = 39,
   R300_OPERATION_ID_AFFINE_IOTA = 40,
   R300_OPERATION_ID_MULTILIMB7_U32_MUL = 41,
   R300_OPERATION_ID_LOG4_BILINEAR_REDUCE = 42,
   R300_OPERATION_ID_STENCIL_VERSIONED_CAS = 43,
   R300_OPERATION_ID_CAS_CONST_U32 = 44,
   R300_OPERATION_ID_QFMSUB = 45,
   R300_OPERATION_ID_REDUCE_MIN = 46,
   R300_OPERATION_ID_REDUCE_MAX = 47,
   R300_OPERATION_ID_SATURATING_DIFF = 48,
   R300_OPERATION_ID_PARALLEL_4OUT_MAP = 49,
   R300_OPERATION_ID_STENCIL_INVERT_NOT = 50,
   R300_OPERATION_ID_UNARY_AFFINE_MAP = 51,
   R300_OPERATION_ID_UNARY_TRANSCENDENTAL_MAP = 52,
   R300_OPERATION_ID_BINARY_TRANSCENDENTAL_MAP = 53,
   R300_OPERATION_ID_BITWISE_LOGICOP_MAP = 54,
   R300_OPERATION_ID_COUNT = 55,
};

/* Stable implementation and GPU-route identities live beside the operation
 * vocabulary.  They do not imply route state; the compute-verb ledger owns
 * readiness. */
enum r300_operation_implementation_id {
   R300_OPERATION_IMPLEMENTATION_NONE = 0,
   R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER = 1,
   R300_OPERATION_IMPLEMENTATION_COUNT = 2,
};

enum r300_gpu_route_contract_id {
   R300_GPU_ROUTE_CONTRACT_NONE = 0,
   R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER = 1,
   R300_GPU_ROUTE_CONTRACT_COUNT = 2,
};

enum r300_route_admission_id {
   R300_ROUTE_ADMISSION_NONE = 0,
   R300_ROUTE_ADMISSION_R2VB_FP24_IDENTITY = 1,
   R300_ROUTE_ADMISSION_COUNT = 2,
};

/* Virtual operation descriptor: one row per named virtual op in the
 * RS482 compute-as-raster substrate catalog.  Each op lives in a specific
 * numeric domain, has a named theorem, and carries a compact catalog
 * evidence/status summary consulted by the interim cross-ledger validator.
 * implementation_label is preserved provenance prose, not a live symbol;
 * typed implementation IDs own executable bindings. */
struct r300_virtual_op_info {
   enum r300_operation_id      operation_id;
   const char                 *op_name;          /* e.g. "DP4_UINT7_EXACT" */
   enum r300_numeric_domain    domain;
   enum r300_vop_status        status;
   const char                 *theorem;          /* bound proof or NULL */
   const char                 *implementation_label;
};

/* The known virtual op catalog.  Terminated by a row with op_name == NULL.
 * Use r300_virtual_op_count() for the element count. */
extern const struct r300_virtual_op_info r300_virtual_op_catalog[];

/* Number of entries in r300_virtual_op_catalog[] (excludes the NULL sentinel). */
unsigned r300_virtual_op_count(void);

/* Resolve a stable operation identity.  NONE, values outside the enum, and a
 * catalog whose row no longer round-trips its ID fail closed with NULL. */
const struct r300_virtual_op_info *
r300_virtual_op_info_for_id(enum r300_operation_id operation_id);

#ifdef __cplusplus
}
#endif

#endif /* R300_NUMERIC_DOMAIN_H */
