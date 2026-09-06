/* SPDX-License-Identifier: MIT */

#include "r300_zb_depth_control_cell.h"

#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_pm4_builder.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_zb_depth_state.h"

#include "r300_reg.h"
#include "util/macros.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Four dwords per drm_radeon_cs_reloc entry, so a slot's payload indexes
 * the relocation chunk at four times the slot.
 */
#define DEPTH_CONTROL_RELOC_PAYLOAD(slot) ((slot) * 4)

/* Identity PSC swizzle select: X, Y, Z, W in place with a full write
 * mask, the exact per-word value the kernel's TCL-bypass vertex-output
 * check requires on every VAP_PROG_STREAM_CNTL_EXT word.
 */
#define DEPTH_CONTROL_PSC_EXT_IDENTITY 0xF688F688u

/* Two triangles of three vertices each, walked as one list. */
#define DEPTH_CONTROL_VERTEX_COUNT 6u
#define DEPTH_CONTROL_MAX_VTX_INDEX (DEPTH_CONTROL_VERTEX_COUNT - 1u)

/* The near triangle occupies the left half and the far triangle the
 * right, separated by eight pixels so the fill rule of neither reaches
 * the other's samples.
 */
const float
   r300_zb_depth_control_vertices[R300_ZB_DEPTH_CONTROL_VERTEX_DWORDS] = {
       4.0f,  8.0f, R300_ZB_DEPTH_CONTROL_NEAR_Z, 1.0f,
      28.0f,  8.0f, R300_ZB_DEPTH_CONTROL_NEAR_Z, 1.0f,
      16.0f, 56.0f, R300_ZB_DEPTH_CONTROL_NEAR_Z, 1.0f,
      36.0f,  8.0f, R300_ZB_DEPTH_CONTROL_FAR_Z,  1.0f,
      60.0f,  8.0f, R300_ZB_DEPTH_CONTROL_FAR_Z,  1.0f,
      48.0f, 56.0f, R300_ZB_DEPTH_CONTROL_FAR_Z,  1.0f,
};

static void
write_reloc(struct r300_pm4_builder *b, struct r300_zb_depth_control_ib *out,
            uint32_t slot)
{
   if (b->error != 0)
      return;
   if (slot >= R300_ZB_DEPTH_CONTROL_SLOT_COUNT ||
       out->reloc_site_count >= R300_ZB_DEPTH_CONTROL_MAX_RELOC_SITES) {
      b->error = -EINVAL;
      return;
   }

   const uint32_t index =
      r300_pm4_reloc_nop(b, DEPTH_CONTROL_RELOC_PAYLOAD(slot));
   if (index == R300_PM4_NO_INDEX)
      return;

   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_zb_depth_control_reloc_site){
         .ib_index = index,
         .slot = slot,
      };
}

/* Records the site r300_zb_depth_state_emit placed for the depth BO; the
 * emitter reports the position of the payload it wrote, so the site table
 * carries no second derivation of the depth packet's layout.
 */
static void
record_depth_site(struct r300_pm4_builder *b,
                  struct r300_zb_depth_control_ib *out, uint32_t index)
{
   if (b->error != 0)
      return;
   if (index == R300_PM4_NO_INDEX ||
       out->reloc_site_count >= R300_ZB_DEPTH_CONTROL_MAX_RELOC_SITES) {
      b->error = -EINVAL;
      return;
   }
   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_zb_depth_control_reloc_site){
         .ib_index = index,
         .slot = R300_ZB_DEPTH_CONTROL_SLOT_DEPTH,
      };
}

