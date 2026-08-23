/* SPDX-License-Identifier: MIT */

#include "r300_r2vb_fetched_producer.h"
#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_pm4_builder.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_vertex_format.h"

#include "r300_reg.h"
#include "util/macros.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Four dwords per drm_radeon_cs_reloc entry, so a slot's payload indexes
 * the relocation chunk at four times the slot.  The carrier slot is zero
 * in both producer families, so the shared prologue's payload names it.
 */
#define R300_R2VB_FETCHED_PRODUCER_RELOC_PAYLOAD(slot) ((slot) * 4)

/* The slot element rides VAP vector 0 and the source element vector 6,
 * the vectors the immediate producer's qualified tuple uses.
 */
#define R300_R2VB_FETCHED_PRODUCER_PSC_SLOT_VEC 0u
#define R300_R2VB_FETCHED_PRODUCER_PSC_SOURCE_VEC 6u

/* Prologue, US state, and publication tail outside the fetched body; the
 * emission bound adds the contract prefix, the fragment binary's US block,
 * and the fixed fetched body on top.
 */
#define R300_R2VB_FETCHED_PRODUCER_FIXED_MAX_DWORDS 80u

/* RADEON_GEM_DOMAIN_GTT: the one domain every fetched-route BO lives in.
 * The value is the DRM UAPI's, spelled here so the common contract carries
 * no kernel header.
 */
#define R300_R2VB_FETCHED_ROUTE_GTT_DOMAIN 2u

static uint32_t
float_bits(float f)
{
   uint32_t bits;
   memcpy(&bits, &f, sizeof(bits));
   return bits;
}

int
r300_r2vb_fetched_producer_slot_positions(uint32_t count, uint32_t *words,
                                          uint32_t word_count)
{
   if (words == NULL || count == 0)
      return -EINVAL;
   if ((uint64_t)count * 4 > word_count)
      return -ENOSPC;
   for (uint32_t v = 0; v < count; v++) {
      words[v * 4 + 0] = float_bits((float)v + 0.5f);
      words[v * 4 + 1] = float_bits(0.5f);
      words[v * 4 + 2] = float_bits(0.0f);
      words[v * 4 + 3] = float_bits(1.0f);
   }
   return 0;
}

static const struct r300_vertex_format_semantics *
fetched_format(int format_id)
{
   if (format_id != R300_VERTEX_FORMAT_F32_4 &&
       format_id != R300_VERTEX_FORMAT_F32_3 &&
       format_id != R300_VERTEX_FORMAT_F32_2)
      return NULL;
   return r300_vertex_format_semantics((enum r300_vertex_format_id)format_id);
}

int
r300_r2vb_fetched_producer_fetch_state(int format_id,
                                       struct r300_r2vb_fetch_state *out)
{
   const struct r300_vertex_format_semantics *fmt = fetched_format(format_id);
   if (out == NULL || fmt == NULL)
      return -EINVAL;
   memset(out, 0, sizeof(*out));

   out->vap_prog_stream_cntl[0] =
      (R300_DATA_TYPE_FLOAT_4 |
       (R300_R2VB_FETCHED_PRODUCER_PSC_SLOT_VEC << R300_DST_VEC_LOC_SHIFT)) |
      (((uint32_t)fmt->data_type |
        (R300_R2VB_FETCHED_PRODUCER_PSC_SOURCE_VEC << R300_DST_VEC_LOC_SHIFT) |
        R300_LAST_VEC)
       << 16);

