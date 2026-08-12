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
#include <errno.h>
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

/* Hand-authored RS480 64x64 state. This stream is independent of the
 * resolver table, so a copied table mistake cannot make both sides green.
 */
struct known_good_first_draw_write {
   uint16_t reg;
   uint32_t value;
};

static const struct known_good_first_draw_write known_good_writes[] = {
   { R300_RB3D_DSTCACHE_CTLSTAT, 0x0000000a },
   { R300_ZB_ZCACHE_CTLSTAT, 0x00000003 },
   { RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN },
   { R300_GB_ENABLE, 0x00000000 },
   { R300_GB_SELECT, 0x00000000 },
   { R300_GB_AA_CONFIG, 0x00000000 },
   { R300_GB_Z_PEQ_CONFIG, 0x00000000 },
   { R300_RB3D_AARESOLVE_CTL, 0x00000000 },
   { R300_ZB_ZSTENCILCNTL, 0x00000000 },
   { R300_ZB_STENCILREFMASK, 0x00000000 },
   { R300_ZB_BW_CNTL, 0x00000000 },
   { R300_ZB_ZTOP, 0x00000001 },
   { R300_SC_HYPERZ, 0x0000001c },
   { R300_FG_ALPHA_FUNC, 0x00000000 },
   { R300_FG_FOG_BLEND, 0x00000000 },
   { R300_RB3D_ROPCNTL, 0x00000000 },
   { R300_RB3D_CBLEND, 0x00000000 },
   { R300_RB3D_ABLEND, 0x00000000 },
   { RB3D_COLOR_CHANNEL_MASK, 0x0000000f },
   { R300_RB3D_BLEND_COLOR, 0x00000000 },
   { R300_RB3D_DITHER_CTL, 0x00000000 },
   { R500_RB3D_DISCARD_SRC_PIXEL_LTE_THRESHOLD, 0x01010101 },
   { R500_RB3D_DISCARD_SRC_PIXEL_GTE_THRESHOLD, 0xfefefefe },
   { R300_SC_SCISSORS_TL, 0x00b405a0 },
   { R300_SC_SCISSORS_BR, 0x00bbe5df },
   { R300_SC_CLIPRECT_TL_0, 0x00b405a0 },
   { R300_SC_CLIPRECT_BR_0, 0x00bbe5df },
   { R300_SC_CLIP_RULE, 0x0000aaaa },
   { R300_SC_SCREENDOOR, 0x00ffffff },
   { R300_SC_EDGERULE, 0x2da49525 },
   { R300_VAP_CNTL, 0x0014025a },
   { R300_VAP_VTE_CNTL, 0x00000300 },
   { R300_VAP_VTX_STATE_CNTL, 0x00005555 },
   { R300_VAP_VSM_VTX_ASSM, 0x00000001 },
   { R300_VAP_PSC_SGN_NORM_CNTL, 0xaaaaaaaa },
   { R300_VAP_CLIP_CNTL, 0x00010000 },
   { R300_VAP_GB_VERT_CLIP_ADJ, 0x3f800000 },
   { R300_VAP_GB_VERT_DISC_ADJ, 0x3f800000 },
   { R300_VAP_GB_HORZ_CLIP_ADJ, 0x3f800000 },
   { R300_VAP_GB_HORZ_DISC_ADJ, 0x3f800000 },
   { R300_VAP_PVS_STATE_FLUSH_REG, 0x00000000 },
   { VAP_PVS_VTX_TIMEOUT_REG, 0x0000ffff },
   { R300_VAP_VF_MAX_VTX_INDX, 2 },
   { R300_SE_VPORT_XSCALE, 0x00000000 },
   { R300_SE_VPORT_XOFFSET, 0x00000000 },
   { R300_SE_VPORT_YSCALE, 0x00000000 },
   { R300_SE_VPORT_YOFFSET, 0x00000000 },
   { R300_SE_VPORT_ZSCALE, 0x00000000 },
   { R300_SE_VPORT_ZOFFSET, 0x00000000 },
   { R300_GA_POINT_S0, 0x00000000 },
   { R300_GA_POINT_T0, 0x00000000 },
   { R300_GA_POINT_S1, 0x3f800000 },
   { R300_GA_POINT_T1, 0x00000000 },
   { R300_GA_POINT_SIZE, 0x00060006 },
   { R300_GA_POINT_MINMAX, 0x00060006 },
   { R300_GA_LINE_CNTL, 0x00020006 },
   { R300_GA_LINE_STIPPLE_CONFIG, 0x00000000 },
   { R300_GA_LINE_STIPPLE_VALUE, 0x00000000 },
   { R300_GA_POLY_MODE, 0x00000000 },
   { R300_GA_ROUND_MODE, 0x00000005 },
   { R300_GA_OFFSET, 0x00000000 },
   { R300_GA_COLOR_CONTROL, 0x0003aaaa },
   { R300_SU_TEX_WRAP, 0x00000000 },
   { R300_SU_DEPTH_SCALE, 0x4b7fffff },
   { R300_SU_DEPTH_OFFSET, 0x00000000 },
   { R300_SU_POLY_OFFSET_ENABLE, 0x00000000 },
   { R300_SU_CULL_MODE, 0x00000004 },
   { R300_RS_IP_0, 0x00000c00 },
   { R300_RS_COUNT, 0x00040080 },
   { R300_RS_INST_COUNT, 0x00000000 },
   { R300_RS_INST_0, 0x00000000 },
   { R300_US_OUT_FMT_0, 0x00003900 },
   { R300_US_OUT_FMT_0 + 4, 0x0000000f },
   { R300_US_OUT_FMT_0 + 8, 0x0000000f },
   { R300_US_OUT_FMT_0 + 12, 0x0000000f },
   { R300_GB_MSPOS0, 0x66666666 },
   { R300_GB_MSPOS1, 0x06666666 },
   { R300_TX_INVALTAGS, 0x00000000 },
   { R300_TX_ENABLE, 0x00000000 },
};

