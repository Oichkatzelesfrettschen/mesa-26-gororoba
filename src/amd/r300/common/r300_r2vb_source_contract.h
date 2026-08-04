/*
 * SPDX-License-Identifier: MIT
 */

#ifndef AMD_R300_R2VB_SOURCE_CONTRACT_H
#define AMD_R300_R2VB_SOURCE_CONTRACT_H

#include <stdbool.h>
#include <stdint.h>

#include "amd/r300/common/r300_vertex_format.h"

struct r300_r2vb_source_extents {
   uint64_t source_offset;
   uint64_t semantic_end;
   uint64_t hardware_end;
};

enum r300_r2vb_source_stream_index {
   R300_R2VB_SOURCE_STREAM_SLOT = 0,
   R300_R2VB_SOURCE_STREAM_MODEL = 1,
   R300_R2VB_SOURCE_STREAM_COUNT = 2,
};

/* Neutral encodings for the load-bearing fields in one 16-bit
 * VAP_PROG_STREAM_CNTL element.  Keep these independent of r300_reg.h so the
 * source contract can be consumed by Gallium, a future native driver, and
 * host-side parser tests without importing a frontend-specific header. */
#define R300_R2VB_PSC_DATA_TYPE_MASK 0x0fu
#define R300_R2VB_PSC_DST_VEC_SHIFT 8u
#define R300_R2VB_PSC_DST_VEC_MASK 0x1fu
#define R300_R2VB_PSC_LAST_VEC (1u << 13)

struct r300_r2vb_source_stream_contract {
   enum r300_vertex_data_type data_type;
   uint8_t fetch_dwords;
   uint8_t dst_vec;
   bool last;
   uint16_t psc_swizzle;
};

struct r300_r2vb_source_contract {
   enum r300_vertex_format_id format;
   uint32_t stride_bytes;
   uint32_t start;
   uint32_t count;

   uint8_t model_fetch_dwords;
   uint8_t logical_components;
   uint8_t vap_vtx_size_dwords;
   enum r300_vertex_data_type model_data_type;
   uint16_t model_psc_swizzle;

   struct r300_r2vb_source_extents extents;
};

/* Complete two-stream hardware tuple derived from the source contract.  The
 * first CNTL/EXT pair is load-bearing; every later pair must remain zero. */
struct r300_r2vb_source_tuple {
   struct r300_r2vb_source_stream_contract
      stream[R300_R2VB_SOURCE_STREAM_COUNT];
   uint32_t prog_stream_cntl[8];
   uint32_t prog_stream_cntl_ext[8];
   uint8_t vap_vtx_size_dwords;
};

static inline bool
r300_r2vb_float2_source_gate_value(const char *value)
{
   return value && value[0] == '1' && value[1] == '\0';
}

/* This is route policy, not format semantics.  The established automatic
 * source domain remains F32_3/F32_4.  F32_2 enters only through its exact
 * experimental gate; F32_1 remains outside this producer contract. */
static inline bool
r300_r2vb_source_format_admitted(enum r300_vertex_format_id format,
                                  bool float2_enabled)
{
   return format == R300_VERTEX_FORMAT_F32_3 ||
          format == R300_VERTEX_FORMAT_F32_4 ||
          (format == R300_VERTEX_FORMAT_F32_2 && float2_enabled);
}

static inline bool
r300_r2vb_u64_add(uint64_t a, uint64_t b, uint64_t *out)
{
   if (!out || a > UINT64_MAX - b)
      return false;
   *out = a + b;
   return true;
}

static inline bool
r300_r2vb_u64_mul(uint64_t a, uint64_t b, uint64_t *out)
{
   if (!out || (a != 0 && b > UINT64_MAX / a))
      return false;
   *out = a * b;
   return true;
}

/* Compute and validate the two independent bounds:
 *
 * semantic_end: the final byte required by the API-visible attribute record;
 * hardware_end: the final byte the dword-based VAP fetch may read.
 *
 * allocation_bytes is the exact admitted allocation or suballocation extent,
 * never an unrelated parent BO's capacity. */
