/*
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "amd/r300/common/r300_r2vb_source_contract.h"
#include "r300_reg.h"

_Static_assert(R300_VERTEX_DATA_FLOAT_1 == R300_DATA_TYPE_FLOAT_1,
               "neutral FLOAT_1 type must match R300");
_Static_assert(R300_VERTEX_DATA_FLOAT_2 == R300_DATA_TYPE_FLOAT_2,
               "neutral FLOAT_2 type must match R300");
_Static_assert(R300_VERTEX_DATA_FLOAT_3 == R300_DATA_TYPE_FLOAT_3,
               "neutral FLOAT_3 type must match R300");
_Static_assert(R300_VERTEX_DATA_FLOAT_4 == R300_DATA_TYPE_FLOAT_4,
               "neutral FLOAT_4 type must match R300");
_Static_assert(R300_VERTEX_SELECT_ZERO == R300_SWIZZLE_SELECT_FP_ZERO,
               "neutral zero selector must match R300");
_Static_assert(R300_VERTEX_SELECT_ONE == R300_SWIZZLE_SELECT_FP_ONE,
               "neutral one selector must match R300");

static unsigned failures;

#define CHECK(COND, NAME)                 \
   do {                                   \
      if (COND) {                         \
         printf("  ok   - %s\n", NAME); \
      } else {                            \
         printf("  FAIL - %s\n", NAME); \
         failures++;                      \
      }                                   \
   } while (0)

static void
check_format_semantics(void)
{
   static const uint16_t expected_swizzle[] = {
      [R300_VERTEX_FORMAT_F32_1] = R300_VAP_SWIZZLE_X001,
      [R300_VERTEX_FORMAT_F32_2] = R300_VAP_SWIZZLE_XY01,
      [R300_VERTEX_FORMAT_F32_3] = R300_VAP_SWIZZLE_XYZ1,
      [R300_VERTEX_FORMAT_F32_4] = R300_VAP_SWIZZLE_XYZW,
   };

   for (unsigned components = 1; components <= 4; components++) {
      const enum r300_vertex_format_id id =
         r300_vertex_format_from_f32_components(components);
      const struct r300_vertex_format_semantics *format =
         r300_vertex_format_semantics(id);

      CHECK(format != NULL, "F32 semantic record exists");
      CHECK(format && format->physical_components == components,
            "physical component count is exact");
      CHECK(format && format->semantic_record_bytes == components * 4u,
            "semantic byte count is exact");
      CHECK(format && format->hardware_fetch_dwords == components,
            "hardware fetch dword count is exact");
      CHECK(format && format->logical_components == 4,
            "producer-facing value is a logical vec4");
      CHECK(format && r300_vertex_format_psc_swizzle(format) ==
                         expected_swizzle[id],
            "PSC swizzle is X001, XY01, XYZ1, or XYZW");
   }

   CHECK(r300_vertex_format_from_f32_components(0) ==
            R300_VERTEX_FORMAT_INVALID,
         "zero-component F32 format fails closed");
   CHECK(r300_vertex_format_from_f32_components(5) ==
            R300_VERTEX_FORMAT_INVALID,
         "five-component F32 format fails closed");
   CHECK(r300_vertex_format_semantics(R300_VERTEX_FORMAT_INVALID) == NULL,
         "invalid format identity fails closed");
   CHECK(r300_vertex_format_semantics(R300_VERTEX_FORMAT_COUNT) == NULL,
         "format-count sentinel fails closed");
}

static void
check_gate_and_policy(void)
{
   CHECK(r300_r2vb_float2_source_gate_value("1"),
         "FLOAT_2 source gate accepts exact 1");
   CHECK(!r300_r2vb_float2_source_gate_value(NULL),
         "FLOAT_2 source gate rejects unset");
   CHECK(!r300_r2vb_float2_source_gate_value(""),
         "FLOAT_2 source gate rejects empty");
   CHECK(!r300_r2vb_float2_source_gate_value("0"),
         "FLOAT_2 source gate rejects zero");
   CHECK(!r300_r2vb_float2_source_gate_value("true"),
         "FLOAT_2 source gate rejects boolean alias");
   CHECK(!r300_r2vb_float2_source_gate_value("11"),
         "FLOAT_2 source gate rejects prefix match");

   CHECK(!r300_r2vb_source_format_admitted(R300_VERTEX_FORMAT_F32_1, true),
         "FLOAT_1 remains outside the producer contract");
   CHECK(!r300_r2vb_source_format_admitted(R300_VERTEX_FORMAT_F32_2, false),
         "FLOAT_2 remains outside the established source domain");
   CHECK(r300_r2vb_source_format_admitted(R300_VERTEX_FORMAT_F32_2, true),
         "FLOAT_2 enters only through its explicit gate");
   CHECK(r300_r2vb_source_format_admitted(R300_VERTEX_FORMAT_F32_3, false),
         "FLOAT_3 remains the established source control");
   CHECK(r300_r2vb_source_format_admitted(R300_VERTEX_FORMAT_F32_4, false),
         "FLOAT_4 remains the established source control");
}

static void
check_float2_contract(void)
{
   struct r300_r2vb_source_contract contract;

   CHECK(!r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_2, false, 40, 0, 0, 8, 0, 5,
            &contract),
         "FLOAT_2 contract stays closed without its gate");

   CHECK(r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_2, true, 40, 0, 0, 8, 0, 5,
            &contract) &&
            contract.model_fetch_dwords == 2 &&
            contract.logical_components == 4 &&
            contract.vap_vtx_size_dwords == 6 &&
            contract.model_data_type == R300_VERTEX_DATA_FLOAT_2 &&
            contract.model_psc_swizzle == R300_VAP_SWIZZLE_XY01 &&
            contract.extents.source_offset == 0 &&
            contract.extents.semantic_end == 40 &&
            contract.extents.hardware_end == 40,
         "packed FLOAT_2 builds the exact 6-dword XY01 transaction");

   CHECK(r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_2, true, 72, 0, 0, 16, 0, 5,
            &contract) &&
            contract.stride_bytes == 16 &&
            contract.extents.semantic_end == 72 &&
            contract.extents.hardware_end == 72,
         "interleaved FLOAT_2 preserves the application stride");

   CHECK(r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_2, true, 192, 64, 8, 16, 3, 5,
            &contract) &&
            contract.extents.source_offset == 120 &&
            contract.extents.semantic_end == 192 &&
            contract.extents.hardware_end == 192,
         "buffer offset, attribute offset, and first vertex compose exactly");

   CHECK(!r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_2, true, 39, 0, 0, 8, 0, 5,
            &contract),
         "one-byte-short FLOAT_2 allocation fails closed");
   CHECK(!r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_2, true, 40, 0, 0, 4, 0, 5,
            &contract),
         "sub-record FLOAT_2 stride fails closed");
   CHECK(!r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_2, true, 40, 0, 0, 10, 0, 5,
            &contract),
         "non-dword FLOAT_2 stride fails closed");
   CHECK(!r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_2, true, 40, 0, 0, 8, 0, 0,
            &contract),
         "zero-count FLOAT_2 draw fails closed");
   CHECK(!r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_2, true, UINT64_MAX,
            UINT64_MAX - 3u, 8, 8, 0, 1, &contract),
         "source-offset overflow fails closed");
}

static void
check_dual_bounds(void)
{
   struct r300_r2vb_source_extents extents;

   CHECK(r300_r2vb_source_extents_init(
            20, 0, 0, 8, 0, 3, 3, 1, &extents) &&
            extents.semantic_end == 19 && extents.hardware_end == 20,
         "semantic and dword-rounded hardware bounds remain independent");
   CHECK(!r300_r2vb_source_extents_init(
            19, 0, 0, 8, 0, 3, 3, 1, &extents),
         "hardware over-read beyond the admitted allocation fails closed");
}

static void
check_existing_controls(void)
{
   struct r300_r2vb_source_contract contract;

   CHECK(r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_3, false, 60, 0, 0, 12, 0, 5,
            &contract) &&
            contract.model_fetch_dwords == 3 &&
            contract.vap_vtx_size_dwords == 7 &&
            contract.model_psc_swizzle == R300_VAP_SWIZZLE_XYZ1,
         "FLOAT_3 remains the 7-dword XYZ1 positive control");
   CHECK(r300_r2vb_source_contract_init(
            R300_VERTEX_FORMAT_F32_4, false, 80, 0, 0, 16, 0, 5,
            &contract) &&
            contract.model_fetch_dwords == 4 &&
            contract.vap_vtx_size_dwords == 8 &&
            contract.model_psc_swizzle == R300_VAP_SWIZZLE_XYZW,
         "FLOAT_4 remains the 8-dword XYZW positive control");
}

int
main(void)
{
   check_format_semantics();
   check_gate_and_policy();
   check_float2_contract();
   check_dual_bounds();
   check_existing_controls();

   printf("r300 R2VB source contract: %u failure(s)\n", failures);
   return failures != 0;
}
