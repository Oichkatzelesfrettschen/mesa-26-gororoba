/*
 * SPDX-License-Identifier: MIT
 *
 * Carrier policy for the RS482 compute-as-raster dispatch-replay.
 *
 * A carrier policy describes the buffer format and stride contract for a
 * particular virtual op lowering: what the input SSBO is typed as (value
 * format), what the output SSBO carrier is (bit format), and what the exact
 * result range guarantee is.
 *
 * The DP4 path established the canonical form: R32G32B32A32_FLOAT input
 * (stride 16) -> FP24 DP4 ALU -> RGBA8 integer-encoded uint output (stride 4,
 * max exact result 64516 for U7-magnitude operands).  RS482 has no FP32 render
 * target (hardware-confirmed: FP32 color FBO is incomplete, 0x8cdd;
 * EXT_color_buffer_float is absent), so float-domain results that need exact
 * byte readback must be encoded through a byte carrier such as RGBA8.
 *
 * The carrier policy provides a single definition shared by the orchestrator
 * (r3v_compute.c dispatch-replay) and the readback logic so both sides
 * agree on how to pack and unpack results.
 */

#ifndef R300_CARRIER_POLICY_H
#define R300_CARRIER_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "amd/r300/common/r300_numeric_domain.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Encoding strategy for the output carrier.  Determines how the virtual op's
 * result is packed into the render-target and unpacked on readback. */
enum r300_carrier_encoding {
   /* RGBA8 byte-level integer encoding.  A uint result <= 16777215 (2^24-1)
    * packs into the R, G, B, A bytes little-endian:
    *   R = (result      ) & 0xff
    *   G = (result >>  8) & 0xff
    *   B = (result >> 16) & 0xff
    *   A = (result >> 24) & 0xff
    * The DP4 path uses this encoding; byte values are exact because the
    * FP24 exact-integer window guarantees integer precision up to 2^17 for
    * U7-magnitude operands. */
   R300_CARRIER_ENC_RGBA8_UINT,

   /* Identity: the output format matches the input format; no re-encoding.
    * Used for identity-map and binary-map patterns where the carry type is
    * the same across the input and output buffers. */
   R300_CARRIER_ENC_IDENTITY,

   /* Packed UNORM8: normalized float in [0, 1] per channel packed into RGBA8.
    * Used for color and blend-accumulation patterns where the per-channel
    * value is a fraction, not an integer. */
   R300_CARRIER_ENC_RGBA8_UNORM,

   /* Single uint32 counter: a 1-element buffer holds the integer fragment
    * count.  Used for ZPASS reduction.  The counter value is read back as
    * a u64 from the occlusion-query result and written to the output SSBO. */
   R300_CARRIER_ENC_UINT32_COUNTER,

   /* U16 byte-level integer encoding. A uint16 result packed into R, G bytes.
    * B, A channels are unused or metadata. */
   R300_CARRIER_ENC_RGBA8_U16,

   /* U24 byte-level integer encoding. A uint24 result packed into R, G, B bytes.
    * A channel is unused or metadata. */
   R300_CARRIER_ENC_RGBA8_U24,

   /* Raw FP16 bitwise storage packed into RGBA8: raw bits in R/G, B/A carry class/flags. */
   R300_CARRIER_ENC_FP16_RAWBITS_RGBA8,
};

/* API-neutral format identity for carrier policy.  Consumer adapters map
 * these identities to their own format vocabulary; the policy itself carries
 * only the channel layout and numeric representation that the R300 mechanism
 * requires. */
enum r300_carrier_format {
   R300_CARRIER_FORMAT_INVALID = 0,
   R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
   R300_CARRIER_FORMAT_R32_FLOAT,
   R300_CARRIER_FORMAT_R32G32_FLOAT,
   R300_CARRIER_FORMAT_R32G32B32A32_FLOAT,
   R300_CARRIER_FORMAT_COUNT,
};

