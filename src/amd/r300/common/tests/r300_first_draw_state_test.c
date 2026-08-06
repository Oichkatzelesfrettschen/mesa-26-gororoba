/*
 * SPDX-License-Identifier: MIT
 *
 * Poison-model test for the neutral first-draw state contract: the current
 * fixed cell fails the contract for multiple named reasons, the contract
 * emission converges from every poisoned predecessor state, and each
 * proven-gate mutation fails independently.
 */

/* The asserts carry the test's side effects and verdicts, so they stay
 * live in NDEBUG builds.
 */
#undef NDEBUG

#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_tcl_bypass_triangle.h"

#include "r300_reg.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The poison vectors model distinct hostile predecessors: cleared
 * registers, saturated registers, alternating bits, and the three values
 * silicon proved each suppress every color write. A stream is
 * context-independent only when every vector converges to the contract.
 */
static const uint32_t poison_vectors[] = {
   0x00000000, /* all zeroes: the three proven gates all open */
   0xffffffff, /* all ones: enables, masks, and stipple all latched */
   0xaaaaaaaa, /* alternating bits */
   0x55555555, /* alternating bits, inverted phase */
   0x00000f00, /* US_OUT_FMT UNUSED-shaped residue */
};

static uint32_t
find_entry(const struct r300_first_draw_contract *contract, uint16_t reg)
{
   for (uint32_t i = 0; i < contract->count; i++) {
      if (contract->entries[i].reg == reg) {
         return i;
      }
   }
   assert(!"contract entry absent");
   return 0;
}

static bool
report_names(const struct r300_first_draw_contract *contract,
             const struct r300_first_draw_check_report *report, uint16_t reg)
{
   for (uint32_t i = 0; i < report->unsatisfied_count; i++) {
      if (contract->entries[report->unsatisfied[i]].reg == reg) {
         return true;
      }
   }
   return false;
}

