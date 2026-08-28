/* SPDX-License-Identifier: MIT */

#include "r300_tcl_bypass_triangle.h"
#include "r300_first_draw_state.h"
#include "r300_flat_color0_plan.h"
#include "r300_rs_tex_adj_probe.h"
#include "r300_fragment_binary.h"
#include "r300_pm4_builder.h"
#include "r300_r2vb_producer_fs_block.h"
#include "r300_tcl_bypass_sampled_fs_block.h"
#include "r300_tcl_bypass_triangle_fs_block.h"
#include "r300_us_source_read.h"

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

/* The color 0 vector of the same layout: the carrier the GA's
 * provoking-vertex selection and RS_IP_0's color pointer 0 read.
 */
#define R300_TRIANGLE_COLOR0_DST_VEC 2u

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

static bool
checked_u32_multiply(uint32_t left, uint32_t right, uint32_t *product)
{
   if (left != 0 && right > UINT32_MAX / left)
      return false;
   *product = left * right;
   return true;
}

static bool
checked_u32_add(uint32_t left, uint32_t right, uint32_t *sum)
{
   if (right > UINT32_MAX - left)
      return false;
   *sum = left + right;
   return true;
}

static int
clip_output_triangle_count(uint32_t source_triangle_count,
                           uint32_t *output_triangle_count)
{
   if (source_triangle_count < 1u ||
       source_triangle_count > R300_TRIANGLE_MAX_TRIANGLES)
      return -EINVAL;
   if (!checked_u32_multiply(
          source_triangle_count,
          R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT,
          output_triangle_count) ||
       *output_triangle_count > R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES)
      return -EOVERFLOW;
   return 0;
}

static uint32_t
first_segment_max_vertex_index(uint32_t triangle_count)
{
   const uint32_t first_segment_triangle_count =
      MIN2(triangle_count, R300_TRIANGLE_MAX_TRIANGLES);
   return 3u * first_segment_triangle_count - 1u;
}

static int
validate_vertex_segment_offsets(uint32_t triangle_count,
                                uint32_t record_bytes,
                                uint32_t initial_vertex_offset)
{
   uint32_t triangle_offset = 0;
   while (triangle_offset < triangle_count) {
      const uint32_t segment_triangle_count =
         MIN2(triangle_count - triangle_offset, R300_TRIANGLE_MAX_TRIANGLES);
      uint32_t segment_vertex_count;
      uint32_t preceding_vertex_count;
      uint32_t segment_byte_offset;
      uint32_t ignored_vertex_offset;
      if (!checked_u32_multiply(segment_triangle_count, 3u,
                                &segment_vertex_count) ||
          segment_vertex_count > 0xffffu ||
          !checked_u32_multiply(triangle_offset, 3u,
                                &preceding_vertex_count) ||
          !checked_u32_multiply(preceding_vertex_count, record_bytes,
                                &segment_byte_offset) ||
          !checked_u32_add(initial_vertex_offset, segment_byte_offset,
                           &ignored_vertex_offset))
         return -EOVERFLOW;
      triangle_offset += segment_triangle_count;
   }
   return 0;
}

static int
emit_triangle_stream_into(
   const struct r300_tcl_bypass_triangle_params *params, uint32_t *words,
   uint32_t capacity, uint32_t triangle_count,
   struct r300_tcl_bypass_triangle_ib *out)
{
   const struct r300_fragment_binary *fs = params->fragment_binary;

   memset(out, 0, sizeof(*out));
   if (fs == NULL || !fs->validated) {
      return -EINVAL;
   }

