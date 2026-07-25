/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_classic_schedule.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../radeon_compiler.h"
#include "../radeon_program.h"
#include "../radeon_program_constants.h"
#include "../radeon_program_pair.h"

bool
r300_classic_new_sched_enabled(void)
{
   static int gate = -1;
   if (gate < 0) {
      const char *e = getenv("R300_CLASSIC_NEW_SCHED");
      gate = (e && strcmp(e, "1") == 0) ? 1 : 0;
   }
   return gate == 1;
}

/* Presubtract reads the immediately preceding pair's destination -- the
 * US_ALU_RGB_INST bit-31 hazard.  rc_presubtract_src_reg_count(Index) is the
 * operand count the presubtract op consumes, held in the presubtract source
 * slot; the operands themselves sit in Src[0..count-1]. */
static bool
presub_reads_index(const struct rc_pair_sub_instruction *sub, int a, int b)
{
   if (!sub->Src[RC_PAIR_PRESUB_SRC].Used)
      return false;

   const unsigned num = rc_presubtract_src_reg_count(sub->Src[RC_PAIR_PRESUB_SRC].Index);
   for (unsigned i = 0; i < num; i++) {
      if (sub->Src[i].File == RC_FILE_TEMPORARY &&
          ((int)sub->Src[i].Index == a || (int)sub->Src[i].Index == b))
         return true;
   }
   return false;
}

/* Mirror rc_pair_schedule's presub_nop(): when the just-placed pair's
 * presubtract source reads a temporary the preceding pair wrote, that
 * preceding pair must carry the NOP bubble.  A missing bubble under the same
 * adjacency is exactly what the schedule-legality oracle rejects. */
static void
set_presub_nop(struct rc_instruction *emitted)
{
   struct rc_instruction *prev = emitted->Prev;
   if (prev->Type != RC_INSTRUCTION_PAIR)
      return;

   const int prev_rgb = prev->U.P.RGB.WriteMask ? (int)prev->U.P.RGB.DestIndex : -1;
   const int prev_alpha = prev->U.P.Alpha.WriteMask ? (int)prev->U.P.Alpha.DestIndex : -1;

   if (presub_reads_index(&emitted->U.P.RGB, prev_rgb, prev_alpha) ||
       presub_reads_index(&emitted->U.P.Alpha, prev_rgb, prev_alpha))
      prev->U.P.Nop = 1;
}

/* DestIndex and Src[].Index are RC_REGISTER_INDEX_BITS-wide bitfields, so a
 * value assigned through that field is always inside [0, RC_REGISTER_MAX_INDEX)
 * by construction; the writer tables are sized to match.  The check below is
 * a defense against that invariant changing under a future encoding, not a
 * condition this subset's inputs can currently trigger: an index outside the
 * bound is treated as an unmodeled hazard and the whole pass defers rather
 * than indexing a writer table on trust. */
static bool
temp_index_in_range(int idx)
{
   return idx >= 0 && (unsigned)idx < RC_REGISTER_MAX_INDEX;
}

/* Four render-target color exports plus one depth export bound the distinct
 * output destinations a pair stream can address (rc_pair_sub_instruction's
 * Target field is 2 bits wide). */
#define R300_CLASSIC_COLOR_TARGETS 4

struct sched_node {
   struct rc_instruction *inst; /* original instruction; stays linked until commit */
   struct rc_pair_instruction work; /* trial copy: merge candidate, committed on success */
   bool subsumed; /* merged into another node's work; dropped rather than relinked */
   bool emitted;  /* this node's dependency-resolution effect has been applied */
   unsigned unresolved;
   unsigned height; /* critical-path length (in slots) to a DAG sink, computed
                      * over the ORIGINAL per-instruction dependency graph */
};

/* Record that node reader depends on node writer (writer must occupy an
 * earlier slot).  dep is an n x n matrix, dep[reader * n + writer]. */
static void
add_dep(bool *dep, unsigned n, unsigned reader, unsigned writer)
{
   if (reader == writer)
      return;
   dep[reader * n + writer] = true;
}

/* Walk one pipe's write side: temporary-register defines are tracked
 * per (index, channel) so that a VEC-collect's disjoint per-channel MOVs
 * into one temporary index are legal single assignment, while a genuine
 * double write to the same channel still defers the whole pass (the
 * conservative, correct response to an unmodeled hazard); output/depth
 * exports are tracked per (target, channel) as a write-after-write order
 * chain rather than an SSA fact, since re-exporting the same channel from a
 * later instruction is legitimate and must merely stay ordered, not be
 * refused. */
