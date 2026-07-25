/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Direct-NIR software vertex-shader executor for the draw module.
 *
 * A driver that hands draw a PIPE_SHADER_IR_NIR shader (r300 SW-TCL,
 * r300_vs_draw.c) runs it through this CPU interpreter when its instruction
 * shape passes draw_vs_nir_supported.  The executor keeps an SSA-def-indexed
 * value table and
 * delegates every ALU opcode to nir_eval_const_opcode (the same pure
 * per-opcode evaluator constant folding uses), hand-writing only the vertex IO
 * intrinsics, the system-value loads, and nir_if / nir_loop control flow.
 *
 * See docs/gallium/draw-nir-executor-design.md.  draw_create_vs_exec selects
 * this executor for supported NIR and retains the TGSI machine for TGSI input
 * and unsupported NIR shapes.
 */

#include <stdio.h>
#include <stdlib.h>

#include "util/u_math.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/macros.h"
#include "util/bitscan.h"
#include "util/ralloc.h"
#include "pipe/p_shader_tokens.h"
#include "pipe/p_context.h"

#include "nir.h"
#include "nir_constant_expressions.h"
#include "nir/nir_to_tgsi_info.h"

#include "draw_private.h"
#include "draw_context.h"
#include "draw_vs.h"

struct nir_vertex_shader {
   struct draw_vertex_shader base;
   nir_shader *nir;
   nir_function_impl *impl;
   /* One nir_const_value vector per SSA def, sized by impl->ssa_alloc and
    * reused across vertices; run() overwrites every def it reads before use. */
   nir_const_value (*ssa)[NIR_MAX_VEC_COMPONENTS];
   unsigned ssa_alloc;
};

static inline struct nir_vertex_shader *
nir_vertex_shader(struct draw_vertex_shader *vs)
{
   return (struct nir_vertex_shader *)vs;
}

/* Per-run interpreter state: the value table plus the one vertex's worth of
 * IO the caller marshalled into AOS rows, plus the seeded system values. */
struct interp {
   struct nir_vertex_shader *nvs;
   nir_const_value (*ssa)[NIR_MAX_VEC_COMPONENTS];
   const struct draw_buffer_info *constants;
   const float (*input)[4];        /* [slot][xyzw] for this vertex */
   float (*output)[4];             /* [slot][xyzw] for this vertex */
   int instance_id;
   int base_instance;
   int vertex_id;
   int base_vertex;
   int vertex_id_nobase;
   int draw_id;
   unsigned num_inputs;
   unsigned num_outputs;
   unsigned exec_mode;
   /* The block executed immediately before the current one along the taken
    * path, so a phi at a block entry can select the source for that edge. */
   nir_block *prev_block;
};

/* nir loops run until a break; blocks propagate a jump to the enclosing walk. */
enum interp_flow {
   FLOW_NEXT = 0,
   FLOW_BREAK,
   FLOW_CONTINUE,
   FLOW_RETURN,
};

static enum interp_flow
interp_cf_list(struct interp *st, struct exec_list *list);

static inline nir_const_value *
def_vals(struct interp *st, nir_def *def)
{
   assert(def->index < st->nvs->ssa_alloc);
   return st->ssa[def->index];
}

static inline nir_const_value *
src_vals(struct interp *st, nir_src src)
{
   assert(src.ssa->index < st->nvs->ssa_alloc);
   return st->ssa[src.ssa->index];
}

static void
interp_load_const(struct interp *st, nir_load_const_instr *lc)
{
   nir_const_value *dst = def_vals(st, &lc->def);
   for (unsigned c = 0; c < lc->def.num_components; c++)
      dst[c] = lc->value[c];
}

static void
interp_undef(struct interp *st, nir_undef_instr *undef)
{
   nir_const_value *dst = def_vals(st, &undef->def);
   memset(dst, 0, sizeof(nir_const_value) * undef->def.num_components);
}

/* Mirror nir_try_constant_fold_alu's src gathering: swizzle each operand into a
 * dense per-component array, then let nir_eval_const_opcode implement the op. */
