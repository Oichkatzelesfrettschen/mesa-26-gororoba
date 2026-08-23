/* SPDX-License-Identifier: MIT */

#include "r300_tcl_bypass_triangle.h"
#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_pm4_builder.h"
#include "r300_r2vb_producer_fs_block.h"
#include "r300_tcl_bypass_triangle_fs_block.h"

#include "r300_reg.h"
#include "util/macros.h"
#include "util/mesa-blake3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* Four dwords per drm_radeon_cs_reloc entry, so a slot's payload indexes the
 * relocation chunk at four times the slot.
 */
#define R300_TRIANGLE_RELOC_PAYLOAD(slot) ((slot) * 4)

/* Identity PSC swizzle select: X, Y, Z, W in place with a full write mask,
 * the exact per-word value the kernel's TCL-bypass vertex-output check
 * requires on every VAP_PROG_STREAM_CNTL_EXT word.
 */
#define R300_TRIANGLE_PSC_EXT_IDENTITY 0xF688F688u

/* The varying record's second element lands in VAP vector 6, the
 * texture-coordinate-0 vector of the TCL-bypass output layout (position
 * 0, point size 1, colors 2-5, texture coordinates from 6; r300g
 * r300_stream_locations_notcl), the vector RS_IP_0's texture pointer 0
 * reads.
 */
#define R300_TRIANGLE_VARYING_DST_VEC 6u

/* Holds the bare-cell state plus the two-dword-per-clause first-draw
 * contract prefix; the prefix emission checks its room against this bound
 * and the allocation adds the fragment binary's size on top.
 */
#define R300_TRIANGLE_MAX_DWORDS 512

/* Records one relocation site as the builder places it.  A slot outside the
 * cell's slot space, or a site past the site array, refuses the emission
 * rather than storing a site no relocation list can resolve.
 */
static void
write_reloc(struct r300_pm4_builder *b, struct r300_tcl_bypass_triangle_ib *out,
            uint32_t slot)
{
   if (b->error != 0)
      return;
   if (slot >= R300_TRIANGLE_SLOT_COUNT ||
       out->reloc_site_count >= R300_TRIANGLE_MAX_RELOC_SITES) {
      b->error = -EINVAL;
      return;
   }

   const uint32_t index =
      r300_pm4_reloc_nop(b, R300_TRIANGLE_RELOC_PAYLOAD(slot));
   if (index == R300_PM4_NO_INDEX)
      return;

   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_tcl_bypass_triangle_reloc_site){
         .ib_index = index,
         .slot = slot,
      };
}

