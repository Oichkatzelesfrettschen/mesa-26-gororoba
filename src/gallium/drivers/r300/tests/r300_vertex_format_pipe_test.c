/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "r300_vertex_format_pipe.h"

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
check_source_contract_bridge(void)
{
   struct r300_r2vb_source_contract contract;

   CHECK(!r300_r2vb_source_contract_from_pipe(
             PIPE_FORMAT_R32G32_FLOAT, false, 40, 0, 0, 8, 0, 5,
             &contract),
         "FLOAT_2 pipe source remains closed without its gate");
   CHECK(r300_r2vb_source_contract_from_pipe(
            PIPE_FORMAT_R32G32_FLOAT, true, 40, 0, 0, 8, 0, 5,
            &contract) &&
            contract.format == R300_VERTEX_FORMAT_F32_2 &&
            contract.vap_vtx_size_dwords == 6,
         "FLOAT_2 pipe source reaches the six-dword neutral contract");
   CHECK(r300_r2vb_source_contract_from_pipe(
            PIPE_FORMAT_R32G32B32_FLOAT, false, 60, 0, 0, 12, 0, 5,
            &contract) &&
            contract.format == R300_VERTEX_FORMAT_F32_3 &&
            contract.vap_vtx_size_dwords == 7,
         "FLOAT_3 pipe source reaches the seven-dword control contract");
   CHECK(r300_r2vb_source_contract_from_pipe(
            PIPE_FORMAT_R32G32B32A32_FLOAT, false, 80, 0, 0, 16, 0, 5,
            &contract) &&
            contract.format == R300_VERTEX_FORMAT_F32_4 &&
            contract.vap_vtx_size_dwords == 8,
         "FLOAT_4 pipe source reaches the eight-dword control contract");
   CHECK(!r300_r2vb_source_contract_from_pipe(
             PIPE_FORMAT_R8G8B8A8_UNORM, true, 20, 0, 0, 4, 0, 5,
             &contract),
         "non-F32 pipe source cannot construct an R2VB contract");
}

int
main(void)
{
   static const enum pipe_format pipe_formats[] = {
      PIPE_FORMAT_R32_FLOAT,
      PIPE_FORMAT_R32G32_FLOAT,
      PIPE_FORMAT_R32G32B32_FLOAT,
      PIPE_FORMAT_R32G32B32A32_FLOAT,
   };

   for (unsigned components = 1; components <= 4; components++) {
      enum r300_vertex_format_id format = R300_VERTEX_FORMAT_INVALID;
      CHECK(r300_vertex_format_from_pipe(pipe_formats[components - 1],
                                         &format) &&
               format == r300_vertex_format_from_f32_components(components),
            "pipe format maps to the neutral F32 identity");
      CHECK(r300_vertex_format_to_pipe(format) ==
               pipe_formats[components - 1],
            "neutral F32 identity maps back to pipe format");
   }

   enum r300_vertex_format_id format = R300_VERTEX_FORMAT_F32_4;
   CHECK(!r300_vertex_format_from_pipe(PIPE_FORMAT_R8G8B8A8_UNORM,
                                       &format) &&
            format == R300_VERTEX_FORMAT_INVALID,
         "non-F32 pipe format fails closed");
   CHECK(!r300_vertex_format_from_pipe(PIPE_FORMAT_R32G32_FLOAT, NULL),
         "NULL pipe-format output fails closed");
   CHECK(r300_vertex_format_to_pipe(R300_VERTEX_FORMAT_INVALID) ==
            PIPE_FORMAT_NONE,
         "invalid neutral format maps to PIPE_FORMAT_NONE");

   check_source_contract_bridge();

   printf("r300 pipe-format adapter: %u failure(s)\n", failures);
   return failures != 0;
}