   /* The source swizzle reverses the first three logical lanes: the
    * format's own select for (z, y, x, w) in lanes 0..3, so a lane the
    * width lacks arrives as the FP_ZERO or FP_ONE the format synthesizes.
    * r300_vertex_component_select and R300_SWIZZLE_SELECT_* share one
    * encoding.
    */
   const uint32_t source_swizzle =
      ((uint32_t)fmt->select[2] << R300_SWIZZLE_SELECT_X_SHIFT) |
      ((uint32_t)fmt->select[1] << R300_SWIZZLE_SELECT_Y_SHIFT) |
      ((uint32_t)fmt->select[0] << R300_SWIZZLE_SELECT_Z_SHIFT) |
      ((uint32_t)fmt->select[3] << R300_SWIZZLE_SELECT_W_SHIFT) |
      (0xfu << R300_WRITE_ENA_SHIFT);
   out->vap_prog_stream_cntl_ext[0] =
      R300_VAP_SWIZZLE_XYZW | (source_swizzle << 16);

   out->fetch_dwords = 4u + fmt->hardware_fetch_dwords;
   out->vap_vtx_state_cntl = 0x5555;
   out->vap_vsm_vtx_assm = R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0;
   out->vap_out_vtx_fmt[0] = R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT;
   out->vap_out_vtx_fmt[1] = R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS;
   /* Point stuffing off and TEX0 replicated from the VAP: the first-draw
    * contract's GB_ENABLE value, restated because the fetched body writes
    * the register.
    */
   out->gb_enable = 0;
   out->rs_count = R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN;
   out->rs_inst_count = 0;
   out->rs_ip[0] = R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
                   R300_RS_SEL_T(R300_RS_SEL_C1) |
                   R300_RS_SEL_R(R300_RS_SEL_C2) |
                   R300_RS_SEL_Q(R300_RS_SEL_C3);
   out->rs_inst[0] = R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
                     R300_RS_INST_TEX_ADDR(0);
   return 0;
}

static bool
layout_valid(const struct r300_r2vb_producer_layout *layout)
{
   return layout->count >= 1 &&
          layout->count <= R300_R2VB_PRODUCER_MAX_COUNT &&
          layout->height == 1 && layout->width == layout->pitch_pixels &&
          layout->pitch_pixels == ((layout->count + 1u) & ~1u);
}