int
r300_tcl_bypass_triangle_emit_into(
   const struct r300_tcl_bypass_triangle_params *params, uint32_t *words,
   uint32_t capacity, struct r300_tcl_bypass_triangle_ib *out)
{
   const struct r300_fragment_binary *fs = params->fragment_binary;

   memset(out, 0, sizeof(*out));
   if (fs == NULL || !fs->validated) {
      return -EINVAL;
   }

   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, words, capacity);

   /* First-draw state prefix: the contract's writes land before any cell
    * state, so every register the draw depends on -- the three proven
    * color-write gates included -- comes from this stream rather than
    * from the previous client.  The builder reserves the emitter's declared
    * extent and admits exactly that many dwords, so the count <= capacity
    * invariant stays the builder's alone.
    */
   if (params->first_draw_contract != NULL && b.error == 0) {
      const uint32_t state_dwords =
         r300_first_draw_state_dwords(params->first_draw_contract);
      if (r300_pm4_builder_reserve(&b, state_dwords)) {
         const int emitted = r300_first_draw_state_emit(
            params->first_draw_contract, &b.words[b.count], state_dwords);
         if (emitted < 0)
            b.error = emitted;
         else if ((uint32_t)emitted != state_dwords)
            b.error = -EINVAL;
         else
            b.count += state_dwords;
      }
   }

   /* The bare cell has no first-draw prefix, so establish the three state
    * values that control the fixed window-space triangle before its draw:
    * VTE selects pretransformed XY/Z, the MRT0 channel mask enables every
    * color lane, and the vertex-list bound admits all three vertices. The
    * r300g producer writes resolve with `(rg --fixed-strings
    * "R300_VAP_VTE_CNTL" src/gallium/drivers/r300)` and
    * `(rg --fixed-strings "R300_VAP_VF_MAX_VTX_INDX"
    * src/gallium/drivers/r300)`, while the register definitions resolve with
    * `(rg --fixed-strings "RB3D_COLOR_CHANNEL_MASK"
    * src/amd/r300/common/r300_reg.h)`. */
   if (params->first_draw_contract == NULL && b.error == 0) {
      r300_pm4_reg(&b, R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);
      r300_pm4_reg(&b, RB3D_COLOR_CHANNEL_MASK,
                   RB3D_COLOR_CHANNEL_MASK_BLUE_MASK0 |
                      RB3D_COLOR_CHANNEL_MASK_GREEN_MASK0 |
                      RB3D_COLOR_CHANNEL_MASK_RED_MASK0 |
                      RB3D_COLOR_CHANNEL_MASK_ALPHA_MASK0);
      r300_pm4_emit_vertex_index_range(&b, 0, 2);
   }

   /* Vertex path: pretransformed positions bypass the TCL block, one
    * FLOAT_4 stream lands whole in output vector zero, and every PSC
    * extended selector stays identity, so the kernel's vertex-output check
    * can prove VAP_VTX_SIZE = 4 covers the fetch.  The varying cell adds a
    * second FLOAT_4 element into the texture-coordinate-0 vector, declares
    * it as a four-component TEX0 output, and fetches eight dwords per
    * vertex, the identity-list arithmetic the same check proves.
    */
   const uint32_t record_dwords = params->varying ? 8 : 4;
   r300_pm4_reg(&b, R300_VAP_CNTL_STATUS, R300_VAP_TCL_BYPASS);
   r300_pm4_reg(&b, R300_VAP_PROG_STREAM_CNTL_0,
                params->varying
                   ? (R300_DATA_TYPE_FLOAT_4 |
                      (0 << R300_DST_VEC_LOC_SHIFT)) |
                        ((R300_DATA_TYPE_FLOAT_4 |
                          (R300_TRIANGLE_VARYING_DST_VEC
                           << R300_DST_VEC_LOC_SHIFT) |
                          R300_LAST_VEC)
                         << 16)
                   : R300_DATA_TYPE_FLOAT_4 | (0 << R300_DST_VEC_LOC_SHIFT) |
                        R300_LAST_VEC);
   static const uint32_t psc_ext_identity[8] = {
      R300_TRIANGLE_PSC_EXT_IDENTITY, R300_TRIANGLE_PSC_EXT_IDENTITY,
      R300_TRIANGLE_PSC_EXT_IDENTITY, R300_TRIANGLE_PSC_EXT_IDENTITY,
      R300_TRIANGLE_PSC_EXT_IDENTITY, R300_TRIANGLE_PSC_EXT_IDENTITY,
      R300_TRIANGLE_PSC_EXT_IDENTITY, R300_TRIANGLE_PSC_EXT_IDENTITY,
   };
   r300_pm4_packet0(&b, R300_VAP_PROG_STREAM_CNTL_EXT_0, psc_ext_identity,
                    ARRAY_SIZE(psc_ext_identity));
   r300_pm4_reg(&b, R300_VAP_OUTPUT_VTX_FMT_0,
                R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT);
   r300_pm4_reg(&b, R300_VAP_OUTPUT_VTX_FMT_1,
                params->varying ? R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS
                                : 0);
   r300_pm4_reg(&b, R300_VAP_VTX_SIZE, record_dwords);
   if (params->varying) {
      /* The assembler admits position plus texture coordinate 0, and
       * the RS routes that varying: RS_COUNT declares four interpolated
       * components with no rasterized colors, RS_IP_0 reads texture
       * pointer 0 and selects its four channels in order, RS_INST_0
       * writes the result to US input register 0, and RS_INST_COUNT of
       * zero runs instruction 0 alone.  The first-draw contract wrote
       * the position-only forms of these registers ahead of the cell,
       * so the varying cell establishes its own values here.
       */
      r300_pm4_reg(&b, R300_VAP_VSM_VTX_ASSM,
                   R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0);
      r300_pm4_reg(&b, R300_RS_COUNT,
                   R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN);
      r300_pm4_reg(&b, R300_RS_INST_COUNT, 0);
      r300_pm4_reg(&b, R300_RS_IP_0,
                   R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
                      R300_RS_SEL_T(R300_RS_SEL_C1) |
                      R300_RS_SEL_R(R300_RS_SEL_C2) |
                      R300_RS_SEL_Q(R300_RS_SEL_C3));
      r300_pm4_reg(&b, R300_RS_INST_0,
                   R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
                      R300_RS_INST_TEX_ADDR(0));
   }

   /* Fragment program: the owned binary's US/FG block verbatim, then the
    * two register values the descriptor keeps outside the sequence.
    */
   r300_pm4_block(&b, fs->cb_code, fs->cb_code_size);
   r300_pm4_reg(&b, R300_FG_DEPTH_SRC, fs->fg_depth_src);
   r300_pm4_reg(&b, R300_US_W_FMT, fs->us_out_w);

   /* One color target, depth disabled.  RB3D_COLOROFFSET carries the color
    * BO reference; the pitch/format word travels plain because the
    * submission sets RADEON_CS_KEEP_TILING_FLAGS.
    */
   r300_pm4_reg(&b, R300_RB3D_CCTL, 0);
   r300_pm4_reg(&b, R300_ZB_CNTL, 0);
   r300_pm4_reg(&b, R300_RB3D_COLOROFFSET0, 0);
   write_reloc(&b, out, R300_TRIANGLE_SLOT_COLOR);
   r300_pm4_reg(&b, R300_RB3D_COLORPITCH0, params->color_pitch_format);

   /* Vertex fetch: one array, one record per vertex, stride one record. */
   const uint32_t record_bytes = record_dwords * 4;
   const uint32_t vbpntr[3] = {
      1 | R300_VC_FORCE_PREFETCH,
      R300_VBPNTR_SIZE0(record_bytes) | R300_VBPNTR_STRIDE0(record_bytes),
      params->vertex_offset,
   };
   r300_pm4_packet3(&b, R300_PACKET3_3D_LOAD_VBPNTR, vbpntr,
                    ARRAY_SIZE(vbpntr));
   write_reloc(&b, out, R300_TRIANGLE_SLOT_VERTEX);

   /* One vertex-list triangle; the draw packet carries VAP_VF_CNTL. */
   const uint32_t draw = R300_VAP_VF_CNTL__PRIM_TRIANGLES |
                         R300_PRIM_WALK_LIST |
                         (3 << R300_PRIM_NUM_VERTICES_SHIFT);
   r300_pm4_packet3(&b, R300_PACKET3_3D_DRAW_VBUF_2, &draw, 1);

   /* Destination-cache publication retires the color writes before the IB
    * completes.
    */
   r300_pm4_reg(&b, R300_RB3D_DSTCACHE_CTLSTAT,
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);

   const int rc = r300_pm4_builder_finish(&b, &out->ib_size_dwords);
   if (rc != 0) {
      memset(out, 0, sizeof(*out));
      return rc;
   }
   out->ib = words;
   return 0;
}