static void
interp_alu(struct interp *st, nir_alu_instr *alu)
{
   nir_const_value src[NIR_ALU_MAX_INPUTS][NIR_MAX_VEC_COMPONENTS];
   nir_const_value *srcs[NIR_ALU_MAX_INPUTS];

   unsigned bit_size = 0;
   if (!nir_alu_type_get_type_size(nir_op_infos[alu->op].output_type))
      bit_size = alu->def.bit_size;

   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
      if (bit_size == 0 &&
          !nir_alu_type_get_type_size(nir_op_infos[alu->op].input_types[i]))
         bit_size = alu->src[i].src.ssa->bit_size;

      nir_const_value *s = src_vals(st, alu->src[i].src);
      for (unsigned j = 0; j < nir_ssa_alu_instr_src_components(alu, i); j++)
         src[i][j] = s[alu->src[i].swizzle[j]];
      srcs[i] = src[i];
   }
   if (bit_size == 0)
      bit_size = 32;

   nir_eval_const_opcode(alu->op, def_vals(st, &alu->def), NULL,
                         alu->def.num_components, bit_size, srcs,
                         st->exec_mode);
}

static void
interp_intrinsic(struct interp *st, nir_intrinsic_instr *intr)
{
   nir_const_value *dst =
      nir_intrinsic_infos[intr->intrinsic].has_dest ? def_vals(st, &intr->def)
                                                     : NULL;

   switch (intr->intrinsic) {
   case nir_intrinsic_load_ubo: {
      /* nir_lower_uniforms_to_ubo turned every uniform read into this: the
       * block index and the byte offset are both scalar srcs.  Match TGSI:
       * out-of-range or unbound buffers read as zero. */
      unsigned block = src_vals(st, intr->src[0])[0].u32;
      unsigned offset = src_vals(st, intr->src[1])[0].u32;
      const struct draw_buffer_info *cb =
         (block < PIPE_MAX_CONSTANT_BUFFERS) ? &st->constants[block] : NULL;
      for (unsigned c = 0; c < intr->def.num_components; c++) {
         unsigned byte = offset + c * 4;
         if (!cb || !cb->ptr || byte + 4 > cb->size)
            dst[c].u32 = 0;
         else
            dst[c].u32 = *(const uint32_t *)((const char *)cb->ptr + byte);
      }
      break;
   }
   case nir_intrinsic_load_ubo_vec4: {
      /* The vec4-indexed UBO load r300's draw path emits: src[0] is the block,
       * the vec4 register index is nir_intrinsic_base + src[1] (both in vec4
       * units), and component is the first channel -- the same addressing
       * nir_to_tgsi resolves against the const file. */
      unsigned block = src_vals(st, intr->src[0])[0].u32;
      unsigned vec4 = nir_intrinsic_base(intr) + src_vals(st, intr->src[1])[0].u32;
      unsigned comp = nir_intrinsic_component(intr);
      const struct draw_buffer_info *cb =
         (block < PIPE_MAX_CONSTANT_BUFFERS) ? &st->constants[block] : NULL;
      for (unsigned c = 0; c < intr->def.num_components; c++) {
         unsigned word = vec4 * 4 + comp + c;
         if (!cb || !cb->ptr || (word + 1) * 4 > cb->size)
            dst[c].u32 = 0;
         else
            dst[c].u32 = ((const uint32_t *)cb->ptr)[word];
      }
      break;
   }
   case nir_intrinsic_load_input: {
      /* AOS vertex attribute row: base is the driver_location slot draw
       * marshalled, component the starting channel. */
      unsigned slot = nir_intrinsic_base(intr) + src_vals(st, intr->src[0])[0].u32;
      unsigned comp = nir_intrinsic_component(intr);
      assert(slot < st->num_inputs);
      assert(comp + intr->def.num_components <= 4);
      for (unsigned c = 0; c < intr->def.num_components; c++)
         dst[c].f32 = st->input[slot][comp + c];
      break;
   }
   case nir_intrinsic_store_output: {
      unsigned slot = nir_intrinsic_base(intr) + src_vals(st, intr->src[1])[0].u32;
      unsigned comp = nir_intrinsic_component(intr);
      unsigned wrmask = nir_intrinsic_write_mask(intr);
      nir_const_value *s = src_vals(st, intr->src[0]);
      assert(slot < st->num_outputs);
      assert(comp < 4);
      u_foreach_bit(c, wrmask) {
         assert(comp + c < 4);
         st->output[slot][comp + c] = s[c].f32;
      }
      break;
   }
   case nir_intrinsic_load_instance_id:
      dst[0].i32 = st->instance_id;
      break;
   case nir_intrinsic_load_base_instance:
      dst[0].i32 = st->base_instance;
      break;
   case nir_intrinsic_load_vertex_id:
      dst[0].i32 = st->vertex_id;
      break;
   case nir_intrinsic_load_vertex_id_zero_base:
      dst[0].i32 = st->vertex_id_nobase;
      break;
   case nir_intrinsic_load_base_vertex:
   case nir_intrinsic_load_first_vertex:
      dst[0].i32 = st->base_vertex;
      break;
   case nir_intrinsic_load_draw_id:
      dst[0].u32 = (uint32_t)st->draw_id;
      break;
   default:
      /* Loudly surface an unhandled intrinsic during calibration rather than
       * silently transforming vertices with garbage. */
      fprintf(stderr, "draw_vs_nir: unhandled intrinsic '%s'\n",
              nir_intrinsic_infos[intr->intrinsic].name);
      UNREACHABLE("draw_vs_nir: unhandled intrinsic");
   }
}

