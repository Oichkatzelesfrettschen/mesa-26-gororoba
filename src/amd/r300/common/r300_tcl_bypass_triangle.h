/*
 * SPDX-License-Identifier: MIT
 *
 * Fixed TCL-bypass triangle cell: the first native hardware witness.
 */

#ifndef R300_TCL_BYPASS_TRIANGLE_H
#define R300_TCL_BYPASS_TRIANGLE_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

struct r300_fragment_binary;
struct r300_first_draw_contract;

/* BO slots the cell references; the transport binds slot order to the
 * relocation-list order at submission.
 */
enum r300_tcl_bypass_triangle_slot {
   R300_TRIANGLE_SLOT_VERTEX = 0,
   R300_TRIANGLE_SLOT_COLOR = 1,
   /* The sampled cell's texture BO: the TX_OFFSET_0 payload, which the
    * kernel biases by the BO base under RADEON_CS_KEEP_TILING_FLAGS
    * keeping the register's low five bits (r300_packet0_check, radeon
    * r300.c).
    */
   R300_TRIANGLE_SLOT_TEXTURE = 2,
   R300_TRIANGLE_SLOT_COUNT = 3,
};

/* The unsampled cells reference the vertex and color slots alone, so
 * their relocation lists carry exactly two entries; the sampled cell
 * adds the texture slot.
 */
#define R300_TRIANGLE_RENDER_SLOT_COUNT 2u
#define R300_TRIANGLE_SAMPLED_SLOT_COUNT 3u

struct r300_tcl_bypass_triangle_params {
   /* Byte offset of the first vertex inside the vertex BO. */
   uint32_t vertex_offset;
   /* RB3D_COLOROFFSET0 payload: the byte offset inside the color BO
    * where render row 0 starts.  r300_packet0_check (radeon, r300.c)
    * writes ib[idx] = idx_value + reloc->gpu_offset, so the payload
    * travels as the offset the kernel biases by the BO base, and
    * r100_cs_track_check validates offset + pitch * cpp * maxy against
    * the BO size.  The offset carries no alignment mask through that
    * path -- R300_COLOROFFSET_MASK (0xffffffe0, r300_reg.h) names the
    * register's reserved low five bits -- so the admission below is
    * what keeps an unaligned base off the hardware.
    */
   uint32_t color_offset;
   /* RB3D_COLORPITCH0 value: pitch in pixels plus format and endian
    * fields, chosen by the caller from the color BO's layout.
    */
   uint32_t color_pitch_format;
   const struct r300_fragment_binary *fragment_binary;
   /* When set, the emission opens with the neutral first-draw state
    * contract, so the stream establishes every register the draw depends
    * on and renders independent of predecessor context.  A NULL contract
    * emits the bare cell, which establishes VTE coordinate mode, the color
    * write mask, and the three-vertex index bound while retaining the
    * remaining inherited state; the poison-model calibration in
    * r300_first_draw_state_test consumes that form as its known-bad input.
    */
   const struct r300_first_draw_contract *first_draw_contract;
   /* When set, each vertex record carries a second FLOAT_4 behind the
    * position: the TEX0 varying the VAP declares as a four-component
    * output, RS_IP_0 / RS_INST_0 route to US input 0, and the fragment
    * binary reads (r300_tcl_bypass_triangle_varying_fs).  The fetch is
    * eight dwords per vertex at a 32-byte stride.
    */
   bool varying;
   /* When set, the cell samples texture unit 0: the varying vertex path
    * carries the TEX0 coordinate, the TX block programs one enabled
    * unit -- nearest filters, clamp-to-edge wraps, W8Z8Y8X8 texels over
    * the pitch-addressed linear layout -- and TX_OFFSET_0's payload
    * takes texture_offset with the texture BO's reloc.  The kernel
    * tracker computes the texture footprint from these registers and
    * validates it against the BO (r100_cs_track_texture_check, radeon
    * r100.c), so pitch and height describe the real allocation.
    * Requires varying and a fragment binary whose US program fetches
    * unit 0 (r300_tcl_bypass_triangle_sampled_fs).
    */
   bool sampled;
   /* Byte offset of texel row 0 inside the texture BO; the register's
    * low five bits are reserved, so the offset is 32-byte aligned.
    */
   uint32_t texture_offset;
   /* Texel extent and row pitch of the linear W8Z8Y8X8 texture. */
   uint32_t texture_width;
   uint32_t texture_height;
   uint32_t texture_pitch_texels;
   /* The triangles the consumer draws from the record stream: one
    * vertex-list draw of 3 * triangle_count vertices over records
    * 0 .. 3 * triangle_count - 1, the host expansion of an instanced
    * draw (instance i's three records at 3i).  Zero and one both emit
    * the reference single triangle, so every consumer that leaves the
    * field unset keeps its qualified bytes; the ceiling is
    * R300_TRIANGLE_MAX_TRIANGLES.
    */
   uint32_t triangle_count;
};

