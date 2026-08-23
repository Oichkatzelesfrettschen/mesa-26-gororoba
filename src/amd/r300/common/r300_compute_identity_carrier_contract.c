/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_compute_identity_carrier.h"
#include "r300_reg.h"

const struct r300_compute_identity_carrier_contract
   r300_compute_identity_carrier_contract = {
      .operation_id = R300_OPERATION_ID_IDENTITY_MAP,
      .implementation_id =
         R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER,
      .gpu_route_contract_id =
         R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER,
      .admission_id = R300_ROUTE_ADMISSION_R2VB_FP24_IDENTITY,
      .domain = R300_NUM_DOMAIN_FP24_RTZ,
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
      .target = {
         .rb3d_color_format = R300_COLOR_FORMAT_ARGB32323232,
         .us_out_fmt = {
            R300_US_OUT_FMT_C4_32_FP | R300_C0_SEL_B | R300_C1_SEL_G |
               R300_C2_SEL_R | R300_C3_SEL_A,
            R300_US_OUT_FMT_UNUSED,
            R300_US_OUT_FMT_UNUSED,
            R300_US_OUT_FMT_UNUSED,
         },
      },
      .record_dwords = R300_COMPUTE_IDENTITY_CARRIER_RECORD_DWORDS,
      .record_bytes = R300_COMPUTE_IDENTITY_CARRIER_RECORD_BYTES,
      .max_records = R300_COMPUTE_IDENTITY_CARRIER_MAX_RECORDS,
      .input_alignment = R300_COMPUTE_IDENTITY_CARRIER_INPUT_ALIGN,
      .output_alignment = R300_COMPUTE_IDENTITY_CARRIER_OUTPUT_ALIGN,
};
