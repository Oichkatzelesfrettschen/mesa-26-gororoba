/* -*- mesa-c++  -*-
 * Copyright 2022 Collabora LTD
 * Author: Gert Wollny <gert.wollny@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#include "sfn_ra.h"

#include "sfn_alu_defines.h"
#include "sfn_debug.h"
#include "sfn_instr_controlflow.h"
#include "sfn_instr_export.h"
#include "sfn_instr_fetch.h"
#include "sfn_liverangeevaluator.h"
#include "sfn_shader.h"

#include "util/u_debug.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>
#include <queue>

namespace r600 {

class ComponentInterference {
public:
   using Row = std::vector<int>;

   void prepare_row(int row);

   void add(size_t idx1, size_t idx2);

   auto row(int idx) const -> const Row&
   {
      assert((size_t)idx < m_rows.size());
      return m_rows[idx];
   }

private:
   std::vector<Row> m_rows;
};

class Interference {
public:
   Interference(LiveRangeMap& map);

   const auto& row(int comp, int index) const
   {
      assert(comp < 4);
      return m_components_maps[comp].row(index);
   }

private:
   void initialize();
   void initialize(ComponentInterference& comp, LiveRangeMap::ChannelLiveRange& clr);

   LiveRangeMap& m_map;
   std::array<ComponentInterference, 4> m_components_maps;
};

void
ComponentInterference::prepare_row(int row)
{
   m_rows.resize(row + 1);
}

void
ComponentInterference::add(size_t idx1, size_t idx2)
{
   assert(idx1 > idx2);
   assert(m_rows.size() > idx1);
   m_rows[idx1].push_back(idx2);
   m_rows[idx2].push_back(idx1);
}

Interference::Interference(LiveRangeMap& map):
    m_map(map)
{
   initialize();
}

void
Interference::initialize()
{
   for (int i = 0; i < 4; ++i) {
      initialize(m_components_maps[i], m_map.component(i));
   }
}

void
Interference::initialize(ComponentInterference& comp_interference,
                         LiveRangeMap::ChannelLiveRange& clr)
{
   for (size_t row = 0; row < clr.size(); ++row) {
      auto& row_entry = clr[row];
      comp_interference.prepare_row(row);
      for (size_t col = 0; col < row; ++col) {
         auto& col_entry = clr[col];
         if (row_entry.m_end > col_entry.m_start && row_entry.m_start < col_entry.m_end)
            comp_interference.add(row, col);
      }
   }
}

static inline int
register_bank(int color)
{
   return color & 1;
}

static int
live_range_overlap_weight(const LiveRangeEntry& lhs, const LiveRangeEntry& rhs)
{
   int overlap_start = std::max(lhs.m_start, rhs.m_start);
   int overlap_end = std::min(lhs.m_end, rhs.m_end);
   int overlap = overlap_end - overlap_start;
   return overlap > 0 ? overlap : 1;
}

static int
live_range_hotness_weight(const LiveRangeEntry& lr)
{
   int const span = std::max(1, lr.m_end - lr.m_start);
   int const early_bias = std::max(1, 256 - lr.m_start);

   /* Approximate block-frequency impact from linear instruction position:
    * values that start earlier and live longer are treated as hotter. */
   int hotness = 1 + (early_bias / 64) + (span / 64);
   if (lr.m_alu_clause_local)
      hotness += 1;
   return std::min(hotness, 8);
}

static int
read_port_conflict_penalty(const LiveRangeEntry& candidate,
                           const LiveRangeMap::ChannelLiveRange& live_ranges,
                           const ComponentInterference::Row& adjacency,
                           int color)
{
   /* Keep low-register preference but bias harder away from same-bank pressure. */
   int penalty = color / 16;
   int same_bank_pressure = 0;
   int const candidate_hotness = live_range_hotness_weight(candidate);

   for (auto adj : adjacency) {
      const auto& other = live_ranges[adj];
      if (other.m_color == g_registers_unused)
         continue;

      int weight = live_range_overlap_weight(candidate, other);
      int const overlap_pressure =
         weight * ((candidate_hotness + live_range_hotness_weight(other)) / 2);
      if (candidate.m_alu_clause_local || other.m_alu_clause_local)
         weight = (weight * 3) / 2;

      if (register_bank(other.m_color) == register_bank(color)) {
         penalty += 4 * weight;
         same_bank_pressure += overlap_pressure;
      } else {
         penalty += weight;
      }
   }

   /* Second-order pressure term: avoid packing hot overlapping ranges into
    * one bank even when direct interference allows it. */
   penalty += same_bank_pressure;

   return penalty;
}

