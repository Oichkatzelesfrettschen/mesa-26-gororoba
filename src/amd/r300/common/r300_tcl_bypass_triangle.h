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
struct r300_flat_color0_plan;

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
   /* The composed cell's second pass: its own vertex records and its own
    * color target.  The first pass's color slot and the texture slot
    * resolve to one buffer object at submission, under a write domain
    * and a read domain; the kernel validates each use site, so the two
    * stay separate slots.
    */
   R300_TRIANGLE_SLOT_COMPOSED_VERTEX = 3,
   R300_TRIANGLE_SLOT_COMPOSED_COLOR = 4,
   R300_TRIANGLE_SLOT_COUNT = 5,
};

/* The unsampled cells reference the vertex and color slots alone, so
 * their relocation lists carry exactly two entries; the sampled cell
 * adds the texture slot.
 */
#define R300_TRIANGLE_RENDER_SLOT_COUNT 2u
#define R300_TRIANGLE_SAMPLED_SLOT_COUNT 3u
#define R300_TRIANGLE_COMPOSED_SLOT_COUNT 5u

enum r300_triangle_lane_order {
   /* Target bytes [B, G, R, A]: VK_FORMAT_B8G8R8A8_UNORM. */
   R300_TRIANGLE_LANES_B8G8R8A8 = 0,
   /* Target bytes [R, G, B, A]: VK_FORMAT_R8G8B8A8_UNORM. */
   R300_TRIANGLE_LANES_R8G8B8A8 = 1,
};

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
   /* When set (requires varying, a first-draw contract, and no texture
    * sampling), the varying rides the TCL-bypass color 0 vector instead
    * of TEX0: the PSC lands the record's second FLOAT_4 in vector 2,
    * VAP_OUTPUT_VTX_FMT_0 declares COLOR_0_PRESENT, and the GA/RS
    * interpolation state comes from the contract the plan was applied
    * to (r300_flat_color0_plan_apply_contract), so the cell writes no
    * RS block of its own.  The fragment binary stays the pass-through:
    * RS_INST_0 delivers color 0 into the US input TEX0 filled.
    */
   const struct r300_flat_color0_plan *flat_color0;
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
   /* Memory lane order of the 32-bpp texels; FORMAT1's per-channel
    * selects route the W8Z8Y8X8 word's X/Y/Z/W bytes to shader R/G/B/A,
    * one select set per order.
    */
   enum r300_triangle_lane_order texture_lanes;
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

/* Homogeneous clipping can turn one input triangle into at most seven
 * output triangles.  The fixed-capacity vertex allocation reserves every
 * slot, while the command stream presents the allocation to hardware as at
 * most seven draws whose local vertex counts each fit the 16-bit fields.
 */
#define R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT 7u
#define R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES \
   (R300_TRIANGLE_MAX_TRIANGLES * \
    R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT)
#define R300_TRIANGLE_CLIP_MAX_DRAW_SEGMENTS 7u
static_assert(R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES == 152915u,
              "clip output capacity is exact");
static_assert(R300_TRIANGLE_CLIP_MAX_DRAW_SEGMENTS ==
                 R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT,
              "one full draw segment exists per input expansion bound");

/* One IB position whose payload names a relocation slot. */
struct r300_tcl_bypass_triangle_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

/* The five-site cells reference each slot exactly once, and the site
 * validator proves that uniqueness in one uint32_t of slot bits, so the
 * slot space stays inside 32.  The cleared multisample cell adds a clear
 * half whose two sites reuse the color and cover-vertex slots, since the
 * kernel resolves every relocation NOP naming one index to the same
 * entry; its seven-site sequence is matched in full instead.
 */
#define R300_TRIANGLE_MSAA_CLEAR_SITE_COUNT 7u
#define R300_TRIANGLE_CLIP_RENDER_SITE_COUNT \
   (1u + R300_TRIANGLE_CLIP_MAX_DRAW_SEGMENTS)
#define R300_TRIANGLE_CLIP_SAMPLED_SITE_COUNT \
   (2u + R300_TRIANGLE_CLIP_MAX_DRAW_SEGMENTS)