static bool
record_pipe_defs(const struct rc_pair_sub_instruction *sub, unsigned pos, bool is_alpha_pipe,
                 int *temp_writer, int color_writer[R300_CLASSIC_COLOR_TARGETS][4],
                 int *depth_writer, bool *dep, unsigned n)
{
   if (sub->WriteMask) {
      if (!temp_index_in_range(sub->DestIndex))
         return false;
      for (unsigned ch = 0; ch < 4; ch++) {
         if (!(sub->WriteMask & (1u << ch)))
            continue;
         int *slot = &temp_writer[sub->DestIndex * 4 + ch];
         if (*slot >= 0 && *slot != (int)pos)
            return false;
         *slot = (int)pos;
      }
   }

   /* RGB.OutputWriteMask carries X/Y/Z bits directly; Alpha.OutputWriteMask
    * carries a single low bit meaning "this lane writes the export's W
    * channel" (rc_pair_translate ORs GET_BIT(mask,3) into bit 0, not bit 3),
    * matching radeon_pair_translate.c's set_pair_instruction. */
   const unsigned out_mask =
      is_alpha_pipe ? (sub->OutputWriteMask ? (1u << 3) : 0u) : (sub->OutputWriteMask & RC_MASK_XYZ);
   if (out_mask) {
      const unsigned target = sub->Target;
      for (unsigned ch = 0; ch < 4; ch++) {
         if (!(out_mask & (1u << ch)))
            continue;
         int *slot = &color_writer[target][ch];
         if (*slot >= 0)
            add_dep(dep, n, pos, (unsigned)*slot);
         *slot = (int)pos;
      }
   }

   if (is_alpha_pipe && sub->DepthWriteMask) {
      if (*depth_writer >= 0)
         add_dep(dep, n, pos, (unsigned)*depth_writer);
      *depth_writer = (int)pos;
   }

   return true;
}

/* Conservative read-side dependency: every Used, RC_FILE_TEMPORARY source
 * slot depends on every channel-writer recorded for its index.  This does
 * not resolve which specific channel(s) the pair's swizzle actually reads
 * (that requires walking Arg[]/Swizzle per operand), so it can serialize a
 * reader behind a channel-writer it does not truly touch when the index was
 * split across more than one definer (the VEC-collect case) -- a scheduling
 * freedom cost, never a correctness one: the reader never gets to run before
 * every producer that touched its source index. */
static bool
record_pipe_reads(const struct rc_pair_sub_instruction *sub, unsigned pos, const int *temp_writer,
                  bool *dep, unsigned n)
{
   for (unsigned slot = 0; slot < 4; slot++) {
      if (!sub->Src[slot].Used || sub->Src[slot].File != RC_FILE_TEMPORARY)
         continue;
      if (!temp_index_in_range(sub->Src[slot].Index))
         return false;
      for (unsigned ch = 0; ch < 4; ch++) {
         const int writer = temp_writer[sub->Src[slot].Index * 4 + ch];
         if (writer >= 0)
            add_dep(dep, n, pos, (unsigned)writer);
      }
   }
   return true;
}

/* Two ready nodes are simultaneously schedulable only when neither depends
 * on the other, which the ready-set invariant (unresolved == 0) already
 * guarantees; merging them into one pair is therefore always dependency-safe
 * when rc_pair_try_merge itself accepts the source-slot/presubtract shape. */
static bool
try_merge_ready_pair(struct sched_node *nodes, const unsigned *ready, unsigned ready_count,
                     unsigned *merge_rgb, unsigned *merge_alpha)
{
   for (unsigned a = 0; a < ready_count; a++) {
      const unsigned rgb_idx = ready[a];
      if (nodes[rgb_idx].work.Alpha.Opcode != RC_OPCODE_NOP)
         continue; /* not RGB-shaped: already carries an alpha op */
      for (unsigned b = 0; b < ready_count; b++) {
         const unsigned alpha_idx = ready[b];
         if (alpha_idx == rgb_idx)
            continue;
         if (nodes[alpha_idx].work.RGB.Opcode != RC_OPCODE_NOP)
            continue; /* not Alpha-shaped: already carries a rgb op */

         struct rc_pair_instruction trial = nodes[rgb_idx].work;
         if (rc_pair_try_merge(&trial, &nodes[alpha_idx].work)) {
            nodes[rgb_idx].work = trial;
            *merge_rgb = rgb_idx;
            *merge_alpha = alpha_idx;
            return true;
         }
      }
   }
   return false;
}