int
r300_tcl_bypass_triangle_emit(
   const struct r300_tcl_bypass_triangle_params *params,
   struct r300_tcl_bypass_triangle_ib *out)
{
   const struct r300_fragment_binary *fs = params->fragment_binary;

   memset(out, 0, sizeof(*out));
   if (fs == NULL || !fs->validated) {
      return -EINVAL;
   }

   const uint32_t capacity = R300_TRIANGLE_MAX_DWORDS + fs->cb_code_size;
   uint32_t *ib = calloc(capacity, sizeof(uint32_t));
   if (ib == NULL) {
      return -ENOMEM;
   }

   const int rc = r300_tcl_bypass_triangle_emit_into(params, ib, capacity,
                                                     out);
   if (rc != 0) {
      free(ib);
      return rc;
   }
   out->owns_ib = true;
   return 0;
}

void
r300_tcl_bypass_triangle_release(struct r300_tcl_bypass_triangle_ib *ib)
{
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_tcl_bypass_triangle_validate_reloc_sites(
   const struct r300_tcl_bypass_triangle_ib *ib)
{
   /* The emitter places one site per slot in a fixed order, so the cell's
    * relocation list is fully determined: every slot appears once, each site
    * indexes the payload of a relocation NOP inside the stream, and that
    * payload names the slot.  A relocation list built from sites failing any
    * of these resolves a BO into a position the stream does not reference.
    */
   if (ib->reloc_site_count != R300_TRIANGLE_SLOT_COUNT)
      return -EINVAL;

   /* The uniqueness set below is one uint32_t of slot bits. */
   static_assert(R300_TRIANGLE_SLOT_COUNT <= 32,
                 "slot uniqueness is proven in a 32-bit mask");

   uint32_t seen = 0;
   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_tcl_bypass_triangle_reloc_site *site =
         &ib->reloc_sites[i];
      if (site->slot >= R300_TRIANGLE_SLOT_COUNT)
         return -EINVAL;
      if ((seen & (1u << site->slot)) != 0)
         return -EEXIST;
      seen |= 1u << site->slot;

      /* The site is the payload dword of a relocation NOP, so the header
       * precedes it; an in-range ordinary dword that happens to equal the
       * payload does not qualify.
       */
      if (site->ib_index == 0 || site->ib_index >= ib->ib_size_dwords)
         return -ERANGE;
      if (ib->ib[site->ib_index - 1] != CP_PACKET3(R300_PM4_PACKET3_NOP, 0))
         return -EINVAL;
      if (ib->ib[site->ib_index] != R300_TRIANGLE_RELOC_PAYLOAD(site->slot))
         return -EINVAL;
   }

   /* The relocation list follows the stream: the color target is programmed
    * before the vertex array is bound.  Command-stream order is its own
    * fact, distinct from enum order, so the expected sequence is spelled
    * out rather than derived.
    */
   static const uint32_t expected_slots[] = {
      R300_TRIANGLE_SLOT_COLOR,
      R300_TRIANGLE_SLOT_VERTEX,
   };
   static_assert(ARRAY_SIZE(expected_slots) == R300_TRIANGLE_SLOT_COUNT,
                 "every slot has a place in the stream order");
   for (uint32_t i = 0; i < R300_TRIANGLE_SLOT_COUNT; i++) {
      if (ib->reloc_sites[i].slot != expected_slots[i])
         return -EINVAL;
      if (i > 0 &&
          ib->reloc_sites[i - 1].ib_index >= ib->reloc_sites[i].ib_index)
         return -EINVAL;
   }

   return 0;
}