int
r300_zb_depth_control_emit_into(
   const struct r300_zb_depth_control_params *params, uint32_t *words,
   uint32_t capacity, struct r300_zb_depth_control_ib *out)
{
   memset(out, 0, sizeof(*out));
   if (params == NULL || words == NULL)
      return -EINVAL;

   const struct r300_fragment_binary *fs = params->fragment_binary;
   if (fs == NULL || !fs->validated)
      return -EINVAL;
   if (params->first_draw_contract == NULL)
      return -EINVAL;

   /* The surface, then the two facts the cell needs of it: it encodes,
    * and the host addresses it by coordinate in both directions.  The
    * cell writes its pre-draw depth per pixel and reads the result back
    * as a row-major image, so uniform initialization alone leaves it
    * without the transform either half needs.
    */
   const struct r300_zb_depth_surface *surface =
      params->surface != NULL ? params->surface
                              : &r300_zb_depth_surface_z16_linear;
   const int surface_rc = r300_zb_depth_surface_check(surface);
   if (surface_rc != 0)
      return surface_rc;
   if (!surface->logical_pixel_addressing || !surface->logical_image_readback)
      return -EINVAL;
   /* The cell's vertices, scissor, allocation, and oracle carry the
    * target extent as constants, and the oracle indexes the readback
    * with R300_ZB_DEPTH_CONTROL_PITCH_PIXELS while the stream binds
    * surface->pitch_pixels.  A descriptor naming any other geometry
    * would emit one layout and be read at another, so the cell takes
    * exactly the geometry it draws.
    */
   if (surface->width != R300_ZB_DEPTH_CONTROL_TARGET_WIDTH ||
       surface->height != R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT ||
       surface->pitch_pixels != R300_ZB_DEPTH_CONTROL_PITCH_PIXELS ||
       surface->allocation_rows != R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS)
      return -EINVAL;

   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, words, capacity);

   /* The contract's writes land before any cell state, so the depth
    * comparison, the color-write gates, and the vertex bound all come
    * from this stream rather than from the previous client.
    */
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

   /* Vertex path: pretransformed positions bypass the TCL block, one
    * FLOAT_4 stream lands whole in output vector zero, and every PSC
    * extended selector stays identity, so the kernel's vertex-output
    * check can prove VAP_VTX_SIZE = 4 covers the fetch.
    */
   r300_pm4_reg(&b, R300_VAP_CNTL_STATUS, R300_VAP_TCL_BYPASS);
   r300_pm4_reg(&b, R300_VAP_PROG_STREAM_CNTL_0,
                R300_DATA_TYPE_FLOAT_4 | (0 << R300_DST_VEC_LOC_SHIFT) |
                   R300_LAST_VEC);
   static const uint32_t psc_ext_identity[8] = {
      DEPTH_CONTROL_PSC_EXT_IDENTITY, DEPTH_CONTROL_PSC_EXT_IDENTITY,
      DEPTH_CONTROL_PSC_EXT_IDENTITY, DEPTH_CONTROL_PSC_EXT_IDENTITY,
      DEPTH_CONTROL_PSC_EXT_IDENTITY, DEPTH_CONTROL_PSC_EXT_IDENTITY,
      DEPTH_CONTROL_PSC_EXT_IDENTITY, DEPTH_CONTROL_PSC_EXT_IDENTITY,
   };
   r300_pm4_packet0(&b, R300_VAP_PROG_STREAM_CNTL_EXT_0, psc_ext_identity,
                    ARRAY_SIZE(psc_ext_identity));
   r300_pm4_reg(&b, R300_VAP_OUTPUT_VTX_FMT_0,
                R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT);
   r300_pm4_reg(&b, R300_VAP_OUTPUT_VTX_FMT_1, 0);
   r300_pm4_reg(&b, R300_VAP_VTX_SIZE, 4);

   /* Fragment program: the owned binary's US/FG block verbatim, then the
    * two register values the descriptor keeps outside the sequence.  One
    * constant color covers both triangles, so the color verdict rests on
    * which half the device wrote rather than on two shader outputs.
    */
   r300_pm4_block(&b, fs->cb_code, fs->cb_code_size);
   r300_pm4_reg(&b, R300_FG_DEPTH_SRC, fs->fg_depth_src);
   r300_pm4_reg(&b, R300_US_W_FMT, fs->us_out_w);

   /* One color target.  RB3D_COLOROFFSET carries the color BO reference;
    * the pitch/format word travels plain because the submission sets
    * RADEON_CS_KEEP_TILING_FLAGS, the same flag under which
    * r300_packet0_check skips the ZB_DEPTHPITCH relocation below.
    */
   r300_pm4_reg(&b, R300_RB3D_CCTL, 0);
   r300_pm4_reg(&b, R300_RB3D_COLOROFFSET0, 0);
   write_reloc(&b, out, R300_ZB_DEPTH_CONTROL_SLOT_COLOR);
   r300_pm4_reg(&b, R300_RB3D_COLORPITCH0, params->color_pitch_format);

   /* Depth binding and test.  ZB_CNTL's Z_ENABLE is also what arms the
    * kernel's depth-buffer size check, so the surface the stream binds is
    * the one r300_cs_track_check measures against pitch, cpp, and the
    * scissor height.
    */
   const struct r300_zb_depth_state_params depth = {
      .pitch_pixels = surface->pitch_pixels,
      .depth_format = surface->depth_format,
      .pitch_tile_bits = r300_zb_depth_surface_tile_bits(surface),
      .depth_offset_bytes = params->depth_offset_bytes,
      .depth_relocation_payload =
         DEPTH_CONTROL_RELOC_PAYLOAD(R300_ZB_DEPTH_CONTROL_SLOT_DEPTH),
      .depth_function = R300_ZS_LESS,
      .depth_write = true,
   };
   uint32_t depth_reloc_index = R300_PM4_NO_INDEX;
   const int depth_rc =
      r300_zb_depth_state_emit(&b, &depth, &depth_reloc_index);
   if (depth_rc != 0 && b.error == 0)
      b.error = depth_rc;
   record_depth_site(&b, out, depth_reloc_index);

   /* Vertex fetch: one array, sixteen bytes per vertex, stride sixteen. */
   const uint32_t vbpntr[3] = {
      1 | R300_VC_FORCE_PREFETCH,
      R300_VBPNTR_SIZE0(16) | R300_VBPNTR_STRIDE0(16),
      params->vertex_offset,
   };
   r300_pm4_packet3(&b, R300_PACKET3_3D_LOAD_VBPNTR, vbpntr,
                    ARRAY_SIZE(vbpntr));
   write_reloc(&b, out, R300_ZB_DEPTH_CONTROL_SLOT_VERTEX);

   /* One triangle list of six vertices: the array pointer is the draw's
    * only start, so both triangles ride one walk and one LOAD_VBPNTR.
    */
   const uint32_t draw =
      R300_VAP_VF_CNTL__PRIM_TRIANGLES | R300_PRIM_WALK_LIST |
      (DEPTH_CONTROL_VERTEX_COUNT << R300_PRIM_NUM_VERTICES_SHIFT);
   r300_pm4_packet3(&b, R300_PACKET3_3D_DRAW_VBUF_2, &draw, 1);

   /* Destination-cache publication retires the color writes before the IB
    * completes; the Z cache holds the depth writes on its own path, so
    * the depth readback needs its own flush.  This writes 0x4f18 through
    * the CS parser's allowlist, the transaction the first-draw contract's
    * ordering barrier already uses.
    */
   r300_pm4_reg(&b, R300_RB3D_DSTCACHE_CTLSTAT,
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
   r300_pm4_reg(&b, R300_ZB_ZCACHE_CTLSTAT,
                R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);

   const int rc = r300_pm4_builder_finish(&b, &out->ib_size_dwords);
   if (rc != 0) {
      memset(out, 0, sizeof(*out));
      return rc;
   }
   out->ib = words;
   return 0;
}