/* The vertex-list draw names its count in VAP_VF_CNTL's 16-bit
 * NUM_VERTICES field and the contract's VAP_VF_MAX_VTX_INDX is a 16-bit
 * index, so 3 * triangle_count - 1 <= 0xffff.
 */
#define R300_TRIANGLE_MAX_TRIANGLES 21845u

/* One IB position whose payload names a relocation slot. */
struct r300_tcl_bypass_triangle_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

/* The emitter references each slot exactly once, so the site array holds one
 * entry per slot.  The site validator proves slot uniqueness in one uint32_t
 * of slot bits, so the slot space stays inside 32.
 */
#define R300_TRIANGLE_MAX_RELOC_SITES R300_TRIANGLE_SLOT_COUNT
static_assert(R300_TRIANGLE_SLOT_COUNT <= 32,
              "slot uniqueness is proven in a 32-bit mask");

struct r300_tcl_bypass_triangle_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_tcl_bypass_triangle_reloc_site
      reloc_sites[R300_TRIANGLE_MAX_RELOC_SITES];
   uint32_t reloc_site_count;
   /* Set when the emission allocated ib, so the release frees what it owns
    * and leaves caller storage alone.
    */
   bool owns_ib;
};

/* Emits the complete fixed cell: the first-draw contract prefix when the
 * params carry one, then TCL bypass, one FLOAT_4 position stream
 * with identity PSC selectors, VAP_VTX_SIZE = 4, position-only VAP output,
 * the owned fragment binary verbatim, one color target, depth disabled,
 * destination-cache publication, one vertex-list triangle draw.  Returns 0
 * or a negative errno; the caller owns the returned IB allocation.
 */
int r300_tcl_bypass_triangle_emit(
   const struct r300_tcl_bypass_triangle_params *params,
   struct r300_tcl_bypass_triangle_ib *out);

/* Emits into caller storage of exactly capacity dwords.  The write refuses
 * with -ENOSPC the moment an operation would pass that bound, leaving the
 * words already placed intact and reporting no dword count, so a destination
 * too small for the cell yields a refusal rather than a short stream.
 */
int r300_tcl_bypass_triangle_emit_into(
   const struct r300_tcl_bypass_triangle_params *params, uint32_t *words,
   uint32_t capacity, struct r300_tcl_bypass_triangle_ib *out);

void r300_tcl_bypass_triangle_release(struct r300_tcl_bypass_triangle_ib *ib);

/* Checks the emitted relocation sites against the stream they index: one site
 * per slot in stream order, each inside the stream, and each naming the slot
 * whose payload sits at that index.  Returns 0 or a negative errno.
 */
int r300_tcl_bypass_triangle_validate_reloc_sites(
   const struct r300_tcl_bypass_triangle_ib *ib);

/* Builds the cell's fragment binary from the compiled constant-color US
 * block (r300_tcl_bypass_triangle_fs_block.h, baked by
 * r300_tcl_bypass_fs_tool from the classic compiler ladder).  Every
 * pre-hardware consumer -- manifest tool, native recorder, harness --
 * takes the block from here so their IBs stay byte-identical.  Returns 0
 * or a negative errno; the caller owns the binary.
 */