/* Resolve one phi's selected predecessor source into a temporary vector. */
static void
interp_phi_src_to_tmp(struct interp *st, nir_phi_instr *phi,
                      nir_const_value *tmp)
{
   nir_foreach_phi_src(src, phi) {
      if (src->pred == st->prev_block) {
         nir_const_value *s = src_vals(st, src->src);
         for (unsigned c = 0; c < phi->def.num_components; c++)
            tmp[c] = s[c];
         return;
      }
   }
   UNREACHABLE("draw_vs_nir: phi has no source for the taken predecessor");
}

/* Phis in a block are parallel copies: a loop header may read a sibling phi
 * that lives in the same block when the taken edge is a backedge.  Stage every
 * selected source first, then write destinations, so swaps stay correct. */
static void
interp_phis_parallel(struct interp *st, nir_block *block)
{
   unsigned nphi = 0;
   nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_phi)
         break;
      nphi++;
   }
   if (!nphi)
      return;

   nir_const_value (*staged)[NIR_MAX_VEC_COMPONENTS] =
      malloc(sizeof(*staged) * nphi);
   if (!staged)
      UNREACHABLE("draw_vs_nir: phi stage allocation failed");

   unsigned i = 0;
   nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_phi)
         break;
      interp_phi_src_to_tmp(st, nir_instr_as_phi(instr), staged[i]);
      i++;
   }

   i = 0;
   nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_phi)
         break;
      nir_phi_instr *phi = nir_instr_as_phi(instr);
      nir_const_value *dst = def_vals(st, &phi->def);
      for (unsigned c = 0; c < phi->def.num_components; c++)
         dst[c] = staged[i][c];
      i++;
   }
   free(staged);
}

static enum interp_flow
interp_block(struct interp *st, nir_block *block)
{
   interp_phis_parallel(st, block);

   nir_foreach_instr(instr, block) {
      switch (instr->type) {
      case nir_instr_type_phi:
         /* Resolved in the parallel stage above. */
         break;
      case nir_instr_type_load_const:
         interp_load_const(st, nir_instr_as_load_const(instr));
         break;
      case nir_instr_type_undef:
         interp_undef(st, nir_instr_as_undef(instr));
         break;
      case nir_instr_type_alu:
         interp_alu(st, nir_instr_as_alu(instr));
         break;
      case nir_instr_type_intrinsic:
         interp_intrinsic(st, nir_instr_as_intrinsic(instr));
         break;
      case nir_instr_type_jump: {
         st->prev_block = block;
         switch (nir_instr_as_jump(instr)->type) {
         case nir_jump_break:    return FLOW_BREAK;
         case nir_jump_continue: return FLOW_CONTINUE;
         case nir_jump_return:   return FLOW_RETURN;
         default:                UNREACHABLE("draw_vs_nir: unhandled jump");
         }
      }
      default:
         UNREACHABLE("draw_vs_nir: unhandled instr type");
      }
   }
   st->prev_block = block;
   return FLOW_NEXT;
}