#define R300_TRIANGLE_MAX_RELOC_SITES R300_TRIANGLE_CLIP_SAMPLED_SITE_COUNT
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
 * per buffer reference in stream order, each inside the stream, and each
 * naming the slot whose payload sits at that index.  Expanded streams repeat
 * the vertex slot once per draw segment.  Returns 0 or a negative errno.
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

/* Emits the sampled cell at the reference target pitch: the varying
 * vertex path carries the TEX0 coordinate, the sampled fragment binary
 * fetches TX unit 0, and the TX block programs the declared linear
 * W8Z8Y8X8 texture with its offset on the texture relocation slot.
 */
int r300_tcl_bypass_triangle_sampled_emit(
   uint32_t width, uint32_t height, uint32_t triangle_count,
   uint32_t texture_offset, uint32_t texture_width,
   uint32_t texture_height, uint32_t texture_pitch_texels,
   enum r300_triangle_lane_order texture_lanes,
   struct r300_tcl_bypass_triangle_ib *out);

/* The sampled clip-space capacity form reserves seven output triangles for
 * every source triangle.  Its vertex stream is emitted as one to seven
 * ordered hardware draws, each with its own vertex relocation.
 */
int r300_tcl_bypass_triangle_clip_space_sampled_emit(
   uint32_t width, uint32_t height, uint32_t source_triangle_count,
   uint32_t texture_offset, uint32_t texture_width,
   uint32_t texture_height, uint32_t texture_pitch_texels,
   enum r300_triangle_lane_order texture_lanes,
   struct r300_tcl_bypass_triangle_ib *out);

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

/* The extent/record-shape clip-space capacity form.  source_triangle_count
 * remains bounded by R300_TRIANGLE_MAX_TRIANGLES; the emitted output stream
 * reserves exactly seven whole, ordered triangles per source triangle.
 */
int r300_tcl_bypass_triangle_clip_space_family_emit(
   uint32_t width, uint32_t height, bool varying,
   uint32_t source_triangle_count,
   struct r300_tcl_bypass_triangle_ib *out);

/* The direct Flat cell family: the varying record shape through color
 * 0 under a plan the contract carries.  The family form admits the
 * canonical direct plan alone (r300_flat_color0_plan_validate) and is
 * the route's emitter; the plan form realizes any plan, so a
 * calibration mutation's byte deviation is observable and its refusal
 * by the family form is a separate fact.  clip_space selects the
 * clipper's output triangle count from a source count, as the varying
 * clip-space family does.
 */
int r300_tcl_bypass_triangle_flat_color0_family_emit(
   uint32_t width, uint32_t height, bool clip_space,
   uint32_t triangle_count, const struct r300_flat_color0_plan *plan,
   struct r300_tcl_bypass_triangle_ib *out);
int r300_tcl_bypass_triangle_flat_color0_plan_emit(
   uint32_t width, uint32_t height, bool clip_space,
   uint32_t triangle_count, const struct r300_flat_color0_plan *plan,
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
   /* The producer admitted the call and classified the footprint.  A
    * refused call leaves this false with every counter zero, which a
    * judged total mismatch would otherwise be indistinguishable from.
    */
   bool judged;
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
   /* When set, the pass carries the TEX0 varying record shape -- eight
    * dwords per vertex, RS_IP_0 / RS_INST_0 routing, the pass-through
    * fragment binary -- and emits through the cell family at the
    * reference target rather than the constant-color render-shape
    * emitter; color_bits is then unused.  The two-pass emitter reads
    * it per pass.
    */
   bool varying;
   /* When set with varying, the pass carries the varying through the
    * color 0 vector under the canonical direct Flat plan
    * (r300_flat_color0_plan_direct_first): hardware provoking-vertex
    * selection rather than host replication.
    */
   bool flat_color0;
};