int r300_tcl_bypass_triangle_reference_fs(struct r300_fragment_binary *fs);

/* Builds the sampled cell's fragment binary: the sampled-texture US
 * block compiled by r300_tcl_bypass_fs_tool, fetching texture unit 0 at
 * the TEX0 coordinate.
 */
int r300_tcl_bypass_triangle_sampled_fs(struct r300_fragment_binary *fs);

/* Builds the varying cell's fragment binary: the varying-passthrough US
 * block that moves interpolator 0 to the color output.  Returns 0 or a
 * negative errno; the caller owns the binary.
 */
int r300_tcl_bypass_triangle_varying_fs(struct r300_fragment_binary *fs);

/* Resolves the first-draw contract for the cell's 64x64 target and three
 * vertices with the texture block disabled.  Every pre-hardware consumer
 * -- manifest tool, native recorder, harness reference -- takes the
 * contract from here so their contract-prefixed cells stay byte-identical.
 * Returns
 * 0 or a negative errno.
 */
int r300_tcl_bypass_triangle_reference_contract(
   struct r300_first_draw_contract *out);

/* Emits the complete reference cell -- reference fragment binary,
 * reference first-draw contract, vertex offset zero, linear 64-pixel
 * B8G8R8A8 pitch -- so every fixed-cell authority (native recorder,
 * arming runner, manifest tool, harness reference) produces one
 * byte-identical IB from one construction.  Returns 0 or a negative
 * errno; the caller owns the returned IB allocation.
 */
int r300_tcl_bypass_triangle_reference_emit(
   struct r300_tcl_bypass_triangle_ib *out);

/* Emits the cell for a target extent inside the published maximum.
 * r300_first_draw_contract_resolve confines the extent to its
 * GEOMETRY_PARAMETER entries, so the extent reaches the hardware
 * through two register words alone -- the SC_SCISSORS_BR and
 * SC_CLIPRECT_BR_0 payloads, biased by R300_SCISSORS_OFFSET /
 * R300_CLIPRECT_OFFSET (1440 in both axes on non-R500 silicon, per
 * r300_reg.h) -- while RB3D_COLORPITCH0 keeps the 64-pixel word for
 * every admitted extent: COLORPITCH.COLORPITCH holds the pitch in
 * 2-pixel units (AMD R3xx 3D Registers, RB3D_COLORPITCH bits 13:1),
 * pitch is a memory-layout property, and the scissor bounds the raster
 * inside each fixed 256-byte row.  At the maximum extent the emission
 * is byte-identical to the reference cell, so the qualified digest
 * anchors the family; every other extent differs in the two
 * scissor-family dwords alone, the invariant the cell test pins.
 * Returns -EINVAL for an extent outside 1..64 on either axis.
 */
int r300_tcl_bypass_triangle_extent_emit(
   uint32_t width, uint32_t height,
   struct r300_tcl_bypass_triangle_ib *out);

/* The varying cell at an extent inside the published maximum, and its
 * reference form at the maximum: the reference contract and pitch over
 * position-plus-varying records with the pass-through fragment binary.
 * The family relates to the position-only cell as its extent family
 * does: every extent differs from the varying reference in the two
 * scissor-family dwords alone.
 */
int r300_tcl_bypass_triangle_varying_extent_emit(
   uint32_t width, uint32_t height,
   struct r300_tcl_bypass_triangle_ib *out);

int r300_tcl_bypass_triangle_varying_reference_emit(
   struct r300_tcl_bypass_triangle_ib *out);

/* The cell family over every admitted parameter: the extent, the record
 * shape (position-only or position-plus-varying), and the triangle
 * count.  triangle_count 1 at the maximum extent is the reference cell
 * of the record shape; a count T differs from it in the contract's
 * VAP_VF_MAX_VTX_INDX payload (3T - 1) and the draw packet's
 * NUM_VERTICES field (3T), the two dwords the host expansion of an
 * instanced draw moves.  Returns -EINVAL for an extent outside 1..64 on
 * either axis or a count outside 1..R300_TRIANGLE_MAX_TRIANGLES.
 */