static inline bool
r300_r2vb_source_extents_init(uint64_t allocation_bytes,
                               uint64_t buffer_offset,
                               uint64_t attribute_offset,
                               uint32_t stride_bytes,
                               uint32_t start,
                               uint32_t count,
                               uint32_t semantic_record_bytes,
                               uint32_t hardware_fetch_dwords,
                               struct r300_r2vb_source_extents *out)
{
   if (!out || count == 0 || semantic_record_bytes == 0 ||
       hardware_fetch_dwords == 0)
      return false;

   uint64_t start_bytes;
   uint64_t source_offset;
   uint64_t tail_bytes;
   uint64_t final_record;
   uint64_t semantic_end;
   uint64_t hardware_bytes = (uint64_t)hardware_fetch_dwords * 4u;
   uint64_t hardware_end;

   if (!r300_r2vb_u64_mul(start, stride_bytes, &start_bytes) ||
       !r300_r2vb_u64_add(buffer_offset, attribute_offset, &source_offset) ||
       !r300_r2vb_u64_add(source_offset, start_bytes, &source_offset) ||
       !r300_r2vb_u64_mul((uint64_t)count - 1u, stride_bytes, &tail_bytes) ||
       !r300_r2vb_u64_add(source_offset, tail_bytes, &final_record) ||
       !r300_r2vb_u64_add(final_record, semantic_record_bytes,
                          &semantic_end) ||
       !r300_r2vb_u64_add(final_record, hardware_bytes, &hardware_end))
      return false;

   if (semantic_end > allocation_bytes || hardware_end > allocation_bytes)
      return false;

   *out = (struct r300_r2vb_source_extents) {
      .source_offset = source_offset,
      .semantic_end = semantic_end,
      .hardware_end = hardware_end,
   };
   return true;
}

/* Build the current slot-plus-model producer transaction.  The slot stream is
 * fixed F32_4.  A gated F32_2 model therefore yields 4 + 2 = 6 fetched dwords,
 * XY01 at the producer input, and an unchanged FP32x4 producer output carrier. */
static inline bool
r300_r2vb_source_contract_init(enum r300_vertex_format_id format,
                                bool float2_enabled,
                                uint64_t allocation_bytes,
                                uint64_t buffer_offset,
                                uint64_t attribute_offset,
                                uint32_t stride_bytes,
                                uint32_t start,
                                uint32_t count,
                                struct r300_r2vb_source_contract *out)
{
   const struct r300_vertex_format_semantics *model =
      r300_vertex_format_semantics(format);
   const struct r300_vertex_format_semantics *slot =
      r300_vertex_format_semantics(R300_VERTEX_FORMAT_F32_4);

   if (!out || !model || !slot ||
       !r300_r2vb_source_format_admitted(format, float2_enabled))
      return false;

   const uint32_t hardware_fetch_bytes = model->hardware_fetch_dwords * 4u;
   if (stride_bytes == 0 || stride_bytes % 4u != 0 ||
       stride_bytes < model->semantic_record_bytes ||
       stride_bytes < hardware_fetch_bytes)
      return false;

   struct r300_r2vb_source_extents extents;
   if (!r300_r2vb_source_extents_init(
          allocation_bytes, buffer_offset, attribute_offset, stride_bytes,
          start, count, model->semantic_record_bytes,
          model->hardware_fetch_dwords, &extents))
      return false;

   *out = (struct r300_r2vb_source_contract) {
      .format = format,
      .stride_bytes = stride_bytes,
      .start = start,
      .count = count,
      .model_fetch_dwords = model->hardware_fetch_dwords,
      .logical_components = model->logical_components,
      .vap_vtx_size_dwords =
         slot->hardware_fetch_dwords + model->hardware_fetch_dwords,
      .model_data_type = model->data_type,
      .model_psc_swizzle = r300_vertex_format_psc_swizzle(model),
      .extents = extents,
   };
   return true;
}

static inline uint16_t
r300_r2vb_source_stream_cntl(
   const struct r300_r2vb_source_stream_contract *stream)
{
   if (!stream || stream->dst_vec > R300_R2VB_PSC_DST_VEC_MASK)
      return 0;

   return ((uint16_t)stream->data_type & R300_R2VB_PSC_DATA_TYPE_MASK) |
          ((uint16_t)stream->dst_vec << R300_R2VB_PSC_DST_VEC_SHIFT) |
          (stream->last ? R300_R2VB_PSC_LAST_VEC : 0u);
}