/* The composed render-then-sample cell: one stream renders the first
 * shape, publishes its color writes through the destination-cache
 * flush every cell closes with, then invalidates the texture tags and
 * samples that target into the second shape.  The first target is both
 * the render half's color slot and the sample half's texture slot, so
 * the submission binds one buffer object under two use sites, which is
 * what the kernel validates.  The texture geometry comes from the
 * render shape alone -- its extent, row pitch, lane order, and target
 * offset are the texture's -- so the two halves cannot disagree about
 * the bytes between them.
 */
struct r300_triangle_composed_render_sample {
   struct r300_triangle_render_shape render;
   struct r300_triangle_render_shape sample;
};

/* Emits the composed cell.  Returns 0 or a negative errno; the caller
 * owns the returned IB allocation.
 */
int r300_tcl_bypass_triangle_composed_render_sample_emit(
   const struct r300_triangle_composed_render_sample *composed,
   struct r300_tcl_bypass_triangle_ib *out);

/* Binds the cell's relocation payloads to a submission's own relocation
 * indices.  The emitter writes each payload as its slot number, which
 * holds while every slot names a distinct buffer object, since the
 * winsys then assigns indices in slot order.  A cell naming one buffer
 * object under two use sites -- the composed cell's first target, which
 * its render half writes and its sample half reads -- meets a winsys
 * that merges duplicate handles into one relocation entry
 * (radeon_drm_vk_reloc_list_add) exactly as the kernel does, so the
 * indices past the first duplicate shift down and the payloads stop
 * naming the buffers they were emitted for.  slot_indices[slot] carries
 * the merged index for each slot the cell references; the payloads then
 * name the merged chunk, and a second merge over the same list is
 * idempotent.  Returns 0 or a negative errno, and validates the sites
 * against the emitted form first, so binding twice refuses.
 */
/* The relocation index each composed slot resolves to once the winsys
 * has merged the shared first target into one entry: the map a recorder
 * reproduces from its own reference array, and the map an offline
 * emitter binds with to reach the digest the recorded cell carries.
 */
extern const uint32_t
   r300_tcl_bypass_triangle_composed_slot_index[R300_TRIANGLE_SLOT_COUNT];

/* The multisample resolve cell: one stream renders the reference
 * triangle into a sample-expanded color surface with GB_AA_CONFIG's
 * subsample set live, then binds that same surface as the render target
 * a second time with RB3D_AARESOLVE_CTL in resolve mode and covers the
 * whole extent, which sends the downsampled samples to
 * RB3D_AARESOLVE_OFFSET.  The multisample surface is written and never
 * read by the host (r300_texture_initial_domain places an
 * nr_samples > 1 resource in RADEON_DOMAIN_VRAM alone); the resolve
 * destination is the buffer the oracle reads.
 *
 * The resolve half's fragment constant is a color no multisample sample
 * holds, so the destination separates the two readings of the resolve
 * semantics in one submission: downsampled samples give the render
 * half's color, a fragment write that reaches the destination gives
 * this constant, and a destination holding neither over judged pixels
 * is the mixture the third counter reports.
 */
/* The subsample state a half of the multisample cell executes under.
 * The first-draw contract carries GB_AA_CONFIG, both GB_MSPOS words, and
 * RB3D_AARESOLVE_CTL, so this declaration reaches the stream through the
 * contract rather than ahead of it.
 */
struct r300_triangle_multisample_state {
   /* 2 or 4: the subsample sets GB_MSPOS0 and GB_MSPOS1 carry. */
   uint32_t sample_count;
   /* This half runs under AARESOLVE_MODE_RESOLVE, so the color backend
    * sends its downsampled output to RB3D_AARESOLVE_OFFSET.
    */
   bool resolve;
};