int r300_tcl_bypass_triangle_family_emit(
   uint32_t width, uint32_t height, bool varying, uint32_t triangle_count,
   struct r300_tcl_bypass_triangle_ib *out);

/* The cell's render geometry.  The manifest publishes these and the contract
 * resolution derives scissor, clip, and pitch from them, so one change moves
 * every consumer together.  The allocation carries one row past the render
 * extent, which the output oracle reads as its canary.
 */
#define R300_TRIANGLE_TARGET_WIDTH 64u
#define R300_TRIANGLE_TARGET_HEIGHT 64u
#define R300_TRIANGLE_TARGET_PITCH_PIXELS 64u
/* One row past the render extent, the row the output oracle reads as its
 * canary.
 */
#define R300_TRIANGLE_CANARY_ROWS 1u
#define R300_TRIANGLE_ALLOCATION_ROWS \
   (R300_TRIANGLE_TARGET_HEIGHT + R300_TRIANGLE_CANARY_ROWS)
#define R300_TRIANGLE_COLOR_BYTES \
   (R300_TRIANGLE_TARGET_PITCH_PIXELS * R300_TRIANGLE_ALLOCATION_ROWS * 4u)

/* The canonical IB artifact encoding is little-endian uint32_t dwords: dword
 * i occupies bytes [4i, 4i+4) as (byte0 = dword & 0xff, byte1 = dword >> 8,
 * byte2 = dword >> 16, byte3 = dword >> 24).  ib.bin, the BLAKE3 digest, the
 * manifest, and cross-host identity all resolve against this encoding, so a
 * host's native uint32_t layout never enters the artifact.
 */
void r300_triangle_ib_serialize(const uint32_t *dwords, uint32_t count,
                                uint8_t *out);

/* BLAKE3 over the canonical encoding above.  Every authority that names a
 * digest -- staging manifest, arming runner, native recorder, queue
 * pre-submit recomputation -- takes it from here, so a digest disagreement is
 * a stream disagreement rather than two hashes of different byte ranges or
 * two hosts' memory layouts.
 */
#define R300_TRIANGLE_DIGEST_SIZE 32
void r300_triangle_ib_digest(const uint32_t *ib, uint32_t ib_size_dwords,
                             uint8_t out[R300_TRIANGLE_DIGEST_SIZE]);

/* The same digest as a NUL-terminated lowercase hex string, the form the
 * arming gate compares.
 */
void r300_triangle_ib_digest_hex(const uint32_t *ib, uint32_t ib_size_dwords,
                                 char out[2 * R300_TRIANGLE_DIGEST_SIZE + 1]);

/* The dword index of the draw packet's header.  The draw is the cell's last
 * packet before the cache publication, so a replay reporting a verdict at a
 * packet index names it against this.
 */
uint32_t r300_triangle_draw_dword(
   const struct r300_tcl_bypass_triangle_ib *ib);

/* Packs RB3D_COLORPITCH0 for the cell's linear little-endian B8G8R8A8 target
 * the way r300_texture.c derives surf->pitch: the pitch in pixels ORed with
 * the register's ARGB8888 color-format field, tiling and endian fields zero.
 * Returns 0 when the pitch is odd or exceeds the register's pitch field.
 */
uint32_t r300_rb3d_colorpitch0_pack_argb8888(uint32_t pitch_pixels);

/* The compiled fragment program writes (0.125, 0.375, 0.625, 0.875).
 * The Vulkan B8G8R8A8_UNORM target stores those normalized components as
 * little-endian bytes [B, G, R, A], so the output dword is 0xdf20609f and
 * red/blue or any other lane swap changes the oracle verdict.
 */