/* Decrement unresolved for every node that depends on writer, making it
 * ready the instant its last unresolved predecessor clears. */
static void
release_dependents(struct sched_node *nodes, const bool *dep, unsigned n, unsigned writer)
{
   for (unsigned reader = 0; reader < n; reader++) {
      if (dep[reader * n + writer])
         nodes[reader].unresolved--;
   }
}

void
r300_classic_schedule(struct radeon_compiler *cc, void *user)
{
   struct rc_instruction *const sentinel = &cc->Program.Instructions;

   /* The subset shape is a straight line of ALU pairs.  A TEX block, control
    * flow, or a still-normal instruction all break the dependency model this
    * pass relies on, so defer to the legacy scheduler that models them.
    *
    * user carries the same disable_optimizations flag rc_pair_schedule reads
    * as its Opt field (radeon_pair_schedule.c's pair_instructions gates its
    * try_convert_and_pair RGB<->Alpha *conversion* search on it -- turning a
    * half-full pair's unused lane into the opposite pipe before merging).
    * This pass merges already-shaped RGB-only/Alpha-only pairs but performs
    * no such conversion search, so it has no analogous lever the flag could
    * gate; user is forwarded unread to whichever scheduler ends up running. */
   unsigned n = 0;
   for (struct rc_instruction *i = sentinel->Next; i != sentinel; i = i->Next) {
      if (i->Type != RC_INSTRUCTION_PAIR) {
         rc_pair_schedule(cc, user);
         return;
      }
      n++;
   }
   if (n == 0)
      return;

   struct sched_node *nodes = calloc(n, sizeof(*nodes));
   int *temp_writer = malloc((size_t)RC_REGISTER_MAX_INDEX * 4 * sizeof(*temp_writer));
   bool *dep = calloc((size_t)n * n, sizeof(*dep));
   unsigned *order = malloc(n * sizeof(*order));
   unsigned *ready = malloc(n * sizeof(*ready));
   if (!nodes || !temp_writer || !dep || !order || !ready) {
      free(nodes);
      free(temp_writer);
      free(dep);
      free(order);
      free(ready);
      rc_pair_schedule(cc, user);
      return;
   }
   for (unsigned i = 0; i < (unsigned)RC_REGISTER_MAX_INDEX * 4; i++)
      temp_writer[i] = -1;
   int color_writer[R300_CLASSIC_COLOR_TARGETS][4];
   for (unsigned t = 0; t < R300_CLASSIC_COLOR_TARGETS; t++)
      for (unsigned ch = 0; ch < 4; ch++)
         color_writer[t][ch] = -1;
   int depth_writer = -1;

   /* Pass 1: snapshot every pair into a trial working copy and record its
    * defines.  Output/depth write-after-write edges land in dep directly
    * (the previous writer of the same (target, channel) is already known at
    * this point in the single forward walk); temporary defines populate
    * temp_writer for pass 2's read-side edges. */
   bool safe = true;
   unsigned pos = 0;
   for (struct rc_instruction *i = sentinel->Next; i != sentinel && safe; i = i->Next, pos++) {
      nodes[pos].inst = i;
      nodes[pos].work = i->U.P;
      safe = record_pipe_defs(&nodes[pos].work.RGB, pos, false, temp_writer, color_writer,
                              &depth_writer, dep, n) &&
             record_pipe_defs(&nodes[pos].work.Alpha, pos, true, temp_writer, color_writer,
                              &depth_writer, dep, n);
   }

   /* Pass 2: read-side RAW edges against the now-complete temp_writer map. */
   for (unsigned j = 0; j < n && safe; j++) {
      safe = record_pipe_reads(&nodes[j].work.RGB, j, temp_writer, dep, n) &&
             record_pipe_reads(&nodes[j].work.Alpha, j, temp_writer, dep, n);
   }

   if (!safe) {
      free(nodes);
      free(temp_writer);
      free(dep);
      free(order);
      free(ready);
      rc_pair_schedule(cc, user);
      return;
   }

   for (unsigned j = 0; j < n; j++)
      for (unsigned k = 0; k < n; k++)
         if (dep[j * n + k])
            nodes[j].unresolved++;

   /* Critical-path height: rc_pair_translate emits pairs in a valid
    * def-before-use order, so every dependency of node j has index < j;
    * processing high-to-low means every successor's height is already known.
    * Scheduling by descending height (ties broken by ascending index, the
    * old program-order behavior) prioritizes the longest remaining chain of
    * work instead of merely reproducing input order -- the tie-break that
    * makes the scheduling decision itself a real choice instead of a no-op. */
   for (unsigned j = n; j-- > 0;) {
      unsigned h = 1;
      for (unsigned k = j + 1; k < n; k++)
         if (dep[k * n + j] && nodes[k].height + 1 > h)
            h = nodes[k].height + 1;
      nodes[j].height = h;
   }

   unsigned remaining = n;
   unsigned order_count = 0;
   while (remaining > 0) {
      unsigned ready_count = 0;
      for (unsigned j = 0; j < n; j++)
         if (!nodes[j].emitted && !nodes[j].subsumed && nodes[j].unresolved == 0)
            ready[ready_count++] = j;

      /* A single-assignment-per-channel RAW-plus-WAW graph is acyclic for
       * any program a real front end emits; a dependency cycle here is an
       * unmodeled hazard the pass cannot schedule around.  Defer to the
       * legacy scheduler on the untouched instruction list (every mutation
       * so far landed in nodes[].work trial copies, never in the live
       * rc_instruction) instead of hard-failing the compile. */
      if (ready_count == 0) {
         free(nodes);
         free(temp_writer);
         free(dep);
         free(order);
         free(ready);
         rc_pair_schedule(cc, user);
         return;
      }

      unsigned merge_rgb, merge_alpha;
      if (try_merge_ready_pair(nodes, ready, ready_count, &merge_rgb, &merge_alpha)) {
         nodes[merge_alpha].subsumed = true;
         nodes[merge_alpha].emitted = true;
         nodes[merge_rgb].emitted = true;
         order[order_count++] = merge_rgb;
         release_dependents(nodes, dep, n, merge_rgb);
         release_dependents(nodes, dep, n, merge_alpha);
         remaining -= 2;
         continue;
      }

      unsigned pick = ready[0];
      for (unsigned r = 1; r < ready_count; r++) {
         if (nodes[ready[r]].height > nodes[pick].height)
            pick = ready[r];
      }
      nodes[pick].emitted = true;
      order[order_count++] = pick;
      release_dependents(nodes, dep, n, pick);
      remaining -= 1;
   }

   /* This pass merges already RGB-only/Alpha-only pairs but does not perform
    * legacy's RGB<->Alpha *conversion* search (try_convert_and_pair), so a
    * translated stream whose merged count still exceeds the hardware
    * ALU-instruction envelope needs that stronger compaction instead of a
    * schedule already too long for rc_validate_final_shader to pass.  Every
    * mutation up to here lives in trial copies, so the live instruction list
    * is still exactly what rc_pair_translate produced. */
   if (cc->max_alu_insts > 0 && order_count > (unsigned)cc->max_alu_insts) {
      free(nodes);
      free(temp_writer);
      free(dep);
      free(order);
      free(ready);
      rc_pair_schedule(cc, user);
      return;
   }

   /* Commit: write the trial copies back into the real instructions, relink
    * the survivors in schedule order, and re-derive the presubtract NOP
    * bubble on each placement exactly as the legacy emit loop does.  A
    * subsumed node's instruction is simply left unlinked -- its content now
    * lives inside the pair it was merged into. */
   for (unsigned idx = 0; idx < order_count; idx++)
      nodes[order[idx]].inst->U.P = nodes[order[idx]].work;

   for (unsigned j = 0; j < n; j++)
      rc_remove_instruction(nodes[j].inst);

   struct rc_instruction *after = sentinel;
   for (unsigned idx = 0; idx < order_count; idx++) {
      struct rc_instruction *inst = nodes[order[idx]].inst;
      inst->U.P.Nop = 0;
      rc_insert_instruction(after, inst);
      set_presub_nop(inst);
      after = inst;
   }

   free(nodes);
   free(temp_writer);
   free(dep);
   free(order);
   free(ready);
}