struct r300_triangle_msaa_resolve {
   /* The multisample color surface.  Its pitch is the single-sample
    * pitch; the sample count multiplies the allocation's layer size and
    * leaves the stride alone (r300_texture_desc.c).
    */
   struct r300_triangle_render_shape render;
   /* The resolve destination.  RB3D_AARESOLVE_PITCH carries a raw pixel
    * pitch in bits 1 through 13, the same field position
    * RB3D_COLORPITCH0 gives its stride, and RB3D_AARESOLVE_OFFSET
    * carries a 32-byte-aligned base in bits 31:5.  The shape's geometry
    * is what the cell emits; its color_bits carry the oracle's
    * expectation, so the destination admits on geometry alone.
    */
   struct r300_triangle_render_shape destination;
   /* 2 or 4: the subsample sets GB_MSPOS0 and GB_MSPOS1 carry. */
   uint32_t sample_count;
   /* The resolve half's fragment constant, RGBA binary32 bit patterns on
    * the FP24 lattice, distinct from the render half's color.
    */
   uint32_t resolve_color_bits[4];
   /* A clear half runs ahead of the render half: the cover triangle
    * drawn under the same subsample set with clear_color_bits as its
    * fragment constant, so every sample of the multisample surface
    * holds a known color before the triangle lands.  R300 carries no
    * fast clear (r300g clears through a draw as well), so the cover
    * draw is the clear.  The resolve then carries the clear color to
    * every fully exterior destination pixel, which the exterior oracle
    * judges.
    */
   bool clear;
   uint32_t clear_color_bits[4];
};

/* Emits the multisample resolve cell.  Returns 0 or a negative errno;
 * the caller owns the returned IB allocation.
 */
int r300_tcl_bypass_triangle_msaa_resolve_emit(
   const struct r300_triangle_msaa_resolve *msaa,
   struct r300_tcl_bypass_triangle_ib *out);

/* The relocation index each slot of the multisample cell resolves to
 * once the winsys has merged the multisample surface's two use sites --
 * the render half's color target and the resolve half's -- into one
 * entry.
 */
extern const uint32_t
   r300_tcl_bypass_triangle_msaa_slot_index[R300_TRIANGLE_SLOT_COUNT];

/* The resolve half's vertices: one triangle at (0, 0), (2w, 0), (0, 2h)
 * whose interior covers the whole extent, so the scissor bounds the
 * coverage to the target and every pixel reaches the color backend.  A
 * resolve emits only for the pixels a fragment covers, so full coverage
 * is what sends the whole surface to the resolve destination.
 */
void r300_tcl_bypass_triangle_cover_vertices(
   const struct r300_triangle_render_shape *shape,
   float out[R300_TRIANGLE_VERTEX_DWORDS]);

/* The GB_AA_CONFIG word a sample count names, or 0 for a count with no
 * subsample set.
 */
uint32_t r300_tcl_bypass_triangle_gb_aa_config(uint32_t sample_count);

/* The GB_MSPOS0 (index 0) or GB_MSPOS1 (index 1) word for a sample
 * count, packed as r300_get_mspos packs it: (x, y) nibble pairs followed
 * by the minimum subpixel distance from the pixel edge, with distance 8
 * encoded as 7.
 */
uint32_t r300_tcl_bypass_triangle_gb_mspos(uint32_t index,
                                           uint32_t sample_count);

int r300_tcl_bypass_triangle_bind_reloc_indices(
   struct r300_tcl_bypass_triangle_ib *ib, const uint32_t *slot_indices,
   uint32_t slot_index_count);

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

/* The geometry alone: extent, pitch, lane order, triangle count, and
 * the target base.  A verdict producer that takes its interior values
 * as arguments admits on this, because a cell whose fragment color
 * arrives through the TX unit carries no R300_PFS_PARAM_0 constant.
 * 0 for admitted geometry, -EINVAL otherwise.
 */
int r300_tcl_bypass_triangle_render_shape_validate_geometry(
   const struct r300_triangle_render_shape *shape);

/* The geometry and the fragment constant together, for an emitter that
 * writes color_bits into R300_PFS_PARAM_0 or an oracle that derives its
 * expectation from them.  0 for an admitted shape, -EINVAL otherwise.
 */
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

/* The arbitrary render-shape clip-space capacity form.  The shape continues
 * to describe one record shape; source_triangle_count sizes the separate
 * fixed-capacity vertex stream at seven output triangles per input.
 */