#define R300_TRIANGLE_DRAW_COLOR_RED 0x20u
#define R300_TRIANGLE_DRAW_COLOR_GREEN 0x60u
#define R300_TRIANGLE_DRAW_COLOR_BLUE 0x9fu
#define R300_TRIANGLE_DRAW_COLOR_ALPHA 0xdfu
#define R300_TRIANGLE_DRAW_COLOR_B8G8R8A8 \
   (R300_TRIANGLE_DRAW_COLOR_BLUE | \
    (R300_TRIANGLE_DRAW_COLOR_GREEN << 8) | \
    (R300_TRIANGLE_DRAW_COLOR_RED << 16) | \
    (R300_TRIANGLE_DRAW_COLOR_ALPHA << 24))

/* Deterministic pre-draw fill for the color target; it differs from the
 * draw color in every byte lane, so any device write is detectable.
 */
#define R300_TRIANGLE_COLOR_SENTINEL 0xa5a5a5a5u

/* Output-oracle verdict over a sentinel-initialized 64-pixel-pitch
 * B8G8R8A8 target.  executed reports any deviation from the sentinel;
 * interior demands the draw color at margin-checked sample points
 * inside the analytic triangle; exterior demands the sentinel at
 * in-extent points outside it; canary demands the sentinel in the
 * sub-pitch padding band of every rendered row and in every row past
 * the render extent.  The sample counts report how many analytic
 * candidates carried at least the fill-rule margin: a pass with zero
 * samples cannot exist, because each pass verdict requires its count
 * positive, so an extent too small to witness fails closed.
 */
struct r300_triangle_oracle_verdict {
   bool executed;
   bool interior_pass;
   bool exterior_pass;
   bool canary_pass;
   uint32_t interior_samples;
   uint32_t exterior_samples;
   /* The varying oracle's largest per-channel byte distance between an
    * interior sample and its interpolated expectation; the constant-color
    * oracle compares exactly and reports zero. */
   uint32_t interior_max_deviation;
};

void r300_tcl_bypass_triangle_oracle(
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_oracle_verdict *verdict);

/* The extent-parameterized oracle: the analytic triangle is the fixed
 * NDC reference payload through the viewport transform at this extent,
 * so the verdict matches what the admitted public draw renders there.
 * pixels carries the fixed 64-pixel row pitch whatever the extent --
 * the indexing and the padding-band canary read
 * R300_TRIANGLE_TARGET_PITCH_PIXELS columns per row -- and an extent
 * outside the emitter's admitted domain fails every pass with zero
 * samples.  size_bytes must cover the full retained footprint,
 * pitch * (height + 1) pixels; a shorter buffer carries no observable
 * canary band and fails every pass with zero samples.
 */
void r300_tcl_bypass_triangle_extent_oracle(
   uint32_t width, uint32_t height, const uint32_t *pixels,
   uint32_t size_bytes, struct r300_triangle_oracle_verdict *verdict);

/* The varying oracle: the interior expectation is the barycentric
 * interpolation, at each sample's pixel center, of the three vertex
 * colors (RGBA per vertex in vertex order, the analytic window-space
 * triangle through the viewport transform at the extent), each channel
 * converted to 8 bits by rounding; an interior sample passes when every
 * channel lies within R300_TRIANGLE_VARYING_ORACLE_TOLERANCE bytes of
 * that expectation, the band the RS interpolator's FP24 arithmetic and
 * the UNORM conversion stay inside.  Exterior, canary, and extent rules
 * are the constant-color oracle's.
 */
#define R300_TRIANGLE_VARYING_ORACLE_TOLERANCE 2u
void r300_tcl_bypass_triangle_varying_extent_oracle(
   uint32_t width, uint32_t height, const float vertex_colors[12],
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_oracle_verdict *verdict);

/* The render-shape family: the four target parameters a Vulkan render
 * pass places on the qualified cell, each moving a named register class
 * and nothing else.  The extent moves the two scissor-family payloads
 * the first-draw contract resolves (r300_tcl_bypass_triangle_extent_emit
 * documents the words); the pitch moves the RB3D_COLORPITCH0 payload
 * alone, since pitch is a memory-layout property; the lane order moves
 * the contract's US_OUT_FMT_0 payload alone, the four C*_SEL fields
 * that place shader components into target bytes; the fragment constant
 * moves the four R300_PFS_PARAM_0 payloads of the fragment block alone,
 * the FP24 words the constant-color program reads.  The reference shape
 * emits byte-identical to r300_tcl_bypass_triangle_reference_emit, so
 * the qualified digest anchors the family, and the render-shape test
 * pins the per-parameter dword deltas.
 */