int
r300_tcl_bypass_triangle_reference_fs(struct r300_fragment_binary *fs)
{
   return r300_fragment_binary_init(
      fs, r300_tcl_bypass_triangle_fs_block,
      sizeof(r300_tcl_bypass_triangle_fs_block) /
         sizeof(r300_tcl_bypass_triangle_fs_block[0]),
      R300_TCL_BYPASS_TRIANGLE_FS_FG_DEPTH_SRC,
      R300_TCL_BYPASS_TRIANGLE_FS_US_OUT_W,
      "r300-tcl-bypass-triangle-compiled");
}

int
r300_tcl_bypass_triangle_varying_fs(struct r300_fragment_binary *fs)
{
   /* The varying-passthrough US program moves interpolator 0 to the
    * color output and carries no output-format dependence -- US_OUT_FMT
    * rides the first-draw contract -- so the block the R2VB producer
    * pass compiled shades this cell's ARGB8888 target unchanged.
    */
   return r300_fragment_binary_init(
      fs, r300_r2vb_producer_fs_block,
      sizeof(r300_r2vb_producer_fs_block) /
         sizeof(r300_r2vb_producer_fs_block[0]),
      R300_R2VB_PRODUCER_FS_FG_DEPTH_SRC, R300_R2VB_PRODUCER_FS_US_OUT_W,
      "r300-varying-passthrough-compiled");
}

/* The cell's target is little-endian B8G8R8A8, so the shader's four
 * 8-bit output components select blue, green, red, and alpha in that
 * order; r300_first_draw_contract_set_us_out_fmt_0 places the word.
 */
static int
r300_tcl_bypass_triangle_set_target_format(
   struct r300_first_draw_contract *contract)
{
   return r300_first_draw_contract_set_us_out_fmt_0(
      contract, R300_US_OUT_FMT_C4_8 | R300_C0_SEL_B | R300_C1_SEL_G |
                   R300_C2_SEL_R | R300_C3_SEL_A);
}

int
r300_tcl_bypass_triangle_reference_contract(
   struct r300_first_draw_contract *out)
{
   /* The draw fetches vertices 0..2 and binds no texture; the contract
    * resolution derives scissor, clip, and the maximum vertex index from
    * the published target geometry.
    */
   struct r300_first_draw_params params = {
      .chip_family = CHIP_RS480,
      .width = R300_TRIANGLE_TARGET_WIDTH,
      .height = R300_TRIANGLE_TARGET_HEIGHT,
      .min_vtx_index = 0,
      .max_vtx_index = 2,
      .texture_enabled = false,
   };
   int rc = r300_first_draw_contract_resolve(&params, out);
   if (rc != 0)
      return rc;
   return r300_tcl_bypass_triangle_set_target_format(out);
}

static int
extent_emit(uint32_t width, uint32_t height, bool varying,
            struct r300_tcl_bypass_triangle_ib *out)
{
   if (width < 1 || width > R300_TRIANGLE_TARGET_WIDTH || height < 1 ||
       height > R300_TRIANGLE_TARGET_HEIGHT)
      return -EINVAL;

   struct r300_fragment_binary fs;
   int rc = varying ? r300_tcl_bypass_triangle_varying_fs(&fs)
                    : r300_tcl_bypass_triangle_reference_fs(&fs);
   if (rc != 0)
      return rc;