/* Build the exact slot-plus-model PSC/VAP tuple.  Destination vectors are
 * caller-owned interface state, but must be distinct five-bit values. */
static inline bool
r300_r2vb_source_tuple_init(const struct r300_r2vb_source_contract *contract,
                             uint8_t slot_dst_vec,
                             uint8_t model_dst_vec,
                             struct r300_r2vb_source_tuple *out)
{
   const struct r300_vertex_format_semantics *slot =
      r300_vertex_format_semantics(R300_VERTEX_FORMAT_F32_4);
   const struct r300_vertex_format_semantics *model =
      contract ? r300_vertex_format_semantics(contract->format) : NULL;

   if (!contract || !out || !slot || !model ||
       slot_dst_vec > R300_R2VB_PSC_DST_VEC_MASK ||
       model_dst_vec > R300_R2VB_PSC_DST_VEC_MASK ||
       slot_dst_vec == model_dst_vec ||
       contract->model_fetch_dwords != model->hardware_fetch_dwords ||
       contract->logical_components != model->logical_components ||
       contract->model_data_type != model->data_type ||
       contract->model_psc_swizzle !=
          r300_vertex_format_psc_swizzle(model) ||
       contract->vap_vtx_size_dwords !=
          slot->hardware_fetch_dwords + model->hardware_fetch_dwords)
      return false;

   *out = (struct r300_r2vb_source_tuple) {0};
   out->stream[R300_R2VB_SOURCE_STREAM_SLOT] =
      (struct r300_r2vb_source_stream_contract) {
         .data_type = slot->data_type,
         .fetch_dwords = slot->hardware_fetch_dwords,
         .dst_vec = slot_dst_vec,
         .last = false,
         .psc_swizzle = r300_vertex_format_psc_swizzle(slot),
      };
   out->stream[R300_R2VB_SOURCE_STREAM_MODEL] =
      (struct r300_r2vb_source_stream_contract) {
         .data_type = model->data_type,
         .fetch_dwords = model->hardware_fetch_dwords,
         .dst_vec = model_dst_vec,
         .last = true,
         .psc_swizzle = r300_vertex_format_psc_swizzle(model),
      };

   const uint16_t slot_cntl = r300_r2vb_source_stream_cntl(
      &out->stream[R300_R2VB_SOURCE_STREAM_SLOT]);
   const uint16_t model_cntl = r300_r2vb_source_stream_cntl(
      &out->stream[R300_R2VB_SOURCE_STREAM_MODEL]);
   out->prog_stream_cntl[0] =
      (uint32_t)slot_cntl | ((uint32_t)model_cntl << 16);
   out->prog_stream_cntl_ext[0] =
      (uint32_t)out->stream[R300_R2VB_SOURCE_STREAM_SLOT].psc_swizzle |
      ((uint32_t)out->stream[R300_R2VB_SOURCE_STREAM_MODEL].psc_swizzle
       << 16);
   out->vap_vtx_size_dwords = contract->vap_vtx_size_dwords;
   return true;
}

/* Validate a complete emitted tuple, including the zero tail.  This is a
 * structural source-transaction check: it does not prove BO residency,
 * LOAD_VBPNTR relocation identity, topology, clipping, or final delivery. */
static inline bool
r300_r2vb_source_tuple_matches(
   const struct r300_r2vb_source_tuple *expected,
   const uint32_t prog_stream_cntl[8],
   const uint32_t prog_stream_cntl_ext[8],
   uint32_t vap_vtx_size_dwords)
{
   if (!expected || !prog_stream_cntl || !prog_stream_cntl_ext ||
       vap_vtx_size_dwords != expected->vap_vtx_size_dwords ||
       prog_stream_cntl[0] != expected->prog_stream_cntl[0] ||
       prog_stream_cntl_ext[0] != expected->prog_stream_cntl_ext[0])
      return false;

   for (unsigned i = 1; i < 8; i++) {
      if (prog_stream_cntl[i] != 0 || prog_stream_cntl_ext[i] != 0)
         return false;
   }
   return true;
}

#endif /* AMD_R300_R2VB_SOURCE_CONTRACT_H */