int
r300_r2vb_fetched_producer_emit_into(
   const struct r300_r2vb_fetched_producer_params *params, uint32_t *words,
   uint32_t capacity, struct r300_r2vb_fetched_producer_ib *out)
{
   if (params == NULL || out == NULL)
      return -EINVAL;
   memset(out, 0, sizeof(*out));

   const struct r300_r2vb_producer_layout *layout = &params->layout;
   const struct r300_fragment_binary *fs = params->fragment_binary;
   const struct r300_vertex_format_semantics *fmt =
      fetched_format(params->source.format_id);
   if (!layout_valid(layout) || fmt == NULL)
      return -EINVAL;
   if (fs == NULL || !fs->validated)
      return -EINVAL;
   /* Dword-granular fetch: the VBPNTR pointer and stride fields carry
    * dwords, and a stride below the record overlaps consecutive fetches.
    */
   if (params->source.offset_bytes % 4 != 0 ||
       params->source.stride_bytes % 4 != 0 ||
       params->source.stride_bytes < fmt->semantic_record_bytes ||
       params->slot_offset_bytes % 4 != 0)
      return -EINVAL;

   struct r300_r2vb_fetch_state state;
   int rc = r300_r2vb_fetched_producer_fetch_state(params->source.format_id,
                                                   &state);
   if (rc != 0)
      return rc;

   const struct r300_r2vb_fetch_pass_params fetch = {
      .state = &state,
      .stream = {
         {
            .role = R300_R2VB_BO_SLOT,
            .size_dwords = R300_R2VB_FETCHED_PRODUCER_SLOT_RECORD_BYTES / 4,
            .stride_dwords = R300_R2VB_FETCHED_PRODUCER_SLOT_RECORD_BYTES / 4,
            .offset_bytes = params->slot_offset_bytes,
            .bo_size_bytes = params->slot_bo_size_bytes,
            .relocation_payload = R300_R2VB_FETCHED_PRODUCER_RELOC_PAYLOAD(
               R300_R2VB_FETCHED_PRODUCER_SLOT_SLOT),
         },
         {
            .role = R300_R2VB_BO_MODEL,
            .size_dwords = fmt->hardware_fetch_dwords,
            .stride_dwords = params->source.stride_bytes / 4,
            .offset_bytes = params->source.offset_bytes,
            .bo_size_bytes = params->source.bo_size_bytes,
            .relocation_payload = R300_R2VB_FETCHED_PRODUCER_RELOC_PAYLOAD(
               R300_R2VB_FETCHED_PRODUCER_SLOT_SOURCE),
         },
      },
      .vertex_count = layout->count,
      .vf_prim = R300_VAP_VF_CNTL__PRIM_POINTS,
   };

   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, words, capacity);

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

   uint32_t carrier_site = R300_PM4_NO_INDEX;
   r300_r2vb_producer_prologue_emit(&b, params->carrier_offset, layout,
                                    &carrier_site);
   r300_r2vb_producer_fs_emit(&b, fs);

   /* The fetched body replaces the immediate pass's VAP tuple, varying
    * routing, and embedded draw: it writes the same register set from
    * the derived state and then fetches both arrays.
    */
   out->fetch_body_start = b.count;
   struct r300_r2vb_fetch_pass_relocs body_relocs;
   if (b.error == 0) {
      rc = r300_r2vb_fetch_pass_emit(&b, &fetch, &body_relocs);
      if (rc != 0) {
         memset(out, 0, sizeof(*out));
         return rc;
      }
   }

   r300_r2vb_producer_tail_emit(&b);

   rc = r300_pm4_builder_finish(&b, &out->ib_size_dwords);
   if (rc != 0) {
      memset(out, 0, sizeof(*out));
      return rc;
   }
   out->ib = words;
   out->reloc_sites[0] = (struct r300_r2vb_producer_reloc_site){
      .ib_index = carrier_site,
      .slot = R300_R2VB_FETCHED_PRODUCER_SLOT_CARRIER,
   };
   out->reloc_sites[1] = (struct r300_r2vb_producer_reloc_site){
      .ib_index = body_relocs.ib_index[0],
      .slot = R300_R2VB_FETCHED_PRODUCER_SLOT_SLOT,
   };
   out->reloc_sites[2] = (struct r300_r2vb_producer_reloc_site){
      .ib_index = body_relocs.ib_index[1],
      .slot = R300_R2VB_FETCHED_PRODUCER_SLOT_SOURCE,
   };
   out->reloc_site_count = R300_R2VB_FETCHED_PRODUCER_SLOT_COUNT;
   return 0;
}

int
r300_r2vb_fetched_producer_emit(
   const struct r300_r2vb_fetched_producer_params *params,
   struct r300_r2vb_fetched_producer_ib *out)
{
   if (params == NULL || out == NULL)
      return -EINVAL;
   memset(out, 0, sizeof(*out));
   const struct r300_fragment_binary *fs = params->fragment_binary;
   if (fs == NULL || !fs->validated)
      return -EINVAL;

   uint32_t capacity = R300_R2VB_FETCHED_PRODUCER_FIXED_MAX_DWORDS +
                       fs->cb_code_size + R300_R2VB_FETCH_PASS_DWORDS;
   if (params->first_draw_contract != NULL)
      capacity +=
         r300_first_draw_state_dwords(params->first_draw_contract);

   uint32_t *ib = calloc(capacity, sizeof(uint32_t));
   if (ib == NULL)
      return -ENOMEM;
   const int rc =
      r300_r2vb_fetched_producer_emit_into(params, ib, capacity, out);
   if (rc != 0) {
      free(ib);
      return rc;
   }
   out->owns_ib = true;
   return 0;
}

