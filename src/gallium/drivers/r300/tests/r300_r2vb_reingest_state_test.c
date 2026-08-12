/*
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "r300_context.h"
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
         "closed vertex-assembly gate reserves no assembly packet dwords");
   CHECK(r300_r2vb_vte_w0_dwords(false) == 0,
         "closed VTE W0 gate reserves no override packet dwords");
   CHECK(r300_r2vb_vte_restore_dwords(false) == 0,
         "closed VTE W0 gate reserves no restore packet dwords");

   CHECK(r300_r2vb_vte_w0_dwords(true) == 2,
         "VTE W0 gate reserves the override packet");
   CHECK(r300_r2vb_vte_restore_dwords(true) == 2,
         "VTE W0 gate reserves a same-IB restore packet");

   CHECK(r300_r2vb_position_only_output_enabled(true, false),
         "position-output gate enables the position-only output packet");
   CHECK(r300_r2vb_position_only_output_dwords(true, false) == 3,
         "position-output gate reserves one output SEQ packet");
   CHECK(r300_r2vb_position_only_assembly_dwords(false) == 0,
         "position-output gate does not reserve the assembly packet");

   CHECK(r300_r2vb_position_only_output_enabled(false, true),
         "vertex-assembly gate enables the position-only output packet");
   CHECK(r300_r2vb_position_only_output_dwords(false, true) == 3,
         "vertex-assembly gate reserves one output SEQ packet");
   CHECK(r300_r2vb_position_only_assembly_dwords(true) == 3,
         "vertex-assembly gate reserves one assembly SEQ packet");

   CHECK(r300_r2vb_position_only_output_dwords(true, true) == 3,
         "position-output and vertex-assembly gates share one output SEQ packet");
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

static void
check_full_flush_rearm(bool has_tcl)
{
   static struct r300_context context;
   static struct r300_screen screen;
   struct r300_context *r300 = &context;
   struct r300_atom *atom;
   unsigned atom_index = 0;

   memset(&context, 0, sizeof(context));
   memset(&screen, 0, sizeof(screen));
   context.screen = &screen;
   context.dirty_hw = 7;
   screen.caps.has_tcl = has_tcl;

   foreach_atom(r300, atom) {
      atom->state = atom_index % 3 == 0 ? atom : NULL;
      atom->allow_null_state = atom_index % 3 == 1;
      atom->dirty = false;
      atom_index++;
   }
   context.vs_state.state = &context.vs_state;
   context.vs_constants.state = &context.vs_constants;
   context.clip_state.state = &context.clip_state;

   r300_rearm_after_hardware_flush(&context);

   CHECK(context.dirty_hw == 0,
         "full rearm clears the hardware-dirty counter");
   CHECK(context.vertex_arrays_dirty,
         "full rearm dirties vertex arrays");

   foreach_atom(r300, atom) {
      bool expected = atom->state || atom->allow_null_state;
      if (!has_tcl && (atom == &context.vs_state ||
                       atom == &context.vs_constants ||
                       atom == &context.clip_state))
         expected = false;
      CHECK(atom->dirty == expected,
            "full rearm dirties every eligible atom exactly once");
   }
   CHECK(context.first_dirty == &context.gpu_flush,
         "full rearm starts the dirty span at the first atom");
   CHECK(context.last_dirty == &context.query_start + 1,
         "full rearm ends the dirty span at the last atom");
}

int
main(void)
{
   check_gate_matrix();
   check_position_only_state();
   check_full_flush_rearm(true);
   check_full_flush_rearm(false);

   if (failures) {
      fprintf(stderr, "r300_r2vb_reingest_state_test: %u failure(s)\n",
              failures);
      return 1;
   }

   printf("r300_r2vb_reingest_state_test: all checks passed\n");
   return 0;
}