static enum interp_flow
interp_cf_list(struct interp *st, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block: {
         enum interp_flow f = interp_block(st, nir_cf_node_as_block(node));
         if (f != FLOW_NEXT)
            return f;
         break;
      }
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         /* nif->condition is always a 1-bit nir_bool, and nir_const_value's
          * .b (a C _Bool) is one byte; nir_eval_const_opcode's compare/logic
          * evaluators write only that byte (nir_constant_expressions.c,
          * evaluate_ige/evaluate_ine/... bit_size == 1 case), leaving the
          * other three bytes of the union whatever the ssa slot last held.
          * Reading .u32 here picked up that stale upper-byte garbage and
          * could take the wrong branch whenever the slot's previous 32-bit
          * write left nonzero high bytes. */
         bool cond = src_vals(st, nif->condition)[0].b;
         enum interp_flow f =
            interp_cf_list(st, cond ? &nif->then_list : &nif->else_list);
         if (f != FLOW_NEXT)
            return f;
         break;
      }
      case nir_cf_node_loop: {
         nir_loop *loop = nir_cf_node_as_loop(node);
         /* draw_create_vs_nir runs nir_lower_continue_constructs before this
          * interpreter ever sees the shader, folding glsl_to_nir's for-loop
          * continue construct into the ordinary body/backedge shape below. */
         assert(!nir_loop_has_continue_construct(loop));
         for (;;) {
            enum interp_flow f = interp_cf_list(st, &loop->body);
            if (f == FLOW_BREAK)
               break;
            if (f == FLOW_RETURN)
               return FLOW_RETURN;
            /* FLOW_NEXT and FLOW_CONTINUE both re-enter the body: a nir loop
             * runs until an explicit break. */
         }
         break;
      }
      default:
         UNREACHABLE("draw_vs_nir: unhandled cf node");
      }
   }
   return FLOW_NEXT;
}

static void
vs_nir_prepare(struct draw_vertex_shader *shader, struct draw_context *draw)
{
   /* Each nir_vertex_shader owns its nir and value table, so unlike the shared
    * tgsi_exec_machine there is no cross-shader rebind hazard. */
   assert(!draw->llvm);
}