struct Group {
   int priority;
   std::array<PRegister, 4> channels;
};

static inline bool
operator<(const Group& lhs, const Group& rhs)
{
   return lhs.priority < rhs.priority;
}

using GroupRegisters = std::priority_queue<Group>;

static bool
group_allocation(LiveRangeMap& lrm,
                 const Interference& interference,
                 GroupRegisters& groups)
{
   int color = 0;
   // allocate grouped registers
   while (!groups.empty()) {
      auto group = groups.top();
      groups.pop();

      int start_comp = 0;
      while (!group.channels[start_comp])
         ++start_comp;

      sfn_log << SfnLog::merge << "Color group with " << *group.channels[start_comp]
              << "\n";

      // don't restart registers for exports, we may be able tp merge the
      // export calls, is fthe registers are consecutive
      if (group.priority > 0)
         color = 0;

      int best_color = g_registers_end;
      int best_penalty = std::numeric_limits<int>::max();

      for (int test_color = color; test_color < g_registers_end; ++test_color) {
         bool color_in_use = false;
         int penalty = 0;

         for (int comp = start_comp; comp < 4; ++comp) {
            if (!group.channels[comp])
               continue;

            auto& component_life_ranges = lrm.component(comp);
            const auto& candidate = component_life_ranges[group.channels[comp]->index()];
            auto& adjecencies = interference.row(comp, group.channels[comp]->index());

            for (auto adj_index : adjecencies) {
               if (component_life_ranges[adj_index].m_color == test_color) {
                  color_in_use = true;
                  break;
               }
            }

            if (color_in_use)
               break;

            penalty += read_port_conflict_penalty(
               candidate, component_life_ranges, adjecencies, test_color);
         }

         if (color_in_use)
            continue;

         if (penalty < best_penalty) {
            best_penalty = penalty;
            best_color = test_color;
            if (best_penalty == 0)
               break;
         }
      }

      if (best_color == g_registers_end)
         return false;

      color = best_color;
      sfn_log << SfnLog::merge << "Select color " << color
              << " (read-port penalty=" << best_penalty << ")\n";

      /* Coloring successful */
      for (auto reg : group.channels) {
         if (reg) {
            auto& vregs = lrm.component(reg->chan());
            auto& vreg_cmp = vregs[reg->index()];
            assert(vreg_cmp.m_start != -1 || vreg_cmp.m_end != -1);
            vreg_cmp.m_color = color;
         }
      }
   }

   return true;
}

static bool
scalar_allocation(LiveRangeMap& lrm, const Interference& interference)
{
   for (int comp = 0; comp < 4; ++comp) {
      auto& live_ranges = lrm.component(comp);
      for (auto& r : live_ranges) {
         if (r.m_color != g_registers_unused)
            continue;

         if (r.m_start == -1 && r.m_end == -1)
            continue;

         sfn_log << SfnLog::merge << "Color " << *r.m_register << "\n";

         auto& adjecency = interference.row(comp, r.m_register->index());

         int best_color = g_registers_end;
         int best_penalty = std::numeric_limits<int>::max();
         for (int color = 0; color < g_registers_end; ++color) {
            bool color_in_use = false;
            for (auto adj : adjecency) {
               if (live_ranges[adj].m_color == color) {
                  color_in_use = true;
                  break;
               }
            }

            if (color_in_use)
               continue;

            int penalty = read_port_conflict_penalty(r, live_ranges, adjecency, color);
            if (penalty < best_penalty) {
               best_penalty = penalty;
               best_color = color;
               if (best_penalty == 0)
                  break;
            }
         }
         if (best_color == g_registers_end)
            return false;

         r.m_color = best_color;
         sfn_log << SfnLog::merge << "Select color " << best_color
                 << " (read-port penalty=" << best_penalty << ")\n";
      }
   }
   return true;
}