int
r300_zb_depth_control_emit(const struct r300_zb_depth_control_params *params,
                           struct r300_zb_depth_control_ib *out)
{
   memset(out, 0, sizeof(*out));
   if (params == NULL)
      return -EINVAL;

   const struct r300_fragment_binary *fs = params->fragment_binary;
   if (fs == NULL || !fs->validated)
      return -EINVAL;

   const uint32_t capacity =
      R300_ZB_DEPTH_CONTROL_MAX_DWORDS + fs->cb_code_size;
   uint32_t *ib = calloc(capacity, sizeof(uint32_t));
   if (ib == NULL)
      return -ENOMEM;

   const int rc = r300_zb_depth_control_emit_into(params, ib, capacity, out);
   if (rc != 0) {
      free(ib);
      return rc;
   }
   out->owns_ib = true;
   return 0;
}

void
r300_zb_depth_control_release(struct r300_zb_depth_control_ib *ib)
{
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_zb_depth_control_validate_reloc_sites(
   const struct r300_zb_depth_control_ib *ib)
{
   if (ib->reloc_site_count != R300_ZB_DEPTH_CONTROL_SLOT_COUNT)
      return -EINVAL;

   static_assert(R300_ZB_DEPTH_CONTROL_SLOT_COUNT <= 32,
                 "slot uniqueness is proven in a 32-bit mask");

   uint32_t seen = 0;
   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_zb_depth_control_reloc_site *site =
         &ib->reloc_sites[i];
      if (site->slot >= R300_ZB_DEPTH_CONTROL_SLOT_COUNT)
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
      if (ib->ib[site->ib_index] != DEPTH_CONTROL_RELOC_PAYLOAD(site->slot))
         return -EINVAL;
   }

   /* The relocation list follows the stream: the color target is
    * programmed, then the depth surface bound, then the vertex array.
    * Command-stream order is its own fact, distinct from enum order, so
    * the expected sequence is spelled out rather than derived.
    */
   static const uint32_t expected_slots[] = {
      R300_ZB_DEPTH_CONTROL_SLOT_COLOR,
      R300_ZB_DEPTH_CONTROL_SLOT_DEPTH,
      R300_ZB_DEPTH_CONTROL_SLOT_VERTEX,
   };
   static_assert(ARRAY_SIZE(expected_slots) ==
                    R300_ZB_DEPTH_CONTROL_SLOT_COUNT,
                 "every slot has a place in the stream order");
   for (uint32_t i = 0; i < R300_ZB_DEPTH_CONTROL_SLOT_COUNT; i++) {
      if (ib->reloc_sites[i].slot != expected_slots[i])
         return -EINVAL;
      if (i > 0 &&
          ib->reloc_sites[i - 1].ib_index >= ib->reloc_sites[i].ib_index)
         return -EINVAL;
   }

   return 0;
}

