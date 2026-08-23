/*
 * SPDX-License-Identifier: MIT
 *
 * Carrier-policy contracts for candidate RS482 compute-as-raster paths.
 *
 * A carrier policy describes a possible buffer format and stride contract:
 * what an input SSBO would be typed as (value format), what an output SSBO
 * carrier would use (bit format), and what exact result range is represented.
 *
 * The DP4 path established the canonical form: R32G32B32A32_FLOAT input
 * (stride 16) -> FP24 DP4 ALU -> RGBA8 integer-encoded uint output (stride 4,
 * max exact result 64516 for U7-magnitude operands).  RS482 has no FP32 render
 * target (hardware-confirmed: FP32 color FBO is incomplete, 0x8cdd;
 * EXT_color_buffer_float is absent), so float-domain results that need exact
 * byte readback must be encoded through a byte carrier such as RGBA8.
 *
 * These compiled objects are contract inventory.  No current runtime route
 * selects them merely because they are registered here.
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
   /* RGBA8 byte-level integer encoding.  The packer emits the low three bytes
    * and optionally the high byte in little-endian order:
    *   R = (result      ) & 0xff
    *   G = (result >>  8) & 0xff
    *   B = (result >> 16) & 0xff
    *   A = (result >> 24) & 0xff
    * Exactness depends on the numeric domain that produces the value; the U7
    * DP4 contract, for example, remains inside the FP24 exact-integer window. */
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

/* One candidate carrier contract.  Operations may share a policy or have no
 * policy, and a policy object does not certify a live route. */
struct r300_carrier_policy {
   const char                 *name;              /* stable diagnostic label */
   enum r300_numeric_domain    domain;
   enum r300_carrier_encoding  encoding;
   enum r300_carrier_format    value_format;      /* SSBO input texture format */
   enum r300_carrier_format    bit_format;        /* RT output carrier format */
   unsigned                    input_stride;      /* bytes per element in the input buffer */
   unsigned                    output_stride;     /* bytes per element in the output buffer */
   enum r300_bound_kind        max_exact_result_kind;
   uint64_t                    max_exact_result;
   bool                        pack_alpha_byte;   /* A carries bits 24-31 */
   bool                        requires_fp32_rt;  /* true = not viable on RS482 */
};

/* Compiled carrier-policy objects.
 *
 * r300_carrier_identity: identity-map (out[gid] = in[gid]).
 *   This candidate uses RGBA8 input and output with identical strides.
 *
 * r300_carrier_dp4_u7: DP4 with U7-magnitude operands (exact).
 *   Input: R32G32B32A32_FLOAT, stride 16 (one vec4 operand per element).
 *   Output: R8G8B8A8_UNORM, stride 4 (RGBA8 uint-encoded result).
 *   Max exact result: 64516 (4*(2^7-1)^2, hardware-confirmed).
 *   Uses RGBA8_UINT encoding; the A channel carries bits 24-31 when the
 *   result reaches 2^24, but the U7-exact domain caps at 64516 < 2^17,
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
 * The operand magnitude is not visible at NIR classify time; a selector that
 * knows the runtime bound can use this helper without implying route liveness.
 */
const struct r300_carrier_policy *
r300_carrier_dp4_select(unsigned max_operand_magnitude);

#ifdef __cplusplus
}
#endif

#endif /* R300_CARRIER_POLICY_H */