   /* The extent parameterizes the contract's GEOMETRY_PARAMETER entries
    * alone; the pitch word stays the reference cell's, so the row
    * layout and every other register class are the qualified bytes.
    */
   struct r300_first_draw_params draw_params = {
      .chip_family = CHIP_RS480,
      .width = width,
      .height = height,
      .min_vtx_index = 0,
      .max_vtx_index = 2,
      .texture_enabled = false,
   };
   struct r300_first_draw_contract contract;
   rc = r300_first_draw_contract_resolve(&draw_params, &contract);
   if (rc != 0) {
      r300_fragment_binary_finish(&fs);
      return rc;
   }
   rc = r300_tcl_bypass_triangle_set_target_format(&contract);
   if (rc != 0) {
      r300_fragment_binary_finish(&fs);
      return rc;
   }

   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_pitch_format =
         r300_rb3d_colorpitch0_pack_argb8888(R300_TRIANGLE_TARGET_PITCH_PIXELS),
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
      .varying = varying,
   };
   rc = r300_tcl_bypass_triangle_emit(&params, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

int
r300_tcl_bypass_triangle_extent_emit(
   uint32_t width, uint32_t height,
   struct r300_tcl_bypass_triangle_ib *out)
{
   return extent_emit(width, height, false, out);
}

int
r300_tcl_bypass_triangle_reference_emit(
   struct r300_tcl_bypass_triangle_ib *out)
{
   return r300_tcl_bypass_triangle_extent_emit(R300_TRIANGLE_TARGET_WIDTH,
                                               R300_TRIANGLE_TARGET_HEIGHT,
                                               out);
}

int
r300_tcl_bypass_triangle_varying_extent_emit(
   uint32_t width, uint32_t height,
   struct r300_tcl_bypass_triangle_ib *out)
{
   return extent_emit(width, height, true, out);
}

int
r300_tcl_bypass_triangle_varying_reference_emit(
   struct r300_tcl_bypass_triangle_ib *out)
{
   return extent_emit(R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
                      true, out);
}

uint32_t
r300_rb3d_colorpitch0_pack_argb8888(uint32_t pitch_pixels)
{
   /* R300_COLORPITCH_MASK covers bits 1-13, so the pitch is even and at
    * most 0x3ffe pixels; the format field is R300_COLOR_FORMAT_ARGB8888,
    * and the linear little-endian target keeps tile, microtile, and
    * endian at zero.
    */
   if (pitch_pixels == 0 || (pitch_pixels & 1) != 0 ||
       pitch_pixels > R300_COLORPITCH_MASK)
      return 0;
   return pitch_pixels | R300_COLOR_FORMAT_ARGB8888;
}

/* Edge function twice the signed area of (a, b, p); the triangle's
 * vertices wind so every interior point yields three positive values.
 */
static float
triangle_edgef(float ax, float ay, float bx, float by, float px, float py)
{
   return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Signed pixel distance from p to the line through (a, b), positive on
 * the triangle's interior side; the margin tests below divide the edge
 * function by the edge length.
 */
static float
triangle_edge_distance(float ax, float ay, float bx, float by, float px,
                       float py)
{
   const float dx = bx - ax, dy = by - ay;
   const float length = sqrtf(dx * dx + dy * dy);
   return triangle_edgef(ax, ay, bx, by, px, py) / length;
}

struct triangle_geometry {
   /* Window-space vertices: the NDC reference payload through the
    * viewport transform at the oracle's extent.
    */
   float v[6];
};

static struct triangle_geometry
triangle_geometry_at(uint32_t width, uint32_t height)
{
   static const float ndc[6] = { -0.75f, -0.75f, 0.75f, -0.75f,
                                 0.0f, 0.75f };
   struct triangle_geometry g;
   for (unsigned i = 0; i < 3; i++) {
      g.v[i * 2 + 0] = (ndc[i * 2 + 0] + 1.0f) * ((float)width / 2.0f);
      g.v[i * 2 + 1] = (ndc[i * 2 + 1] + 1.0f) * ((float)height / 2.0f);
   }
   return g;
}

/* The minimum of the three signed edge distances: positive inside with
 * that margin, negative outside with it.  Sample points require at
 * least a two-pixel magnitude, so the verdict does not ride the
 * hardware's exact fill rule.
 */
static float
triangle_signed_margin(const struct triangle_geometry *g, float px,
                       float py)
{
   float margin = triangle_edge_distance(g->v[0], g->v[1], g->v[2],
                                         g->v[3], px, py);
   const float d1 = triangle_edge_distance(g->v[2], g->v[3], g->v[4],
                                           g->v[5], px, py);
   const float d2 = triangle_edge_distance(g->v[4], g->v[5], g->v[0],
                                           g->v[1], px, py);
   if (d1 < margin)
      margin = d1;
   if (d2 < margin)
      margin = d2;
   return margin;
}

#define R300_TRIANGLE_ORACLE_MARGIN 2.0f

static bool
triangle_pixel_candidate(float candidate_x, float candidate_y,
                         uint32_t width, uint32_t height, uint32_t *x,
                         uint32_t *y)
{
   /* Check the floating-point domain before conversion.  A negative or
    * non-finite coordinate has no defined conversion to uint32_t, and a
    * candidate outside the extent cannot witness either oracle pass.
    */
   if (!isfinite(candidate_x) || !isfinite(candidate_y) ||
       candidate_x < 0.0f || candidate_y < 0.0f ||
       candidate_x >= (float)width || candidate_y >= (float)height)
      return false;

   *x = (uint32_t)candidate_x;
   *y = (uint32_t)candidate_y;
   return *x < width && *y < height;
}

void
r300_tcl_bypass_triangle_extent_oracle(
   uint32_t width, uint32_t height, const uint32_t *pixels,
   uint32_t size_bytes, struct r300_triangle_oracle_verdict *verdict)
{
   /* The verdict producer admits the same domain the emitter admits: an
    * extent outside it fails every pass with zero samples, so an
    * inadmissible call reads as a failed verdict rather than dividing
    * by a zero edge length or wrapping the extent arithmetic.
    */
   if (width < 1 || width > R300_TRIANGLE_TARGET_WIDTH || height < 1 ||
       height > R300_TRIANGLE_TARGET_HEIGHT) {
      *verdict = (struct r300_triangle_oracle_verdict){ 0 };
      return;
   }

   /* The verdict reads the full retained footprint: every rendered row
    * at the fixed pitch plus the canary row past the render extent.  A
    * buffer short of that footprint carries no observable canary band,
    * so the truncated call fails closed before any pass initializes
    * rather than leaving canary_pass vacuously true.
    */
   const uint64_t required_bytes =
      (uint64_t)R300_TRIANGLE_TARGET_PITCH_PIXELS * (height + 1u) *
      sizeof(uint32_t);
   if (pixels == NULL || size_bytes < required_bytes) {
      *verdict = (struct r300_triangle_oracle_verdict){ 0 };
      return;
   }

   const struct triangle_geometry g = triangle_geometry_at(width, height);

   *verdict = (struct r300_triangle_oracle_verdict) {
      .interior_pass = true,
      .exterior_pass = true,
      .canary_pass = true,
   };

   const uint32_t pixel_count = size_bytes / 4;
   for (uint32_t i = 0; i < pixel_count; i++) {
      if (pixels[i] != R300_TRIANGLE_COLOR_SENTINEL) {
         verdict->executed = true;
         break;
      }
   }

   /* Interior candidates: the centroid and its midpoints toward each
    * vertex.  A candidate qualifies with the fill-rule margin inside
    * the triangle and inside the extent; the count is part of the
    * verdict, and zero qualifying candidates fail the pass, so an
    * extent whose triangle is too small to witness refuses instead of
    * passing vacuously.
    */
   const float cx = (g.v[0] + g.v[2] + g.v[4]) / 3.0f;
   const float cy = (g.v[1] + g.v[3] + g.v[5]) / 3.0f;
   const float interior_candidates[8] = {
      cx, cy,
      (cx + g.v[0]) / 2.0f, (cy + g.v[1]) / 2.0f,
      (cx + g.v[2]) / 2.0f, (cy + g.v[3]) / 2.0f,
      (cx + g.v[4]) / 2.0f, (cy + g.v[5]) / 2.0f,
   };
   for (unsigned i = 0; i < 4; i++) {
      uint32_t x, y;
      if (!triangle_pixel_candidate(interior_candidates[i * 2 + 0],
                                     interior_candidates[i * 2 + 1], width,
                                     height, &x, &y) ||
          triangle_signed_margin(&g, (float)x + 0.5f, (float)y + 0.5f) <
             R300_TRIANGLE_ORACLE_MARGIN)
         continue;
      verdict->interior_samples++;
      const uint32_t index = y * R300_TRIANGLE_TARGET_PITCH_PIXELS + x;
      if (index >= pixel_count ||
          pixels[index] != R300_TRIANGLE_DRAW_COLOR_B8G8R8A8)
         verdict->interior_pass = false;
   }
   if (verdict->interior_samples == 0)
      verdict->interior_pass = false;

   /* Exterior candidates: the extent's corners and edge midpoints,
    * qualified by the same margin outside the triangle.  The final four
    * candidates remain inside the triangle's vertex bounding box while
    * sitting outside its sloped edges, so a rectangle fill overdraw is
    * observable even when it leaves the extent boundary untouched.
    */
   const float last_x = (float)(width - 1), last_y = (float)(height - 1);
   const float bbox_mid_y = (g.v[1] + g.v[5]) / 2.0f;
   const float bbox_inset =
      fminf(4.0f, fminf((float)width, (float)height) / 16.0f);
   const float bbox_lower_y = g.v[5] - bbox_inset;
   const float exterior_candidates[24] = {
      0.0f, 0.0f, last_x, 0.0f, 0.0f, last_y, last_x, last_y,
      last_x / 2.0f, 0.0f, last_x / 2.0f, last_y,
      0.0f, last_y / 2.0f, last_x, last_y / 2.0f,
      g.v[0] + bbox_inset, bbox_mid_y, g.v[2] - bbox_inset, bbox_mid_y,
      g.v[0] + bbox_inset, bbox_lower_y, g.v[2] - bbox_inset, bbox_lower_y,
   };
   for (unsigned i = 0; i < 12; i++) {
      uint32_t x, y;
      if (!triangle_pixel_candidate(exterior_candidates[i * 2 + 0],
                                    exterior_candidates[i * 2 + 1], width,
                                    height, &x, &y) ||
          triangle_signed_margin(&g, (float)x + 0.5f, (float)y + 0.5f) >
             -R300_TRIANGLE_ORACLE_MARGIN)
         continue;
      verdict->exterior_samples++;
      const uint32_t index = y * R300_TRIANGLE_TARGET_PITCH_PIXELS + x;
      if (index >= pixel_count ||
          pixels[index] != R300_TRIANGLE_COLOR_SENTINEL)
         verdict->exterior_pass = false;
   }
   if (verdict->exterior_samples == 0)
      verdict->exterior_pass = false;

   /* Canary: the sub-pitch padding band of every rendered row, then
    * every row past the render extent, all at the sentinel.
    */
   for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = width; x < R300_TRIANGLE_TARGET_PITCH_PIXELS;
           x++) {
         const uint32_t index = y * R300_TRIANGLE_TARGET_PITCH_PIXELS + x;
         if (index < pixel_count &&
             pixels[index] != R300_TRIANGLE_COLOR_SENTINEL)
            verdict->canary_pass = false;
      }
   }
   for (uint32_t i = R300_TRIANGLE_TARGET_PITCH_PIXELS * height;
        i < pixel_count; i++) {
      if (pixels[i] != R300_TRIANGLE_COLOR_SENTINEL)
         verdict->canary_pass = false;
   }
}