#define R300_TRIANGLE_VERTEX_DWORDS 12

enum r300_triangle_lane_order {
   /* Target bytes [B, G, R, A]: VK_FORMAT_B8G8R8A8_UNORM. */
   R300_TRIANGLE_LANES_B8G8R8A8 = 0,
   /* Target bytes [R, G, B, A]: VK_FORMAT_R8G8B8A8_UNORM. */
   R300_TRIANGLE_LANES_R8G8B8A8 = 1,
};

/* Extent and pitch ceiling of the family.  RB3D_COLORPITCH0 admits a
 * pitch to 8190 pixels and the first-draw contract an extent to
 * R300_FDS_MAX_EXTENT; the family's ceiling is the largest target a
 * dEQP render case binds against this route, and every emitted word
 * inside it is pinned by the same delta test, so the ceiling rises only
 * with the retained silicon receipt for the larger footprint.
 */
#define R300_TRIANGLE_RENDER_MAX_EXTENT 256u

struct r300_triangle_render_shape {
   /* Render extent in pixels, 1..R300_TRIANGLE_RENDER_MAX_EXTENT. */
   uint32_t width;
   uint32_t height;
   /* Row pitch in pixels: >= width, <= the extent ceiling, and a multiple
    * of 8, the linear 32-bpp width alignment r300g's surface layout
    * emits (r300_get_pixel_alignment, DIM_WIDTH, src/gallium/drivers/
    * r300/r300_texture_desc.c).
    */
   uint32_t pitch_pixels;
   enum r300_triangle_lane_order lanes;
   /* The fragment constant as four IEEE-754 binary32 bit patterns,
    * RGBA, each on the FP24 lattice (r300_fp24_quantize_bits leaves it
    * unchanged), so the register word is the value the oracle predicts.
    */
   uint32_t color_bits[4];
   /* Pinned to 1: r300_tcl_bypass_triangle_render_shape_vertices writes
    * one triangle's three records regardless of this field, so a value
    * other than 1 would claim a draw the vertex writer never produces.
    */
   uint32_t triangle_count;
   /* Byte offset inside the color BO where render row 0 starts: the
    * RB3D_COLOROFFSET0 payload, a multiple of
    * R300_TRIANGLE_TARGET_OFFSET_ALIGNMENT inside
    * R300_TRIANGLE_MAX_TARGET_OFFSET.  The reference shape carries 0,
    * so a target bound at the allocation base emits the reference
    * bytes.
    */
   uint32_t target_offset;
};

/* RB3D_COLOROFFSET holds the base in its bits 31:5
 * (R300_COLOROFFSET_MASK = 0xffffffe0, r300_reg.h), so a base carrying
 * any of the reserved low five bits names an address the register
 * cannot encode.  The kernel's packet check adds the relocation base
 * without masking, so the driver's admission is the one gate against
 * that address reaching the hardware.
 */
#define R300_TRIANGLE_TARGET_OFFSET_ALIGNMENT 32u
/* The offset ceiling keeps offset plus the largest admitted footprint
 * -- R300_TRIANGLE_RENDER_MAX_EXTENT pitch over one row past the
 * maximum extent -- inside 32 bits, so
 * r300_tcl_bypass_triangle_render_shape_color_bytes returns an exact
 * uint32_t sum for every admitted shape.
 */
#define R300_TRIANGLE_MAX_TARGET_OFFSET (1u << 24)

/* The reference shape: 64x64 at pitch 64, B8G8R8A8 lanes, the
 * byte-order oracle constant (0.125, 0.375, 0.625, 0.875), one triangle.
 */
void r300_tcl_bypass_triangle_render_shape_reference(
   struct r300_triangle_render_shape *out);