struct AluRegister {
   int lifetime;
   LiveRangeEntry *lre;
};

static inline bool operator < (const AluRegister& lhs, const AluRegister& rhs)
{
   return lhs.lifetime > rhs.lifetime;
}

using AluClauseRegisters = std::priority_queue<AluRegister>;


static void
scalar_clause_local_allocation (LiveRangeMap& lrm, const Interference&  interference)
{
   for (int comp = 0; comp < 4; ++comp) {
      AluClauseRegisters clause_reg;
      auto& live_ranges = lrm.component(comp);
      for (auto& r : live_ranges) {

         sfn_log << SfnLog::merge << "LR: " << *r.m_register
                 <<  "[ " << r.m_start << ", " << r.m_end
                  << " ], AC: " << r.m_alu_clause_local
                  << " Color; " << r.m_color << "\n";

         if (r.m_color != g_registers_unused)
            continue;

         if (r.m_start == -1 &&
             r.m_end == -1)
            continue;

         if (!r.m_alu_clause_local)
            continue;

         int len = r.m_end - r.m_start;
         if (len > 1) {
            clause_reg.push({len, &r});
            sfn_log << SfnLog::merge << "Consider " << *r.m_register
                    << " for clause local\n";
         }
      }

      while (!clause_reg.empty()) {
         auto& r = clause_reg.top().lre;
         clause_reg.pop();

         sfn_log << SfnLog::merge << "Color " << *r->m_register << "\n";

         auto& adjecency = interference.row(comp, r->m_register->index());

         int best_color = g_clause_local_end;
         int best_penalty = std::numeric_limits<int>::max();
         for (int color = g_clause_local_start; color < g_clause_local_end; ++color) {
            bool color_in_use = false;
            for (auto adj : adjecency) {
               if (live_ranges[adj].m_color == color) {
                  color_in_use = true;
                  break;
               }
            }

            if (color_in_use)
               continue;

            int penalty = read_port_conflict_penalty(*r, live_ranges, adjecency, color);
            if (penalty < best_penalty) {
               best_penalty = penalty;
               best_color = color;
               if (best_penalty == 0)
                  break;
            }
         }
         if (best_color == g_clause_local_end)
            break;

         r->m_color = best_color;
         sfn_log << SfnLog::merge << "Select clause-local color " << best_color
                 << " (read-port penalty=" << best_penalty << ")\n";
      }
   }
}

bool
register_allocation(LiveRangeMap& lrm)
{
   Interference interference(lrm);

   std::map<int, Group> groups;

   // setup fixed colors and group relationships
   for (int i = 0; i < 4; ++i) {
      auto& comp = lrm.component(i);
      for (auto& entry : comp) {
         sfn_log << SfnLog::merge << "Prepare RA for " << *entry.m_register << " ["
                 << entry.m_start << ", " << entry.m_end << "]\n";
         auto pin = entry.m_register->pin();
         if (entry.m_start == -1 && entry.m_end == -1) {
            if (pin == pin_group || pin == pin_chgr)
               entry.m_register->set_chan(7);
            continue;
         }

         auto sel = entry.m_register->sel();
         /* fully pinned registers contain system values with the
          * definite register index, and array values are allocated
          * right after the system registers, so just reuse the IDs (for now) */
         if (pin == pin_fully || pin == pin_array) {
            /* Must set all array element entries */
            sfn_log << SfnLog::merge << "Pin color " << sel << " to " << *entry.m_register
                    << "\n";
            entry.m_color = sel;
         } else if (pin == pin_group || pin == pin_chgr) {
            /* Groups must all have the same sel() value, because they are
             * used as vec4 registers */
            auto igroup = groups.find(sel);
            if (igroup != groups.end()) {
               igroup->second.channels[i] = entry.m_register;
               assert(comp[entry.m_register->index()].m_register->index() ==
                      entry.m_register->index());
            } else {
               int priority = entry.m_use.test(LiveRangeEntry::use_export)
                                 ? -entry.m_end
                                 : entry.m_start;
               Group group{
                  priority, {nullptr, nullptr, nullptr, nullptr}
               };
               group.channels[i] = entry.m_register;
               assert(comp[group.channels[i]->index()].m_register->index() ==
                      entry.m_register->index());
               groups[sel] = group;
            }
         }
      }
   }

   GroupRegisters groups_sorted;
   for (auto& [sel, group] : groups)
      groups_sorted.push(group);

   if (!group_allocation(lrm, interference, groups_sorted))
      return false;

   scalar_clause_local_allocation(lrm, interference);

   if (!scalar_allocation(lrm, interference))
      return false;

   for (int i = 0; i < 4; ++i) {
      auto& comp = lrm.component(i);
      for (auto& entry : comp) {
         sfn_log << SfnLog::merge << "Set " << *entry.m_register << " to ";
         entry.m_register->set_sel(entry.m_color);
         /* No need for any pinning past this point, keeping the flags just makes
          * testing more difficult.
          */
         entry.m_register->set_pin(pin_none);
         entry.m_register->reset_flag(Register::pin_start);
         sfn_log << SfnLog::merge << *entry.m_register << "\n";
      }
   }

   return true;
}


