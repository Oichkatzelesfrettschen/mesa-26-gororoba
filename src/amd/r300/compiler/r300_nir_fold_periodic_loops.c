/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_nir.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"

/* A counted loop whose carried values undergo a pure component
 * permutation each iteration -- res = res.yzwx and friends -- has the
 * closed form final = P^(N mod ord(P))(initial), so the loop's value
 * is a static reswizzle of its inputs no matter how large N is.
 * Rewriting the outside-loop uses of the loop-header phis to that
 * reswizzle leaves the phi cycle and the induction arithmetic dead:
 * nir_opt_dce removes the phis and nir_opt_dead_cf deletes the
 * side-effect-free loop, so a 101-iteration for/while/do-while never
 * reaches the unroller at all.  R300's fragment pipeline has no
 * dynamic control flow and a 64-ALU ceiling, which makes this fold
 * the difference between a one-instruction shader and a refused
 * unroll. */

struct slot {
   nir_phi_instr *phi;
   uint8_t comp;
};

#define MAX_SLOTS 64

struct perm_ctx {
   nir_loop *loop;
   struct slot slots[MAX_SLOTS];
   unsigned num_slots;
   /* map[i] = slot index whose INITIAL value lands in slot i after one
    * iteration. */
   int map[MAX_SLOTS];
};

static int
slot_index(struct perm_ctx *ctx, nir_phi_instr *phi, unsigned comp)
{
   for (unsigned i = 0; i < ctx->num_slots; i++)
      if (ctx->slots[i].phi == phi && ctx->slots[i].comp == comp)
         return (int)i;
   return -1;
}

/* Trace a value component through mov/vec chains to a loop-header phi
 * component.  Returns the slot index or -1 when the chain leaves the
 * pure-permutation domain. */
static int
trace_to_slot(struct perm_ctx *ctx, nir_def *def, unsigned comp,
              unsigned depth)
{
   if (depth > 16)
      return -1;
   if (nir_def_instr(def)->type == nir_instr_type_phi)
      return slot_index(ctx, nir_instr_as_phi(nir_def_instr(def)), comp);
   if (nir_def_instr(def)->type != nir_instr_type_alu)
      return -1;
   nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(def));
   if (alu->op == nir_op_mov) {
      return trace_to_slot(ctx, alu->src[0].src.ssa,
                           alu->src[0].swizzle[comp], depth + 1);
   }
   if (nir_op_is_vec(alu->op)) {
      return trace_to_slot(ctx, alu->src[comp].src.ssa,
                           alu->src[comp].swizzle[0], depth + 1);
   }
   return -1;
}

static bool
cf_node_is_inside_loop(nir_cf_node *node, nir_loop *loop)
{
   for (; node; node = node->parent) {
      if (node == &loop->cf_node)
         return true;
   }
   return false;
}

static bool
use_is_inside_loop(nir_src *use, nir_loop *loop)
{
   if (nir_src_is_if(use))
      return cf_node_is_inside_loop(&nir_src_use_if(use)->cf_node, loop);
   return cf_node_is_inside_loop(
      &nir_src_use_instr(use)->block->cf_node, loop);
}

static bool
block_is_inside_loop(nir_block *block, nir_loop *loop)
{
   for (nir_cf_node *node = &block->cf_node; node; node = node->parent) {
      if (node == &loop->cf_node)
         return true;
   }
   return false;
}