static void
vs_nir_run_linear(struct draw_context *draw,
                  struct draw_vertex_shader *shader,
                  const float (*input)[4],
                  float (*output)[4],
                  const struct draw_buffer_info *constants,
                  unsigned count,
                  unsigned input_stride,
                  unsigned output_stride,
                  const unsigned *fetch_elts)
{
   struct nir_vertex_shader *nvs = nir_vertex_shader(shader);
   const bool clamp_vertex_color = draw->rasterizer->clamp_vertex_color;
   /* Sysval-only shaders may pass a NULL input pointer; size the dummy to
    * PIPE_MAX_ATTRIBS so a NULL-input run never reads past the array even
    * when asserts are compiled out. */
   static const float dummy_input_rows[PIPE_MAX_ATTRIBS][4];

   struct interp st = {
      .nvs = nvs,
      .ssa = nvs->ssa,
      .constants = constants,
      .instance_id = draw->instance_id,
      .base_instance = draw->start_instance,
      .draw_id = (int)draw->pt.user.drawid,
      .num_inputs = shader->info.num_inputs,
      .num_outputs = shader->info.num_outputs,
      .exec_mode = nvs->nir->info.float_controls_execution_mode,
   };

   assert(!draw->llvm);

   for (unsigned i = 0; i < count; i++) {
      /* Same vertex-id arithmetic the tgsi_exec path uses so numeric results
       * match: basevertex is eltBias for indexed draws, start_index otherwise. */
      int basevertex = draw->pt.user.eltSize ? draw->pt.user.eltBias
                                             : draw->start_index;
      st.base_vertex = basevertex;
      st.vertex_id = fetch_elts ? fetch_elts[i] : (int)(i + basevertex);
      st.vertex_id_nobase = fetch_elts ? (int)(fetch_elts[i] - basevertex)
                                       : (int)i;
      st.input = input ? input : dummy_input_rows;
      st.output = output;
      st.prev_block = NULL;

      interp_cf_list(&st, &nvs->impl->body);

      /* clamp_vertex_color mirrors the tgsi_exec unswizzle: SATURATE the
       * COLOR/BCOLOR outputs only, keyed on the scanned output semantic. */
      if (clamp_vertex_color) {
         for (unsigned slot = 0; slot < shader->info.num_outputs; slot++) {
            enum tgsi_semantic name = shader->info.output_semantic_name[slot];
            if (name == TGSI_SEMANTIC_COLOR || name == TGSI_SEMANTIC_BCOLOR) {
               for (unsigned c = 0; c < 4; c++)
                  output[slot][c] = SATURATE(output[slot][c]);
            }
         }
      }

      if (input)
         input = (const float (*)[4])((const char *)input + input_stride);
      output = (float (*)[4])((char *)output + output_stride);
   }
}

static void
vs_nir_delete(UNUSED struct draw_context *draw, struct draw_vertex_shader *dvs)
{
   struct nir_vertex_shader *nvs = nir_vertex_shader(dvs);
   ralloc_free(nvs->nir);
   FREE(nvs);
}

/* One draw AOS row (input[slot][4] / output[slot][4]) per vec4 location, so a
 * lowered load_input/store_output base is the driver_location slot directly. */
static unsigned
draw_vs_nir_io_slots(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/* The interpreter indexes AOS rows with each lowered intrinsic's base plus
 * constant offset.  NIR semantic masks describe API locations and num_inputs
 * is producer metadata, so neither value alone defines this executable span. */
static bool
draw_vs_nir_io_spans(nir_shader *nir,
                     unsigned *input_span,
                     unsigned *output_span)
{
   *input_span = 0;
   *output_span = 0;

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         unsigned base;
         unsigned offset;

         switch (intr->intrinsic) {
         case nir_intrinsic_load_input:
            if (!nir_src_is_const(intr->src[0]) ||
                nir_intrinsic_component(intr) + intr->def.num_components > 4)
               return false;
            base = nir_intrinsic_base(intr);
            offset = nir_src_as_uint(intr->src[0]);
            if (base >= PIPE_MAX_ATTRIBS ||
                offset >= PIPE_MAX_ATTRIBS - base)
               return false;
            *input_span = MAX2(*input_span, base + offset + 1);
            break;

         case nir_intrinsic_store_output: {
            const unsigned component = nir_intrinsic_component(intr);
            const unsigned write_mask = nir_intrinsic_write_mask(intr);
            if (!nir_src_is_const(intr->src[1]) || component >= 4 ||
                (write_mask >> (4 - component)))
               return false;
            base = nir_intrinsic_base(intr);
            offset = nir_src_as_uint(intr->src[1]);
            if (base >= PIPE_MAX_SHADER_OUTPUTS ||
                offset >= PIPE_MAX_SHADER_OUTPUTS - base)
               return false;
            *output_span = MAX2(*output_span, base + offset + 1);
            break;
         }

         default:
            break;
         }
      }
   }

   return true;
}

static void
draw_vs_nir_lower(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_lower_continue_constructs);
   if (!nir->options->lower_uniforms_to_ubo)
      NIR_PASS(_, nir, nir_lower_uniforms_to_ubo, false, false);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            draw_vs_nir_io_slots, 0);
   NIR_PASS(_, nir, nir_opt_dce);

   /* nir_lower_io changes the executable I/O shape.  Refresh inputs_read and
    * outputs_written before nir_tgsi_scan_shader derives the AOS row counts. */
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