/* Rounds a normalized channel to its UNORM8 byte, the conversion the
 * color buffer applies to the shaded value.
 */
static uint32_t
unorm8_round(float value)
{
   if (!(value > 0.0f))
      return 0;
   if (value >= 1.0f)
      return 255;
   return (uint32_t)(value * 255.0f + 0.5f);
}

void
r300_tcl_bypass_triangle_varying_extent_oracle(
   uint32_t width, uint32_t height, const float vertex_colors[12],
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_oracle_verdict *verdict)
{
   /* The constant-color oracle carries the geometry, the exterior and
    * canary passes, and the sample qualification; it judges the interior
    * against the constant draw color, so its interior verdict is
    * discarded and recomputed here against the interpolated expectation
    * over the same candidates.
    */
   r300_tcl_bypass_triangle_extent_oracle(width, height, pixels, size_bytes,
                                          verdict);
   if (verdict->interior_samples == 0 && verdict->exterior_samples == 0 &&
       !verdict->executed)
      return;

   const struct triangle_geometry g = triangle_geometry_at(width, height);
   const uint32_t pixel_count = size_bytes / 4;
   const float cx = (g.v[0] + g.v[2] + g.v[4]) / 3.0f;
   const float cy = (g.v[1] + g.v[3] + g.v[5]) / 3.0f;
   const float interior_candidates[8] = {
      cx, cy,
      (cx + g.v[0]) / 2.0f, (cy + g.v[1]) / 2.0f,
      (cx + g.v[2]) / 2.0f, (cy + g.v[3]) / 2.0f,
      (cx + g.v[4]) / 2.0f, (cy + g.v[5]) / 2.0f,
   };
   /* Twice the signed area; the barycentric weights divide the edge
    * functions by it.  The extent oracle above refused a degenerate
    * extent, so the area is nonzero here. */
   const float area = triangle_edgef(g.v[0], g.v[1], g.v[2], g.v[3],
                                     g.v[4], g.v[5]);
   verdict->interior_pass = true;
   verdict->interior_samples = 0;
   verdict->interior_max_deviation = 0;
   for (unsigned i = 0; i < 4; i++) {
      uint32_t x, y;
      if (!triangle_pixel_candidate(interior_candidates[i * 2 + 0],
                                    interior_candidates[i * 2 + 1], width,
                                    height, &x, &y))
         continue;
      const float px = (float)x + 0.5f, py = (float)y + 0.5f;
      if (triangle_signed_margin(&g, px, py) < R300_TRIANGLE_ORACLE_MARGIN)
         continue;
      verdict->interior_samples++;
      const uint32_t index = y * R300_TRIANGLE_TARGET_PITCH_PIXELS + x;
      if (index >= pixel_count) {
         verdict->interior_pass = false;
         continue;
      }
      /* Weight of vertex i is the edge function of the opposite edge. */
      const float w0 = triangle_edgef(g.v[2], g.v[3], g.v[4], g.v[5], px, py) /
                       area;
      const float w1 = triangle_edgef(g.v[4], g.v[5], g.v[0], g.v[1], px, py) /
                       area;
      const float w2 = 1.0f - w0 - w1;
      const uint32_t observed = pixels[index];
      /* B8G8R8A8: byte 0 blue, 1 green, 2 red, 3 alpha; the vertex
       * colors are RGBA. */
      static const unsigned channel_of_byte[4] = { 2, 1, 0, 3 };
      for (unsigned byte = 0; byte < 4; byte++) {
         const unsigned c = channel_of_byte[byte];
         const float value = w0 * vertex_colors[c] +
                             w1 * vertex_colors[4 + c] +
                             w2 * vertex_colors[8 + c];
         const uint32_t expected = unorm8_round(value);
         const uint32_t got = (observed >> (8 * byte)) & 0xffu;
         const uint32_t deviation =
            got > expected ? got - expected : expected - got;
         if (deviation > verdict->interior_max_deviation)
            verdict->interior_max_deviation = deviation;
         if (deviation > R300_TRIANGLE_VARYING_ORACLE_TOLERANCE)
            verdict->interior_pass = false;
      }
   }
   if (verdict->interior_samples == 0)
      verdict->interior_pass = false;
}