void
r300_r2vb_fetched_producer_release(struct r300_r2vb_fetched_producer_ib *ib)
{
   if (ib == NULL)
      return;
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_r2vb_fetched_producer_validate_reloc_sites(
   const struct r300_r2vb_fetched_producer_ib *ib)
{
   if (ib == NULL || ib->ib == NULL)
      return -EINVAL;
   if (ib->reloc_site_count != R300_R2VB_FETCHED_PRODUCER_SLOT_COUNT)
      return -EINVAL;
   if (ib->fetch_body_start == 0 ||
       ib->fetch_body_start + R300_R2VB_FETCH_PASS_DWORDS > ib->ib_size_dwords)
      return -ERANGE;
   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_r2vb_producer_reloc_site *site = &ib->reloc_sites[i];
      if (site->slot != i)
         return -EINVAL;
      if (site->ib_index == 0 || site->ib_index >= ib->ib_size_dwords)
         return -ERANGE;
      if (i > 0 && ib->reloc_sites[i - 1].ib_index >= site->ib_index)
         return -EINVAL;
      if (ib->ib[site->ib_index - 1] != CP_PACKET3(R300_PM4_PACKET3_NOP, 0))
         return -EINVAL;
      if (ib->ib[site->ib_index] !=
          R300_R2VB_FETCHED_PRODUCER_RELOC_PAYLOAD(site->slot))
         return -EINVAL;
      /* The carrier site precedes the fetched body; the two array sites
       * sit inside it.
       */
      const bool inside_body =
         site->ib_index > ib->fetch_body_start &&
         site->ib_index < ib->fetch_body_start + R300_R2VB_FETCH_PASS_DWORDS;
      if ((i == 0) == inside_body)
         return -ERANGE;
   }
   return 0;
}

/* Reference geometry over a caller source: the reference count and
 * single-row layout, the first-draw contract resolved for that row, and
 * the reference fragment binary.
 */
static int
reference_geometry_emit(const struct r300_r2vb_fetched_source *source,
                        uint32_t slot_offset_bytes,
                        uint64_t slot_bo_size_bytes,
                        struct r300_r2vb_fetched_producer_ib *out)
{
   memset(out, 0, sizeof(*out));

   struct r300_r2vb_producer_layout layout;
   int rc = r300_r2vb_producer_layout_single_row(
      R300_R2VB_PRODUCER_REFERENCE_COUNT, &layout);
   if (rc != 0)
      return rc;

   struct r300_first_draw_params draw_params = {
      .chip_family = CHIP_RS480,
      .width = layout.width,
      .height = layout.height,
      .min_vtx_index = 0,
      .max_vtx_index = layout.count - 1,
      .texture_enabled = false,
   };
   struct r300_first_draw_contract contract;
   rc = r300_first_draw_contract_resolve(&draw_params, &contract);
   if (rc != 0)
      return rc;