int
main(void)
{
   struct r300_first_draw_params params = {
      .width = 64,
      .height = 64,
      .max_vtx_index = 2,
      .texture_enabled = false,
   };
   struct r300_first_draw_contract contract;
   assert(r300_first_draw_contract_resolve(&params, &contract) == 0);
   assert(contract.count >= 75);

   /* Parameter derivation: the scissor spans the full 64x64 target with
    * the non-R500 1440 bias, matching the traced reference values.
    */
   assert(contract.entries[find_entry(&contract, R300_SC_SCISSORS_TL)].value ==
          0x00b405a0);
   assert(contract.entries[find_entry(&contract, R300_SC_SCISSORS_BR)].value ==
          0x00bbe5df);
   assert(contract.entries[find_entry(&contract,
                                      R300_VAP_VF_MAX_VTX_INDX)].value == 2);

   /* Out-of-range extents refuse. */
   struct r300_first_draw_params bad = params;
   bad.width = 0;
   struct r300_first_draw_contract scratch;
   assert(r300_first_draw_contract_resolve(&bad, &scratch) == -22);
   bad.width = 4096 - 1440 + 2;
   assert(r300_first_draw_contract_resolve(&bad, &scratch) == -22);

   /* The reference artifacts stay out: no depth-resource or dummy-texture
    * descriptor word is emitted for a draw that binds neither.
    */
   for (uint32_t i = 0; i < contract.count; i++) {
      assert(contract.entries[i].reg != R300_ZB_DEPTHOFFSET);
      assert(contract.entries[i].reg != R300_TX_OFFSET_0);
      assert(contract.entries[i].disposition != R300_FDS_REFERENCE_ARTIFACT);
   }

   /* Known-bad calibration: the current fixed cell leaves the contract
    * unsatisfied for multiple named reasons, and under all-zero poison the
    * three silicon-proven gates are among them. A checker reporting only
    * its first failure would hide two open gates behind the third.
    */
   struct r300_fragment_binary fs;
   assert(r300_tcl_bypass_triangle_reference_fs(&fs) == 0);
   struct r300_tcl_bypass_triangle_params cell_params = {
      .vertex_offset = 0,
      .color_pitch_format = 0x00c00040,
      .fragment_binary = &fs,
   };
   struct r300_tcl_bypass_triangle_ib cell;
   assert(r300_tcl_bypass_triangle_emit(&cell_params, &cell) == 0);

   struct r300_first_draw_check_report report;
   uint32_t cell_failures = r300_first_draw_state_check(
      &contract, cell.ib, cell.ib_size_dwords, 0x00000000, &report);
   assert(cell_failures >= 70);
   assert(report_names(&contract, &report, R300_US_OUT_FMT_0));
   assert(report_names(&contract, &report, RB3D_COLOR_CHANNEL_MASK));
   assert(report_names(&contract, &report, R300_SC_SCREENDOOR));
   printf("known-bad: fixed cell leaves %u of %u clauses unsatisfied\n",
          cell_failures, contract.count);

   /* The contract emission converges from every poison vector, and a
    * successor stream -- contract state followed by the current cell --
    * still satisfies every clause, so the cell overwrites nothing the
    * contract establishes incompatibly except the values the contract
    * itself owns.
    */
   uint32_t state_ib[256];
   int state_dwords = r300_first_draw_state_emit(&contract, state_ib, 256);
   assert(state_dwords > 0);

   uint32_t successor[1024];
   memcpy(successor, state_ib, (size_t)state_dwords * 4);
   memcpy(successor + state_dwords, cell.ib, cell.ib_size_dwords * 4);
   uint32_t successor_dwords = (uint32_t)state_dwords + cell.ib_size_dwords;

   for (unsigned v = 0; v < sizeof(poison_vectors) / 4; v++) {
      assert(r300_first_draw_state_check(&contract, state_ib,
                                         (uint32_t)state_dwords,
                                         poison_vectors[v], &report) == 0);
      uint32_t left = r300_first_draw_state_check(
         &contract, successor, successor_dwords, poison_vectors[v], &report);
      if (left != 0) {
         for (uint32_t i = 0; i < report.unsatisfied_count; i++) {
            fprintf(stderr, "poison 0x%08x leaves %s\n", poison_vectors[v],
                    contract.entries[report.unsatisfied[i]].name);
         }
      }
      assert(left == 0);
   }

   /* Gate mutations fail independently: dropping any one proven gate from
    * the emission is detected alone, so the orthogonal-gate composition
    * cannot hide behind the other two.
    */
   const uint16_t gates[] = {R300_US_OUT_FMT_0, RB3D_COLOR_CHANNEL_MASK,
                             R300_SC_SCREENDOOR};
   for (unsigned g = 0; g < 3; g++) {
      uint32_t mutated[256];
      memcpy(mutated, state_ib, (size_t)state_dwords * 4);
      for (int i = 0; i + 1 < state_dwords; i += 2) {
         if (mutated[i] == CP_PACKET0(gates[g], 0)) {
            /* Overwrite the pair with a harmless scratch write so packet
             * structure survives while the gate write disappears.
             */
            mutated[i] = CP_PACKET0(R300_SU_TEX_WRAP, 0);
            mutated[i + 1] = 0;
         }
      }
      uint32_t left = r300_first_draw_state_check(
         &contract, mutated, (uint32_t)state_dwords, 0x00000000, &report);
      assert(left == 1);
      assert(report_names(&contract, &report, gates[g]));
   }

   /* Write-order control: emission is deterministic, and the ordering
    * fragments precede the pipelined state -- the cache flush pair lands
    * before any RB3D backend write.
    */
   uint32_t state_ib2[256];
   assert(r300_first_draw_state_emit(&contract, state_ib2, 256) ==
          state_dwords);
   assert(memcmp(state_ib, state_ib2, (size_t)state_dwords * 4) == 0);
   assert(state_ib[0] == CP_PACKET0(RADEON_WAIT_UNTIL, 0));
   assert(state_ib[2] == CP_PACKET0(R300_RB3D_DSTCACHE_CTLSTAT, 0));
   int short_room = r300_first_draw_state_emit(&contract, state_ib2, 4);
   assert(short_room == -28);

   r300_tcl_bypass_triangle_release(&cell);
   printf("r300_first_draw_state_test: %u contract clauses, %zu poison "
          "vectors, 3 gate mutations: OK\n",
          contract.count, sizeof(poison_vectors) / 4);
   return 0;
}
