/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_compute_identity_carrier.h"

#include "amd_family.h"
#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_r2vb_carrier_delivery.h"
#include "r300_vertex_format.h"
#include "r300_vertex_stream.h"

#include <errno.h>
#include <string.h>

int
r300_compute_identity_carrier_layout(uint32_t record_count,
                                     struct r300_r2vb_producer_layout *out)
{
   if (out == NULL || record_count < 1 ||
       record_count > r300_compute_identity_carrier_contract.max_records)
      return -EINVAL;
   return r300_r2vb_producer_layout_single_row(record_count, out);
}

uint64_t
r300_compute_identity_carrier_output_bytes(
   const struct r300_r2vb_producer_layout *layout)
{
   return (uint64_t)layout->pitch_pixels * layout->height *
          r300_compute_identity_carrier_contract.record_bytes;
}

int
r300_compute_identity_carrier_emit(
   const struct r300_compute_identity_carrier_params *params,
   struct r300_r2vb_fetched_producer_ib *out)
{
   if (params == NULL || out == NULL)
      return -EINVAL;
   memset(out, 0, sizeof(*out));

   struct r300_r2vb_producer_layout layout;
   int rc = r300_compute_identity_carrier_layout(params->record_count,
                                                 &layout);
   if (rc != 0)
      return rc;
   if (params->carrier_offset %
          r300_compute_identity_carrier_contract.output_alignment != 0 ||
       params->source_offset %
          r300_compute_identity_carrier_contract.input_alignment != 0)
      return -EINVAL;
   /* The color backend's bound: the kernel CS checker holds
    * pitch * height * cpp + offset inside the BO, so the route holds it
    * here before the stream exists. */
   if (r300_compute_identity_carrier_output_bytes(&layout) >
       params->carrier_bo_size_bytes - params->carrier_offset)
      return -ERANGE;

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
   const struct r300_r2vb_fetched_producer_params producer = {
      .carrier_offset = params->carrier_offset,
      .layout = layout,
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
      .target = &r300_compute_identity_carrier_contract.target,
      .source = {
         .format_id = r300_compute_identity_carrier_contract.input_format_id,
         .offset_bytes = params->source_offset,
         .stride_bytes = r300_compute_identity_carrier_contract.record_bytes,
         .bo_size_bytes = params->source_bo_size_bytes,
      },
      .slot_offset_bytes = params->slot_offset_bytes,
      .slot_bo_size_bytes = params->slot_bo_size_bytes,
   };
   rc = r300_r2vb_fetched_producer_emit(&producer, out);
   r300_fragment_binary_finish(&fs);
   if (rc != 0)
      return rc;
   rc = r300_r2vb_fetched_producer_validate_reloc_sites(out);
   if (rc != 0) {
      r300_r2vb_fetched_producer_release(out);
      memset(out, 0, sizeof(*out));
   }
   return rc;
}

int
r300_compute_identity_carrier_reference_emit(
   struct r300_r2vb_fetched_producer_ib *out)
{
   const struct r300_compute_identity_carrier_params params = {
      .record_count = R300_COMPUTE_IDENTITY_CARRIER_REFERENCE_RECORDS,
      .carrier_offset = 0,
      .carrier_bo_size_bytes = R300_R2VB_FETCHED_REFERENCE_BO_BYTES,
      .source_offset = 0,
      .source_bo_size_bytes = R300_R2VB_FETCHED_REFERENCE_BO_BYTES,
      .slot_offset_bytes = 0,
      .slot_bo_size_bytes = R300_R2VB_FETCHED_REFERENCE_BO_BYTES,
   };
   return r300_compute_identity_carrier_emit(&params, out);
}

int
r300_compute_identity_carrier_expected(const uint32_t *input,
                                       uint32_t record_count, uint32_t *out,
                                       uint32_t out_dwords)
{
   if (input == NULL || out == NULL || record_count == 0)
      return -EINVAL;
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)input,
      .stride = r300_compute_identity_carrier_contract.record_bytes,
      .size_bytes =
         (uint64_t)record_count *
         r300_compute_identity_carrier_contract.record_bytes,
   };
   return r300_r2vb_identity_deliver(
      r300_compute_identity_carrier_contract.input_format_id,
      &stream, 0, record_count, out, out_dwords);
}