#define KNOWN_GOOD_WRITE_COUNT \
   (sizeof(known_good_writes) / sizeof(known_good_writes[0]))

static uint32_t
known_good_stream(uint32_t *ib)
{
   for (uint32_t i = 0; i < sizeof(known_good_writes) /
                                sizeof(known_good_writes[0]); i++) {
      ib[2 * i] = CP_PACKET0(known_good_writes[i].reg, 0);
      ib[2 * i + 1] = known_good_writes[i].value;
   }
   return (uint32_t)(2 * (sizeof(known_good_writes) /
                           sizeof(known_good_writes[0])));
}

static bool
is_ordering_barrier_reg(uint16_t reg)
{
   switch (reg) {
   case R300_RB3D_DSTCACHE_CTLSTAT:
   case R300_ZB_ZCACHE_CTLSTAT:
   case RADEON_WAIT_UNTIL:
   case R300_VAP_PVS_STATE_FLUSH_REG:
   case R300_TX_INVALTAGS:
      return true;
   default:
      return false;
   }
}

static uint32_t
known_good_one_reg_mspos_stream(uint32_t *ib)
{
   uint32_t n = 0;
   const uint32_t write_count = sizeof(known_good_writes) /
                                sizeof(known_good_writes[0]);
   for (uint32_t i = 0; i < write_count; i++) {
      const struct known_good_first_draw_write *write = &known_good_writes[i];
      if (write->reg == R300_GB_MSPOS0) {
         assert(i + 1 < write_count);
         assert(known_good_writes[i + 1].reg == R300_GB_MSPOS1);
         ib[n++] = CP_PACKET0(R300_GB_MSPOS0, 0) | RADEON_ONE_REG_WR;
         ib[n++] = write->value;
         i++;
      } else {
         ib[n++] = CP_PACKET0(write->reg, 0);
         ib[n++] = write->value;
      }
   }
   return n;
}

static uint32_t
known_good_late_barrier_stream(uint32_t *ib)
{
   uint32_t n = 0;
   const uint32_t write_count = sizeof(known_good_writes) /
                                sizeof(known_good_writes[0]);
   for (uint32_t i = 0; i < write_count; i++) {
      if (is_ordering_barrier_reg(known_good_writes[i].reg))
         continue;
      ib[n++] = CP_PACKET0(known_good_writes[i].reg, 0);
      ib[n++] = known_good_writes[i].value;
   }

   ib[n++] = CP_PACKET3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
   ib[n++] = R300_VAP_VF_CNTL__PRIM_TRIANGLES;

   for (uint32_t i = 0; i < write_count; i++) {
      if (!is_ordering_barrier_reg(known_good_writes[i].reg))
         continue;
      ib[n++] = CP_PACKET0(known_good_writes[i].reg, 0);
      ib[n++] = known_good_writes[i].value;
   }
   return n;
}