/* ---- GPR Spill-to-Scratch Infrastructure ---- */

struct SpillCandidate {
   int component;       /* 0-3: which channel */
   int lr_index;        /* index into LiveRangeMap::component(comp) */
   int range_length;    /* m_end - m_start */
   int interference;    /* number of interfering LRs */
   float benefit;       /* spill heuristic score */
   Register *reg;
};

struct LiveRangePressureSummary {
   unsigned live_range_count;
   unsigned max_component_pressure;
   int max_component;
};

static bool
debug_sfn_ra_spill_enabled()
{
   return debug_get_bool_option("TERAKAN_DEBUG_SFN_RA_SPILL", false);
}

static int
debug_sfn_ra_spill_max_iterations(int default_max_spill_iterations)
{
   int64_t override = debug_get_num_option(
      "TERAKAN_DEBUG_SFN_RA_SPILL_MAX_ITER",
      default_max_spill_iterations);

   if (override < 0)
      return default_max_spill_iterations;

   return static_cast<int>(override);
}

static LiveRangePressureSummary
summarize_live_range_pressure(LiveRangeMap& lrm)
{
   LiveRangePressureSummary summary{0, 0, -1};

   for (int comp = 0; comp < 4; ++comp) {
      auto& live_ranges = lrm.component(comp);
      for (auto& candidate : live_ranges) {
         if (candidate.m_start == -1 && candidate.m_end == -1)
            continue;

         ++summary.live_range_count;

         unsigned pressure = 0;
         for (auto& active : live_ranges) {
            if (active.m_start == -1 && active.m_end == -1)
               continue;

            if (active.m_start <= candidate.m_start &&
                active.m_end > candidate.m_start) {
               ++pressure;
            }
         }

         if (pressure > summary.max_component_pressure) {
            summary.max_component_pressure = pressure;
            summary.max_component = comp;
         }
      }
   }

   return summary;
}

static void
log_spill_pressure(const char *event,
                   int iter,
                   const LiveRangePressureSummary& summary)
{
   std::cerr << "TERAKAN_SFN_RA_SPILL event=" << event
             << " iter=" << iter
             << " live_ranges=" << summary.live_range_count
             << " max_component_pressure=" << summary.max_component_pressure
             << " max_component=" << summary.max_component << "\n";
}