int
r300_zb_depth_control_reference_contract(struct r300_first_draw_contract *out)
{
   struct r300_first_draw_params params = {
      .chip_family = CHIP_RS480,
      .width = R300_ZB_DEPTH_CONTROL_TARGET_WIDTH,
      .height = R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT,
      .min_vtx_index = 0,
      .max_vtx_index = DEPTH_CONTROL_MAX_VTX_INDEX,
      .texture_enabled = false,
   };
   const int rc = r300_first_draw_contract_resolve(&params, out);
   if (rc != 0)
      return rc;

   /* The target is little-endian B8G8R8A8, so the shader's four 8-bit
    * output components select blue, green, red, and alpha in that order.
    */
   return r300_first_draw_contract_set_us_out_fmt_0(
      out, R300_US_OUT_FMT_C4_8 | R300_C0_SEL_B | R300_C1_SEL_G |
              R300_C2_SEL_R | R300_C3_SEL_A);
}

int
r300_zb_depth_control_reference_emit(struct r300_zb_depth_control_ib *out)
{
   struct r300_fragment_binary fs;
   int rc = r300_tcl_bypass_triangle_reference_fs(&fs);
   if (rc != 0)
      return rc;

   struct r300_first_draw_contract contract;
   rc = r300_zb_depth_control_reference_contract(&contract);
   if (rc != 0) {
      r300_fragment_binary_finish(&fs);
      return rc;
   }

   const struct r300_zb_depth_control_params params = {
      .vertex_offset = 0,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(
         R300_ZB_DEPTH_CONTROL_PITCH_PIXELS),
      .depth_offset_bytes = 0,
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };
   rc = r300_zb_depth_control_emit(&params, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

uint32_t
r300_zb_depth_control_draw_dword(const struct r300_zb_depth_control_ib *ib)
{
   /* The draw precedes the two cache publications, each a two-dword
    * PACKET0 register write, and the draw packet itself is a header plus
    * one body dword.
    */
   const uint32_t trailing = 2u * 2u + 2u;
   if (ib->ib_size_dwords < trailing)
      return 0;
   return ib->ib_size_dwords - trailing;
}

/* Edge function twice the signed area of (a, b, p); both triangles wind
 * so every interior point yields three positive values.
 */
static float
edgef(float ax, float ay, float bx, float by, float px, float py)
{
   return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Signed pixel distance from p to the line through (a, b), positive on
 * the triangle's interior side.
 */
static float
edge_distance(float ax, float ay, float bx, float by, float px, float py)
{
   const float dx = bx - ax, dy = by - ay;
   return edgef(ax, ay, bx, by, px, py) / sqrtf(dx * dx + dy * dy);
}

/* The minimum of the three signed edge distances: positive inside with
 * that margin, negative outside with it.  A sample carries a verdict only
 * at least this far from an edge, so no verdict rides the hardware's
 * exact fill rule.
 */
#define DEPTH_CONTROL_ORACLE_MARGIN 2.0f

static float
signed_margin(const float *v, float px, float py)
{
   float m = edge_distance(v[0], v[1], v[2], v[3], px, py);
   const float d1 = edge_distance(v[2], v[3], v[4], v[5], px, py);
   const float d2 = edge_distance(v[4], v[5], v[0], v[1], px, py);
   if (d1 < m)
      m = d1;
   if (d2 < m)
      m = d2;
   return m;
}

/* Which region a pixel center belongs to, at the verdict margin.  A pixel
 * inside neither triangle's margin and outside neither's is in a fill-rule
 * band and carries no verdict.
 */
enum depth_control_region {
   DEPTH_CONTROL_REGION_NONE,
   DEPTH_CONTROL_REGION_NEAR,
   DEPTH_CONTROL_REGION_FAR,
   DEPTH_CONTROL_REGION_EXTERIOR,
};

static enum depth_control_region
region_of(uint32_t x, uint32_t y)
{
   /* The vertex payload is window space already, so the oracle's geometry
    * is the same array the vertex BO carries: one authority for what the
    * device rasterizes and what the verdict expects.
    */
   const float near_v[6] = {
      r300_zb_depth_control_vertices[0], r300_zb_depth_control_vertices[1],
      r300_zb_depth_control_vertices[4], r300_zb_depth_control_vertices[5],
      r300_zb_depth_control_vertices[8], r300_zb_depth_control_vertices[9],
   };
   const float far_v[6] = {
      r300_zb_depth_control_vertices[12], r300_zb_depth_control_vertices[13],
      r300_zb_depth_control_vertices[16], r300_zb_depth_control_vertices[17],
      r300_zb_depth_control_vertices[20], r300_zb_depth_control_vertices[21],
   };
   const float px = (float)x + 0.5f, py = (float)y + 0.5f;
   const float near_margin = signed_margin(near_v, px, py);
   const float far_margin = signed_margin(far_v, px, py);

   if (near_margin >= DEPTH_CONTROL_ORACLE_MARGIN)
      return DEPTH_CONTROL_REGION_NEAR;
   if (far_margin >= DEPTH_CONTROL_ORACLE_MARGIN)
      return DEPTH_CONTROL_REGION_FAR;
   if (near_margin <= -DEPTH_CONTROL_ORACLE_MARGIN &&
       far_margin <= -DEPTH_CONTROL_ORACLE_MARGIN)
      return DEPTH_CONTROL_REGION_EXTERIOR;
   return DEPTH_CONTROL_REGION_NONE;
}

/* Bytes each oracle reads: every rendered row at the cell's pitch plus
 * the row past the render extent, which carries the canary.  A shorter
 * buffer holds no canary, so the verdict fails closed with zero samples
 * rather than leaving canary_pass vacuously true.
 */
#define DEPTH_CONTROL_ORACLE_PIXELS \
   (R300_ZB_DEPTH_CONTROL_PITCH_PIXELS * R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS)

void
r300_zb_depth_control_color_oracle(
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_zb_depth_control_color_verdict *verdict)
{
   *verdict = (struct r300_zb_depth_control_color_verdict){ 0 };
   if (pixels == NULL ||
       size_bytes < DEPTH_CONTROL_ORACLE_PIXELS * sizeof(uint32_t))
      return;

   verdict->near_pass = true;
   verdict->far_pass = true;
   verdict->exterior_pass = true;
   verdict->canary_pass = true;

   for (uint32_t i = 0; i < DEPTH_CONTROL_ORACLE_PIXELS; i++) {
      if (pixels[i] != R300_TRIANGLE_COLOR_SENTINEL) {
         verdict->executed = true;
         break;
      }
   }

   for (uint32_t y = 0; y < R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT; y++) {
      for (uint32_t x = 0; x < R300_ZB_DEPTH_CONTROL_TARGET_WIDTH; x++) {
         const uint32_t pixel =
            pixels[y * R300_ZB_DEPTH_CONTROL_PITCH_PIXELS + x];
         const bool drawn = pixel == R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
         const bool sentinel = pixel == R300_TRIANGLE_COLOR_SENTINEL;

         switch (region_of(x, y)) {
         case DEPTH_CONTROL_REGION_NEAR:
            verdict->near_samples++;
            verdict->near_colored += drawn ? 1u : 0u;
            if (!drawn)
               verdict->near_pass = false;
            break;
         case DEPTH_CONTROL_REGION_FAR:
            verdict->far_samples++;
            verdict->far_colored += drawn ? 1u : 0u;
            if (!sentinel)
               verdict->far_pass = false;
            break;
         case DEPTH_CONTROL_REGION_EXTERIOR:
            verdict->exterior_samples++;
            if (!sentinel)
               verdict->exterior_pass = false;
            break;
         case DEPTH_CONTROL_REGION_NONE:
            break;
         }
      }

      /* Sub-pitch padding band of this rendered row.  The reference cell
       * renders the full pitch, so the band is empty there and the row
       * past the render extent carries the canary alone.
       */
      for (uint32_t x = R300_ZB_DEPTH_CONTROL_TARGET_WIDTH;
           x < R300_ZB_DEPTH_CONTROL_PITCH_PIXELS; x++) {
         if (pixels[y * R300_ZB_DEPTH_CONTROL_PITCH_PIXELS + x] !=
             R300_TRIANGLE_COLOR_SENTINEL)
            verdict->canary_pass = false;
      }
   }

   for (uint32_t x = 0; x < R300_ZB_DEPTH_CONTROL_PITCH_PIXELS; x++) {
      if (pixels[R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT *
                    R300_ZB_DEPTH_CONTROL_PITCH_PIXELS +
                 x] != R300_TRIANGLE_COLOR_SENTINEL)
         verdict->canary_pass = false;
   }

   if (verdict->near_samples == 0)
      verdict->near_pass = false;
   if (verdict->far_samples == 0)
      verdict->far_pass = false;
   if (verdict->exterior_samples == 0)
      verdict->exterior_pass = false;
}

void
r300_zb_depth_control_depth_oracle(
   const uint16_t *depth, uint32_t size_bytes,
   struct r300_zb_depth_control_depth_verdict *verdict)
{
   *verdict = (struct r300_zb_depth_control_depth_verdict){ 0 };
   if (depth == NULL ||
       size_bytes < DEPTH_CONTROL_ORACLE_PIXELS * sizeof(uint16_t))
      return;

   verdict->near_pass = true;
   verdict->far_pass = true;
   verdict->exterior_pass = true;
   verdict->canary_pass = true;
   verdict->near_min = UINT16_MAX;
   verdict->near_max = 0;

   for (uint32_t i = 0; i < DEPTH_CONTROL_ORACLE_PIXELS; i++) {
      if (depth[i] != R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL) {
         verdict->written = true;
         break;
      }
   }

   for (uint32_t y = 0; y < R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT; y++) {
      for (uint32_t x = 0; x < R300_ZB_DEPTH_CONTROL_TARGET_WIDTH; x++) {
         const uint16_t stored =
            depth[y * R300_ZB_DEPTH_CONTROL_PITCH_PIXELS + x];
         const bool preserved =
            stored == R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL;

         switch (region_of(x, y)) {
         case DEPTH_CONTROL_REGION_NEAR:
            verdict->near_samples++;
            if (stored < verdict->near_min)
               verdict->near_min = stored;
            if (stored > verdict->near_max)
               verdict->near_max = stored;
            /* R300_ZS_LESS with Z_WRITE_ENABLE stores a value that
             * compared below the sentinel and above the near plane's
             * floor, so two one-sided bounds are the predicate and no
             * window-Z to Z16 rounding rule enters it.  Zero passes the
             * upper bound under any rounding rule, so a surface a device
             * never wrote and a host fill that landed as zeros would
             * satisfy that bound alone.
             */
            if (stored >= R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL ||
                stored == 0)
               verdict->near_pass = false;
            break;
         case DEPTH_CONTROL_REGION_FAR:
            verdict->far_samples++;
            if (!preserved)
               verdict->far_pass = false;
            break;
         case DEPTH_CONTROL_REGION_EXTERIOR:
            verdict->exterior_samples++;
            if (!preserved)
               verdict->exterior_pass = false;
            break;
         case DEPTH_CONTROL_REGION_NONE:
            break;
         }
      }

      for (uint32_t x = R300_ZB_DEPTH_CONTROL_TARGET_WIDTH;
           x < R300_ZB_DEPTH_CONTROL_PITCH_PIXELS; x++) {
         if (depth[y * R300_ZB_DEPTH_CONTROL_PITCH_PIXELS + x] !=
             R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL)
            verdict->canary_pass = false;
      }
   }

   for (uint32_t x = 0; x < R300_ZB_DEPTH_CONTROL_PITCH_PIXELS; x++) {
      if (depth[R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT *
                   R300_ZB_DEPTH_CONTROL_PITCH_PIXELS +
                x] != R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL)
         verdict->canary_pass = false;
   }

   if (verdict->near_samples == 0)
      verdict->near_pass = false;
   if (verdict->far_samples == 0)
      verdict->far_pass = false;
   if (verdict->exterior_samples == 0)
      verdict->exterior_pass = false;
   if (verdict->near_min > verdict->near_max) {
      verdict->near_min = 0;
      verdict->near_max = 0;
   }
}