/* Full carrier policy for one virtual op family.  One instance per admitted
 * virtual op.  The orchestrator resolves the concrete API format and stride
 * from the bound descriptor sets at dispatch time; these fields define the
 * canonical contract so the orchestrator and readback logic agree. */
struct r300_carrier_policy {
   const char                 *name;              /* stable diagnostic label */
   enum r300_numeric_domain    domain;
   enum r300_carrier_encoding  encoding;
   enum r300_carrier_format    value_format;      /* SSBO input texture format */
   enum r300_carrier_format    bit_format;        /* RT output carrier format */
   unsigned                    input_stride;      /* bytes per element in the input buffer */
   unsigned                    output_stride;     /* bytes per element in the output buffer */
   unsigned                    max_exact_result;  /* max integer result with exact encoding; 0 = N/A */
   bool                        encodes_full_uint32; /* true when the A channel carries bits 24-31 */
   bool                        requires_fp32_rt;  /* true = not viable on RS482 */
};

/* Canonical carrier policy instances.
 *
 * r300_carrier_identity: identity-map (out[gid] = in[gid]).
 *   Input and output share the format; the orchestrator determines
 *   the actual format from the descriptor binding at dispatch time.
 *
 * r300_carrier_dp4_u7: DP4 with U7-magnitude operands (exact).
 *   Input: R32G32B32A32_FLOAT, stride 16 (one vec4 operand per element).
 *   Output: R8G8B8A8_UNORM, stride 4 (RGBA8 uint-encoded result).
 *   Max exact result: 64516 (4*(2^7-1)^2, hardware-confirmed).
 *   Uses RGBA8_UINT encoding; the A channel carries bits 24-31 when the
 *   result exceeds 2^16, but the U7-exact domain caps at 64516 < 2^17,
 *   so the A byte is always zero for admitted exact-domain results.
 *
 * r300_carrier_dp4_u8_boundary: DP4 with U8 operands (precision boundary).
 *   Same formats as dp4_u7.  Results inside the FP24 exact-integer window
 *   (<= 2^17) are exact; larger U8 dot values are the hardware-confirmed
 *   boundary behavior (4*(2^8-1)^2 = 260100 > 2^17).
 *
 * r300_carrier_blend_acc: blend-add accumulation (histogram).
 *   Input: element-typed SSBO; format from descriptor.
 *   Output: 1xN RB3D RT, RGBA8_UNORM for per-bin normalized float sums.
 *
 * r300_carrier_zpass: ZPASS fragment-count reduction.
 *   Input: predicate SSBO (R8G8B8A8_UNORM or element format from descriptor).
 *   Output: single uint32 counter (UINT32_COUNTER encoding via occlusion query). */
extern const struct r300_carrier_policy r300_carrier_identity;
extern const struct r300_carrier_policy r300_carrier_dp4_u7;
extern const struct r300_carrier_policy r300_carrier_dp4_u8_boundary;
extern const struct r300_carrier_policy r300_carrier_blend_acc;
extern const struct r300_carrier_policy r300_carrier_zpass;
extern const struct r300_carrier_policy r300_carrier_ieee16_classify;
extern const struct r300_carrier_policy r300_carrier_ieee16_mul;
extern const struct r300_carrier_policy r300_carrier_ieee16_result;
extern const struct r300_carrier_policy r300_carrier_ieee16_debug;

/* Select the appropriate unsigned DP4 carrier policy given the maximum operand
 * magnitude.  Returns r300_carrier_dp4_u7 (exact) when max_operand_magnitude
 * <= 127, r300_carrier_dp4_u8_boundary (precision-boundary) for 128..255, and
 * NULL outside the unsigned-byte carrier domain.
 * The operand magnitude is not visible at NIR classify time; callers that know
 * the runtime bound use this helper to select the right policy before dispatch.
 */
const struct r300_carrier_policy *
r300_carrier_dp4_select(unsigned max_operand_magnitude);

#ifdef __cplusplus
}
#endif

#endif /* R300_CARRIER_POLICY_H */