static SpillCandidate
select_spill_candidate(LiveRangeMap& lrm, const Interference& ifg)
{
   SpillCandidate best{-1, -1, 0, 0, -1.0f, nullptr};

   for (int comp = 0; comp < 4; ++comp) {
      auto& live_ranges = lrm.component(comp);
      for (size_t i = 0; i < live_ranges.size(); ++i) {
         auto& lr = live_ranges[i];

         /* Skip dead, already-colored, or system registers */
         if (lr.m_start == -1 && lr.m_end == -1)
            continue;
         if (lr.m_color != g_registers_unused)
            continue;

         auto pin = lr.m_register->pin();
         /* Don't spill pinned/system/array registers */
         if (pin == pin_fully || pin == pin_array)
            continue;

         int range_len = lr.m_end - lr.m_start;
         if (range_len < 4)  /* thrashing guard: don't spill tiny ranges */
            continue;

         int ifc = (int)ifg.row(comp, lr.m_register->index()).size();

         /* Spill benefit = range * interference.
          * Higher = better spill candidate. */
         float benefit = (float)(range_len * ifc);

         if (benefit > best.benefit) {
            best = SpillCandidate{comp, (int)i, range_len, ifc, benefit, lr.m_register};
         }
      }
   }

   return best;
}

bool
register_allocation_with_spill(LiveRangeMap& lrm,
                                Shader& shader,
                                int max_spill_iterations)
{
   bool const instrument_spill = debug_sfn_ra_spill_enabled();
   int const effective_max_spill_iterations =
      debug_sfn_ra_spill_max_iterations(max_spill_iterations);

   /* First try without spilling */
   if (register_allocation(lrm))
      return true;

   sfn_log << SfnLog::merge << "RA failed, attempting spill (up to "
           << effective_max_spill_iterations << " iterations)\n";

   if (instrument_spill)
      log_spill_pressure("initial_fail", -1, summarize_live_range_pressure(lrm));

   int scratch_slot = 0;

   for (int iter = 0; iter < effective_max_spill_iterations; ++iter) {
      /* Re-evaluate liveness after any prior spill modifications */
      auto fresh_lrm = LiveRangeEvaluator().run(shader);

      /* Build interference for fresh LRM */
      Interference ifg(fresh_lrm);

      auto fresh_pressure = summarize_live_range_pressure(fresh_lrm);
      if (instrument_spill)
         log_spill_pressure("iter_begin", iter, fresh_pressure);

      /* Select best spill candidate */
      auto candidate = select_spill_candidate(fresh_lrm, ifg);
      if (candidate.component < 0) {
         R600_ERR("Spill iteration %d: no viable candidate found\n", iter);
         return false;
      }

      sfn_log << SfnLog::merge << "Spill iter " << iter
              << ": spilling " << *candidate.reg
              << " (range=" << candidate.range_length
              << ", ifg=" << candidate.interference
              << ", benefit=" << candidate.benefit
              << ") to scratch slot " << scratch_slot << "\n";

      if (instrument_spill) {
         std::cerr << "TERAKAN_SFN_RA_SPILL event=candidate"
                   << " iter=" << iter
                   << " reg=" << *candidate.reg
                   << " component=" << candidate.component
                   << " range=" << candidate.range_length
                   << " interference=" << candidate.interference
                   << " benefit=" << candidate.benefit
                   << " scratch_slot=" << scratch_slot << "\n";
      }

      /* Insert spill store + reload into the shader IR.
       *
       * Simplified approach for Phase 1:
       * Find the first and second ALU blocks. Insert the store at end
       * of the first block (after the def), insert a reload at start
       * of the second block (before the use), and replace all uses of
       * the original register with the reloaded value. */

      auto& vf = shader.value_factory();

      /* Create the scratch write (CF instruction -- cheap) */
      RegisterVec4::Swizzle store_swz = {7, 7, 7, 7};
      store_swz[candidate.component] = candidate.component;

      auto store_value = vf.temp_vec4(pin_group, store_swz);

      /* MOV the spilled value into the temp vec4 */
      auto mov_to_scratch = new AluInstr(op1_mov,
                                         store_value[candidate.component],
                                         candidate.reg,
                                         AluInstr::write);
      mov_to_scratch->set_alu_flag(alu_no_schedule_bias);

      auto scratch_write = new ScratchIOInstr(
         store_value, scratch_slot, 16, 0, 1 << candidate.component);

      /* Create the scratch read */
      RegisterVec4::Swizzle load_swz = {7, 7, 7, 7};
      load_swz[candidate.component] = candidate.component;

      auto reload_dest = vf.temp_vec4(pin_group, load_swz);

      bool inserted_store = false;
      bool inserted_load = false;
      int store_block_index = -1;
      int reload_block_index = -1;

      if (shader.chip_class() >= ISA_CC_R700) {
         /* Evergreen+: use LoadFromScratch (VTX fetch) for reads */
         auto wait = new ControlFlowInstr(ControlFlowInstr::cf_wait_ack);
         auto *fetch = new LoadFromScratch(
            reload_dest, load_swz,
            vf.literal(scratch_slot),
            scratch_slot + 1);
         fetch->add_required_instr(wait);

         int alu_block_index = 0;
         for (auto block : shader.func()) {
            if (block->type() != Block::alu)
               continue;

            int const current_block_index = alu_block_index++;

            if (!inserted_store) {
               block->push_back(mov_to_scratch);
               block->push_back(scratch_write);
               inserted_store = true;
               store_block_index = current_block_index;
               continue;
            }

            if (!inserted_load) {
               /* Insert wait + fetch before the block's instructions */
               shader.emit_instruction(wait);
               shader.emit_instruction(fetch);
               shader.chain_scratch_read(wait);
               shader.chain_scratch_read(fetch);

               /* Replace uses of spilled reg with reload dest */
               for (auto& instr : *block) {
                  auto alu = instr->as_alu();
                  if (alu) {
                     alu->replace_source(candidate.reg,
                                        reload_dest[candidate.component]);
                  }
               }
               inserted_load = true;
               reload_block_index = current_block_index;
               break;
            }
         }
      } else {
         /* Pre-R700: use ScratchIOInstr for reads */
         auto scratch_read = new ScratchIOInstr(
            reload_dest, scratch_slot, 16, 0, 0xf, true);

         int alu_block_index = 0;
         for (auto block : shader.func()) {
            if (block->type() != Block::alu)
               continue;

            int const current_block_index = alu_block_index++;

            if (!inserted_store) {
               block->push_back(mov_to_scratch);
               block->push_back(scratch_write);
               inserted_store = true;
               store_block_index = current_block_index;
               continue;
            }

            if (!inserted_load) {
               block->insert(block->begin(), scratch_read);
               for (auto& instr : *block) {
                  auto alu = instr->as_alu();
                  if (alu) {
                     alu->replace_source(candidate.reg,
                                        reload_dest[candidate.component]);
                  }
               }
               inserted_load = true;
               reload_block_index = current_block_index;
               break;
            }
         }
      }

      if (!inserted_store || !inserted_load) {
         sfn_log << SfnLog::merge << "Spill insertion failed\n";
         return false;
      }

      if (instrument_spill) {
         std::cerr << "TERAKAN_SFN_RA_SPILL event=inserted"
                   << " iter=" << iter
                   << " store_block=" << store_block_index
                   << " reload_block=" << reload_block_index
                   << " scratch_slot=" << scratch_slot << "\n";
      }

      shader.set_flag(Shader::sh_needs_scratch_space);
      scratch_slot++;

      /* Retry RA with the modified shader */
      auto retry_lrm = LiveRangeEvaluator().run(shader);
      auto retry_pressure = summarize_live_range_pressure(retry_lrm);
      if (register_allocation(retry_lrm)) {
         if (instrument_spill)
            log_spill_pressure("retry_success", iter, retry_pressure);

         sfn_log << SfnLog::merge << "Spill succeeded after "
                 << (iter + 1) << " iteration(s), "
                 << scratch_slot << " scratch slot(s)\n";
         lrm = std::move(retry_lrm);
         return true;
      }

      if (instrument_spill)
         log_spill_pressure("retry_fail", iter, retry_pressure);

      sfn_log << SfnLog::merge << "RA still fails after spill iter "
              << iter << ", continuing...\n";
   }

   R600_ERR("Register allocation failed after %d spill iterations\n",
            effective_max_spill_iterations);
   return false;
}


} // namespace r600