static bool
fold_loop(nir_builder *b, nir_loop *loop)
{
   if (nir_loop_has_continue_construct(loop))
      return false;
   if (!loop->info || !loop->info->exact_trip_count_known)
      return false;
   const unsigned trip = loop->info->max_trip_count;

   nir_block *header = nir_loop_first_block(loop);

   struct perm_ctx ctx = {.loop = loop};

   /* Collect the carried slots.  Every header phi has exactly two
    * sources: the preheader initial and the latch update. */
   nir_foreach_phi(phi, header) {
      if (exec_list_length(&phi->srcs) != 2)
         return false;
      if (phi->def.num_components > 4 ||
          ctx.num_slots + phi->def.num_components > MAX_SLOTS)
         return false;
      for (unsigned c = 0; c < phi->def.num_components; c++) {
         ctx.slots[ctx.num_slots].phi = phi;
         ctx.slots[ctx.num_slots].comp = c;
         ctx.num_slots++;
      }
   }
   if (!ctx.num_slots)
      return false;

   /* Classify each phi: permutation-carried (latch value traces to
    * header-phi components) or auxiliary (induction counters and
    * friends), which must stay loop-local. */
   bool is_perm[MAX_SLOTS] = {false};
   for (unsigned i = 0; i < ctx.num_slots; i++) {
      nir_phi_instr *phi = ctx.slots[i].phi;
      nir_def *latch_val = NULL;
      nir_def *init_val = NULL;
      nir_foreach_phi_src(psrc, phi) {
         if (block_is_inside_loop(psrc->pred, loop))
            latch_val = psrc->src.ssa;
         else
            init_val = psrc->src.ssa;
      }
      if (!latch_val || !init_val)
         return false;
      const int src_slot =
         trace_to_slot(&ctx, latch_val, ctx.slots[i].comp, 0);
      if (src_slot >= 0) {
         ctx.map[i] = src_slot;
         is_perm[i] = true;
      } else {
         ctx.map[i] = -1;
      }
   }

   /* A phi mixing permutation and non-permutation components, or a
    * non-permutation phi observed outside the loop, is out of scope. */
   for (unsigned i = 0; i < ctx.num_slots; i++) {
      nir_phi_instr *phi = ctx.slots[i].phi;
      if (is_perm[i] != is_perm[slot_index(&ctx, phi, 0)])
         return false;
      if (is_perm[i])
         continue;
      nir_foreach_use(use, &phi->def) {
         if (!use_is_inside_loop(use, loop))
            return false;
      }
   }

   /* The permutation slots must map bijectively among themselves. */
   bool seen[MAX_SLOTS] = {false};
   unsigned num_perm = 0;
   for (unsigned i = 0; i < ctx.num_slots; i++) {
      if (!is_perm[i])
         continue;
      num_perm++;
      if (!is_perm[ctx.map[i]] || seen[ctx.map[i]])
         return false;
      seen[ctx.map[i]] = true;
   }
   if (!num_perm)
      return false;

   /* P^trip via cycle order: the order of a permutation on <= 64 slots
    * of interest here is small, so step the map trip % ord times, with
    * ord computed as the lcm of cycle lengths. */
   unsigned ord = 1;
   for (unsigned i = 0; i < ctx.num_slots; i++) {
      if (!is_perm[i])
         continue;
      unsigned len = 1;
      for (int j = ctx.map[i]; j != (int)i; j = ctx.map[j])
         len++;
      /* lcm(ord, len) */
      unsigned a = ord, bl = len;
      while (bl) {
         unsigned t = a % bl;
         a = bl;
         bl = t;
      }
      ord = ord / a * len;
   }
   unsigned steps = trip % ord;

   int final_map[MAX_SLOTS];
   for (unsigned i = 0; i < ctx.num_slots; i++) {
      int s = (int)i;
      for (unsigned k = 0; k < steps && s >= 0; k++)
         s = ctx.map[s];
      final_map[i] = s;
   }

   /* Emit the closed form after the loop and retarget every
    * outside-loop use. */
   bool progress = false;
   for (unsigned i = 0; i < ctx.num_slots;) {
      nir_phi_instr *phi = ctx.slots[i].phi;
      const unsigned w = phi->def.num_components;
      if (!is_perm[i]) {
         i += w;
         continue;
      }
      bool has_outside_use = false;
      nir_foreach_use(use, &phi->def) {
         if (!use_is_inside_loop(use, loop))
            has_outside_use = true;
      }
      if (!has_outside_use) {
         i += w;
         continue;
      }
      b->cursor = nir_after_cf_node(&loop->cf_node);
      nir_def *chans[4];
      for (unsigned c = 0; c < w; c++) {
         const struct slot *src = &ctx.slots[final_map[i + c]];
         nir_def *init = NULL;
         nir_foreach_phi_src(psrc, src->phi) {
            if (!block_is_inside_loop(psrc->pred, loop))
               init = psrc->src.ssa;
         }
         chans[c] = nir_channel(b, init, src->comp);
      }
      nir_def *folded = nir_vec(b, chans, w);
      nir_foreach_use_safe(use, &phi->def) {
         if (!use_is_inside_loop(use, loop))
            nir_src_rewrite(use, folded);
      }
      progress = true;
      i += w;
   }
   return progress;
}

static bool
fold_loops_in_cf_list(nir_builder *b, struct exec_list *cf_list)
{
   bool progress = false;
   foreach_list_typed(nir_cf_node, node, node, cf_list) {
      switch (node->type) {
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         progress |= fold_loops_in_cf_list(b, &nif->then_list);
         progress |= fold_loops_in_cf_list(b, &nif->else_list);
         break;
      }
      case nir_cf_node_loop: {
         nir_loop *loop = nir_cf_node_as_loop(node);
         /* Innermost loops only; a nested loop inside the body takes
          * itself out of the pure-permutation domain via trace_to_slot
          * anyway, but skipping keeps the analysis honest. */
         bool nested = fold_loops_in_cf_list(b, &loop->body);
         progress |= nested;
         if (!nested)
            progress |= fold_loop(b, loop);
         break;
      }
      default:
         break;
      }
   }
   return progress;
}

bool
r300_nir_fold_periodic_loops(nir_shader *s)
{
   bool progress = false;
   nir_foreach_function_impl(impl, s) {
      nir_metadata_require(impl, nir_metadata_loop_analysis,
                           (nir_variable_mode)0, (int)false);
      nir_builder b = nir_builder_create(impl);
      const bool p = fold_loops_in_cf_list(&b, &impl->body);
      progress |= p;
      nir_progress(p, impl, nir_metadata_none);
   }
   return progress;
}