/* Opt-in calibration report: which opcodes, intrinsics, and control-flow nodes
 * a shader actually reaches after lowering, so the corpus can prove it widened
 * coverage (in particular that nir_if / nir_loop survive to the interpreter and
 * are not flattened away before draw). */
DEBUG_GET_ONCE_BOOL_OPTION(draw_nir_exec_stats, "DRAW_NIR_EXEC_STATS", false)

static void
draw_vs_nir_count_cf(struct exec_list *list, unsigned *n_if, unsigned *n_loop)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      if (node->type == nir_cf_node_if) {
         nir_if *nif = nir_cf_node_as_if(node);
         (*n_if)++;
         draw_vs_nir_count_cf(&nif->then_list, n_if, n_loop);
         draw_vs_nir_count_cf(&nif->else_list, n_if, n_loop);
      } else if (node->type == nir_cf_node_loop) {
         (*n_loop)++;
         draw_vs_nir_count_cf(&nir_cf_node_as_loop(node)->body, n_if, n_loop);
      }
   }
}

static void
draw_vs_nir_dump_stats(nir_function_impl *impl, const char *name)
{
   bool op_seen[nir_num_opcodes] = {0};
   bool intr_seen[nir_num_intrinsics] = {0};
   unsigned n_instr = 0, n_if = 0, n_loop = 0;

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         n_instr++;
         if (instr->type == nir_instr_type_alu)
            op_seen[nir_instr_as_alu(instr)->op] = true;
         else if (instr->type == nir_instr_type_intrinsic)
            intr_seen[nir_instr_as_intrinsic(instr)->intrinsic] = true;
      }
   }
   draw_vs_nir_count_cf(&impl->body, &n_if, &n_loop);

   fprintf(stderr, "DRAW_NIR_EXEC_STATS shader='%s' instr=%u if=%u loop=%u\n",
           name ? name : "?", n_instr, n_if, n_loop);
   fprintf(stderr, "  ops:");
   for (unsigned i = 0; i < nir_num_opcodes; i++)
      if (op_seen[i])
         fprintf(stderr, " %s", nir_op_infos[i].name);
   fprintf(stderr, "\n  intr:");
   for (unsigned i = 0; i < nir_num_intrinsics; i++)
      if (intr_seen[i])
         fprintf(stderr, " %s", nir_intrinsic_infos[i].name);
   fprintf(stderr, "\n");
}

/* Per-shader admission gate for draw_create_vs_exec: decides whether the
 * interpreter above can run this NIR vertex shader at all, or whether the
 * caller must keep the nir_to_tgsi bridge.  interp_intrinsic and interp_block
 * hit UNREACHABLE on any intrinsic, instruction type, or jump kind they do
 * not implement, so this predicate must classify a shader as unsupported
 * before draw_create_vs_nir ever builds it -- not after the executor has
 * already aborted mid-draw.
 *
 * draw_create_vs_nir lowers the incoming nir_shader in place
 * (nir_lower_continue_constructs, nir_lower_uniforms_to_ubo, nir_lower_io,
 * nir_opt_dce) before the interpreter ever sees it, so scanning the
 * pre-lowering shader would have to reimplement an allowlist of the deref
 * and uniform forms that those passes turn into supported intrinsics.
 * Instead this predicate clones the shader, runs the identical lowering
 * pipeline on the clone, and scans the clone's entry impl: what it inspects
 * is bit-for-bit what the interpreter would receive, so it cannot admit a
 * shape draw_create_vs_nir's UNREACHABLE paths would reject.  The clone is
 * discarded; state->ir.nir stays untouched for the unsupported-shape bridge. */
