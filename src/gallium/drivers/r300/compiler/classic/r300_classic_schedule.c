/*
 * Copyright (c) 2026 Terascale Functionalists
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

/* A pair sub-instruction writes a temporary exactly when its per-channel
 * WriteMask is set; an output write carries OutputWriteMask/Target instead and
 * leaves WriteMask clear (rc_pair_translate's destination handling).  The
 * scheduler orders only temporary def/use edges, so an output-only pair
 * produces no definition. */
static int
sub_temp_def(const struct rc_pair_sub_instruction *sub)
{
   return sub->WriteMask ? (int)sub->DestIndex : -1;
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

   const int prev_rgb = sub_temp_def(&prev->U.P.RGB);
   const int prev_alpha = sub_temp_def(&prev->U.P.Alpha);

   if (presub_reads_index(&emitted->U.P.RGB, prev_rgb, prev_alpha) ||
       presub_reads_index(&emitted->U.P.Alpha, prev_rgb, prev_alpha))
      prev->U.P.Nop = 1;
}

/* Every temporary a pair reads, across both pipes and all four source slots
 * (the presubtract slot included): each producer is an ordering predecessor.
 * Records up to cap distinct indices into out[]; returns the count. */
static unsigned
gather_temp_reads(const struct rc_pair_instruction *p, unsigned *out, unsigned cap)
{
   unsigned count = 0;
   for (unsigned pipe = 0; pipe < 2; pipe++) {
      const struct rc_pair_sub_instruction *sub = pipe ? &p->Alpha : &p->RGB;
      for (unsigned i = 0; i < 4; i++) {
         if (!sub->Src[i].Used || sub->Src[i].File != RC_FILE_TEMPORARY)
            continue;
         const unsigned idx = sub->Src[i].Index;
         bool seen = false;
         for (unsigned k = 0; k < count; k++)
            seen |= (out[k] == idx);
         if (!seen && count < cap)
            out[count++] = idx;
      }
   }
   return count;
}

/* A pair writes RC_FILE_OUTPUT (the fragment color export) through
 * OutputWriteMask/Target rather than through DestIndex (rc_pair_translate's
 * destination handling), and writes depth through the separate
 * Alpha.DepthWriteMask bit -- neither carries a temporary index the writer[]
 * map can key on.  Two such writes must keep their original relative order:
 * a later export must not race ahead of an earlier one to the same output. */
static bool
pair_writes_output(const struct rc_pair_instruction *p)
{
   return p->RGB.OutputWriteMask || p->Alpha.OutputWriteMask || p->Alpha.DepthWriteMask;
}

/* Up to four source slots on each of two pipes bound the distinct
 * temporaries one pair can read (eight), plus the single output/depth
 * write-order edge every pair may carry -- nine predecessors bounds any
 * pair's dependency set. */
#define R300_CLASSIC_MAX_PREDS 9

struct sched_node {
   struct rc_instruction *inst;
   unsigned orig_index;
   int pred[R300_CLASSIC_MAX_PREDS];
   unsigned num_pred;
   unsigned unresolved;
   bool emitted;
   /* Index of the nearest earlier output/depth-writing pair, or -1; folded
    * into pred[] as a WAW ordering edge alongside the RAW temp edges. */
   int output_pred;
};

/* DestIndex and Src[].Index are RC_REGISTER_INDEX_BITS-wide bitfields, so a
 * value assigned through that field is always inside [0, RC_REGISTER_MAX_INDEX)
 * by construction; writer[] is sized to match.  The check below is a
 * defense against that invariant changing under a future encoding, not a
 * condition this subset's inputs can currently trigger: an index outside the
 * bound is treated as an unmodeled hazard and the whole pass defers rather
 * than indexing writer[] on trust. */
static bool
temp_index_in_range(int idx)
{
   return idx >= 0 && (unsigned)idx < RC_REGISTER_MAX_INDEX;
}

