/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300_classic_select.h"

#include "nir_builder.h"
#include "util/hash_table.h"
#include "util/ralloc.h"

#include "../radeon_program_constants.h"

/* The selector maps each nir_def to a classic source descriptor: inputs,
 * constants, and immediates map to file references with a composed swizzle,
 * ALU and TEX results map to the defining classic instruction, and fneg/fabs
 * fold into the descriptor's modifier bits instead of emitting anything.
 * Everything outside the phase-1 subset lands in reject() with a named
 * reason -- rejection is a supported result, silent dropping is not. */

struct sel_ctx {
   void *mem_ctx;
   struct r300_classic_program *prog;
   struct r300_classic_select_result *result;
   /* nir_def* -> struct r300_classic_src* (ralloc'd). */
   struct hash_table *value_map;
   const char *reject;
};

static bool
reject(struct sel_ctx *ctx, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   ctx->reject = ralloc_vasprintf(ctx->mem_ctx, fmt, args);
   va_end(args);
   return false;
}

static void
map_def(struct sel_ctx *ctx, const nir_def *def,
        struct r300_classic_src src)
{
   struct r300_classic_src *entry =
      ralloc(ctx->mem_ctx, struct r300_classic_src);
   *entry = src;
   _mesa_hash_table_insert(ctx->value_map, def, entry);
}

static const struct r300_classic_src *
lookup_def(struct sel_ctx *ctx, const nir_def *def)
{
   struct hash_entry *e = _mesa_hash_table_search(ctx->value_map, def);
   return e ? e->data : NULL;
}

/* Compose an RC swizzle with a NIR per-channel select array: result channel
 * i reads what the base descriptor's channel select[i] read.  Channels past
 * the value's width repeat the last valid select, matching how a vec4 ALU
 * consumes a narrower operand. */
static unsigned
compose_swizzle(unsigned base, const uint8_t *select, unsigned num)
{
   unsigned swz = 0;
   for (unsigned c = 0; c < 4; c++) {
      unsigned lane = select[c < num ? c : num - 1];
      swz |= GET_SWZ(base, lane) << (c * 3);
   }
   return swz;
}

/* Resolve a NIR ALU source to a classic descriptor with the NIR swizzle
 * composed in. */
static bool
get_alu_src(struct sel_ctx *ctx, const nir_alu_instr *alu, unsigned s,
            struct r300_classic_src *out)
{
   const struct r300_classic_src *base =
      lookup_def(ctx, alu->src[s].src.ssa);
   if (!base)
      return reject(ctx, "operand of '%s' has no selected value",
                    nir_op_infos[alu->op].name);
   *out = *base;
   out->swizzle = compose_swizzle(base->swizzle, alu->src[s].swizzle,
                                  alu->src[s].src.ssa->num_components);
   return true;
}

static bool
get_plain_src(struct sel_ctx *ctx, const nir_def *def, const char *what,
              struct r300_classic_src *out)
{
   const struct r300_classic_src *base = lookup_def(ctx, def);
   if (!base)
      return reject(ctx, "%s has no selected value", what);
   *out = *base;
   return true;
}

static bool
select_load_const(struct sel_ctx *ctx, const nir_load_const_instr *lc)
{
   if (lc->def.bit_size != 32)
      return reject(ctx, "load_const bit_size %u", lc->def.bit_size);

   float v[4] = {0, 0, 0, 0};
   for (unsigned c = 0; c < lc->def.num_components && c < 4; c++)
      v[c] = lc->value[c].f32;

   struct r300_classic_immediates *imm = &ctx->result->immediates;
   unsigned slot;
   for (slot = 0; slot < imm->count; slot++)
      if (memcmp(imm->values[slot], v, sizeof(v)) == 0)
         break;
   if (slot == imm->count) {
      if (imm->count >= R300_CLASSIC_MAX_IMMEDIATES)
         return reject(ctx, "immediate table full");
      if (imm->first_index + imm->count >= ctx->prog->target->max_const_regs)
         return reject(ctx, "immediates exceed the constant file");
      memcpy(imm->values[imm->count++], v, sizeof(v));
   }

   map_def(ctx, &lc->def, (struct r300_classic_src){
      .file = R300C_FILE_CONST,
      .index = imm->first_index + slot,
      .swizzle = RC_SWIZZLE_XYZW,
   });
   return true;
}

