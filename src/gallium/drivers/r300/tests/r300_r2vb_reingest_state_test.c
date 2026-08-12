/*
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdio.h>

#include "r300_r2vb_reingest_state.h"

static unsigned failures;

#define CHECK(condition, name)                                                \
   do {                                                                       \
      if (!(condition)) {                                                     \
         fprintf(stderr, "FAIL %s: %s\n", name, #condition);                \
         failures++;                                                          \
      }                                                                        \
   } while (0)

static void
check_gate_matrix(void)
{
   CHECK(!r300_r2vb_position_only_output_enabled(false, false),
         "closed gates emit no position-only output packet");
   CHECK(r300_r2vb_position_only_output_dwords(false, false) == 0,
         "closed gates reserve no output packet dwords");
   CHECK(r300_r2vb_position_only_assembly_dwords(false) == 0,
         "closed C1c gate reserves no assembly packet dwords");
   CHECK(r300_r2vb_vte_w0_dwords(false) == 0,
         "closed C1b gate reserves no W0 packet dwords");
   CHECK(r300_r2vb_vte_restore_dwords(false) == 0,
         "closed C1b gate reserves no VTE restore packet dwords");

   CHECK(r300_r2vb_vte_w0_dwords(true) == 2,
         "C1b reserves the VTE W0 write packet");
   CHECK(r300_r2vb_vte_restore_dwords(true) == 2,
         "C1b reserves a same-IB VTE restore packet");

   CHECK(r300_r2vb_position_only_output_enabled(true, false),
         "C0 enables the position-only output packet");
   CHECK(r300_r2vb_position_only_output_dwords(true, false) == 3,
         "C0 reserves one output SEQ packet");
   CHECK(r300_r2vb_position_only_assembly_dwords(false) == 0,
         "C0 does not reserve the assembly packet");

   CHECK(r300_r2vb_position_only_output_enabled(false, true),
         "C1c enables the position-only output packet");
   CHECK(r300_r2vb_position_only_output_dwords(false, true) == 3,
         "C1c reserves one output SEQ packet");
   CHECK(r300_r2vb_position_only_assembly_dwords(true) == 3,
         "C1c reserves one assembly SEQ packet");

   CHECK(r300_r2vb_position_only_output_dwords(true, true) == 3,
         "C0 and C1c share one output SEQ packet");
}

static void
check_position_only_state(void)
{
   const uint32_t good_fmt0 = r300_r2vb_position_only_output_fmt0();
   const uint32_t good_fmt1 = r300_r2vb_position_only_output_fmt1();

   CHECK(r300_r2vb_position_only_output_matches(good_fmt0, good_fmt1),
         "position-only format is a known-good tuple");
   CHECK(r300_r2vb_position_only_vtx_state_cntl() == 0,
         "position-only assembly clears VTX_STATE_CNTL");
   CHECK(r300_r2vb_position_only_vtx_assm() == R300_INPUT_CNTL_POS,
         "position-only assembly selects POS only");

   CHECK(!r300_r2vb_position_only_output_matches(
            good_fmt0 | R300_VAP_OUTPUT_VTX_FMT_0__PT_SIZE_PRESENT, good_fmt1),
         "inherited point-size output is a known-bad tuple");
   CHECK(!r300_r2vb_position_only_output_matches(
            good_fmt0, R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS),
         "inherited texcoord output is a known-bad tuple");
}

int
main(void)
{
   check_gate_matrix();
   check_position_only_state();

   if (failures) {
      fprintf(stderr, "r300_r2vb_reingest_state_test: %u failure(s)\n",
              failures);
      return 1;
   }

   printf("r300_r2vb_reingest_state_test: all checks passed\n");
   return 0;
}