void
r300_classic_schedule(struct radeon_compiler *cc, void *user)
{
   struct rc_instruction *const sentinel = &cc->Program.Instructions;

   /* The subset shape is a straight line of ALU pairs in single-assignment
    * form.  A TEX block, control flow, a still-normal instruction, or a
    * destination index written twice all break the RAW-only ordering this
    * pass relies on, so defer to the legacy scheduler that models them.
    *
    * user carries the same disable_optimizations flag rc_pair_schedule reads
    * as its Opt field (radeon_pair_schedule.c's pair_instructions gates the
    * extra try_convert_and_pair RGB<->Alpha conversion search on it).  This
    * pass performs no such heuristic search and no RGB/Alpha merging -- only
    * a canonical dependency-preserving reorder of the pairs rc_pair_translate
    * already produced -- so it has no analogous lever the flag could gate,
    * and it forwards user unread to whichever scheduler ends up running. */
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

   /* This pass only reorders the pairs rc_pair_translate already emitted; it
    * never merges a half-full RGB-only pair with a half-full Alpha-only one
    * the way rc_pair_schedule's merge_instructions() does.  When the
    * translated stream is already longer than the hardware ALU-instruction
    * envelope, only that merging compaction can still fit it, so defer to
    * the scheduler that can attempt it instead of handing
    * rc_validate_final_shader a schedule already too long to pass. */
   if (cc->max_alu_insts > 0 && n > (unsigned)cc->max_alu_insts) {
      rc_pair_schedule(cc, user);
      return;
   }

   struct sched_node *nodes = calloc(n, sizeof(*nodes));
   int *writer = malloc(RC_REGISTER_MAX_INDEX * sizeof(*writer));
   unsigned *order = malloc(n * sizeof(*order));
   if (!nodes || !writer || !order) {
      free(nodes);
      free(writer);
      free(order);
      rc_pair_schedule(cc, user);
      return;
   }
   for (unsigned i = 0; i < RC_REGISTER_MAX_INDEX; i++)
      writer[i] = -1;

   /* Map every temporary to its single defining pair; a second, distinct
    * definer means the stream is not single-assignment and the pass defers.
    * The same walk threads the output/depth write-order chain (WAW ordering
    * for RC_FILE_OUTPUT and depth writes, which carry no temporary index of
    * their own to key writer[] on) and rejects any out-of-range register
    * index the pass cannot safely model. */
   bool ssa = true;
   int last_output_writer = -1;
   unsigned pos = 0;
   for (struct rc_instruction *i = sentinel->Next; i != sentinel; i = i->Next, pos++) {
      nodes[pos].inst = i;
      nodes[pos].orig_index = pos;
      const int defs[2] = {sub_temp_def(&i->U.P.RGB), sub_temp_def(&i->U.P.Alpha)};
      for (unsigned d = 0; d < 2 && ssa; d++) {
         if (defs[d] < 0)
            continue;
         if (!temp_index_in_range(defs[d])) {
            ssa = false;
         } else if (writer[defs[d]] >= 0 && writer[defs[d]] != (int)pos) {
            ssa = false;
         } else {
            writer[defs[d]] = (int)pos;
         }
      }

      nodes[pos].output_pred = -1;
      if (pair_writes_output(&i->U.P)) {
         nodes[pos].output_pred = last_output_writer;
         last_output_writer = (int)pos;
      }
   }
   if (!ssa) {
      free(nodes);
      free(writer);
      free(order);
      rc_pair_schedule(cc, user);
      return;
   }

   /* Predecessor sets: a pair depends on the definer of each temporary it
    * reads, plus the nearest earlier output/depth writer if it is one
    * itself.  Under single assignment these RAW edges, together with the
    * output WAW chain, are the only ordering constraints, so any
    * topological order preserves every computed value and every export. */
   bool reads_out_of_range = false;
   for (unsigned j = 0; j < n; j++) {
      unsigned reads[R300_CLASSIC_MAX_PREDS];
      const unsigned num_reads =
         gather_temp_reads(&nodes[j].inst->U.P, reads, R300_CLASSIC_MAX_PREDS);
      for (unsigned r = 0; r < num_reads; r++) {
         if (!temp_index_in_range((int)reads[r])) {
            reads_out_of_range = true;
            continue;
         }
         const int w = writer[reads[r]];
         if (w < 0 || w == (int)j)
            continue;
         bool dup = false;
         for (unsigned k = 0; k < nodes[j].num_pred; k++)
            dup |= (nodes[j].pred[k] == w);
         if (!dup && nodes[j].num_pred < R300_CLASSIC_MAX_PREDS)
            nodes[j].pred[nodes[j].num_pred++] = w;
      }
      if (nodes[j].output_pred >= 0 && nodes[j].output_pred != (int)j) {
         bool dup = false;
         for (unsigned k = 0; k < nodes[j].num_pred; k++)
            dup |= (nodes[j].pred[k] == nodes[j].output_pred);
         if (!dup && nodes[j].num_pred < R300_CLASSIC_MAX_PREDS)
            nodes[j].pred[nodes[j].num_pred++] = nodes[j].output_pred;
      }
      nodes[j].unresolved = nodes[j].num_pred;
   }
   if (reads_out_of_range) {
      free(nodes);
      free(writer);
      free(order);
      rc_pair_schedule(cc, user);
      return;
   }

   /* Greedy list schedule: emit the lowest-original-index ready pair, which
    * keeps a valid input order stable while draining the ready set through
    * the dependency graph.  n passes over at most n ready candidates. */
   for (unsigned emitted = 0; emitted < n; emitted++) {
      int pick = -1;
      for (unsigned j = 0; j < n; j++) {
         if (nodes[j].emitted || nodes[j].unresolved != 0)
            continue;
         if (pick < 0 || nodes[j].orig_index < nodes[pick].orig_index)
            pick = (int)j;
      }
      /* A single-assignment RAW-plus-output-WAW graph is acyclic for any
       * program a real front end emits; a dependency cycle here is an
       * unmodeled hazard the pass cannot schedule around.  Defer to the
       * legacy scheduler on the untouched instruction list (nothing has
       * been unlinked yet) instead of hard-failing the compile. */
      if (pick < 0) {
         free(nodes);
         free(writer);
         free(order);
         rc_pair_schedule(cc, user);
         return;
      }
      nodes[pick].emitted = true;
      order[emitted] = (unsigned)pick;
      for (unsigned j = 0; j < n; j++) {
         if (nodes[j].emitted)
            continue;
         for (unsigned k = 0; k < nodes[j].num_pred; k++) {
            if (nodes[j].pred[k] == pick) {
               nodes[j].unresolved--;
               break;
            }
         }
      }
   }

   /* Relink the pairs into the scheduled order, re-deriving the presubtract
    * NOP bubble on each placement the way the legacy emit loop does. */
   for (unsigned i = 0; i < n; i++)
      rc_remove_instruction(nodes[i].inst);

   struct rc_instruction *after = sentinel;
   for (unsigned i = 0; i < n; i++) {
      struct rc_instruction *inst = nodes[order[i]].inst;
      inst->U.P.Nop = 0;
      rc_insert_instruction(after, inst);
      set_presub_nop(inst);
      after = inst;
   }

   free(nodes);
   free(writer);
   free(order);
}