static bool
select_intrinsic(struct sel_ctx *ctx, nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input: {
      if (!nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]))
         return reject(ctx, "indirect input addressing");
      const unsigned component = nir_intrinsic_component(intr);
      uint8_t select[4];
      for (unsigned c = 0; c < 4; c++)
         select[c] = MIN2(component + c, 3);
      map_def(ctx, &intr->def, (struct r300_classic_src){
         .file = R300C_FILE_INPUT,
         .index = nir_intrinsic_base(intr),
         .swizzle = compose_swizzle(RC_SWIZZLE_XYZW, select, 4),
      });
      return true;
   }
   case nir_intrinsic_store_output: {
      if (!nir_src_is_const(intr->src[1]) || nir_src_as_uint(intr->src[1]))
         return reject(ctx, "indirect output addressing");
      const nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      enum r300_classic_op op;
      if (sem.location == FRAG_RESULT_COLOR ||
          sem.location == FRAG_RESULT_DATA0)
         op = R300C_OP_EXPORT_COLOR;
      else if (sem.location == FRAG_RESULT_DEPTH)
         op = R300C_OP_EXPORT_DEPTH;
      else
         return reject(ctx, "store_output to location %u", sem.location);

      struct r300_classic_src src;
      if (!get_plain_src(ctx, intr->src[0].ssa, "store_output value", &src))
         return false;
      struct r300_classic_instr *e = r300_classic_instr_append(ctx->prog, op);
      if (!e)
         return reject(ctx, "out of memory");
      e->src[0] = src;
      return true;
   }
   default:
      return reject(ctx, "intrinsic '%s' outside the classic subset",
                    nir_intrinsic_infos[intr->intrinsic].name);
   }
}

/* One row per phase-1 ALU op; fneg/fabs and identity vecN fold into source
 * descriptors instead. */
static bool
alu_op_map(nir_op op, enum r300_classic_op *out)
{
   switch (op) {
   case nir_op_mov:    *out = R300C_OP_MOV; return true;
   case nir_op_fadd:   *out = R300C_OP_ADD; return true;
   case nir_op_fmul:   *out = R300C_OP_MUL; return true;
   case nir_op_ffma:
   case nir_op_fmad:   *out = R300C_OP_MAD; return true;
   case nir_op_fdot3:  *out = R300C_OP_DP3; return true;
   case nir_op_fdot4:  *out = R300C_OP_DP4; return true;
   case nir_op_fmin:   *out = R300C_OP_MIN; return true;
   case nir_op_fmax:   *out = R300C_OP_MAX; return true;
   case nir_op_ffract: *out = R300C_OP_FRC; return true;
   case nir_op_frcp:   *out = R300C_OP_RCP; return true;
   case nir_op_frsq:   *out = R300C_OP_RSQ; return true;
   default:            return false;
   }
}

static bool
select_alu(struct sel_ctx *ctx, nir_alu_instr *alu)
{
   /* Modifier folding: the descriptor carries negate/abs, so fneg and fabs
    * select nothing. */
   if (alu->op == nir_op_fneg || alu->op == nir_op_fabs) {
      struct r300_classic_src src;
      if (!get_alu_src(ctx, alu, 0, &src))
         return false;
      if (alu->op == nir_op_fneg) {
         src.negate = !src.negate;
      } else {
         src.abs = true;
         src.negate = false;
      }
      map_def(ctx, &alu->def, src);
      return true;
   }

   /* A vecN whose lanes all read one def is a swizzle; distinct defs need a
    * channel-merge the SSA IR does not model until register allocation. */
   if (alu->op == nir_op_vec2 || alu->op == nir_op_vec3 ||
       alu->op == nir_op_vec4) {
      const unsigned num = nir_op_infos[alu->op].num_inputs;
      const nir_def *def0 = alu->src[0].src.ssa;
      for (unsigned s = 1; s < num; s++)
         if (alu->src[s].src.ssa != def0)
            return reject(ctx, "vec%u composes distinct defs", num);
      const struct r300_classic_src *base = lookup_def(ctx, def0);
      if (!base)
         return reject(ctx, "vec%u operand has no selected value", num);
      uint8_t select[4];
      for (unsigned c = 0; c < 4; c++)
         select[c] = alu->src[c < num ? c : num - 1].swizzle[0];
      struct r300_classic_src src = *base;
      src.swizzle = compose_swizzle(base->swizzle, select, 4);
      map_def(ctx, &alu->def, src);
      return true;
   }

   enum r300_classic_op op;
   if (!alu_op_map(alu->op, &op))
      return reject(ctx, "nir op '%s' outside the classic subset",
                    nir_op_infos[alu->op].name);

   struct r300_classic_instr *i = r300_classic_instr_append(ctx->prog, op);
   if (!i)
      return reject(ctx, "out of memory");
   i->writemask = BITFIELD_MASK(alu->def.num_components);
   for (unsigned s = 0; s < i->num_srcs; s++)
      if (!get_alu_src(ctx, alu, s, &i->src[s]))
         return false;

   map_def(ctx, &alu->def, (struct r300_classic_src){
      .file = R300C_FILE_SSA,
      .def = i,
      .swizzle = RC_SWIZZLE_XYZW,
   });
   return true;
}

