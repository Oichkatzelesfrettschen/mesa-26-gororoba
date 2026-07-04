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

/* Up to four source slots on each of two pipes bound the distinct
 * temporaries one pair can read, and therefore its predecessor count. */
#define R300_CLASSIC_MAX_PREDS 8

struct sched_node {
   struct rc_instruction *inst;
   unsigned orig_index;
   int pred[R300_CLASSIC_MAX_PREDS];
   unsigned num_pred;
   unsigned unresolved;
   bool emitted;
};

void
r300_classic_schedule(struct radeon_compiler *cc, void *user)
{
   struct rc_instruction *const sentinel = &cc->Program.Instructions;

   /* The subset shape is a straight line of ALU pairs in single-assignment
    * form.  A TEX block, control flow, a still-normal instruction, or a
    * destination index written twice all break the RAW-only ordering this
    * pass relies on, so defer to the legacy scheduler that models them. */
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
    * definer means the stream is not single-assignment and the pass defers. */
   bool ssa = true;
   unsigned pos = 0;
   for (struct rc_instruction *i = sentinel->Next; i != sentinel; i = i->Next, pos++) {
      nodes[pos].inst = i;
      nodes[pos].orig_index = pos;
      const int defs[2] = {sub_temp_def(&i->U.P.RGB), sub_temp_def(&i->U.P.Alpha)};
      for (unsigned d = 0; d < 2 && ssa; d++) {
         if (defs[d] < 0)
            continue;
         if (writer[defs[d]] >= 0 && writer[defs[d]] != (int)pos)
            ssa = false;
         else
            writer[defs[d]] = (int)pos;
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
    * reads.  Under single assignment these RAW edges are the only ordering
    * constraint, so any topological order preserves every computed value. */
   for (unsigned j = 0; j < n; j++) {
      unsigned reads[R300_CLASSIC_MAX_PREDS];
      const unsigned num_reads =
         gather_temp_reads(&nodes[j].inst->U.P, reads, R300_CLASSIC_MAX_PREDS);
      for (unsigned r = 0; r < num_reads; r++) {
         const int w = writer[reads[r]];
         if (w < 0 || w == (int)j)
            continue;
         bool dup = false;
         for (unsigned k = 0; k < nodes[j].num_pred; k++)
            dup |= (nodes[j].pred[k] == w);
         if (!dup && nodes[j].num_pred < R300_CLASSIC_MAX_PREDS)
            nodes[j].pred[nodes[j].num_pred++] = w;
      }
      nodes[j].unresolved = nodes[j].num_pred;
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
      /* A single-assignment RAW graph is acyclic, so a ready pair always
       * exists until every pair is placed. */
      if (pick < 0) {
         free(nodes);
         free(writer);
         free(order);
         rc_error(cc, "r300_classic_schedule: dependency cycle");
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