int r300_tcl_bypass_triangle_clip_space_render_shape_emit(
   const struct r300_triangle_render_shape *shape,
   uint32_t source_triangle_count,
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

/* The exact-coverage verdict: every dword of the rendered footprint is
 * classified against the analytic triangle, so an overdraw, an
 * underdraw, and a fill-rule deviation each appear as a nonzero
 * mismatch count rather than escaping between sample points.  A pixel
 * center is interior when the three edge functions share a sign;
 * centers that land exactly on an edge are ambiguous, counted, and
 * refuse the verdict, so an extent whose geometry sits on the tie-break
 * takes the sampled oracle above instead.
 */
struct r300_triangle_coverage_verdict {
   /* The producer admitted the call and classified the footprint.  A
    * refused call leaves this false with every counter zero, which a
    * judged total mismatch would otherwise be indistinguishable from.
    */
   bool judged;
   /* Every dword classified and the interior set equal to the analytic
    * one: mismatch and ambiguous both zero, interior equal to analytic.
    */
   bool coverage_exact;
   /* The canary rows past the render extent, and any dwords below the
    * target offset, hold the exterior dword.
    */
   bool canary_pass;
   uint32_t interior_pixels;
   uint32_t exterior_pixels;
   uint32_t analytic_pixels;
   /* Pixel centers on an edge, excluded from the analytic set. */
   uint32_t ambiguous_pixels;
   /* Dwords inside the extent equal to neither expectation. */
   uint32_t mismatch_pixels;
};

/* The predicted dword at one interior pixel center, for a fragment
 * source that varies across the triangle: the caller's own model of
 * what the shader delivers there.
 */
typedef uint32_t (*r300_triangle_interior_expectation)(void *data, uint32_t x,
                                                       uint32_t y);

/* Judges pixels over the shape's footprint.  interior_dwords carries the
 * admitted interior values -- one for a constant-color draw, several
 * when the fragment source varies over the triangle -- so the verdict
 * proves the drawn region's shape and its exterior, and a caller that
 * admits several values keeps their placement to its own check.  A
 * caller that models the variation supplies expectation instead, and
 * each interior pixel is then judged against the dword the model
 * predicts at that center, so placement joins the verdict; the admitted
 * set is unread in that form.
 */
void r300_tcl_bypass_triangle_coverage_oracle_predicted(
   const struct r300_triangle_render_shape *shape,
   const uint32_t *interior_dwords, uint32_t interior_dword_count,
   r300_triangle_interior_expectation expectation, void *expectation_data,
   uint32_t exterior_dword, const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_coverage_verdict *verdict);

/* The admitted-set form: the predicted form with no model. */
void r300_tcl_bypass_triangle_coverage_oracle(
   const struct r300_triangle_render_shape *shape,
   const uint32_t *interior_dwords, uint32_t interior_dword_count,
   uint32_t exterior_dword, const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_coverage_verdict *verdict);

/* The expected target of a draw whose every interior pixel carries one
 * dword: the analytic interior at interior_dword, every other dword of
 * the footprint -- exterior, canary rows, and the bytes below the
 * target offset -- at exterior_dword, and each pixel center exactly on
 * an edge at exterior_dword as well.  The buffer is the shape's full
 * footprint: target_offset plus pitch * (height + canary rows) dwords.
 * Returns 0 or -EINVAL for an inadmissible shape or a short buffer.
 */
int r300_tcl_bypass_triangle_expected_target(
   const struct r300_triangle_render_shape *shape, uint32_t interior_dword,
   uint32_t exterior_dword, uint32_t *pixels, uint32_t size_bytes);

/* Byte comparison of two targets over the footprint, skipping the
 * pixel centers exactly on an edge, whose coverage the fill rule
 * decides.  Returns the number of differing dwords, or a negative errno
 * for an inadmissible shape or a short buffer; judged reports whether
 * the comparison ran.
 */
int r300_tcl_bypass_triangle_target_compare(
   const struct r300_triangle_render_shape *shape, const uint32_t *expected,
   const uint32_t *observed, uint32_t size_bytes, bool *judged);

/* The analytic interior alone, for a target whose exterior carries no
 * predicted value: a render whose load op is
 * VK_ATTACHMENT_LOAD_OP_DONT_CARE, or a resolve destination the device
 * writes only where the resolving draw covers.  The verdict reads the
 * centers the geometry covers and leaves the exterior, the pitch
 * padding, and the canary row unjudged, so it proves the drawn region
 * received the admitted values and carries no claim about the bytes
 * around it -- the sentinel corner the coverage verdict provides is
 * the price.  The denominator is the pixel center, so a multisampled
 * or resolved target answers to its subsample positions instead and
 * takes an inset region rather than this one.  analytic_pixels is the
 * denominator, and a refused call
 * reports zero of it with interior_exact false, so an inadmissible
 * shape or a short buffer reads as a refusal rather than a pass.
 */
struct r300_triangle_interior_verdict {
   /* The producer admitted the call and classified the footprint.  A
    * refused call leaves this false with every counter zero, which a
    * judged total mismatch would otherwise be indistinguishable from.
    */
   bool judged;
   bool interior_exact;
   uint32_t analytic_pixels;
   uint32_t interior_pixels;
   uint32_t ambiguous_pixels;
};

void r300_tcl_bypass_triangle_interior_oracle(
   const struct r300_triangle_render_shape *shape,
   const uint32_t *interior_dwords, uint32_t interior_dword_count,
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_interior_verdict *verdict);

/* The two-pass cell: two render-shape cells concatenated the way a
 * command buffer that records two render passes with a draw each is
 * installed.  The first cell's relocation payloads name its own slots,
 * which are the first two merged indices; the second cell's payloads
 * are bound to the merged positions the winsys rule assigns them --
 * first-add order, one entry per handle -- so the emitted stream is the
 * recorded stream dword for dword and its digest is the one the arming
 * gate compares.  Each cell opens with its own first-draw contract and
 * closes with the RB3D_DSTCACHE_CTLSTAT flush, so no state crosses the
 * pass boundary.
 *
 * The binding admits a second pass that shares the first pass's vertex
 * page (index 0) or color target (index 1) or brings its own (the next
 * unused index, vertex before color).  A vertex page that is a color
 * target, a color target that is a vertex page, or a second color that
 * is the second vertex aliases two roles over one buffer object, which
 * the render cell's ownership of its target excludes; an index that
 * skips a position names an entry the merge never creates.
 */
struct r300_triangle_multi_pass {
   struct r300_triangle_render_shape pass[2];
   /* Merged relocation index of the second pass's vertex page: 0 (the
    * first pass's page) or 2 (its own).
    */
   uint32_t second_vertex_index;
   /* Merged relocation index of the second pass's color target: 1 (the
    * first pass's target), or the next unused index, 2 when the vertex
    * page is shared and 3 when it is not.
    */
   uint32_t second_color_index;
};

/* One color and one vertex site per pass. */
#define R300_TRIANGLE_MULTI_PASS_SITE_COUNT 4u

/* Returns 0 when the binding is one the merge produces, else -EINVAL. */
int r300_tcl_bypass_triangle_multi_pass_binding_validate(
   const struct r300_triangle_multi_pass *mp);

/* The merged reference count the binding implies: 2, 3, or 4. */
uint32_t r300_tcl_bypass_triangle_multi_pass_reference_count(
   const struct r300_triangle_multi_pass *mp);

/* Emits the two-pass stream in its bound form.  Returns 0 or a negative
 * errno; the caller owns the returned IB.  The result binds no further:
 * its second-pass payloads already name merged indices, so
 * r300_tcl_bypass_triangle_bind_reloc_indices refuses it.
 */
int r300_tcl_bypass_triangle_multi_pass_emit(
   const struct r300_triangle_multi_pass *mp,
   struct r300_tcl_bypass_triangle_ib *out);

/* Emits the same bound two-pass stream with seven clip-capacity triangle
 * slots per source triangle in each pass.
 */
int r300_tcl_bypass_triangle_clip_space_multi_pass_emit(
   const struct r300_triangle_multi_pass *mp,
   struct r300_tcl_bypass_triangle_ib *out);

/* The subsample positions the multisample modes place, in the 1/12
 * subpixel grid GB_TILE_CONFIG.SUBPIXEL selects, as the (x, y) pairs
 * GB_MSPOS0 and GB_MSPOS1 carry in nibble lanes.  Sample count 1 puts
 * its one sample at the pixel center (6, 6); 2 and 4 take the diagonal
 * sets r300g programs.
 */
#define R300_TRIANGLE_SUBPIXEL_GRID 12u
#define R300_TRIANGLE_MAX_SUBSAMPLES 4u

uint32_t r300_tcl_bypass_triangle_subsample_positions(
   uint32_t sample_count,
   uint8_t positions[R300_TRIANGLE_MAX_SUBSAMPLES][2]);

/* The interior verdict over a multisampled or resolved target.  A pixel
 * is judged only when every subsample clears the analytic edges by
 * R300_TRIANGLE_SAMPLE_MARGIN pixels, so the verdict rides neither the
 * resolve's blend nor the hardware's fill rule: an edge-adjacent pixel
 * whose samples straddle an edge carries a blend no admitted dword
 * names, and a subsample landing exactly on an edge -- which the 4x
 * grid does, its thirds meeting the slope -2 edge from (56, 8) to
 * (32, 56) at 64 sample positions -- has no defined side.  Both stay
 * unjudged and counted.  The judged footprint is a strict subset of the
 * pixel-center footprint r300_tcl_bypass_triangle_interior_oracle
 * takes: at the reference geometry it holds 1152 pixels at one sample,
 * 1128 at two, and 1104 at four, against that oracle's 1152.
 */
struct r300_triangle_sample_set_verdict {
   /* The producer admitted the call and classified the footprint.  A
    * refused call leaves this false with every counter zero, which a
    * judged total mismatch would otherwise be indistinguishable from.
    */
   bool judged;
   bool interior_exact;
   uint32_t analytic_pixels;
   uint32_t interior_pixels;
   uint32_t unjudged_pixels;
};

void r300_tcl_bypass_triangle_sample_set_oracle(
   const struct r300_triangle_render_shape *shape, uint32_t sample_count,
   const uint32_t *interior_dwords, uint32_t interior_dword_count,
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_sample_set_verdict *verdict);

/* The exterior verdict over a resolved target whose multisample surface
 * was cleared: a pixel is judged when every subsample clears the
 * analytic edges outward by R300_TRIANGLE_SAMPLE_MARGIN, so its resolve
 * reads the clear color alone.  The same edge band stays unjudged, and
 * the verdict's counters keep their names: analytic_pixels is the fully
 * exterior denominator inside the extent, interior_pixels the count that
 * holds an admitted dword, interior_exact their equality.  At the
 * reference geometry the denominator is 2944 at one sample, 2920 at
 * two, and 2896 at four, which with the interior and unjudged counts
 * partitions the 4096-pixel extent.
 */
void r300_tcl_bypass_triangle_sample_set_exterior_oracle(
   const struct r300_triangle_render_shape *shape, uint32_t sample_count,
   const uint32_t *exterior_dwords, uint32_t exterior_dword_count,
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_sample_set_verdict *verdict);

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

/* The varying record payload at the shape's own extent: the reference
 * TEX0 coordinates, which are normalized and so carry across extents,
 * behind positions the viewport transform places at this width and
 * height.  The array above is this writer's output at the reference
 * extent, so a caller filling a sample half at another extent reaches
 * the coordinates its texture fetch reads rather than the reference
 * window's.
 */
void r300_tcl_bypass_triangle_varying_shape_vertices(
   const struct r300_triangle_render_shape *shape,
   float out[R300_TRIANGLE_VARYING_VERTEX_DWORDS]);
extern const float r300_tcl_bypass_triangle_varying_colors[12];

#endif /* R300_TCL_BYPASS_TRIANGLE_H */