void
r300_tcl_bypass_triangle_oracle(const uint32_t *pixels, uint32_t size_bytes,
                                struct r300_triangle_oracle_verdict *verdict)
{
   r300_tcl_bypass_triangle_extent_oracle(R300_TRIANGLE_TARGET_WIDTH,
                                          R300_TRIANGLE_TARGET_HEIGHT,
                                          pixels, size_bytes, verdict);
}

/* TCL bypass consumes screen-space positions: an inset triangle inside the
 * 64x64 target, z = 0, w = 1.
 */
const float r300_tcl_bypass_triangle_vertices[R300_TRIANGLE_VERTEX_DWORDS] = {
    8.0f,  8.0f, 0.0f, 1.0f,
   56.0f,  8.0f, 0.0f, 1.0f,
   32.0f, 56.0f, 0.0f, 1.0f,
};

const float r300_tcl_bypass_triangle_varying_colors[12] = {
   0.125f, 0.125f, 0.25f, 1.0f,
   0.875f, 0.125f, 0.25f, 1.0f,
   0.5f,   0.875f, 0.25f, 1.0f,
};

const float r300_tcl_bypass_triangle_varying_vertices
   [R300_TRIANGLE_VARYING_VERTEX_DWORDS] = {
    8.0f,  8.0f, 0.0f, 1.0f, 0.125f, 0.125f, 0.25f, 1.0f,
   56.0f,  8.0f, 0.0f, 1.0f, 0.875f, 0.125f, 0.25f, 1.0f,
   32.0f, 56.0f, 0.0f, 1.0f, 0.5f,   0.875f, 0.25f, 1.0f,
};