static bool
select_tex(struct sel_ctx *ctx, nir_tex_instr *tex)
{
   if (tex->op != nir_texop_tex)
      return reject(ctx, "texop %d outside the classic subset",
                    (int)tex->op);

   int coord_idx = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   if (coord_idx < 0)
      return reject(ctx, "tex without a coordinate source");
   for (unsigned s = 0; s < tex->num_srcs; s++) {
      switch (tex->src[s].src_type) {
      case nir_tex_src_coord:
      case nir_tex_src_texture_deref:
      case nir_tex_src_sampler_deref:
         break;
      default:
         return reject(ctx, "tex source kind %d outside the classic subset",
                       tex->src[s].src_type);
      }
   }

   struct r300_classic_src coord;
   if (!get_plain_src(ctx, tex->src[coord_idx].src.ssa, "tex coordinate",
                      &coord))
      return false;

   struct r300_classic_instr *i =
      r300_classic_instr_append(ctx->prog, R300C_OP_TEX);
   if (!i)
      return reject(ctx, "out of memory");
   i->writemask = BITFIELD_MASK(tex->def.num_components);
   i->tex_unit = tex->texture_index;
   i->src[0] = coord;

   map_def(ctx, &tex->def, (struct r300_classic_src){
      .file = R300C_FILE_SSA,
      .def = i,
      .swizzle = RC_SWIZZLE_XYZW,
   });
   return true;
}

static int
io_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

bool
r300_classic_select(void *mem_ctx, nir_shader *nir,
                    const struct r300_classic_target *target,
                    unsigned num_driver_consts,
                    struct r300_classic_select_result *result)
{
   memset(result, 0, sizeof(*result));
   result->immediates.first_index = num_driver_consts;

   if (nir->info.stage != MESA_SHADER_FRAGMENT) {
      result->reject_reason =
         ralloc_strdup(mem_ctx, "classic selection is fragment-only");
      return true;
   }

   /* The entry lowering the selector owns: samplers to indices and IO
    * variables to load_input/store_output, the same shapes nir_to_rc
    * consumes them in. */
   NIR_PASS(_, nir, nir_lower_samplers);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            io_type_size, (nir_lower_io_options)0);
   /* The production optimizer splits fmad into fmul+fadd; nir_to_rc refuses
    * them in its late-algebraic stage (nir_opt_algebraic_late under
    * nir_float_muladd_support_fuse).  Selection consumes the same re-fused
    * shape rather than duplicating a MAD-fusion pass of its own. */
   NIR_PASS(_, nir, nir_opt_algebraic_late);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   if (exec_list_length(&impl->body) != 1) {
      result->reject_reason = ralloc_strdup(
         mem_ctx, "control flow: the classic US is straight-line only");
      return true;
   }

   struct sel_ctx ctx = {
      .mem_ctx = mem_ctx,
      .result = result,
      .prog = r300_classic_program_create(mem_ctx, target),
      .value_map = _mesa_pointer_hash_table_create(mem_ctx),
   };
   if (!ctx.prog || !ctx.value_map)
      return false;

   nir_block *block = nir_start_block(impl);
   nir_foreach_instr (instr, block) {
      bool ok;
      switch (instr->type) {
      case nir_instr_type_load_const:
         ok = select_load_const(&ctx, nir_instr_as_load_const(instr));
         break;
      case nir_instr_type_intrinsic:
         ok = select_intrinsic(&ctx, nir_instr_as_intrinsic(instr));
         break;
      case nir_instr_type_alu:
         ok = select_alu(&ctx, nir_instr_as_alu(instr));
         break;
      case nir_instr_type_tex:
         ok = select_tex(&ctx, nir_instr_as_tex(instr));
         break;
      case nir_instr_type_deref:
         /* Dead sampler derefs survive nir_lower_samplers; they select
          * nothing. */
         ok = true;
         break;
      case nir_instr_type_undef:
         ok = reject(&ctx, "undef value reaches selection");
         break;
      default:
         ok = reject(&ctx, "nir instr type %d outside the classic subset",
                     instr->type);
         break;
      }
      if (!ok) {
         result->reject_reason = ctx.reject;
         result->program = NULL;
         return true;
      }
   }

   result->program = ctx.prog;
   return true;
}