   if (triangle_count == 0 ||
       triangle_count > R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES)
      return -EINVAL;
   /* The sampled cell rides the varying vertex path for its coordinate,
    * the TX width/height masks hold extent - 1 in 11 bits, the pitch
    * covers each row, and TX_OFFSET_0's low five bits are reserved.
    */
   if (params->sampled &&
       (!params->varying || params->texture_width == 0 ||
        params->texture_height == 0 || params->texture_width > 2048 ||
        params->texture_height > 2048 ||
        params->texture_pitch_texels < params->texture_width ||
        params->texture_pitch_texels > 0x4000 ||
        (params->texture_offset & 31) != 0 ||
        (params->texture_lanes != R300_TRIANGLE_LANES_B8G8R8A8 &&
         params->texture_lanes != R300_TRIANGLE_LANES_R8G8B8A8)))
      return -EINVAL;
   /* The color-0 carrier is a varying record shape whose interpolation
    * state the contract establishes; the sampled cell's TX coordinate
    * rides TEX0 and keeps its own RS block.
    */
   const bool flat_color0 = params->flat_color0 != NULL;
   if (flat_color0 && (!params->varying || params->sampled ||
                       params->first_draw_contract == NULL))
      return -EINVAL;
   const uint32_t record_dwords = params->varying ? 8u : 4u;
   const uint32_t record_bytes = record_dwords * 4u;
   const uint32_t first_segment_triangle_count =
      MIN2(triangle_count, R300_TRIANGLE_MAX_TRIANGLES);
   uint32_t first_segment_vertex_count;
   if (!checked_u32_multiply(first_segment_triangle_count, 3u,
                             &first_segment_vertex_count))
      return -EOVERFLOW;
   const int offset_validation = validate_vertex_segment_offsets(
      triangle_count, record_bytes, params->vertex_offset);
   if (offset_validation != 0)
      return offset_validation;

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
      r300_pm4_emit_vertex_index_range(&b, 0,
                                       first_segment_vertex_count - 1u);
   }

   /* Vertex path: pretransformed positions bypass the TCL block, one
    * FLOAT_4 stream lands whole in output vector zero, and every PSC
    * extended selector stays identity, so the kernel's vertex-output check
    * can prove VAP_VTX_SIZE = 4 covers the fetch.  The varying cell adds a
    * second FLOAT_4 element into the texture-coordinate-0 vector, declares
    * it as a four-component TEX0 output, and fetches eight dwords per
    * vertex, the identity-list arithmetic the same check proves.
    */
   r300_pm4_reg(&b, R300_VAP_CNTL_STATUS, R300_VAP_TCL_BYPASS);
   r300_pm4_reg(&b, R300_VAP_PROG_STREAM_CNTL_0,
                params->varying
                   ? (R300_DATA_TYPE_FLOAT_4 |
                      (0 << R300_DST_VEC_LOC_SHIFT)) |
                        ((R300_DATA_TYPE_FLOAT_4 |
                          ((flat_color0 ? R300_TRIANGLE_COLOR0_DST_VEC
                                        : R300_TRIANGLE_VARYING_DST_VEC)
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
                R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT |
                   (flat_color0 ? R300_VAP_OUTPUT_VTX_FMT_0__COLOR_0_PRESENT
                                : 0));
   r300_pm4_reg(&b, R300_VAP_OUTPUT_VTX_FMT_1,
                params->varying && !flat_color0
                   ? R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS
                   : 0);
   r300_pm4_reg(&b, R300_VAP_VTX_SIZE, record_dwords);
   if (params->varying && !flat_color0) {
      /* The assembler admits position plus texture coordinate 0, and
       * the RS routes that varying: RS_COUNT declares four interpolated
       * components with no rasterized colors, RS_IP_0 reads texture
       * pointer 0 and selects its four channels in order, RS_INST_0
       * writes the result to US input register 0, and RS_INST_COUNT of
       * zero runs instruction 0 alone.  The first-draw contract wrote
       * the position-only forms of these registers ahead of the cell,
       * so the varying cell establishes its own values here.
       */
      const struct r300_rs_tex_adj_probe_plan *probe = params->rs_tex_adj;
      struct r300_rs_tex_adj_probe_plan control;
      if (probe == NULL) {
         /* The control plan reproduces the varying cell's constants
          * exactly, so a probe candidate differs from these bytes in
          * its one control bit alone. */
         r300_rs_tex_adj_probe_plan_control(&control);
         probe = &control;
      }
      r300_pm4_reg(&b, R300_VAP_VSM_VTX_ASSM,
                   R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0);
      r300_pm4_reg(&b, R300_RS_COUNT,
                   r300_rs_tex_adj_probe_plan_rs_count(probe));
      r300_pm4_reg(&b, R300_RS_INST_COUNT, 0);
      r300_pm4_reg(&b, R300_RS_IP_0,
                   r300_rs_tex_adj_probe_plan_rs_ip_0(probe));
      r300_pm4_reg(&b, R300_RS_INST_0,
                   r300_rs_tex_adj_probe_plan_rs_inst(probe, 0));
      if (r300_rs_tex_adj_probe_plan_writes_rs_inst_1(probe))
         r300_pm4_reg(&b, R300_RS_INST_1,
                      r300_rs_tex_adj_probe_plan_rs_inst(probe, 1));
   }

   /* The sampled cell's TX unit 0: nearest filters and clamp-to-edge
    * wraps make each fetch one texel read, W8Z8Y8X8 gives the kernel
    * tracker cpp 4, and TX_PITCH_EN with FORMAT2's pitch - 1 makes the
    * tracker validate pitch * cpp * height against the texture BO
    * (r100_cs_track_texture_check).  TX_OFFSET_0's payload rides the
    * texture reloc; the invalidate ahead of the enable drops stale
    * texture-cache tags before the first fetch.
    */
   if (params->sampled) {
      r300_pm4_reg(&b, R300_TX_INVALTAGS, 0);
      r300_pm4_reg(&b, R300_TX_ENABLE, R300_TX_ENABLE_0);
      r300_pm4_reg(&b, R300_TX_FILTER0_0,
                   (R300_TX_CLAMP_TO_EDGE << R300_TX_WRAP_S_SHIFT) |
                      (R300_TX_CLAMP_TO_EDGE << R300_TX_WRAP_T_SHIFT) |
                      R300_TX_MAG_FILTER_NEAREST |
                      R300_TX_MIN_FILTER_NEAREST);
      r300_pm4_reg(&b, R300_TX_FILTER1_0, 0);
      r300_pm4_reg(&b, R300_TX_BORDER_COLOR_0, 0);
      r300_pm4_reg(&b, R300_TX_FORMAT0_0,
                   ((params->texture_width - 1)
                    << R300_TX_WIDTHMASK_SHIFT) |
                      ((params->texture_height - 1)
                       << R300_TX_HEIGHTMASK_SHIFT) |
                      R300_TX_PITCH_EN);
      /* FORMAT1's per-channel selects default to X, so silicon returns
       * byte X in every lane without them (RS482 readback: 0x20202020
       * for texel 20,60,a0,e0 with selects absent); each lane order
       * takes the select set that routes its memory bytes to R/G/B/A.
       */
      r300_pm4_reg(&b, R300_TX_FORMAT1_0,
                   params->texture_lanes == R300_TRIANGLE_LANES_B8G8R8A8
                      ? R300_EASY_TX_FORMAT(X, Y, Z, W, W8Z8Y8X8)
                      : R300_EASY_TX_FORMAT(Z, Y, X, W, W8Z8Y8X8));
      r300_pm4_reg(&b, R300_TX_FORMAT2_0,
                   params->texture_pitch_texels - 1);
      r300_pm4_reg(&b, R300_TX_OFFSET_0, params->texture_offset);
      write_reloc(&b, out, R300_TRIANGLE_SLOT_TEXTURE);
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
   r300_pm4_reg(&b, R300_RB3D_COLOROFFSET0, params->color_offset);
   write_reloc(&b, out, R300_TRIANGLE_SLOT_COLOR);
   r300_pm4_reg(&b, R300_RB3D_COLORPITCH0, params->color_pitch_format);

   /* Each hardware draw uses local vertex indices and a rebased byte offset,
    * so the 16-bit count and maximum-index fields never describe more than
    * one 21,845-triangle segment.  Segment boundaries preserve triangle
    * order and can divide one source triangle's seven reserved output slots,
    * because no output triangle is itself divided.
    */
   uint32_t emitted_triangle_count = 0;
   while (emitted_triangle_count < triangle_count && b.error == 0) {
      const uint32_t segment_triangle_count = MIN2(
         triangle_count - emitted_triangle_count,
         R300_TRIANGLE_MAX_TRIANGLES);
      uint32_t segment_vertex_count;
      uint32_t preceding_vertex_count;
      uint32_t segment_byte_offset;
      uint32_t vertex_offset;
      if (!checked_u32_multiply(segment_triangle_count, 3u,
                                &segment_vertex_count) ||
          !checked_u32_multiply(emitted_triangle_count, 3u,
                                &preceding_vertex_count) ||
          !checked_u32_multiply(preceding_vertex_count, record_bytes,
                                &segment_byte_offset) ||
          !checked_u32_add(params->vertex_offset, segment_byte_offset,
                           &vertex_offset)) {
         b.error = -EOVERFLOW;
         break;
      }

      const uint32_t vbpntr[3] = {
         1 | R300_VC_FORCE_PREFETCH,
         R300_VBPNTR_SIZE0(record_bytes) | R300_VBPNTR_STRIDE0(record_bytes),
         vertex_offset,
      };
      r300_pm4_packet3(&b, R300_PACKET3_3D_LOAD_VBPNTR, vbpntr,
                       ARRAY_SIZE(vbpntr));
      write_reloc(&b, out, R300_TRIANGLE_SLOT_VERTEX);

      const uint32_t draw =
         R300_VAP_VF_CNTL__PRIM_TRIANGLES | R300_PRIM_WALK_LIST |
         (segment_vertex_count << R300_PRIM_NUM_VERTICES_SHIFT);
      r300_pm4_packet3(&b, R300_PACKET3_3D_DRAW_VBUF_2, &draw, 1);
      emitted_triangle_count += segment_triangle_count;
   }

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
r300_tcl_bypass_triangle_emit_into(
   const struct r300_tcl_bypass_triangle_params *params, uint32_t *words,
   uint32_t capacity, struct r300_tcl_bypass_triangle_ib *out)
{
   const uint32_t triangle_count =
      params->triangle_count ? params->triangle_count : 1u;
   if (triangle_count > R300_TRIANGLE_MAX_TRIANGLES)
      return -EINVAL;
   return emit_triangle_stream_into(params, words, capacity, triangle_count,
                                    out);
}

static int
emit_triangle_stream(const struct r300_tcl_bypass_triangle_params *params,
                     uint32_t triangle_count,
                     struct r300_tcl_bypass_triangle_ib *out)
{
   const struct r300_fragment_binary *fs = params->fragment_binary;

   memset(out, 0, sizeof(*out));
   if (fs == NULL || !fs->validated)
      return -EINVAL;

   const uint32_t capacity = R300_TRIANGLE_MAX_DWORDS + fs->cb_code_size;
   uint32_t *ib = calloc(capacity, sizeof(uint32_t));
   if (ib == NULL)
      return -ENOMEM;

   const int rc = emit_triangle_stream_into(params, ib, capacity,
                                            triangle_count, out);
   if (rc != 0) {
      free(ib);
      return rc;
   }
   out->owns_ib = true;
   return 0;
}

int
r300_tcl_bypass_triangle_emit(
   const struct r300_tcl_bypass_triangle_params *params,
   struct r300_tcl_bypass_triangle_ib *out)
{
   const uint32_t triangle_count =
      params->triangle_count ? params->triangle_count : 1u;
   if (triangle_count > R300_TRIANGLE_MAX_TRIANGLES)
      return -EINVAL;
   return emit_triangle_stream(params, triangle_count, out);
}

void
r300_tcl_bypass_triangle_release(struct r300_tcl_bypass_triangle_ib *ib)
{
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

static bool
reloc_sequence_matches(const struct r300_tcl_bypass_triangle_ib *ib,
                       const uint32_t *slots, uint32_t slot_count)
{
   if (ib->reloc_site_count != slot_count)
      return false;
   for (uint32_t site_index = 0; site_index < slot_count; site_index++) {
      if (ib->reloc_sites[site_index].slot != slots[site_index])
         return false;
   }
   return true;
}

static bool
reloc_repeated_vertex_sequence_matches(
   const struct r300_tcl_bypass_triangle_ib *ib,
   const uint32_t *prefix_slots, uint32_t prefix_slot_count)
{
   if (ib->reloc_site_count < prefix_slot_count + 1u ||
       ib->reloc_site_count >
          prefix_slot_count + R300_TRIANGLE_CLIP_MAX_DRAW_SEGMENTS)
      return false;
   for (uint32_t site_index = 0; site_index < prefix_slot_count;
        site_index++) {
      if (ib->reloc_sites[site_index].slot != prefix_slots[site_index])
         return false;
   }
   for (uint32_t site_index = prefix_slot_count;
        site_index < ib->reloc_site_count; site_index++) {
      if (ib->reloc_sites[site_index].slot != R300_TRIANGLE_SLOT_VERTEX)
         return false;
   }
   return true;
}

int
r300_tcl_bypass_triangle_validate_reloc_sites(
   const struct r300_tcl_bypass_triangle_ib *ib)
{
   if (ib == NULL || ib->ib == NULL || ib->reloc_site_count < 2u ||
       ib->reloc_site_count > R300_TRIANGLE_MAX_RELOC_SITES)
      return -EINVAL;

   /* Every site is the payload of a relocation NOP and initially names its
    * semantic slot.  Expanded render and sampled cells intentionally repeat
    * the vertex slot once per draw segment; the exact sequence check below
    * distinguishes those repetitions from malformed duplicates.
    */
   static_assert(R300_TRIANGLE_SLOT_COUNT <= 32,
                 "slot uniqueness is proven in a 32-bit mask");
   uint32_t seen = 0;
   bool repeated_slot = false;
   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_tcl_bypass_triangle_reloc_site *site =
         &ib->reloc_sites[i];
      if (site->slot >= R300_TRIANGLE_SLOT_COUNT)
         return -EINVAL;
      if ((seen & (1u << site->slot)) != 0)
         repeated_slot = true;
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

   /* The ordinary and expanded relocation lists follow the stream: target
    * state precedes one to seven vertex-array bindings.  Sampled state adds
    * the texture before the target.  The composed and multisample forms stay
    * exact because their repeated slots describe different passes rather
    * than homogeneous clip-capacity segments.
    */
   static const uint32_t render_prefix[] = {
      R300_TRIANGLE_SLOT_COLOR,
   };
   static const uint32_t sampled_prefix[] = {
      R300_TRIANGLE_SLOT_TEXTURE,
      R300_TRIANGLE_SLOT_COLOR,
   };
   static const uint32_t composed_slots[] = {
      R300_TRIANGLE_SLOT_COLOR,
      R300_TRIANGLE_SLOT_VERTEX,
      R300_TRIANGLE_SLOT_TEXTURE,
      R300_TRIANGLE_SLOT_COMPOSED_COLOR,
      R300_TRIANGLE_SLOT_COMPOSED_VERTEX,
   };
   static const uint32_t msaa_slots[] = {
      R300_TRIANGLE_SLOT_COLOR,
      R300_TRIANGLE_SLOT_VERTEX,
      R300_TRIANGLE_SLOT_COMPOSED_COLOR,
      R300_TRIANGLE_SLOT_TEXTURE,
      R300_TRIANGLE_SLOT_COMPOSED_VERTEX,
   };
   static const uint32_t msaa_clear_slots[] = {
      R300_TRIANGLE_SLOT_COLOR,
      R300_TRIANGLE_SLOT_COMPOSED_VERTEX,
      R300_TRIANGLE_SLOT_COLOR,
      R300_TRIANGLE_SLOT_VERTEX,
      R300_TRIANGLE_SLOT_COMPOSED_COLOR,
      R300_TRIANGLE_SLOT_TEXTURE,
      R300_TRIANGLE_SLOT_COMPOSED_VERTEX,
   };

   const bool sequence_matches =
      reloc_repeated_vertex_sequence_matches(
         ib, render_prefix, ARRAY_SIZE(render_prefix)) ||
      reloc_repeated_vertex_sequence_matches(
         ib, sampled_prefix, ARRAY_SIZE(sampled_prefix)) ||
      reloc_sequence_matches(ib, composed_slots, ARRAY_SIZE(composed_slots)) ||
      reloc_sequence_matches(ib, msaa_slots, ARRAY_SIZE(msaa_slots)) ||
      reloc_sequence_matches(ib, msaa_clear_slots,
                             ARRAY_SIZE(msaa_clear_slots));
   if (!sequence_matches)
      return repeated_slot ? -EEXIST : -EINVAL;

   /* Every cell's relocations follow the stream, so the sites rise
    * whichever exact sequence matched.
    */
   for (uint32_t i = 1; i < ib->reloc_site_count; i++)
      if (ib->reloc_sites[i - 1].ib_index >= ib->reloc_sites[i].ib_index)
         return -EINVAL;
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
r300_tcl_bypass_triangle_sampled_fs(struct r300_fragment_binary *fs)
{
   /* The sampled US program fetches texture unit 0 at interpolator 0's
    * coordinate and writes the fetched texel to the color output; the
    * TX block the sampled cell emits resolves the fetch.
    */
   return r300_fragment_binary_init(
      fs, r300_tcl_bypass_sampled_fs_block,
      sizeof(r300_tcl_bypass_sampled_fs_block) /
         sizeof(r300_tcl_bypass_sampled_fs_block[0]),
      R300_TCL_BYPASS_SAMPLED_FS_FG_DEPTH_SRC,
      R300_TCL_BYPASS_SAMPLED_FS_US_OUT_W,
      "r300-sampled-texture-compiled");
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
family_emit_triangle_stream(
   uint32_t width, uint32_t height, bool varying, uint32_t triangle_count,
   const struct r300_flat_color0_plan *flat_color0,
   const struct r300_rs_tex_adj_probe_plan *rs_tex_adj,
   struct r300_tcl_bypass_triangle_ib *out)
{
   if (flat_color0 != NULL && rs_tex_adj != NULL)
      return -EINVAL;
   if (flat_color0 != NULL || rs_tex_adj != NULL)
      varying = true;
   if (width < 1 || width > R300_TRIANGLE_TARGET_WIDTH || height < 1 ||
       height > R300_TRIANGLE_TARGET_HEIGHT || triangle_count < 1 ||
       triangle_count > R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES)
      return -EINVAL;

   struct r300_fragment_binary fs;
   int rc = varying ? r300_tcl_bypass_triangle_varying_fs(&fs)
                    : r300_tcl_bypass_triangle_reference_fs(&fs);
   if (rc != 0)
      return rc;

   /* The extent and the vertex-index bound parameterize the contract's
    * GEOMETRY_PARAMETER entries alone; the pitch word stays the
    * reference cell's, so the row layout and every other register
    * class are the qualified bytes.
    */
   struct r300_first_draw_params draw_params = {
      .chip_family = CHIP_RS480,
      .width = width,
      .height = height,
      .min_vtx_index = 0,
      .max_vtx_index = first_segment_max_vertex_index(triangle_count),
      .texture_enabled = false,
   };
   struct r300_first_draw_contract contract;
   rc = r300_first_draw_contract_resolve(&draw_params, &contract);
   if (rc != 0) {
      r300_fragment_binary_finish(&fs);
      return rc;
   }
   rc = r300_tcl_bypass_triangle_set_target_format(&contract);
   if (rc == 0 && flat_color0 != NULL)
      rc = r300_flat_color0_plan_apply_contract(flat_color0, &contract);
   if (rc == 0 && rs_tex_adj != NULL)
      rc = r300_rs_tex_adj_probe_plan_apply_contract(rs_tex_adj, &contract);
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
      .flat_color0 = flat_color0,
      .rs_tex_adj = rs_tex_adj,
      .triangle_count = triangle_count,
   };
   rc = emit_triangle_stream(&params, triangle_count, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

static int
family_emit(uint32_t width, uint32_t height, bool varying,
            uint32_t triangle_count, struct r300_tcl_bypass_triangle_ib *out)
{
   if (triangle_count < 1u ||
       triangle_count > R300_TRIANGLE_MAX_TRIANGLES)
      return -EINVAL;
   return family_emit_triangle_stream(width, height, varying, triangle_count,
                                      NULL, NULL, out);
}

static int
extent_emit(uint32_t width, uint32_t height, bool varying,
            struct r300_tcl_bypass_triangle_ib *out)
{
   return family_emit(width, height, varying, 1, out);
}

int
r300_tcl_bypass_triangle_family_emit(
   uint32_t width, uint32_t height, bool varying, uint32_t triangle_count,
   struct r300_tcl_bypass_triangle_ib *out)
{
   return family_emit(width, height, varying, triangle_count, out);
}

int
r300_tcl_bypass_triangle_clip_space_family_emit(
   uint32_t width, uint32_t height, bool varying,
   uint32_t source_triangle_count,
   struct r300_tcl_bypass_triangle_ib *out)
{
   uint32_t output_triangle_count;
   const int rc = clip_output_triangle_count(source_triangle_count,
                                             &output_triangle_count);
   if (rc != 0)
      return rc;
   return family_emit_triangle_stream(width, height, varying,
                                      output_triangle_count, NULL, NULL,
                                      out);
}

int
r300_tcl_bypass_triangle_flat_color0_plan_emit(
   uint32_t width, uint32_t height, bool clip_space,
   uint32_t triangle_count, const struct r300_flat_color0_plan *plan,
   struct r300_tcl_bypass_triangle_ib *out)
{
   if (plan == NULL || out == NULL)
      return -EINVAL;
   uint32_t output_triangle_count = triangle_count;
   if (clip_space) {
      const int rc =
         clip_output_triangle_count(triangle_count, &output_triangle_count);
      if (rc != 0)
         return rc;
   } else if (triangle_count < 1u ||
              triangle_count > R300_TRIANGLE_MAX_TRIANGLES) {
      return -EINVAL;
   }
   return family_emit_triangle_stream(width, height, true,
                                      output_triangle_count, plan, NULL,
                                      out);
}

int
r300_tcl_bypass_triangle_rs_tex_adj_plan_emit(
   uint32_t width, uint32_t height, bool clip_space,
   uint32_t triangle_count, const struct r300_rs_tex_adj_probe_plan *plan,
   struct r300_tcl_bypass_triangle_ib *out)
{
   if (plan == NULL || out == NULL)
      return -EINVAL;
   uint32_t output_triangle_count = triangle_count;
   if (clip_space) {
      const int rc =
         clip_output_triangle_count(triangle_count, &output_triangle_count);
      if (rc != 0)
         return rc;
   } else if (triangle_count < 1u ||
              triangle_count > R300_TRIANGLE_MAX_TRIANGLES) {
      return -EINVAL;
   }
   return family_emit_triangle_stream(width, height, true,
                                      output_triangle_count, NULL, plan,
                                      out);
}

int
r300_tcl_bypass_triangle_rs_tex_adj_family_emit(
   uint32_t width, uint32_t height, bool clip_space,
   uint32_t triangle_count, const struct r300_rs_tex_adj_probe_plan *plan,
   struct r300_tcl_bypass_triangle_ib *out)
{
   if (out != NULL)
      memset(out, 0, sizeof(*out));
   const int rc = r300_rs_tex_adj_probe_plan_validate(plan);
   if (rc != 0)
      return rc;
   return r300_tcl_bypass_triangle_rs_tex_adj_plan_emit(
      width, height, clip_space, triangle_count, plan, out);
}

int
r300_tcl_bypass_triangle_flat_color0_family_emit(
   uint32_t width, uint32_t height, bool clip_space,
   uint32_t triangle_count, const struct r300_flat_color0_plan *plan,
   struct r300_tcl_bypass_triangle_ib *out)
{
   if (out != NULL)
      memset(out, 0, sizeof(*out));
   const int rc = r300_flat_color0_plan_validate(plan);
   if (rc != 0)
      return rc;
   return r300_tcl_bypass_triangle_flat_color0_plan_emit(
      width, height, clip_space, triangle_count, plan, out);
}

static int
sampled_emit_triangle_stream(
   uint32_t width, uint32_t height, uint32_t triangle_count,
   uint32_t texture_offset, uint32_t texture_width,
   uint32_t texture_height, uint32_t texture_pitch_texels,
   enum r300_triangle_lane_order texture_lanes,
   struct r300_tcl_bypass_triangle_ib *out)
{
   if (width < 1 || width > R300_TRIANGLE_TARGET_WIDTH || height < 1 ||
       height > R300_TRIANGLE_TARGET_HEIGHT || triangle_count < 1 ||
       triangle_count > R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES)
      return -EINVAL;

   struct r300_fragment_binary fs;
   int rc = r300_tcl_bypass_triangle_sampled_fs(&fs);
   if (rc != 0)
      return rc;

   /* The sampled cell owns the TX block on top of the contract, so the
    * contract skips its TX writes and the cell's own enable stands.
    */
   struct r300_first_draw_params draw_params = {
      .chip_family = CHIP_RS480,
      .width = width,
      .height = height,
      .min_vtx_index = 0,
      .max_vtx_index = first_segment_max_vertex_index(triangle_count),
      .texture_enabled = true,
   };
   struct r300_first_draw_contract contract;
   rc = r300_first_draw_contract_resolve(&draw_params, &contract);
   if (rc == 0)
      rc = r300_tcl_bypass_triangle_set_target_format(&contract);
   if (rc != 0) {
      r300_fragment_binary_finish(&fs);
      return rc;
   }

   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(
         R300_TRIANGLE_TARGET_PITCH_PIXELS),
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
      .varying = true,
      .sampled = true,
      .texture_offset = texture_offset,
      .texture_width = texture_width,
      .texture_height = texture_height,
      .texture_pitch_texels = texture_pitch_texels,
      .texture_lanes = texture_lanes,
      .triangle_count = triangle_count,
   };
   rc = emit_triangle_stream(&params, triangle_count, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

int
r300_tcl_bypass_triangle_sampled_emit(
   uint32_t width, uint32_t height, uint32_t triangle_count,
   uint32_t texture_offset, uint32_t texture_width,
   uint32_t texture_height, uint32_t texture_pitch_texels,
   enum r300_triangle_lane_order texture_lanes,
   struct r300_tcl_bypass_triangle_ib *out)
{
   if (triangle_count < 1u ||
       triangle_count > R300_TRIANGLE_MAX_TRIANGLES)
      return -EINVAL;
   return sampled_emit_triangle_stream(
      width, height, triangle_count, texture_offset, texture_width,
      texture_height, texture_pitch_texels, texture_lanes, out);
}

int
r300_tcl_bypass_triangle_clip_space_sampled_emit(
   uint32_t width, uint32_t height, uint32_t source_triangle_count,
   uint32_t texture_offset, uint32_t texture_width,
   uint32_t texture_height, uint32_t texture_pitch_texels,
   enum r300_triangle_lane_order texture_lanes,
   struct r300_tcl_bypass_triangle_ib *out)
{
   uint32_t output_triangle_count;
   const int rc = clip_output_triangle_count(source_triangle_count,
                                             &output_triangle_count);
   if (rc != 0)
      return rc;
   return sampled_emit_triangle_stream(
      width, height, output_triangle_count, texture_offset, texture_width,
      texture_height, texture_pitch_texels, texture_lanes, out);
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

static uint32_t unorm8_round(float value);

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

void
r300_tcl_bypass_triangle_window_vertices(uint32_t width, uint32_t height,
                                         float out[6])
{
   const struct triangle_geometry g = triangle_geometry_at(width, height);
   memcpy(out, g.v, sizeof(g.v));
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
r300_tcl_bypass_triangle_render_shape_oracle(
   const struct r300_triangle_render_shape *shape, const uint32_t *pixels,
   uint32_t size_bytes, struct r300_triangle_oracle_verdict *verdict)
{
   /* The verdict producer admits the same domain the emitter admits: a
    * shape outside it fails every pass with zero samples, so an
    * inadmissible call reads as a failed verdict rather than dividing
    * by a zero edge length or wrapping the extent arithmetic.
    */
   if (shape == NULL ||
       r300_tcl_bypass_triangle_render_shape_validate(shape) != 0) {
      *verdict = (struct r300_triangle_oracle_verdict){ 0 };
      return;
   }
   const uint32_t width = shape->width, height = shape->height;
   const uint32_t pitch = shape->pitch_pixels;
   const uint32_t draw_color =
      r300_tcl_bypass_triangle_render_shape_draw_dword(shape);

   /* The verdict reads the full retained footprint: every rendered row
    * at the pitch plus the canary row past the render extent.  A
    * buffer short of that footprint carries no observable canary band,
    * so the truncated call fails closed before any pass initializes
    * rather than leaving canary_pass vacuously true.
    */
   const uint64_t required_bytes =
      (uint64_t)shape->target_offset +
      (uint64_t)pitch * (height + 1u) * sizeof(uint32_t);
   if (pixels == NULL || size_bytes < required_bytes) {
      *verdict = (struct r300_triangle_oracle_verdict){ 0 };
      return;
   }

   /* Render row 0 sits at the target offset, so the rendered rows index
    * from there while the dwords below it stay part of the untouched
    * band the canary reads.
    */
   const uint32_t offset_dwords = shape->target_offset / 4u;
   const uint32_t *rows = pixels + offset_dwords;
   const uint32_t row_count = size_bytes / 4u - offset_dwords;

   const struct triangle_geometry g = triangle_geometry_at(width, height);

   *verdict = (struct r300_triangle_oracle_verdict) {
      .judged = true,
      .interior_pass = true,
      .exterior_pass = true,
      .canary_pass = true,
   };

   const uint32_t pixel_count = row_count;
   for (uint32_t i = 0; i < size_bytes / 4u; i++) {
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
      const uint32_t index = y * pitch + x;
      if (index >= pixel_count ||
          rows[index] != draw_color)
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
      const uint32_t index = y * pitch + x;
      if (index >= pixel_count ||
          rows[index] != R300_TRIANGLE_COLOR_SENTINEL)
         verdict->exterior_pass = false;
   }
   if (verdict->exterior_samples == 0)
      verdict->exterior_pass = false;

   /* Canary: every dword below the target offset, the sub-pitch
    * padding band of every rendered row, then every row past the render
    * extent, all at the sentinel.
    */
   for (uint32_t i = 0; i < offset_dwords; i++) {
      if (pixels[i] != R300_TRIANGLE_COLOR_SENTINEL)
         verdict->canary_pass = false;
   }
   for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = width; x < pitch; x++) {
         const uint32_t index = y * pitch + x;
         if (index < pixel_count &&
             rows[index] != R300_TRIANGLE_COLOR_SENTINEL)
            verdict->canary_pass = false;
      }
   }
   for (uint32_t i = pitch * height; i < pixel_count; i++) {
      if (rows[i] != R300_TRIANGLE_COLOR_SENTINEL)
         verdict->canary_pass = false;
   }
}

void
r300_tcl_bypass_triangle_extent_oracle(
   uint32_t width, uint32_t height, const uint32_t *pixels,
   uint32_t size_bytes, struct r300_triangle_oracle_verdict *verdict)
{
   /* The fixed-pitch oracle is the reference shape at this extent; its
    * domain stays the extent emitter's, so an extent past 64 fails
    * closed here even though the render-shape family admits it.
    */
   if (width < 1 || width > R300_TRIANGLE_TARGET_WIDTH || height < 1 ||
       height > R300_TRIANGLE_TARGET_HEIGHT) {
      *verdict = (struct r300_triangle_oracle_verdict){ 0 };
      return;
   }
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.width = width;
   shape.height = height;
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, pixels, size_bytes,
                                                verdict);
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

/* Classifies one pixel center against the triangle: 1 interior, 0
 * exterior, -1 exactly on an edge.  The three edge functions share a
 * sign inside the triangle whichever winding the vertices carry.
 */
static int
triangle_center_class(const struct triangle_geometry *g, float px, float py)
{
   const float e[3] = {
      triangle_edgef(g->v[0], g->v[1], g->v[2], g->v[3], px, py),
      triangle_edgef(g->v[2], g->v[3], g->v[4], g->v[5], px, py),
      triangle_edgef(g->v[4], g->v[5], g->v[0], g->v[1], px, py),
   };
   for (unsigned i = 0; i < 3; i++) {
      if (e[i] == 0.0f)
         return -1;
   }
   const bool positive = e[0] > 0.0f && e[1] > 0.0f && e[2] > 0.0f;
   const bool negative = e[0] < 0.0f && e[1] < 0.0f && e[2] < 0.0f;
   return positive || negative ? 1 : 0;
}

void
r300_tcl_bypass_triangle_coverage_oracle(
   const struct r300_triangle_render_shape *shape,
   const uint32_t *interior_dwords, uint32_t interior_dword_count,
   uint32_t exterior_dword, const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_coverage_verdict *verdict)
{
   r300_tcl_bypass_triangle_coverage_oracle_predicted(
      shape, interior_dwords, interior_dword_count, NULL, NULL,
      exterior_dword, pixels, size_bytes, verdict);
}

void
r300_tcl_bypass_triangle_coverage_oracle_predicted(
   const struct r300_triangle_render_shape *shape,
   const uint32_t *interior_dwords, uint32_t interior_dword_count,
   r300_triangle_interior_expectation expectation, void *expectation_data,
   uint32_t exterior_dword, const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_coverage_verdict *verdict)
{
   /* The verdict reads the shape's extent, pitch, and target base and
    * takes its interior values as arguments, so geometry is the whole
    * admission; a cell whose fragment color arrives through the TX unit
    * carries no R300_PFS_PARAM_0 constant to judge.  An inadmissible
    * call leaves judged false rather than indexing past the buffer.
    */
   *verdict = (struct r300_triangle_coverage_verdict){ 0 };
   if (shape == NULL || pixels == NULL ||
       (expectation == NULL &&
        (interior_dwords == NULL || interior_dword_count == 0)) ||
       r300_tcl_bypass_triangle_render_shape_validate_geometry(shape) != 0)
      return;
   const uint32_t width = shape->width, height = shape->height;
   const uint32_t pitch = shape->pitch_pixels;
   const uint64_t required_bytes =
      (uint64_t)shape->target_offset +
      (uint64_t)pitch * (height + R300_TRIANGLE_CANARY_ROWS) * sizeof(uint32_t);
   if (size_bytes < required_bytes)
      return;

   const struct triangle_geometry g = triangle_geometry_at(width, height);
   const uint32_t offset_dwords = shape->target_offset / 4u;
   const uint32_t *rows = pixels + offset_dwords;
   verdict->judged = true;
   verdict->coverage_exact = true;
   verdict->canary_pass = true;

   for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
         const uint32_t observed = rows[y * pitch + x];
         const int expected =
            triangle_center_class(&g, (float)x + 0.5f, (float)y + 0.5f);
         /* The model predicts one dword per interior center, so a
          * pixel the model does not cover keeps the admitted-set test
          * and the exterior stays the exterior dword either way.
          */
         bool is_interior_value = false;
         if (expectation != NULL && expected == 1) {
            is_interior_value = observed == expectation(expectation_data, x, y);
         } else {
            for (uint32_t i = 0; i < interior_dword_count; i++)
               is_interior_value |= observed == interior_dwords[i];
         }
         if (expected < 0)
            verdict->ambiguous_pixels++;
         else if (expected == 1)
            verdict->analytic_pixels++;
         if (is_interior_value)
            verdict->interior_pixels++;
         else if (observed == exterior_dword)
            verdict->exterior_pixels++;
         else
            verdict->mismatch_pixels++;
      }
   }
   if (verdict->mismatch_pixels != 0 || verdict->ambiguous_pixels != 0 ||
       verdict->interior_pixels != verdict->analytic_pixels)
      verdict->coverage_exact = false;

   /* The bytes below render row 0 and the canary rows past the extent
    * carry no draw, so each holds the exterior dword; a device write
    * outside the rendered rows is as observable as one inside them.
    */
   for (uint32_t i = 0; i < offset_dwords; i++) {
      if (pixels[i] != exterior_dword)
         verdict->canary_pass = false;
   }
   const uint32_t footprint_dwords =
      pitch * (height + R300_TRIANGLE_CANARY_ROWS);
   for (uint32_t i = height * pitch; i < footprint_dwords; i++) {
      if (rows[i] != exterior_dword)
         verdict->canary_pass = false;
   }
   /* Every column past the render extent in a rendered row lies outside
    * the draw for the same reason, so the pitch padding joins the
    * canary rather than the classified band.
    */
   for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = width; x < pitch; x++) {
         if (rows[y * pitch + x] != exterior_dword)
            verdict->canary_pass = false;
      }
   }
}

void
r300_tcl_bypass_triangle_interior_oracle(
   const struct r300_triangle_render_shape *shape,
   const uint32_t *interior_dwords, uint32_t interior_dword_count,
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_interior_verdict *verdict)
{
   *verdict = (struct r300_triangle_interior_verdict){ 0 };
   if (shape == NULL || pixels == NULL || interior_dwords == NULL ||
       interior_dword_count == 0 ||
       r300_tcl_bypass_triangle_render_shape_validate_geometry(shape) != 0)
      return;
   /* The rendered rows alone are read, so the footprint stops at the
    * render extent; the canary row this verdict leaves unjudged stays
    * outside its bound.
    */
   const uint32_t width = shape->width, height = shape->height;
   const uint32_t pitch = shape->pitch_pixels;
   const uint64_t required_bytes = (uint64_t)shape->target_offset +
                                   (uint64_t)pitch * height * sizeof(uint32_t);
   if (size_bytes < required_bytes)
      return;

   verdict->judged = true;
   const struct triangle_geometry g = triangle_geometry_at(width, height);
   const uint32_t *rows = pixels + shape->target_offset / 4u;
   for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
         const int expected =
            triangle_center_class(&g, (float)x + 0.5f, (float)y + 0.5f);
         if (expected < 0) {
            verdict->ambiguous_pixels++;
            continue;
         }
         if (expected != 1)
            continue;
         verdict->analytic_pixels++;
         const uint32_t observed = rows[y * pitch + x];
         for (uint32_t i = 0; i < interior_dword_count; i++) {
            if (observed == interior_dwords[i]) {
               verdict->interior_pixels++;
               break;
            }
         }
      }
   }
   verdict->interior_exact = verdict->analytic_pixels != 0 &&
                             verdict->ambiguous_pixels == 0 &&
                             verdict->interior_pixels ==
                                verdict->analytic_pixels;
}

/* The sample sets r300g programs into GB_MSPOS0/GB_MSPOS1 for each
 * multisample mode (r300_emit_fb_state_pipelined), in 1/12 subpixel
 * units.  Sample count 1 is the pixel center.
 */
uint32_t
r300_tcl_bypass_triangle_subsample_positions(
   uint32_t sample_count, uint8_t positions[R300_TRIANGLE_MAX_SUBSAMPLES][2])
{
   static const uint8_t locs_1x[1][2] = { { 6, 6 } };
   static const uint8_t locs_2x[2][2] = { { 3, 9 }, { 9, 3 } };
   static const uint8_t locs_4x[4][2] = {
      { 4, 4 }, { 8, 8 }, { 2, 10 }, { 10, 2 }
   };
   const uint8_t (*src)[2];
   switch (sample_count) {
   case 1: src = locs_1x; break;
   case 2: src = locs_2x; break;
   case 4: src = locs_4x; break;
   default: return 0;
   }
   if (positions != NULL)
      memcpy(positions, src, sample_count * 2 * sizeof(uint8_t));
   return sample_count;
}

/* The margin a subsample clears an edge by before the verdict judges
 * its pixel.  The judged counts hold across margins from 1/64 to 1/16
 * pixel at the reference geometry, so the value sits on a plateau
 * rather than on a threshold, and it stands far above the float32 edge
 * evaluation's error at these magnitudes.
 */
#define R300_TRIANGLE_SAMPLE_MARGIN 0.0625f

/* One walk serves both sample-set verdicts: a pixel whose every
 * subsample clears the edges inward is the interior denominator, one
 * whose every subsample clears them outward is the exterior denominator,
 * and the edge band between them is unjudged under either.
 */
static void
sample_set_classify(const struct r300_triangle_render_shape *shape,
                    uint32_t sample_count, bool exterior,
                    const uint32_t *admitted_dwords,
                    uint32_t admitted_dword_count, const uint32_t *pixels,
                    uint32_t size_bytes,
                    struct r300_triangle_sample_set_verdict *verdict)
{
   *verdict = (struct r300_triangle_sample_set_verdict){ 0 };
   uint8_t positions[R300_TRIANGLE_MAX_SUBSAMPLES][2];
   const uint32_t samples =
      r300_tcl_bypass_triangle_subsample_positions(sample_count, positions);
   const uint32_t *interior_dwords = admitted_dwords;
   const uint32_t interior_dword_count = admitted_dword_count;
   if (shape == NULL || pixels == NULL || interior_dwords == NULL ||
       interior_dword_count == 0 || samples == 0 ||
       r300_tcl_bypass_triangle_render_shape_validate_geometry(shape) != 0)
      return;
   const uint32_t width = shape->width, height = shape->height;
   const uint32_t pitch = shape->pitch_pixels;
   const uint64_t required_bytes = (uint64_t)shape->target_offset +
                                   (uint64_t)pitch * height * sizeof(uint32_t);
   if (size_bytes < required_bytes)
      return;

   verdict->judged = true;
   const struct triangle_geometry g = triangle_geometry_at(width, height);
   const uint32_t *rows = pixels + shape->target_offset / 4u;
   const float grid = (float)R300_TRIANGLE_SUBPIXEL_GRID;
   for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
         uint32_t inside = 0, outside = 0;
         for (uint32_t s = 0; s < samples; s++) {
            const float margin = triangle_signed_margin(
               &g, (float)x + (float)positions[s][0] / grid,
               (float)y + (float)positions[s][1] / grid);
            if (margin >= R300_TRIANGLE_SAMPLE_MARGIN)
               inside++;
            else if (margin <= -R300_TRIANGLE_SAMPLE_MARGIN)
               outside++;
         }
         if (exterior ? inside == samples : outside == samples)
            continue;
         if ((exterior ? outside : inside) != samples) {
            verdict->unjudged_pixels++;
            continue;
         }
         verdict->analytic_pixels++;
         const uint32_t observed = rows[y * pitch + x];
         for (uint32_t i = 0; i < interior_dword_count; i++) {
            if (observed == interior_dwords[i]) {
               verdict->interior_pixels++;
               break;
            }
         }
      }
   }
   verdict->interior_exact = verdict->analytic_pixels != 0 &&
                             verdict->interior_pixels ==
                                verdict->analytic_pixels;
}

void
r300_tcl_bypass_triangle_sample_set_oracle(
   const struct r300_triangle_render_shape *shape, uint32_t sample_count,
   const uint32_t *interior_dwords, uint32_t interior_dword_count,
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_sample_set_verdict *verdict)
{
   sample_set_classify(shape, sample_count, false, interior_dwords,
                       interior_dword_count, pixels, size_bytes, verdict);
}

void
r300_tcl_bypass_triangle_sample_set_exterior_oracle(
   const struct r300_triangle_render_shape *shape, uint32_t sample_count,
   const uint32_t *exterior_dwords, uint32_t exterior_dword_count,
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_sample_set_verdict *verdict)
{
   sample_set_classify(shape, sample_count, true, exterior_dwords,
                       exterior_dword_count, pixels, size_bytes, verdict);
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
r300_tcl_bypass_triangle_varying_shape_vertices(
   const struct r300_triangle_render_shape *shape,
   float out[R300_TRIANGLE_VARYING_VERTEX_DWORDS])
{
   const struct triangle_geometry g =
      triangle_geometry_at(shape->width, shape->height);
   for (unsigned i = 0; i < 3; i++) {
      out[i * 8 + 0] = g.v[i * 2 + 0];
      out[i * 8 + 1] = g.v[i * 2 + 1];
      out[i * 8 + 2] = 0.0f;
      out[i * 8 + 3] = 1.0f;
      /* The TEX0 payload is normalized, so it rides every extent
       * unchanged; the reference records are this writer's output at
       * the reference extent.
       */
      for (unsigned c = 0; c < 4; c++) {
         out[i * 8 + 4 + c] =
            r300_tcl_bypass_triangle_varying_vertices[i * 8 + 4 + c];
      }
   }
}

static const uint32_t r300_triangle_reference_color_bits[4] = {
   0x3e000000u, 0x3ec00000u, 0x3f200000u, 0x3f600000u,
};

void
r300_tcl_bypass_triangle_render_shape_reference(
   struct r300_triangle_render_shape *out)
{
   *out = (struct r300_triangle_render_shape){
      .width = R300_TRIANGLE_TARGET_WIDTH,
      .height = R300_TRIANGLE_TARGET_HEIGHT,
      .pitch_pixels = R300_TRIANGLE_TARGET_PITCH_PIXELS,
      .lanes = R300_TRIANGLE_LANES_B8G8R8A8,
      .triangle_count = 1,
   };
   memcpy(out->color_bits, r300_triangle_reference_color_bits,
          sizeof(out->color_bits));
}

int
r300_tcl_bypass_triangle_render_shape_validate_geometry(
   const struct r300_triangle_render_shape *shape)
{
   if (shape == NULL || shape->width < 1 || shape->height < 1 ||
       shape->width > R300_TRIANGLE_RENDER_MAX_EXTENT ||
       shape->height > R300_TRIANGLE_RENDER_MAX_EXTENT ||
       shape->pitch_pixels < shape->width ||
       shape->pitch_pixels > R300_TRIANGLE_RENDER_MAX_EXTENT ||
       (shape->pitch_pixels % 8u) != 0 ||
       (shape->lanes != R300_TRIANGLE_LANES_B8G8R8A8 &&
        shape->lanes != R300_TRIANGLE_LANES_R8G8B8A8) ||
       /* The vertex writer (r300_tcl_bypass_triangle_render_shape_vertices)
        * writes one triangle's three records regardless of this field, so
        * a shape claiming more than one triangle would draw records the
        * writer never emits.
        */
       shape->triangle_count != 1 ||
       /* RB3D_COLOROFFSET encodes the base in bits 31:5 and the kernel
        * packet check adds the relocation base without masking, so a
        * base carrying a reserved low bit would reach the hardware as
        * an address the register cannot name.
        */
       (shape->target_offset % R300_TRIANGLE_TARGET_OFFSET_ALIGNMENT) != 0 ||
       shape->target_offset > R300_TRIANGLE_MAX_TARGET_OFFSET)
      return -EINVAL;
   return 0;
}

int
r300_tcl_bypass_triangle_render_shape_validate(
   const struct r300_triangle_render_shape *shape)
{
   if (r300_tcl_bypass_triangle_render_shape_validate_geometry(shape) != 0)
      return -EINVAL;
   /* R300_PFS_PARAM_0 holds the FP24 lattice value, so a constant off
    * the lattice (a NaN included, which quantizes to max finite) would
    * execute a value other than the one the emitter names.
    */
   for (unsigned i = 0; i < 4; i++) {
      if (r300_fp24_quantize_bits(shape->color_bits[i]) !=
          shape->color_bits[i])
         return -EINVAL;
   }
   return 0;
}

/* Rewrites the fragment block's single four-payload PFS_PARAM_0 packet
 * in place; a block carrying zero or several such packets leaves the
 * constant ambiguous and refuses.
 */
static int
rewrite_constant_payloads(uint32_t *block, uint32_t block_dwords,
                          const uint32_t color_bits[4])
{
   uint32_t found = 0;
   uint32_t i = 0;
   while (i < block_dwords) {
      const uint32_t header = block[i];
      const uint32_t count = ((header >> 16) & 0x3fffu) + 1u;
      const uint32_t reg = (header & 0x7fffu) << 2;
      if (i + 1 + count > block_dwords)
         return -EINVAL;
      if (reg == R300_PFS_PARAM_0_X && count == 4) {
         for (unsigned c = 0; c < 4; c++)
            block[i + 1 + c] = r300_fp24_register_bits(color_bits[c]);
         found++;
      }
      i += 1 + count;
   }
   return found == 1 ? 0 : -EINVAL;
}

int
r300_tcl_bypass_triangle_render_shape_fs(
   const struct r300_triangle_render_shape *shape,
   struct r300_fragment_binary *fs)
{
   int rc = r300_tcl_bypass_triangle_render_shape_validate(shape);
   if (rc != 0)
      return rc;
   uint32_t block[ARRAY_SIZE(r300_tcl_bypass_triangle_fs_block)];
   memcpy(block, r300_tcl_bypass_triangle_fs_block, sizeof(block));
   rc = rewrite_constant_payloads(block, ARRAY_SIZE(block),
                                  shape->color_bits);
   if (rc != 0)
      return rc;
   return r300_fragment_binary_init(
      fs, block, ARRAY_SIZE(block),
      R300_TCL_BYPASS_TRIANGLE_FS_FG_DEPTH_SRC,
      R300_TCL_BYPASS_TRIANGLE_FS_US_OUT_W,
      "r300-tcl-bypass-triangle-compiled");
}

/* The render-shape emission with an optional subsample declaration; a
 * NULL declaration is the single-sample contract.
 */
static int
render_shape_emit_aa(const struct r300_triangle_render_shape *shape,
                     const struct r300_triangle_multisample_state *aa,
                     uint32_t triangle_count,
                     struct r300_tcl_bypass_triangle_ib *out)
{
   memset(out, 0, sizeof(*out));
   if (triangle_count < 1u ||
       triangle_count > R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES)
      return -EINVAL;
   struct r300_fragment_binary fs;
   int rc = r300_tcl_bypass_triangle_render_shape_fs(shape, &fs);
   if (rc != 0)
      return rc;

   struct r300_first_draw_params draw_params = {
      .chip_family = CHIP_RS480,
      .width = shape->width,
      .height = shape->height,
      .min_vtx_index = 0,
      .max_vtx_index = first_segment_max_vertex_index(triangle_count),
      .texture_enabled = false,
   };
   /* The contract writes GB_AA_CONFIG, both GB_MSPOS words, and
    * RB3D_AARESOLVE_CTL, so the subsample set travels through it rather
    * than ahead of it: a set programmed before the contract is written
    * back to the single-sample values before this draw runs.
    */
   if (aa != NULL) {
      const uint32_t config =
         r300_tcl_bypass_triangle_gb_aa_config(aa->sample_count);
      if (config == 0) {
         r300_fragment_binary_finish(&fs);
         return -EINVAL;
      }
      draw_params.multisample = true;
      draw_params.gb_aa_config = config;
      draw_params.gb_mspos[0] =
         r300_tcl_bypass_triangle_gb_mspos(0, aa->sample_count);
      draw_params.gb_mspos[1] =
         r300_tcl_bypass_triangle_gb_mspos(1, aa->sample_count);
      draw_params.rb3d_aaresolve_ctl =
         aa->resolve ? (R300_RB3D_AARESOLVE_CTL_AARESOLVE_MODE_RESOLVE |
                        R300_RB3D_AARESOLVE_CTL_AARESOLVE_ALPHA_AVERAGE)
                     : R300_RB3D_AARESOLVE_CTL_AARESOLVE_MODE_NORMAL;
   }
   struct r300_first_draw_contract contract;
   rc = r300_first_draw_contract_resolve(&draw_params, &contract);
   if (rc == 0) {
      const uint32_t lanes =
         shape->lanes == R300_TRIANGLE_LANES_R8G8B8A8
            ? (R300_US_OUT_FMT_C4_8 | R300_C0_SEL_R | R300_C1_SEL_G |
               R300_C2_SEL_B | R300_C3_SEL_A)
            : (R300_US_OUT_FMT_C4_8 | R300_C0_SEL_B | R300_C1_SEL_G |
               R300_C2_SEL_R | R300_C3_SEL_A);
      rc = r300_first_draw_contract_set_us_out_fmt_0(&contract, lanes);
   }
   if (rc != 0) {
      r300_fragment_binary_finish(&fs);
      return rc;
   }

   const uint32_t pitch_word =
      r300_rb3d_colorpitch0_pack_argb8888(shape->pitch_pixels);
   if (pitch_word == 0) {
      r300_fragment_binary_finish(&fs);
      return -EINVAL;
   }
   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_offset = shape->target_offset,
      .color_pitch_format = pitch_word,
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
      .varying = false,
      .triangle_count = triangle_count,
   };
   rc = emit_triangle_stream(&params, triangle_count, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

int
r300_tcl_bypass_triangle_render_shape_emit(
   const struct r300_triangle_render_shape *shape,
   struct r300_tcl_bypass_triangle_ib *out)
{
   return render_shape_emit_aa(shape, NULL,
                               shape != NULL ? shape->triangle_count : 0u,
                               out);
}

int
r300_tcl_bypass_triangle_clip_space_render_shape_emit(
   const struct r300_triangle_render_shape *shape,
   uint32_t source_triangle_count,
   struct r300_tcl_bypass_triangle_ib *out)
{
   uint32_t output_triangle_count;
   const int rc = clip_output_triangle_count(source_triangle_count,
                                             &output_triangle_count);
   if (rc != 0)
      return rc;
   return render_shape_emit_aa(shape, NULL, output_triangle_count, out);
}

/* The US_OUT_FMT_0 word a shape's lane order names: the C*_SEL fields
 * place the shader's components into the target's bytes.
 */
static uint32_t
render_shape_out_fmt(const struct r300_triangle_render_shape *shape)
{
   return shape->lanes == R300_TRIANGLE_LANES_R8G8B8A8
             ? (R300_US_OUT_FMT_C4_8 | R300_C0_SEL_R | R300_C1_SEL_G |
                R300_C2_SEL_B | R300_C3_SEL_A)
             : (R300_US_OUT_FMT_C4_8 | R300_C0_SEL_B | R300_C1_SEL_G |
                R300_C2_SEL_R | R300_C3_SEL_A);
}

/* Emits the composed cell's second pass: the sampled cell over the
 * sample shape's target, reading the render shape's target as its
 * texture.
 */
static int
composed_sample_emit(const struct r300_triangle_composed_render_sample *c,
                     struct r300_tcl_bypass_triangle_ib *out)
{
   struct r300_fragment_binary fs;
   int rc = r300_tcl_bypass_triangle_sampled_fs(&fs);
   if (rc != 0)
      return rc;

   struct r300_first_draw_params draw_params = {
      .chip_family = CHIP_RS480,
      .width = c->sample.width,
      .height = c->sample.height,
      .min_vtx_index = 0,
      .max_vtx_index = 3 * c->sample.triangle_count - 1,
      .texture_enabled = true,
   };
   struct r300_first_draw_contract contract;
   rc = r300_first_draw_contract_resolve(&draw_params, &contract);
   if (rc == 0)
      rc = r300_first_draw_contract_set_us_out_fmt_0(
         &contract, render_shape_out_fmt(&c->sample));
   const uint32_t pitch_word =
      r300_rb3d_colorpitch0_pack_argb8888(c->sample.pitch_pixels);
   if (rc != 0 || pitch_word == 0) {
      r300_fragment_binary_finish(&fs);
      return rc != 0 ? rc : -EINVAL;
   }

   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_offset = c->sample.target_offset,
      .color_pitch_format = pitch_word,
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
      .varying = true,
      .sampled = true,
      .texture_offset = c->render.target_offset,
      .texture_width = c->render.width,
      .texture_height = c->render.height,
      .texture_pitch_texels = c->render.pitch_pixels,
      .texture_lanes = c->render.lanes,
      .triangle_count = c->sample.triangle_count,
   };
   rc = r300_tcl_bypass_triangle_emit(&params, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

const uint32_t
   r300_tcl_bypass_triangle_composed_slot_index[R300_TRIANGLE_SLOT_COUNT] = {
      [R300_TRIANGLE_SLOT_VERTEX] = 0,
      [R300_TRIANGLE_SLOT_COLOR] = 1,
      [R300_TRIANGLE_SLOT_TEXTURE] = 1,
      [R300_TRIANGLE_SLOT_COMPOSED_VERTEX] = 2,
      [R300_TRIANGLE_SLOT_COMPOSED_COLOR] = 3,
};

int
r300_tcl_bypass_triangle_bind_reloc_indices(
   struct r300_tcl_bypass_triangle_ib *ib, const uint32_t *slot_indices,
   uint32_t slot_index_count)
{
   if (ib == NULL || ib->ib == NULL || slot_indices == NULL ||
       slot_index_count > R300_TRIANGLE_SLOT_COUNT)
      return -EINVAL;
   /* The emitted form is the precondition: the validator proves every
    * site sits behind a relocation NOP and carries its own slot's
    * payload, so a cell already bound, or one whose sites were edited,
    * refuses here rather than taking a second remap.
    */
   const int valid = r300_tcl_bypass_triangle_validate_reloc_sites(ib);
   if (valid != 0)
      return valid;
   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      if (ib->reloc_sites[i].slot >= slot_index_count)
         return -EINVAL;
   }
   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_tcl_bypass_triangle_reloc_site *site =
         &ib->reloc_sites[i];
      ib->ib[site->ib_index] =
         R300_TRIANGLE_RELOC_PAYLOAD(slot_indices[site->slot]);
   }
   return 0;
}

/* The multisample cell's slot map after the winsys merges the
 * multisample surface's two use sites into one relocation entry.  The
 * texture slot stays unused; the resolve destination takes the composed
 * color slot, which is the second half's destination in both cells.
 */
const uint32_t
   r300_tcl_bypass_triangle_msaa_slot_index[R300_TRIANGLE_SLOT_COUNT] = {
      [R300_TRIANGLE_SLOT_VERTEX] = 0,
      [R300_TRIANGLE_SLOT_COLOR] = 1,
      [R300_TRIANGLE_SLOT_TEXTURE] = 1,
      [R300_TRIANGLE_SLOT_COMPOSED_VERTEX] = 2,
      [R300_TRIANGLE_SLOT_COMPOSED_COLOR] = 3,
};

/* The subsample sets r300g programs, in 1/12 subpixel units, as the six
 * (x, y) pairs GB_MSPOS0 and GB_MSPOS1 carry together
 * (r300_emit_fb_state_pipelined).  Counts below six repeat their last
 * live sample into the unused lanes, which is what keeps an unused lane
 * from naming a position outside the pixel.
 */
static const uint8_t *
msaa_sample_locs(uint32_t sample_count)
{
   static const uint8_t locs_2x[12] = { 3, 9, 9, 3, 9, 3, 9, 3, 9, 3, 9, 3 };
   static const uint8_t locs_4x[12] = { 4,  4, 8, 8, 2, 10,
                                        10, 2, 10, 2, 10, 2 };
   switch (sample_count) {
   case 2: return locs_2x;
   case 4: return locs_4x;
   default: return NULL;
   }
}

uint32_t
r300_tcl_bypass_triangle_gb_aa_config(uint32_t sample_count)
{
   switch (sample_count) {
   case 2:
      return R300_GB_AA_CONFIG_AA_ENABLE |
             R300_GB_AA_CONFIG_NUM_AA_SUBSAMPLES_2;
   case 4:
      return R300_GB_AA_CONFIG_AA_ENABLE |
             R300_GB_AA_CONFIG_NUM_AA_SUBSAMPLES_4;
   default:
      return 0;
   }
}

#define MSAA_NIBBLES(x0, y0, x1, y1, x2, y2, d0y, d0x)                  \
   (((uint32_t)(x0) & 0xf) | (((uint32_t)(y0) & 0xf) << 4) |            \
    (((uint32_t)(x1) & 0xf) << 8) | (((uint32_t)(y1) & 0xf) << 12) |    \
    (((uint32_t)(x2) & 0xf) << 16) | (((uint32_t)(y2) & 0xf) << 20) |   \
    (((uint32_t)(d0y) & 0xf) << 24) | (((uint32_t)(d0x) & 0xf) << 28))

uint32_t
r300_tcl_bypass_triangle_gb_mspos(uint32_t index, uint32_t sample_count)
{
   const uint8_t *p = msaa_sample_locs(sample_count);
   if (p == NULL || index > 1)
      return 0;
   if (index == 0) {
      /* D0_X carries the distance from the pixel quad's left edge to the
       * first sample in subpixels; the hardware converts an encoded 7
       * into 8, so a true distance of 8 encodes as 7.
       */
      uint32_t distx = 11, disty = 11;
      for (unsigned i = 0; i < 12; i += 2)
         if (p[i] < distx)
            distx = p[i];
      for (unsigned i = 1; i < 12; i += 2)
         if (p[i] < disty)
            disty = p[i];
      if (distx == 8)
         distx = 7;
      return MSAA_NIBBLES(p[0], p[1], p[2], p[3], p[4], p[5], disty, distx);
   }
   uint32_t dist = 11;
   for (unsigned i = 0; i < 12; i++)
      if (p[i] < dist)
         dist = p[i];
   return MSAA_NIBBLES(p[6], p[7], p[8], p[9], p[10], p[11], dist, 0);
}

void
r300_tcl_bypass_triangle_cover_vertices(
   const struct r300_triangle_render_shape *shape,
   float out[R300_TRIANGLE_VERTEX_DWORDS])
{
   const float w = (float)shape->width, h = (float)shape->height;
   const float v[6] = { 0.0f, 0.0f, 2.0f * w, 0.0f, 0.0f, 2.0f * h };
   for (unsigned i = 0; i < 3; i++) {
      out[i * 4 + 0] = v[i * 2 + 0];
      out[i * 4 + 1] = v[i * 2 + 1];
      out[i * 4 + 2] = 0.0f;
      out[i * 4 + 3] = 1.0f;
   }
}

int
r300_tcl_bypass_triangle_msaa_resolve_emit(
   const struct r300_triangle_msaa_resolve *msaa,
   struct r300_tcl_bypass_triangle_ib *out)
{
   memset(out, 0, sizeof(*out));
   /* The render half's color_bits reach R300_PFS_PARAM_0, so that shape
    * takes the full predicate.  The resolve destination reaches the cell
    * through RB3D_AARESOLVE_OFFSET and RB3D_AARESOLVE_PITCH alone and
    * inherits its format from color buffer 0, so its geometry is the
    * whole predicate and its color_bits carry the oracle's expectation
    * rather than an emitted constant.
    */
   if (msaa == NULL ||
       r300_tcl_bypass_triangle_render_shape_validate(&msaa->render) != 0 ||
       r300_tcl_bypass_triangle_render_shape_validate_geometry(
          &msaa->destination) != 0 ||
       r300_tcl_bypass_triangle_gb_aa_config(msaa->sample_count) == 0)
      return -EINVAL;
   /* RB3D_AARESOLVE_PITCH holds a raw pixel pitch in bits 1 through 13,
    * and RB3D_AARESOLVE_OFFSET a base in bits 31:5, so a pitch outside
    * the mask or an unaligned destination offset names a word the
    * register cannot carry.
    */
   if ((msaa->destination.pitch_pixels &
        ~(uint32_t)R300_RB3D_AARESOLVE_PITCH_MASK) != 0 ||
       (msaa->destination.target_offset &
        ~(uint32_t)R300_RB3D_AARESOLVE_OFFSET_MASK) != 0)
      return -EINVAL;

   /* The resolve half renders into the multisample surface again, so it
    * takes the render half's extent, pitch, lane order, and target
    * offset and only its fragment constant differs.
    */
   struct r300_triangle_render_shape resolve_shape = msaa->render;
   memcpy(resolve_shape.color_bits, msaa->resolve_color_bits,
          sizeof(resolve_shape.color_bits));

   /* Each half declares its own subsample state, so the values reach the
    * stream through that half's first-draw contract.  The contract
    * writes GB_AA_CONFIG, both GB_MSPOS words, and RB3D_AARESOLVE_CTL,
    * so a set programmed ahead of it is written back to the
    * single-sample values before the draw the set was meant for.
    */
   const struct r300_triangle_multisample_state render_aa = {
      .sample_count = msaa->sample_count,
      .resolve = false,
   };
   const struct r300_triangle_multisample_state resolve_aa = {
      .sample_count = msaa->sample_count,
      .resolve = true,
   };
   struct r300_tcl_bypass_triangle_ib clear_half = { 0 };
   struct r300_tcl_bypass_triangle_ib render_half, resolve_half;
   int rc;
   /* The clear half is the cover draw under the render half's subsample
    * state with the clear color as its fragment constant: every sample
    * of every pixel in the extent takes the clear color before the
    * triangle lands, so the surface inherits nothing from the allocation.
    */
   if (msaa->clear) {
      struct r300_triangle_render_shape clear_shape = msaa->render;
      memcpy(clear_shape.color_bits, msaa->clear_color_bits,
             sizeof(clear_shape.color_bits));
      if (r300_tcl_bypass_triangle_render_shape_validate(&clear_shape) != 0)
         return -EINVAL;
      rc = render_shape_emit_aa(&clear_shape, &render_aa,
                                clear_shape.triangle_count, &clear_half);
      if (rc != 0)
         return rc;
   }
   rc = render_shape_emit_aa(&msaa->render, &render_aa,
                             msaa->render.triangle_count, &render_half);
   if (rc != 0) {
      r300_tcl_bypass_triangle_release(&clear_half);
      return rc;
   }
   rc = render_shape_emit_aa(&resolve_shape, &resolve_aa,
                             resolve_shape.triangle_count, &resolve_half);
   if (rc != 0) {
      r300_tcl_bypass_triangle_release(&clear_half);
      r300_tcl_bypass_triangle_release(&render_half);
      return rc;
   }

   /* The destination's base and pitch are the caller's alone -- no
    * contract entry names them -- so they stand between the halves,
    * ahead of the resolve half's contract that arms the mode over them,
    * with the destination's relocation behind the offset write the way
    * r300_emit_aa_state orders it.
    */
   uint32_t interlude[8], epilogue[8];
   struct r300_pm4_builder ib_mid, eb;
   r300_pm4_builder_init(&ib_mid, interlude, ARRAY_SIZE(interlude));
   const uint32_t aa[2] = {
      msaa->destination.target_offset,
      msaa->destination.pitch_pixels &
         (uint32_t)R300_RB3D_AARESOLVE_PITCH_MASK,
   };
   r300_pm4_packet0(&ib_mid, R300_RB3D_AARESOLVE_OFFSET, aa, 2);
   const uint32_t reloc_at = r300_pm4_reloc_nop(
      &ib_mid,
      R300_TRIANGLE_RELOC_PAYLOAD(R300_TRIANGLE_SLOT_COMPOSED_COLOR));

   /* Resolve mode and the subsample set both close, so the cell leaves
    * the color backend where it found it.
    */
   r300_pm4_builder_init(&eb, epilogue, ARRAY_SIZE(epilogue));
   r300_pm4_reg(&eb, R300_RB3D_AARESOLVE_CTL,
                R300_RB3D_AARESOLVE_CTL_AARESOLVE_MODE_NORMAL);
   r300_pm4_reg(&eb, R300_GB_AA_CONFIG, R300_GB_AA_CONFIG_AA_DISABLE);

   if (ib_mid.error != 0 || eb.error != 0 ||
       reloc_at == R300_PM4_NO_INDEX) {
      r300_tcl_bypass_triangle_release(&clear_half);
      r300_tcl_bypass_triangle_release(&render_half);
      r300_tcl_bypass_triangle_release(&resolve_half);
      return -EINVAL;
   }

   const uint32_t dwords = clear_half.ib_size_dwords +
                           render_half.ib_size_dwords + ib_mid.count +
                           resolve_half.ib_size_dwords + eb.count;
   uint32_t *ib = calloc(dwords, sizeof(uint32_t));
   if (ib == NULL) {
      r300_tcl_bypass_triangle_release(&clear_half);
      r300_tcl_bypass_triangle_release(&render_half);
      r300_tcl_bypass_triangle_release(&resolve_half);
      return -ENOMEM;
   }
   uint32_t at = 0;
   const uint32_t clear_base = at;
   if (clear_half.ib_size_dwords != 0)
      memcpy(ib + at, clear_half.ib,
             clear_half.ib_size_dwords * sizeof(uint32_t));
   const uint32_t render_base = (at += clear_half.ib_size_dwords);
   memcpy(ib + at, render_half.ib,
          render_half.ib_size_dwords * sizeof(uint32_t));
   const uint32_t mid_base = (at += render_half.ib_size_dwords);
   memcpy(ib + at, interlude, ib_mid.count * sizeof(uint32_t));
   const uint32_t resolve_base = (at += ib_mid.count);
   memcpy(ib + at, resolve_half.ib,
          resolve_half.ib_size_dwords * sizeof(uint32_t));
   at += resolve_half.ib_size_dwords;
   memcpy(ib + at, epilogue, eb.count * sizeof(uint32_t));

   /* The render half keeps its slots.  The resolve half's vertex site
    * takes the composed vertex slot so its own cover geometry binds
    * there, while its color site stays on the color slot: both halves
    * render into the one multisample surface.
    */
   uint32_t site = 0;
   /* The clear half's color site stays on the color slot, the one
    * multisample surface, and its vertex site takes the composed vertex
    * slot: the cover geometry the resolve half binds there.
    */
   for (uint32_t i = 0; i < clear_half.reloc_site_count; i++) {
      struct r300_tcl_bypass_triangle_reloc_site r =
         clear_half.reloc_sites[i];
      r.ib_index += clear_base;
      if (r.slot == R300_TRIANGLE_SLOT_VERTEX)
         r.slot = R300_TRIANGLE_SLOT_COMPOSED_VERTEX;
      ib[r.ib_index] = R300_TRIANGLE_RELOC_PAYLOAD(r.slot);
      out->reloc_sites[site++] = r;
   }
   for (uint32_t i = 0; i < render_half.reloc_site_count; i++) {
      out->reloc_sites[site] = render_half.reloc_sites[i];
      out->reloc_sites[site++].ib_index += render_base;
   }
   out->reloc_sites[site].slot = R300_TRIANGLE_SLOT_COMPOSED_COLOR;
   out->reloc_sites[site++].ib_index = mid_base + reloc_at;
   for (uint32_t i = 0; i < resolve_half.reloc_site_count; i++) {
      struct r300_tcl_bypass_triangle_reloc_site r =
         resolve_half.reloc_sites[i];
      r.ib_index += resolve_base;
      if (r.slot == R300_TRIANGLE_SLOT_VERTEX)
         r.slot = R300_TRIANGLE_SLOT_COMPOSED_VERTEX;
      else if (r.slot == R300_TRIANGLE_SLOT_COLOR)
         r.slot = R300_TRIANGLE_SLOT_TEXTURE;
      ib[r.ib_index] = R300_TRIANGLE_RELOC_PAYLOAD(r.slot);
      out->reloc_sites[site++] = r;
   }
   out->reloc_site_count = site;
   out->ib = ib;
   out->ib_size_dwords = dwords;
   out->owns_ib = true;

   r300_tcl_bypass_triangle_release(&clear_half);
   r300_tcl_bypass_triangle_release(&render_half);
   r300_tcl_bypass_triangle_release(&resolve_half);
   if (r300_tcl_bypass_triangle_validate_reloc_sites(out) != 0) {
      r300_tcl_bypass_triangle_release(out);
      return -EINVAL;
   }
   return 0;
}

int
r300_tcl_bypass_triangle_multi_pass_binding_validate(
   const struct r300_triangle_multi_pass *mp)
{
   if (mp == NULL)
      return -EINVAL;
   const uint32_t v = mp->second_vertex_index, c = mp->second_color_index;
   /* Vertex index 1 is the first color target; 3 skips index 2. */
   if (v != 0 && v != 2)
      return -EINVAL;
   /* Color index 0 is a vertex page; with a shared vertex page the next
    * unused index is 2, with an own page it is 3 and 2 is that page.
    */
   if (v == 0 ? (c != 1 && c != 2) : (c != 1 && c != 3))
      return -EINVAL;
   return 0;
}

uint32_t
r300_tcl_bypass_triangle_multi_pass_reference_count(
   const struct r300_triangle_multi_pass *mp)
{
   if (r300_tcl_bypass_triangle_multi_pass_binding_validate(mp) != 0)
      return 0;
   return 2u + (mp->second_vertex_index == 2 ? 1u : 0u) +
          (mp->second_color_index >= 2 ? 1u : 0u);
}

/* One pass of the two-pass stream.  A constant-color pass takes the
 * render-shape emitter, which carries the pass's own fragment constant
 * and target geometry; a varying pass takes the cell family, the one
 * emitter of the position-plus-TEX0 record shape, at the pass's extent
 * over the reference target.  The deferred-draw route selects between
 * the same two emitters per pass, so a two-render-pass command buffer
 * whose pipelines carry a varying records the stream this reproduces.
 */
static int
multi_pass_emit_one(const struct r300_triangle_render_shape *pass,
                    bool clip_space, struct r300_tcl_bypass_triangle_ib *out)
{
   if (pass->varying && pass->flat_color0) {
      struct r300_flat_color0_plan plan;
      r300_flat_color0_plan_direct_first(&plan);
      return r300_tcl_bypass_triangle_flat_color0_family_emit(
         pass->width, pass->height, clip_space, 1u, &plan, out);
   }
   if (pass->varying && pass->rs_tex_adj_candidate != 0) {
      struct r300_rs_tex_adj_probe_plan plan;
      if (pass->rs_tex_adj_candidate == R300_RS_TEX_ADJ_PROBE_W_SELECT_ONE)
         r300_rs_tex_adj_probe_plan_w_select_one(&plan);
      else
         r300_rs_tex_adj_probe_plan_tex_adj(&plan);
      return r300_tcl_bypass_triangle_rs_tex_adj_family_emit(
         pass->width, pass->height, clip_space, 1u, &plan, out);
   }
   if (pass->varying) {
      return clip_space
                ? r300_tcl_bypass_triangle_clip_space_family_emit(
                     pass->width, pass->height, true, 1u, out)
                : r300_tcl_bypass_triangle_family_emit(
                     pass->width, pass->height, true, 1u, out);
   }
   return clip_space
             ? r300_tcl_bypass_triangle_clip_space_render_shape_emit(
                  pass, 1u, out)
             : r300_tcl_bypass_triangle_render_shape_emit(pass, out);
}

static int
multi_pass_emit(const struct r300_triangle_multi_pass *mp, bool clip_space,
                struct r300_tcl_bypass_triangle_ib *out)
{
   memset(out, 0, sizeof(*out));
   if (r300_tcl_bypass_triangle_multi_pass_binding_validate(mp) != 0)
      return -EINVAL;

   struct r300_tcl_bypass_triangle_ib first, second;
   int rc = multi_pass_emit_one(&mp->pass[0], clip_space, &first);
   if (rc != 0)
      return rc;
   rc = multi_pass_emit_one(&mp->pass[1], clip_space, &second);
   if (rc != 0) {
      r300_tcl_bypass_triangle_release(&first);
      return rc;
   }
   /* The first cell's slot numbers are its merged indices, so it stays
    * in its emitted form; the second binds to the positions the merge
    * assigns it.
    */
   uint32_t slot_index[R300_TRIANGLE_RENDER_SLOT_COUNT] = { 0 };
   slot_index[R300_TRIANGLE_SLOT_VERTEX] = mp->second_vertex_index;
   slot_index[R300_TRIANGLE_SLOT_COLOR] = mp->second_color_index;
   rc = r300_tcl_bypass_triangle_bind_reloc_indices(
      &second, slot_index, R300_TRIANGLE_RENDER_SLOT_COUNT);
   if (rc != 0) {
      r300_tcl_bypass_triangle_release(&first);
      r300_tcl_bypass_triangle_release(&second);
      return rc;
   }

   const uint32_t dwords = first.ib_size_dwords + second.ib_size_dwords;
   uint32_t *ib = calloc(dwords, sizeof(uint32_t));
   if (ib == NULL) {
      r300_tcl_bypass_triangle_release(&first);
      r300_tcl_bypass_triangle_release(&second);
      return -ENOMEM;
   }
   memcpy(ib, first.ib, first.ib_size_dwords * sizeof(uint32_t));
   memcpy(ib + first.ib_size_dwords, second.ib,
          second.ib_size_dwords * sizeof(uint32_t));

   uint32_t site = 0;
   for (uint32_t i = 0; i < first.reloc_site_count; i++)
      out->reloc_sites[site++] = first.reloc_sites[i];
   for (uint32_t i = 0; i < second.reloc_site_count; i++) {
      out->reloc_sites[site] = second.reloc_sites[i];
      out->reloc_sites[site++].ib_index += first.ib_size_dwords;
   }
   out->reloc_site_count = site;
   out->ib = ib;
   out->ib_size_dwords = dwords;
   out->owns_ib = true;
   r300_tcl_bypass_triangle_release(&first);
   r300_tcl_bypass_triangle_release(&second);
   return 0;
}

int
r300_tcl_bypass_triangle_multi_pass_emit(
   const struct r300_triangle_multi_pass *mp,
   struct r300_tcl_bypass_triangle_ib *out)
{
   return multi_pass_emit(mp, false, out);
}

int
r300_tcl_bypass_triangle_clip_space_multi_pass_emit(
   const struct r300_triangle_multi_pass *mp,
   struct r300_tcl_bypass_triangle_ib *out)
{
   return multi_pass_emit(mp, true, out);
}

int
r300_tcl_bypass_triangle_composed_render_sample_emit(
   const struct r300_triangle_composed_render_sample *composed,
   struct r300_tcl_bypass_triangle_ib *out)
{
   memset(out, 0, sizeof(*out));
   if (composed == NULL ||
       r300_tcl_bypass_triangle_render_shape_validate(&composed->render) !=
          0 ||
       r300_tcl_bypass_triangle_render_shape_validate(&composed->sample) != 0)
      return -EINVAL;

   struct r300_tcl_bypass_triangle_ib render_half;
   int rc = r300_tcl_bypass_triangle_render_shape_emit(&composed->render,
                                                       &render_half);
   if (rc != 0)
      return rc;
   struct r300_tcl_bypass_triangle_ib sample_half;
   rc = composed_sample_emit(composed, &sample_half);
   if (rc != 0) {
      r300_tcl_bypass_triangle_release(&render_half);
      return rc;
   }

   const uint32_t dwords =
      render_half.ib_size_dwords + sample_half.ib_size_dwords;
   uint32_t *ib = calloc(dwords, sizeof(uint32_t));
   if (ib == NULL) {
      r300_tcl_bypass_triangle_release(&render_half);
      r300_tcl_bypass_triangle_release(&sample_half);
      return -ENOMEM;
   }
   memcpy(ib, render_half.ib,
          render_half.ib_size_dwords * sizeof(uint32_t));
   memcpy(ib + render_half.ib_size_dwords, sample_half.ib,
          sample_half.ib_size_dwords * sizeof(uint32_t));

   /* The render half keeps its slots.  The sample half's vertex and
    * color sites take the composed slots, so each buffer object the
    * submission binds carries one slot, while its texture site stays on
    * the texture slot the transport resolves to the render half's
    * target.
    */
   uint32_t site = 0;
   for (uint32_t i = 0; i < render_half.reloc_site_count; i++)
      out->reloc_sites[site++] = render_half.reloc_sites[i];
   for (uint32_t i = 0; i < sample_half.reloc_site_count; i++) {
      struct r300_tcl_bypass_triangle_reloc_site s = sample_half.reloc_sites[i];
      s.ib_index += render_half.ib_size_dwords;
      if (s.slot == R300_TRIANGLE_SLOT_VERTEX)
         s.slot = R300_TRIANGLE_SLOT_COMPOSED_VERTEX;
      else if (s.slot == R300_TRIANGLE_SLOT_COLOR)
         s.slot = R300_TRIANGLE_SLOT_COMPOSED_COLOR;
      /* The payload dword names the slot, so the remap rewrites the
       * stream beside the site list. */
      ib[s.ib_index] = R300_TRIANGLE_RELOC_PAYLOAD(s.slot);
      out->reloc_sites[site++] = s;
   }
   out->reloc_site_count = site;
   out->ib = ib;
   out->ib_size_dwords = dwords;
   out->owns_ib = true;

   r300_tcl_bypass_triangle_release(&render_half);
   r300_tcl_bypass_triangle_release(&sample_half);
   if (site != R300_TRIANGLE_COMPOSED_SLOT_COUNT ||
       r300_tcl_bypass_triangle_validate_reloc_sites(out) != 0) {
      r300_tcl_bypass_triangle_release(out);
      return -EINVAL;
   }
   return 0;
}

void
r300_tcl_bypass_triangle_render_shape_vertices(
   const struct r300_triangle_render_shape *shape,
   float out[R300_TRIANGLE_VERTEX_DWORDS])
{
   const struct triangle_geometry g =
      triangle_geometry_at(shape->width, shape->height);
   for (unsigned i = 0; i < 3; i++) {
      out[i * 4 + 0] = g.v[i * 2 + 0];
      out[i * 4 + 1] = g.v[i * 2 + 1];
      out[i * 4 + 2] = 0.0f;
      out[i * 4 + 3] = 1.0f;
   }
}

uint32_t
r300_tcl_bypass_triangle_render_shape_color_bytes(
   const struct r300_triangle_render_shape *shape)
{
   return shape->target_offset +
          shape->pitch_pixels * (shape->height + R300_TRIANGLE_CANARY_ROWS) *
             4u;
}

uint32_t
r300_tcl_bypass_triangle_pack_unorm8_dword(enum r300_triangle_lane_order lanes,
                                           const float rgba[4])
{
   uint32_t byte[4];
   for (unsigned i = 0; i < 4; i++)
      byte[i] = unorm8_round(rgba[i]);
   const uint32_t r = byte[0], g = byte[1], b = byte[2], a = byte[3];
   return lanes == R300_TRIANGLE_LANES_R8G8B8A8
             ? (r | (g << 8) | (b << 16) | (a << 24))
             : (b | (g << 8) | (r << 16) | (a << 24));
}

uint32_t
r300_tcl_bypass_triangle_render_shape_draw_dword(
   const struct r300_triangle_render_shape *shape)
{
   float rgba[4];
   memcpy(rgba, shape->color_bits, sizeof(rgba));
   return r300_tcl_bypass_triangle_pack_unorm8_dword(shape->lanes, rgba);
}

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

int
r300_tcl_bypass_triangle_expected_target(
   const struct r300_triangle_render_shape *shape, uint32_t interior_dword,
   uint32_t exterior_dword, uint32_t *pixels, uint32_t size_bytes)
{
   if (shape == NULL || pixels == NULL ||
       r300_tcl_bypass_triangle_render_shape_validate_geometry(shape) != 0)
      return -EINVAL;
   const uint32_t pitch = shape->pitch_pixels;
   const uint64_t footprint_bytes =
      (uint64_t)shape->target_offset +
      (uint64_t)pitch * (shape->height + R300_TRIANGLE_CANARY_ROWS) *
         sizeof(uint32_t);
   if (size_bytes < footprint_bytes)
      return -EINVAL;
   const uint32_t footprint_dwords = (uint32_t)(footprint_bytes / 4u);
   for (uint32_t i = 0; i < footprint_dwords; i++)
      pixels[i] = exterior_dword;
   const struct triangle_geometry g =
      triangle_geometry_at(shape->width, shape->height);
   uint32_t *rows = pixels + shape->target_offset / 4u;
   for (uint32_t y = 0; y < shape->height; y++) {
      for (uint32_t x = 0; x < shape->width; x++) {
         if (triangle_center_class(&g, (float)x + 0.5f, (float)y + 0.5f) ==
             1)
            rows[y * pitch + x] = interior_dword;
      }
   }
   return 0;
}

int
r300_tcl_bypass_triangle_target_compare(
   const struct r300_triangle_render_shape *shape, const uint32_t *expected,
   const uint32_t *observed, uint32_t size_bytes, bool *judged)
{
   if (judged != NULL)
      *judged = false;
   if (shape == NULL || expected == NULL || observed == NULL ||
       r300_tcl_bypass_triangle_render_shape_validate_geometry(shape) != 0)
      return -EINVAL;
   const uint32_t pitch = shape->pitch_pixels;
   const uint64_t footprint_bytes =
      (uint64_t)shape->target_offset +
      (uint64_t)pitch * (shape->height + R300_TRIANGLE_CANARY_ROWS) *
         sizeof(uint32_t);
   if (size_bytes < footprint_bytes)
      return -EINVAL;
   const uint32_t footprint_dwords = (uint32_t)(footprint_bytes / 4u);
   const uint32_t offset_dwords = shape->target_offset / 4u;
   const struct triangle_geometry g =
      triangle_geometry_at(shape->width, shape->height);
   int differing = 0;
   for (uint32_t i = 0; i < footprint_dwords; i++) {
      if (i >= offset_dwords) {
         const uint32_t rel = i - offset_dwords;
         const uint32_t y = rel / pitch, x = rel % pitch;
         if (y < shape->height && x < shape->width &&
             triangle_center_class(&g, (float)x + 0.5f, (float)y + 0.5f) <
                0)
            continue;
      }
      if (expected[i] != observed[i])
         differing++;
   }
   if (judged != NULL)
      *judged = true;
   return differing;
}