void
r300_triangle_ib_serialize(const uint32_t *dwords, uint32_t count,
                           uint8_t *out)
{
   for (uint32_t i = 0; i < count; i++) {
      const uint32_t dword = dwords[i];
      out[4 * i + 0] = (uint8_t)(dword & 0xff);
      out[4 * i + 1] = (uint8_t)((dword >> 8) & 0xff);
      out[4 * i + 2] = (uint8_t)((dword >> 16) & 0xff);
      out[4 * i + 3] = (uint8_t)((dword >> 24) & 0xff);
   }
}

void
r300_triangle_ib_digest(const uint32_t *ib, uint32_t ib_size_dwords,
                        uint8_t out[R300_TRIANGLE_DIGEST_SIZE])
{
   /* Chunks through a stack buffer sized to a whole number of dwords, so no
    * allocation failure path exists and BLAKE3's streaming update makes the
    * chunk boundary invisible to the digest.
    */
   static const uint32_t chunk_dwords = 256;
   uint8_t chunk[chunk_dwords * 4];

   struct mesa_blake3 ctx;
   _mesa_blake3_init(&ctx);
   for (uint32_t offset = 0; offset < ib_size_dwords; offset += chunk_dwords) {
      const uint32_t n = MIN2(chunk_dwords, ib_size_dwords - offset);
      r300_triangle_ib_serialize(&ib[offset], n, chunk);
      _mesa_blake3_update(&ctx, chunk, n * sizeof(uint32_t));
   }
   _mesa_blake3_final(&ctx, out);
}

void
r300_triangle_ib_digest_hex(const uint32_t *ib, uint32_t ib_size_dwords,
                            char out[2 * R300_TRIANGLE_DIGEST_SIZE + 1])
{
   uint8_t digest[R300_TRIANGLE_DIGEST_SIZE];
   r300_triangle_ib_digest(ib, ib_size_dwords, digest);
   for (unsigned i = 0; i < R300_TRIANGLE_DIGEST_SIZE; i++)
      snprintf(&out[2 * i], 3, "%02x", digest[i]);
}

uint32_t
r300_triangle_draw_dword(const struct r300_tcl_bypass_triangle_ib *ib)
{
   /* The draw is the last type-3 packet the cell emits, so the walk reports
    * the final draw opcode it meets rather than a fixed offset that a
    * contract or fragment-binary change would move.
    */
   uint32_t found = 0;
   uint32_t i = 0;
   while (i < ib->ib_size_dwords) {
      const uint32_t header = ib->ib[i];
      /* A type-2 CP packet is one filler dword with no payload; its bits
       * 29:16 are not a count, so only type-0 and type-3 headers advance
       * past a payload.
       */
      const uint32_t count =
         (header >> 30) == 2 ? 0 : ((header >> 16) & 0x3fff) + 1;
      /* R300_PACKET3_3D_DRAW_VBUF_2 carries the opcode already positioned in
       * bits 8-15, which is the form CP_PACKET3 takes, so the header's
       * opcode field is compared in place rather than shifted down.
       */
      if ((header >> 30) == 3 &&
          (header & 0xff00) == R300_PACKET3_3D_DRAW_VBUF_2)
         found = i;
      i += 1 + count;
   }
   return found;
}