/* 0 for an admitted shape, -EINVAL otherwise. */
int r300_tcl_bypass_triangle_render_shape_validate(
   const struct r300_triangle_render_shape *shape);

/* The reference fragment block with its R300_PFS_PARAM_0 payloads
 * replaced by the shape's constant in the register's FP24 encoding.
 * The caller finishes the binary.
 */
int r300_tcl_bypass_triangle_render_shape_fs(
   const struct r300_triangle_render_shape *shape,
   struct r300_fragment_binary *fs);

int r300_tcl_bypass_triangle_render_shape_emit(
   const struct r300_triangle_render_shape *shape,
   struct r300_tcl_bypass_triangle_ib *out);

/* The pretransformed vertex payload for the shape's extent: the NDC
 * reference triangle through the viewport transform, z = 0, w = 1.
 */
void r300_tcl_bypass_triangle_render_shape_vertices(
   const struct r300_triangle_render_shape *shape,
   float out[R300_TRIANGLE_VERTEX_DWORDS]);

/* The color BO footprint the oracle reads: the target offset plus
 * pitch * (height + 1) pixels, the canary row included.  This sizes the
 * allocation the shape needs; the offset-relative rendered footprint,
 * the quantity a bind admission compares against the remaining bytes of
 * a suballocation, is this value less the offset.
 */
uint32_t r300_tcl_bypass_triangle_render_shape_color_bytes(
   const struct r300_triangle_render_shape *shape);

/* The target dword a normalized RGBA quadruple stores: each channel
 * clamped to [0, 1], rounded to its UNORM8 byte, and placed by the lane
 * order.  The color buffer applies this conversion to the shaded value,
 * and a Vulkan clear color reaches the same bytes through it -- a
 * VkClearColorValue's live member follows the format's numeric type, so
 * a UNORM target reads float32 and the conversion is the whole
 * translation.  A NaN channel stores zero, the value the clamp of an
 * unordered comparison leaves.
 */
uint32_t r300_tcl_bypass_triangle_pack_unorm8_dword(
   enum r300_triangle_lane_order lanes, const float rgba[4]);

/* The dword an interior pixel holds: the shape's constant through the
 * conversion above.
 */
uint32_t r300_tcl_bypass_triangle_render_shape_draw_dword(
   const struct r300_triangle_render_shape *shape);

/* The constant-color oracle over the shape's pitch, extent, and draw
 * dword; every rule of r300_tcl_bypass_triangle_extent_oracle holds with
 * the shape's pitch in place of the fixed 64.  pixels addresses the
 * whole color BO and render row 0 sits at the shape's target offset, so
 * the canary additionally demands the sentinel in every dword below
 * that offset: a device write under the rendered rows is as observable
 * as one past them.
 */
void r300_tcl_bypass_triangle_render_shape_oracle(
   const struct r300_triangle_render_shape *shape, const uint32_t *pixels,
   uint32_t size_bytes, struct r300_triangle_oracle_verdict *verdict);

/* The pretransformed screen-space triangle for a 64x64 color target: three
 * FLOAT_4 positions, sixteen bytes each, the payload of the cell's vertex
 * BO.
 */
extern const float
   r300_tcl_bypass_triangle_vertices[R300_TRIANGLE_VERTEX_DWORDS];

/* The reference varying payload: the same three positions, each followed
 * by the color the reference varying vertex program computes from the
 * clip-space triangle (tint = fma(position, (0.5, 0.5, 0, 0),
 * (0.5, 0.5, 0.25, 1))), so the carrier the CPU route writes for that
 * program over the NDC reference triangle is this array byte for byte.
 * The colors alone, in vertex order, are the varying oracle's vertex
 * colors.
 */
#define R300_TRIANGLE_VARYING_VERTEX_DWORDS 24
extern const float r300_tcl_bypass_triangle_varying_vertices
   [R300_TRIANGLE_VARYING_VERTEX_DWORDS];
extern const float r300_tcl_bypass_triangle_varying_colors[12];

#endif /* R300_TCL_BYPASS_TRIANGLE_H */