   struct r300_fragment_binary fs;
   rc = r300_r2vb_producer_reference_fs(&fs);
   if (rc != 0)
      return rc;
   const struct r300_r2vb_fetched_producer_params params = {
      .carrier_offset = 0,
      .layout = layout,
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
      .source = *source,
      .slot_offset_bytes = slot_offset_bytes,
      .slot_bo_size_bytes = slot_bo_size_bytes,
   };
   rc = r300_r2vb_fetched_producer_emit(&params, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

int
r300_r2vb_fetched_producer_reference_emit(
   int format_id, struct r300_r2vb_fetched_producer_ib *out)
{
   if (out == NULL)
      return -EINVAL;
   const struct r300_vertex_format_semantics *fmt = fetched_format(format_id);
   if (fmt == NULL) {
      memset(out, 0, sizeof(*out));
      return -EINVAL;
   }
   const struct r300_r2vb_fetched_source source = {
      .format_id = format_id,
      .offset_bytes = 0,
      .stride_bytes = fmt->semantic_record_bytes,
      .bo_size_bytes = R300_R2VB_FETCHED_REFERENCE_BO_BYTES,
   };
   return reference_geometry_emit(&source, 0,
                                  R300_R2VB_FETCHED_REFERENCE_BO_BYTES, out);
}

int
r300_r2vb_fetched_route_compose(
   const struct r300_r2vb_fetched_route_params *params,
   struct r300_r2vb_fetched_route_ib *out)
{
   if (params == NULL || out == NULL || params->consumer_words == NULL ||
       params->consumer_dwords == 0)
      return -EINVAL;
   memset(out, 0, sizeof(*out));

   struct r300_r2vb_fetched_producer_ib producer;
   int rc = reference_geometry_emit(&params->source, params->slot_offset_bytes,
                                    params->slot_bo_size_bytes, &producer);
   if (rc != 0)
      return rc;
   rc = r300_r2vb_fetched_producer_validate_reloc_sites(&producer);
   if (rc != 0) {
      r300_r2vb_fetched_producer_release(&producer);
      return rc;
   }

   /* The producer writes the carrier through the color backend and reads
    * the two arrays through the vertex fetch; the consumer reads the
    * carrier and writes the color target.
    */
   const struct r300_pm4_reloc_site producer_sites[3] = {
      {
         .dword_index = producer.reloc_sites[0].ib_index,
         .role = R300_R2VB_BO_CARRIER,
         .read_domains = 0,
         .write_domain = R300_R2VB_FETCHED_ROUTE_GTT_DOMAIN,
      },
      {
         .dword_index = producer.reloc_sites[1].ib_index,
         .role = R300_R2VB_BO_SLOT,
         .read_domains = R300_R2VB_FETCHED_ROUTE_GTT_DOMAIN,
         .write_domain = 0,
      },
      {
         .dword_index = producer.reloc_sites[2].ib_index,
         .role = R300_R2VB_BO_MODEL,
         .read_domains = R300_R2VB_FETCHED_ROUTE_GTT_DOMAIN,
         .write_domain = 0,
      },
   };
   /* The composition reports sites in the order given, so the consumer's
    * two sites are listed in stream order: the TCL-bypass cell retargets
    * the color buffer before it binds the vertex array, and a recorded
    * consumer keeps that order.
    */
   const struct r300_pm4_reloc_site consumer_carrier = {
      .dword_index = params->consumer_carrier_site,
      .role = R300_R2VB_BO_CARRIER,
      .read_domains = R300_R2VB_FETCHED_ROUTE_GTT_DOMAIN,
      .write_domain = 0,
   };
   const struct r300_pm4_reloc_site consumer_color = {
      .dword_index = params->consumer_color_site,
      .role = R300_R2VB_BO_COLOR,
      .read_domains = 0,
      .write_domain = R300_R2VB_FETCHED_ROUTE_GTT_DOMAIN,
   };
   const bool color_first =
      params->consumer_color_site < params->consumer_carrier_site;
   const struct r300_pm4_reloc_site consumer_sites[2] = {
      color_first ? consumer_color : consumer_carrier,
      color_first ? consumer_carrier : consumer_color,
   };
   const struct r300_pm4_fragment fragments[2] = {
      {
         .dwords = producer.ib,
         .dword_count = producer.ib_size_dwords,
         .relocs = producer_sites,
         .reloc_count = ARRAY_SIZE(producer_sites),
      },
      {
         .dwords = params->consumer_words,
         .dword_count = params->consumer_dwords,
         .relocs = consumer_sites,
         .reloc_count = ARRAY_SIZE(consumer_sites),
      },
   };

   const uint64_t total =
      (uint64_t)producer.ib_size_dwords + params->consumer_dwords;
   if (total > UINT32_MAX) {
      r300_r2vb_fetched_producer_release(&producer);
      return -EOVERFLOW;
   }
   uint32_t *words = malloc((size_t)total * sizeof(uint32_t));
   if (words == NULL) {
      r300_r2vb_fetched_producer_release(&producer);
      return -ENOMEM;
   }
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, words, (uint32_t)total);
   rc = r300_pm4_compose(&b, fragments, ARRAY_SIZE(fragments), &params->roles,
                         &out->composition);
   if (rc == 0)
      rc = r300_pm4_builder_finish(&b, &out->ib_size_dwords);
   r300_r2vb_fetched_producer_release(&producer);
   if (rc != 0) {
      free(words);
      memset(out, 0, sizeof(*out));
      return rc;
   }
   out->ib = words;
   out->consumer_start_dwords = out->composition.fragment_start[1];
   return 0;
}

void
r300_r2vb_fetched_route_release(struct r300_r2vb_fetched_route_ib *ib)
{
   if (ib == NULL)
      return;
   free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_r2vb_fetched_route_reference_compose(
   int format_id, struct r300_r2vb_fetched_route_ib *out)
{
   if (out == NULL)
      return -EINVAL;
   memset(out, 0, sizeof(*out));
   const struct r300_vertex_format_semantics *fmt = fetched_format(format_id);
   if (fmt == NULL)
      return -EINVAL;

   struct r300_tcl_bypass_triangle_ib consumer;
   int rc = r300_tcl_bypass_triangle_extent_emit(
      R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, &consumer);
   if (rc != 0)
      return rc;
   rc = r300_tcl_bypass_triangle_validate_reloc_sites(&consumer);
   if (rc != 0) {
      r300_tcl_bypass_triangle_release(&consumer);
      return rc;
   }

   struct r300_r2vb_fetched_route_params params = {
      .source = {
         .format_id = format_id,
         .offset_bytes = 0,
         .stride_bytes = fmt->semantic_record_bytes,
         .bo_size_bytes = R300_R2VB_FETCHED_REFERENCE_BO_BYTES,
      },
      .slot_offset_bytes = 0,
      .slot_bo_size_bytes = R300_R2VB_FETCHED_REFERENCE_BO_BYTES,
      .consumer_words = consumer.ib,
      .consumer_dwords = consumer.ib_size_dwords,
      .roles = {
         .chunk_index = {
            [R300_R2VB_BO_CARRIER] = 0,
            [R300_R2VB_BO_COLOR] = 1,
            [R300_R2VB_BO_SLOT] = 2,
            [R300_R2VB_BO_MODEL] = 3,
         },
      },
   };
   for (uint32_t i = 0; i < consumer.reloc_site_count; i++) {
      if (consumer.reloc_sites[i].slot == R300_TRIANGLE_SLOT_VERTEX)
         params.consumer_carrier_site = consumer.reloc_sites[i].ib_index;
      else
         params.consumer_color_site = consumer.reloc_sites[i].ib_index;
   }
   rc = r300_r2vb_fetched_route_compose(&params, out);
   r300_tcl_bypass_triangle_release(&consumer);
   return rc;
}

int
r300_pm4_scan_reloc_sites(const uint32_t *words, uint32_t dword_count,
                          uint32_t *site_indices, uint32_t *payloads,
                          uint32_t max_sites)
{
   if (words == NULL)
      return -EINVAL;
   uint32_t found = 0;
   uint32_t i = 0;
   while (i < dword_count) {
      const uint32_t header = words[i];
      const uint32_t type = header >> 30;
      uint32_t payload_dwords;
      switch (type) {
      case 0:
      case 3:
         payload_dwords = ((header >> 16) & 0x3fffu) + 1u;
         break;
      case 2:
         /* PACKET2: a one-dword filler. */
         payload_dwords = 0;
         break;
      default:
         return -EINVAL;
      }
      if ((uint64_t)i + 1u + payload_dwords > dword_count)
         return -EINVAL;
      if (header == CP_PACKET3(R300_PM4_PACKET3_NOP, 0)) {
         if (found >= max_sites)
            return -ENOSPC;
         if (site_indices != NULL)
            site_indices[found] = i + 1u;
         if (payloads != NULL)
            payloads[found] = words[i + 1u];
         found++;
      }
      i += 1u + payload_dwords;
   }
   return (int)found;
}