bool
draw_vs_nir_supported(const struct pipe_shader_state *state)
{
   assert(state->type == PIPE_SHADER_IR_NIR);
   nir_shader *nir = nir_shader_clone(NULL, state->ir.nir);

   draw_vs_nir_lower(nir);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   unsigned input_span;
   unsigned output_span;
   bool supported =
      draw_vs_nir_io_spans(nir, &input_span, &output_span);

   if (!supported)
      goto done;

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         switch (instr->type) {
         case nir_instr_type_phi:
         case nir_instr_type_load_const:
         case nir_instr_type_undef:
            break;
         case nir_instr_type_alu:
            /* interp_alu dispatches every opcode through
             * nir_eval_const_opcode, the same per-opcode evaluator constant
             * folding uses; it covers the full nir_op set by construction,
             * so ALU carries no per-opcode allowlist here. */
            break;
         case nir_instr_type_intrinsic:
            switch (nir_instr_as_intrinsic(instr)->intrinsic) {
            case nir_intrinsic_load_ubo:
            case nir_intrinsic_load_ubo_vec4:
            case nir_intrinsic_load_input:
            case nir_intrinsic_store_output:
            case nir_intrinsic_load_instance_id:
            case nir_intrinsic_load_base_instance:
            case nir_intrinsic_load_vertex_id:
            case nir_intrinsic_load_vertex_id_zero_base:
            case nir_intrinsic_load_base_vertex:
            case nir_intrinsic_load_first_vertex:
            case nir_intrinsic_load_draw_id:
               break;
            default:
               /* interp_intrinsic's default case, mirrored here. */
               supported = false;
               break;
            }
            break;
         case nir_instr_type_jump:
            switch (nir_instr_as_jump(instr)->type) {
            case nir_jump_break:
            case nir_jump_continue:
            case nir_jump_return:
               break;
            default:
               supported = false;
               break;
            }
            break;
         default:
            /* nir_instr_type_tex, nir_instr_type_call, and any other type
             * interp_block does not list fall to its UNREACHABLE default. */
            supported = false;
            break;
         }
         if (!supported)
            goto done;
      }
   }

done:
   ralloc_free(nir);
   return supported;
}

struct draw_vertex_shader *
draw_create_vs_nir(struct draw_context *draw,
                   const struct pipe_shader_state *state)
{
   struct nir_vertex_shader *vs = CALLOC_STRUCT(nir_vertex_shader);
   if (!vs)
      return NULL;

   assert(state->type == PIPE_SHADER_IR_NIR);
   nir_shader *nir = state->ir.nir;

   /* glsl_to_nir lowers every GLSL `for` loop to a loop body plus a separate
    * continue construct (nir_loop_continue_list) holding the increment;
    * nir_opt_dce and several other general-purpose passes this factory or its
    * callers may run assert no continue construct survives past this point
    * (dce_cf_list, nir_opt_dce.c), so fold it into the ordinary body/backedge
    * shape before any of them see the shader. */
   /* Keep the admission clone and executable shader on one lowering path. */
   draw_vs_nir_lower(nir);
   unsigned input_span;
   unsigned output_span;
   if (!draw_vs_nir_io_spans(nir, &input_span, &output_span)) {
      ralloc_free(nir);
      FREE(vs);
      return NULL;
   }
   nir_tgsi_scan_shader(nir, &vs->base.info, true);
   vs->base.info.num_inputs = input_span;
   vs->base.info.num_outputs = output_span;

   vs->nir = nir;
   vs->impl = nir_shader_get_entrypoint(nir);
   nir_index_ssa_defs(vs->impl);
   vs->ssa_alloc = vs->impl->ssa_alloc;
   vs->ssa = ralloc_size(nir,
                         sizeof(nir_const_value[NIR_MAX_VEC_COMPONENTS]) *
                         vs->ssa_alloc);

   vs->base.state.type = PIPE_SHADER_IR_NIR;
   vs->base.state.ir.nir = nir;
   vs->base.state.stream_output = state->stream_output;
   vs->base.prepare = vs_nir_prepare;
   vs->base.run_linear = vs_nir_run_linear;
   vs->base.delete = vs_nir_delete;
   vs->base.create_variant = draw_vs_create_variant_generic;

   if (debug_get_option_draw_nir_exec_stats())
      draw_vs_nir_dump_stats(vs->impl, nir->info.name);

   return &vs->base;
}