static uint32_t
known_bad_barrier_boundary_value_stream(uint32_t *ib)
{
   uint32_t n = 0;
   const uint32_t write_count = sizeof(known_good_writes) /
                                sizeof(known_good_writes[0]);
   for (uint32_t i = 0; i < write_count; i++) {
      const struct known_good_first_draw_write *write = &known_good_writes[i];
      uint32_t value = write->value;
      if (is_ordering_barrier_reg(write->reg))
         value ^= 1;
      ib[n++] = CP_PACKET0(write->reg, 0);
      ib[n++] = value;
   }

   ib[n++] = CP_PACKET3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
   ib[n++] = R300_VAP_VF_CNTL__PRIM_TRIANGLES;

   for (uint32_t i = 0; i < write_count; i++) {
      const struct known_good_first_draw_write *write = &known_good_writes[i];
      if (!is_ordering_barrier_reg(write->reg))
         continue;
      ib[n++] = CP_PACKET0(write->reg, 0);
      ib[n++] = write->value;
   }
   return n;
}

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
      .chip_family = CHIP_RS480,
      .width = 64,
      .height = 64,
      .max_vtx_index = 2,
      .texture_enabled = false,
   };
   struct r300_first_draw_contract contract;
   assert(r300_first_draw_contract_resolve(&params, &contract) == 0);
   assert(contract.count >= 75);

   struct r300_first_draw_params wrong_chip = params;
   wrong_chip.chip_family = CHIP_R300;
   struct r300_first_draw_contract scratch;
   assert(r300_first_draw_contract_resolve(&wrong_chip, &scratch) == -ENOTSUP);

   /* Parameter derivation: the scissor spans the full 64x64 target with
    * the non-R500 1440 bias, matching the traced reference values.
    */
   assert(contract.entries[find_entry(&contract, R300_SC_SCISSORS_TL)].value ==
          0x00b405a0);
   assert(contract.entries[find_entry(&contract, R300_SC_SCISSORS_BR)].value ==
          0x00bbe5df);
   assert(contract.entries[find_entry(&contract,
                                      R300_VAP_VF_MAX_VTX_INDX)].value == 2);

   const uint32_t known_good_count = KNOWN_GOOD_WRITE_COUNT;
   assert(contract.count == known_good_count);
   for (uint32_t i = 0; i < known_good_count; i++) {
      assert(contract.entries[i].reg == known_good_writes[i].reg);
      assert(contract.entries[i].value == known_good_writes[i].value);
   }

   /* Out-of-range extents refuse. */
   struct r300_first_draw_params bad = params;
   bad.width = 0;
   assert(r300_first_draw_contract_resolve(&bad, &scratch) == -22);
   /* 4095 - 1440 + 1 is the largest extent whose high coordinate fits. */
   const uint32_t max_extent = 2656;
   bad.width = max_extent;
   assert(r300_first_draw_contract_resolve(&bad, &scratch) == 0);
   bad.width++;
   assert(r300_first_draw_contract_resolve(&bad, &scratch) == -22);
   bad.width = UINT32_MAX;
   assert(r300_first_draw_contract_resolve(&bad, &scratch) == -22);
   bad = params;
   bad.height = 0;
   assert(r300_first_draw_contract_resolve(&bad, &scratch) == -22);
   bad.height = max_extent;
   assert(r300_first_draw_contract_resolve(&bad, &scratch) == 0);
   bad.height++;
   assert(r300_first_draw_contract_resolve(&bad, &scratch) == -22);
   bad.height = UINT32_MAX;
   assert(r300_first_draw_contract_resolve(&bad, &scratch) == -22);

   /* The reference artifacts stay out: no depth-resource or dummy-texture
    * descriptor word is emitted for a draw that binds neither.
    */
   for (uint32_t i = 0; i < contract.count; i++) {
      assert(contract.entries[i].reg != R300_ZB_DEPTHOFFSET);
      assert(contract.entries[i].reg != R300_TX_OFFSET_0);
      assert(contract.entries[i].disposition != R300_FDS_REFERENCE_ARTIFACT);
   }

   /* Known-bad calibration: the bare cell leaves the contract
    * unsatisfied for multiple named reasons, and under all-zero poison the
    * remaining silicon-proven gates are among them. A checker reporting only
    * its first failure would hide open gates behind the first one.
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
   uint32_t known_good_ib[KNOWN_GOOD_WRITE_COUNT * 2];
   const uint32_t known_good_dwords = known_good_stream(known_good_ib);
   assert(known_good_dwords == known_good_count * 2);
   for (unsigned v = 0; v < sizeof(poison_vectors) / 4; v++)
      assert(r300_first_draw_state_check(&contract, known_good_ib,
                                         known_good_dwords, poison_vectors[v],
                                         &report) == 0);

   uint32_t cell_failures = r300_first_draw_state_check(
      &contract, cell.ib, cell.ib_size_dwords, 0x00000000, &report);
   assert(cell_failures >= 70);
   assert(report_names(&contract, &report, R300_US_OUT_FMT_0));
   assert(report_names(&contract, &report, R300_SC_SCREENDOOR));
   printf("known-bad: fixed cell leaves %u of %u clauses unsatisfied\n",
          cell_failures, contract.count);

   /* The contract emission converges from every poison vector, and a
    * contract-prefixed stream -- contract state followed by the current cell --
    * still satisfies every clause, so the cell overwrites nothing the
    * contract establishes incompatibly except the values the contract
    * itself owns.
    */
   uint32_t state_ib[256];
   int state_dwords = r300_first_draw_state_emit(&contract, state_ib, 256);
   assert(state_dwords > 0);
   assert((uint32_t)state_dwords == known_good_dwords);
   assert(memcmp(state_ib, known_good_ib, known_good_dwords * 4) == 0);

   uint32_t one_reg_ib[KNOWN_GOOD_WRITE_COUNT * 2];
   uint32_t one_reg_dwords = known_good_one_reg_mspos_stream(one_reg_ib);
   assert(one_reg_dwords == known_good_dwords - 2);
   assert(r300_first_draw_state_check(&contract, one_reg_ib, one_reg_dwords,
                                      0x12345678, &report) == 1);
   assert(report_names(&contract, &report, R300_GB_MSPOS1));

   uint32_t late_barrier_ib[KNOWN_GOOD_WRITE_COUNT * 2 + 2];
   uint32_t late_barrier_dwords =
      known_good_late_barrier_stream(late_barrier_ib);
   uint32_t ordering_barrier_count = 0;
   for (uint32_t i = 0; i < known_good_count; i++)
      ordering_barrier_count += is_ordering_barrier_reg(known_good_writes[i].reg);
   assert(r300_first_draw_state_check(&contract, late_barrier_ib,
                                      late_barrier_dwords, 0x12345678,
                                      &report) == ordering_barrier_count);
   for (uint32_t i = 0; i < known_good_count; i++) {
      if (is_ordering_barrier_reg(known_good_writes[i].reg))
         assert(report_names(&contract, &report, known_good_writes[i].reg));
   }

   /* Boundary-value calibration: wrong barrier values before the draw remain
    * unsatisfied even when matching writes follow the draw. A post-draw write
    * cannot make a cache or idle operation effective at the first draw.
    */
   uint32_t wrong_barrier_ib[KNOWN_GOOD_WRITE_COUNT * 4 + 2];
   uint32_t wrong_barrier_dwords =
      known_bad_barrier_boundary_value_stream(wrong_barrier_ib);
   assert(wrong_barrier_dwords ==
          known_good_dwords + 2 + 2 * ordering_barrier_count);
   assert(r300_first_draw_state_check(&contract, wrong_barrier_ib,
                                      wrong_barrier_dwords, 0x12345678,
                                      &report) == ordering_barrier_count);
   for (uint32_t i = 0; i < known_good_count; i++) {
      if (is_ordering_barrier_reg(known_good_writes[i].reg))
         assert(report_names(&contract, &report, known_good_writes[i].reg));
   }

   enum { bare_state_dwords = 3 * 2 };
   assert(cell.ib_size_dwords > bare_state_dwords);
   uint32_t prefixed[1024];
   memcpy(prefixed, state_ib, (size_t)state_dwords * 4);
   memcpy(prefixed + state_dwords, cell.ib + bare_state_dwords,
          (cell.ib_size_dwords - bare_state_dwords) * 4);
   uint32_t prefixed_dwords =
      (uint32_t)state_dwords + cell.ib_size_dwords - bare_state_dwords;

   for (unsigned v = 0; v < sizeof(poison_vectors) / 4; v++) {
      assert(r300_first_draw_state_check(&contract, state_ib,
                                         (uint32_t)state_dwords,
                                         poison_vectors[v], &report) == 0);
      uint32_t left = r300_first_draw_state_check(
         &contract, prefixed, prefixed_dwords, poison_vectors[v], &report);
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

   /* Integrated emission: the emitter's contract path produces the exact
    * triangle-contract-prefixed stream. The bare cell's three standalone
    * state writes are replaced by the complete contract entries. The triangle
    * contract specializes the output selector for the B8G8R8A8 target; the
    * neutral resolver keeps its generic selector as a separate witness.
    */
   struct r300_first_draw_contract reference;
   assert(r300_tcl_bypass_triangle_reference_contract(&reference) == 0);
   assert(reference.count == contract.count);
   for (uint32_t i = 0; i < contract.count; i++) {
      if (reference.entries[i].reg == R300_US_OUT_FMT_0) {
         assert(contract.entries[i].reg == R300_US_OUT_FMT_0);
         assert(reference.entries[i].value ==
                (R300_US_OUT_FMT_C4_8 | R300_C0_SEL_B | R300_C1_SEL_G |
                 R300_C2_SEL_R | R300_C3_SEL_A));
      } else {
         assert(memcmp(&reference.entries[i], &contract.entries[i],
                       sizeof(reference.entries[i])) == 0);
      }
   }

   uint32_t reference_state_ib[256];
   int reference_state_dwords =
      r300_first_draw_state_emit(&reference, reference_state_ib, 256);
   assert(reference_state_dwords == state_dwords);
   uint32_t reference_prefixed[1024];
   memcpy(reference_prefixed, reference_state_ib,
          (size_t)reference_state_dwords * 4);
   memcpy(reference_prefixed + reference_state_dwords,
          cell.ib + bare_state_dwords,
          (cell.ib_size_dwords - bare_state_dwords) * 4);
   uint32_t reference_prefixed_dwords =
      (uint32_t)reference_state_dwords + cell.ib_size_dwords -
      bare_state_dwords;

   struct r300_tcl_bypass_triangle_params prefixed_params = cell_params;
   prefixed_params.first_draw_contract = &reference;
   struct r300_tcl_bypass_triangle_ib integrated;
   assert(r300_tcl_bypass_triangle_emit(&prefixed_params, &integrated) == 0);
   assert(integrated.ib_size_dwords == reference_prefixed_dwords);
   assert(memcmp(integrated.ib, reference_prefixed,
                 reference_prefixed_dwords * 4) == 0);
   /* The prefix shifts every relocation site by its dword count; each
    * site still names its slot payload behind a reloc NOP header.
    */
   assert(integrated.reloc_site_count == cell.reloc_site_count);
   for (unsigned i = 0; i < integrated.reloc_site_count; i++) {
      assert(integrated.reloc_sites[i].ib_index ==
             cell.reloc_sites[i].ib_index - bare_state_dwords +
                (uint32_t)state_dwords);
      assert(integrated.reloc_sites[i].slot == cell.reloc_sites[i].slot);
   }
   r300_tcl_bypass_triangle_release(&integrated);

   /* Write-order control: emission is deterministic, the cache flush requests
    * precede the idle wait, and pipelined sample positions follow unpipelined
    * state.
    */
   uint32_t state_ib2[256];
   assert(r300_first_draw_state_emit(&contract, state_ib2, 256) ==
          state_dwords);
   assert(memcmp(state_ib, state_ib2, (size_t)state_dwords * 4) == 0);
   assert(state_ib[0] == CP_PACKET0(R300_RB3D_DSTCACHE_CTLSTAT, 0));
   assert(state_ib[2] == CP_PACKET0(R300_ZB_ZCACHE_CTLSTAT, 0));
   assert(state_ib[4] == CP_PACKET0(RADEON_WAIT_UNTIL, 0));
   const uint32_t vap_index = find_entry(&contract, R300_VAP_CNTL);
   const uint32_t us_index = find_entry(&contract, R300_US_OUT_FMT_0);
   const uint32_t mspos0_index = find_entry(&contract, R300_GB_MSPOS0);
   const uint32_t mspos1_index = find_entry(&contract, R300_GB_MSPOS1);
   assert(mspos0_index > vap_index && mspos0_index > us_index);
   assert(mspos1_index == mspos0_index + 1);
   int short_room = r300_first_draw_state_emit(&contract, state_ib2, 4);
   assert(short_room == -28);

   r300_tcl_bypass_triangle_release(&cell);
   printf("r300_first_draw_state_test: %u contract clauses, %zu poison "
          "vectors, 3 gate mutations: OK\n",
          contract.count, sizeof(poison_vectors) / 4);
   return 0;
}
