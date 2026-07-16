/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300_classic_select.h"

#include "nir_builder.h"
#include "util/hash_table.h"
#include "util/ralloc.h"

#include "../nir_to_rc.h"
#include "../r300_nir.h"
#include "../radeon_code.h"
#include "../radeon_program_constants.h"
#include "r300_shader_semantics.h"

/* The selector maps each nir_def to a classic source descriptor: inputs,
 * constants, and immediates map to file references with a composed swizzle,
 * ALU and TEX results map to the defining classic instruction, and fneg/fabs
 * fold into the descriptor's modifier bits instead of emitting anything.
 * Everything outside the admitted ALU subset lands in reject() with a named
 * reason -- rejection is a supported result, silent dropping is not. */

struct sel_ctx {
   void *mem_ctx;
   struct r300_classic_program *prog;
   struct r300_classic_select_result *result;
   struct r300_shader_semantics *semantics;
   /* nir_def* -> struct r300_classic_src* (ralloc'd). */
   struct hash_table *value_map;
   const char *reject;
   /* R500 emit honors a per-instruction destination writemask; R300/R400
    * emit_tex has no writemask field and always writes all four channels. */
   bool is_r500;
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

/* Record an input's varying slot at its RC input index, mirroring
 * ntr_read_input_output so AllocateHwInputs routes classic-path inputs the
 * same way it routes nir_to_rc's.  WPOS and FACE record and the shared
 * post-frontend machinery does the rest: the vertex side mirrors
 * gl_Position into the wpos varying (r300_nir_add_wpos) and the rasterizer
 * routes it by inputs.wpos, and rc_transform_fragment_face rewrites face
 * reads on the emitted rc_program regardless of front end.  Pointcoord
 * arrives as VAR8 through ntr_fixup_varying_slots and records as a
 * generic. */
static bool
record_input_semantics(struct sel_ctx *ctx, gl_varying_slot location,
                       unsigned base)
{
   struct r300_shader_semantics *sem = ctx->semantics;
   if (!sem)
      return true;
   if (base >= sem->num_total)
      sem->num_total = base + 1;
   switch (location) {
   case VARYING_SLOT_POS:
      /* The raw read never survives selection: the entry's frag-coord
       * reconstruction (classic_lower_wpos) rebuilds gl_FragCoord with a
       * perspective divide and the viewport scale/offset state
       * constants before the value walk. */
      sem->wpos = base;
      return true;
   case VARYING_SLOT_FACE:
      sem->face = base;
      return true;
   case VARYING_SLOT_COL0:
      sem->color[0] = base;
      return true;
   case VARYING_SLOT_COL1:
      sem->color[1] = base;
      return true;
   case VARYING_SLOT_FOGC:
      sem->fog = base;
      return true;
   default:
      if (location >= VARYING_SLOT_VAR0 && location <= VARYING_SLOT_VAR31) {
         const unsigned index = location - VARYING_SLOT_VAR0;
         if (sem->generic[index] == ATTR_UNUSED)
            sem->num_generic++;
         sem->generic[index] = base;
         return true;
      }
      return reject(ctx, "input varying slot %u outside the classic subset",
                    (unsigned)location);
   }
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
 * composed in.  The swizzle array is indexed by destination channel and
 * every entry up to the op's per-source read width selects a lane of the
 * source def, so the compose bound is nir_ssa_alu_instr_src_components --
 * bounding by the def's own width instead drops a wide op's high selects
 * (a vec3 collect read as .xyyz would compose to .xyyy). */
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
                                  nir_ssa_alu_instr_src_components(alu, s));
   return true;
}

/* Dedup a driver-updated state constant into the selection's table; the
 * ntr_add_state_constant discipline. */
static int
add_state_constant(struct r300_classic_select_result *result,
                   unsigned rc_state, unsigned sampler)
{
   struct r300_classic_state_constants *st = &result->states;
   for (unsigned i = 0; i < st->count; i++)
      if (st->entries[i].rc_state == rc_state &&
          st->entries[i].sampler == sampler)
         return (int)i;
   if (st->count >= R300_CLASSIC_MAX_STATE_CONSTANTS)
      return -1;
   st->entries[st->count].rc_state = rc_state;
   st->entries[st->count].sampler = sampler;
   return (int)st->count++;
}

/* The classic state loader for nir_to_rc's shared texture lowering:
 * dedup into the selection's table and emit the load_uniform marker. */
static nir_def *
classic_load_state_cb(void *ctx, nir_builder *b, unsigned rc_state,
                      unsigned sampler, unsigned num_components)
{
   struct r300_classic_select_result *result = ctx;
   const int idx = add_state_constant(result, rc_state, sampler);
   if (idx < 0)
      return NULL;
   return nir_load_uniform(b, num_components, 32, nir_imm_int(b, 0),
                           .base = (unsigned)idx,
                           .range = num_components,
                           .dest_type = nir_type_float32);
}

/* Rebuild gl_FragCoord from the clip-space wpos varying: the vertex side
 * mirrors gl_Position into the varying (r300_nir_add_wpos), and the
 * fragment read needs the perspective divide and the viewport transform
 * -- rcp w, xyz * rcp_w, then xyz * VIEWPORT_SCALE + VIEWPORT_OFFSET with
 * w = rcp_w, the same math ntr_lower_wpos emits.  State constants ride a
 * private load_uniform marker whose base indexes the selection's state
 * table (the ntr_load_state_constant convention); selection maps it to
 * R300C_FILE_STATE. */
static bool
classic_lower_wpos_instr(nir_builder *b, nir_intrinsic_instr *intr,
                         void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_input)
      return false;
   if (nir_intrinsic_io_semantics(intr).location != VARYING_SLOT_POS)
      return false;

   struct r300_classic_select_result *result = data;
   const int scale_idx =
      add_state_constant(result, RC_STATE_R300_VIEWPORT_SCALE, 0);
   const int offset_idx =
      add_state_constant(result, RC_STATE_R300_VIEWPORT_OFFSET, 0);
   if (scale_idx < 0 || offset_idx < 0)
      return false;

   b->cursor = nir_after_instr(&intr->instr);
   nir_def *raw = nir_load_input(b, 4, 32, nir_imm_int(b, 0),
                                 .base = nir_intrinsic_base(intr),
                                 .component = 0,
                                 .io_semantics =
                                    nir_intrinsic_io_semantics(intr),
                                 .dest_type = nir_type_float32);
   nir_def *rcp_w = nir_frcp(b, nir_channel(b, raw, 3));
   nir_def *xyz =
      nir_fmul(b, nir_channels(b, raw, nir_component_mask(3)), rcp_w);
   nir_def *scale = nir_load_uniform(b, 4, 32, nir_imm_int(b, 0),
                                     .base = (unsigned)scale_idx, .range = 4,
                                     .dest_type = nir_type_float32);
   nir_def *offset = nir_load_uniform(b, 4, 32, nir_imm_int(b, 0),
                                      .base = (unsigned)offset_idx,
                                      .range = 4,
                                      .dest_type = nir_type_float32);
   xyz = nir_fadd(b,
                  nir_fmul(b, xyz,
                           nir_channels(b, scale, nir_component_mask(3))),
                  nir_channels(b, offset, nir_component_mask(3)));
   nir_def *wpos = nir_vec4(b, nir_channel(b, xyz, 0),
                            nir_channel(b, xyz, 1), nir_channel(b, xyz, 2),
                            rcp_w);
   const unsigned component = nir_intrinsic_component(intr);
   nir_def *replacement = nir_channels(
      b, wpos, nir_component_mask(intr->num_components) << component);
   nir_def_rewrite_uses_after(&intr->def, replacement);
   return true;
}

static bool
classic_lower_wpos(nir_shader *nir, struct r300_classic_select_result *result)
{
   return nir_shader_intrinsics_pass(nir, classic_lower_wpos_instr,
                                     nir_metadata_control_flow, result);
}

/* nir_lower_int_to_float float-encodes integer-typed constants (a ubo
 * slot offset 1 becomes the bits of 1.0f); decode with the same
 * heuristic ntr_src_as_uint uses. */
static unsigned
src_as_index(nir_src src)
{
   const uint32_t v = nir_src_as_uint(src);
   return v >= fui(1.0) ? (unsigned)uif(v) : v;
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
      /* first_index + count can exceed the physical constant file here:
       * the UBO prescan counts the block-0 extent whether or not every
       * slot is read, and the shared dead-constants pass compacts unused
       * externals and immediates alike after emission
       * (remove_unused_constants, the same fit path nir_to_rc relies
       * on).  A file that still does not fit fails in the backend with
       * the same result either front end produces. */
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
      const nir_io_semantics in_sem = nir_intrinsic_io_semantics(intr);
      if (!record_input_semantics(ctx, in_sem.location,
                                  nir_intrinsic_base(intr)))
         return false;
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
   case nir_intrinsic_load_ubo_vec4: {
      /* The r300 exposes a single UBO; indirect array indexing is lowered
       * before selection.  A block-0 constant-offset read is a plain
       * constant-file vec4 reference at base + offset, the same index
       * ntr_emit_load_ubo computes. */
      if (!nir_src_is_const(intr->src[0]) || src_as_index(intr->src[0]))
         return reject(ctx, "load_ubo_vec4 from a block other than 0");
      if (!nir_src_is_const(intr->src[1]))
         return reject(ctx, "indirect constant addressing");
      const unsigned index =
         nir_intrinsic_base(intr) + src_as_index(intr->src[1]);
      if (index >= ctx->result->immediates.first_index)
         return reject(ctx, "constant read past the prescanned file");
      const unsigned ubo_component = nir_intrinsic_component(intr);
      uint8_t ubo_select[4];
      for (unsigned c = 0; c < 4; c++)
         ubo_select[c] = MIN2(ubo_component + c, 3);
      map_def(ctx, &intr->def, (struct r300_classic_src){
         .file = R300C_FILE_CONST,
         .index = index,
         .swizzle = compose_swizzle(RC_SWIZZLE_XYZW, ubo_select, 4),
      });
      return true;
   }
   case nir_intrinsic_store_output: {
      if (!nir_src_is_const(intr->src[1]) || nir_src_as_uint(intr->src[1]))
         return reject(ctx, "indirect output addressing");
      const nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      enum r300_classic_op op;
      unsigned export_index = 0;
      const int color = mesa_frag_result_get_color_index(sem.location);
      if (sem.location == FRAG_RESULT_COLOR || (color >= 0 && color < 4)) {
         op = R300C_OP_EXPORT_COLOR;
         export_index = color > 0 ? (unsigned)color : 0;
      } else if (sem.location == FRAG_RESULT_DEPTH) {
         op = R300C_OP_EXPORT_DEPTH;
      } else {
         return reject(ctx, "store_output to location %u", sem.location);
      }
      if (sem.dual_source_blend_index)
         return reject(ctx, "dual-source blend index");

      struct r300_classic_src src;
      if (!get_plain_src(ctx, intr->src[0].ssa, "store_output value", &src))
         return false;
      struct r300_classic_instr *e = r300_classic_instr_append(ctx->prog, op);
      if (!e)
         return reject(ctx, "out of memory");
      e->export_index = export_index;
      e->src[0] = src;
      /* R300C_OP_EXPORT_COLOR: record which destination channels this store
       * actually covers so emission can mask the export MOV instead of
       * always writing XYZW (radeon_pair_translate.c's RGB.OutputWriteMask /
       * Alpha.OutputWriteMask honor an arbitrary per-channel mask on an
       * RC_FILE_OUTPUT dst exactly like a temp write); two masked stores to
       * the same color attachment then accumulate into the shared output
       * register the way ntr_emit_store_output's masked MOV does in
       * nir_to_rc.c.  component is the destination's base channel (nonzero
       * only for a component-packed scalar/vec2 output sharing a slot);
       * write_mask bits are relative to that base.  R300C_OP_EXPORT_DEPTH
       * stays a true sink (writemask 0): emission always targets the output
       * register's .w lane regardless of which source component held the
       * depth value, so there is no destination mask to record. */
      if (op == R300C_OP_EXPORT_COLOR) {
         const unsigned component = nir_intrinsic_component(intr);
         e->writemask = (uint8_t)(nir_intrinsic_write_mask(intr) << component);
      }
      return true;
   }
   case nir_intrinsic_load_uniform: {
      /* The private state-constant marker from classic_lower_wpos: base
       * indexes the selection's state table. */
      const unsigned sidx = nir_intrinsic_base(intr);
      if (sidx >= ctx->result->states.count)
         return reject(ctx, "state-constant marker out of range");
      map_def(ctx, &intr->def, (struct r300_classic_src){
         .file = R300C_FILE_STATE,
         .index = sidx,
         .swizzle = RC_SWIZZLE_XYZW,
      });
      return true;
   }
   case nir_intrinsic_terminate: {
      struct r300_classic_instr *k =
         r300_classic_instr_append(ctx->prog, R300C_OP_KILP);
      if (!k)
         return reject(ctx, "out of memory");
      return true;
   }
   case nir_intrinsic_terminate_if: {
      /* Bools reach selection lowered to 0.0/1.0; KIL discards where its
       * source is negative, so the negated channel-0 condition kills exactly
       * the true lanes (the ntr_emit_intrinsic_kill mapping). */
      struct r300_classic_src cond;
      if (!get_plain_src(ctx, intr->src[0].ssa, "terminate_if condition",
                         &cond))
         return false;
      const unsigned lane = GET_SWZ(cond.swizzle, 0);
      cond.swizzle = RC_MAKE_SWIZZLE(lane, lane, lane, lane);
      cond.negate = !cond.negate;
      struct r300_classic_instr *k =
         r300_classic_instr_append(ctx->prog, R300C_OP_KIL);
      if (!k)
         return reject(ctx, "out of memory");
      k->src[0] = cond;
      return true;
   }
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_coarse: {
      const bool is_ddx = intr->intrinsic == nir_intrinsic_ddx ||
                          intr->intrinsic == nir_intrinsic_ddx_coarse;
      struct r300_classic_src src;
      if (!get_plain_src(ctx, intr->src[0].ssa, "derivative operand", &src))
         return false;
      struct r300_classic_instr *d = r300_classic_instr_append(
         ctx->prog, is_ddx ? R300C_OP_DDX : R300C_OP_DDY);
      if (!d)
         return reject(ctx, "out of memory");
      d->writemask = (uint8_t)BITFIELD_MASK(intr->def.num_components);
      d->src[0] = src;
      map_def(ctx, &intr->def, (struct r300_classic_src){
         .file = R300C_FILE_SSA,
         .def = d,
         .swizzle = RC_SWIZZLE_XYZW,
      });
      return true;
   }
   default:
      return reject(ctx, "intrinsic '%s' outside the classic subset",
                    nir_intrinsic_infos[intr->intrinsic].name);
   }
}

/* One row per admitted ALU op; fneg/fabs and identity vecN fold into source
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
   case nir_op_fdot2_replicated: *out = R300C_OP_DP2; return true;
   case nir_op_fdot3:
   case nir_op_fdot3_replicated: *out = R300C_OP_DP3; return true;
   case nir_op_fdot4:
   case nir_op_fdot4_replicated: *out = R300C_OP_DP4; return true;
   case nir_op_fmin:   *out = R300C_OP_MIN; return true;
   case nir_op_fmax:   *out = R300C_OP_MAX; return true;
   case nir_op_ffract: *out = R300C_OP_FRC; return true;
   case nir_op_fround_even: *out = R300C_OP_ROUND; return true;
   case nir_op_frcp:   *out = R300C_OP_RCP; return true;
   case nir_op_frsq:   *out = R300C_OP_RSQ; return true;
   case nir_op_fexp2:  *out = R300C_OP_EX2; return true;
   case nir_op_flog2:  *out = R300C_OP_LG2; return true;
   case nir_op_fsin:   *out = R300C_OP_SIN; return true;
   case nir_op_fcos:   *out = R300C_OP_COS; return true;
   case nir_op_fpow:   *out = R300C_OP_POW; return true;
   /* No slt/sge/seq/sne rows: the R300 fragment US has no comparison
    * opcodes (radeonTransformALU asserts on them); the entry lowering
    * rewrites every set-compare into a CMP-carried fcsel_ge shape. */
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

   /* A vecN whose lanes all read one def is a swizzle; distinct defs become
    * a VEC collect whose source s carries destination channel s through its
    * own channel-s select. */
   if (alu->op == nir_op_vec2 || alu->op == nir_op_vec3 ||
       alu->op == nir_op_vec4) {
      const unsigned num = nir_op_infos[alu->op].num_inputs;
      const nir_def *def0 = alu->src[0].src.ssa;
      bool same_def = true;
      for (unsigned s = 1; s < num; s++)
         if (alu->src[s].src.ssa != def0)
            same_def = false;
      if (same_def) {
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

      struct r300_classic_instr *i =
         r300_classic_instr_append(ctx->prog, R300C_OP_VEC);
      if (!i)
         return reject(ctx, "out of memory");
      i->num_srcs = num;
      i->writemask = (uint8_t)BITFIELD_MASK(num);
      for (unsigned s = 0; s < num; s++) {
         const struct r300_classic_src *base =
            lookup_def(ctx, alu->src[s].src.ssa);
         if (!base)
            return reject(ctx, "vec%u operand has no selected value", num);
         /* Replicate the lane across all channels so channel s (and any
          * grouping neighbor) selects the same value. */
         const uint8_t select[4] = {
            alu->src[s].swizzle[0], alu->src[s].swizzle[0],
            alu->src[s].swizzle[0], alu->src[s].swizzle[0],
         };
         i->src[s] = *base;
         i->src[s].swizzle = compose_swizzle(base->swizzle, select, 4);
      }
      map_def(ctx, &alu->def, (struct r300_classic_src){
         .file = R300C_FILE_SSA,
         .def = i,
         .swizzle = RC_SWIZZLE_XYZW,
      });
      return true;
   }

   /* CMP chooses src1 where src0 < 0, so the fcsel family is a source-fold:
    * fcsel tests src0 != 0 through -|src0|, fcsel_gt negates, and fcsel_ge
    * swaps the chosen operands (the ntr_emit_alu_special mappings). */
   if (alu->op == nir_op_fcsel || alu->op == nir_op_fcsel_gt ||
       alu->op == nir_op_fcsel_ge) {
      struct r300_classic_instr *i =
         r300_classic_instr_append(ctx->prog, R300C_OP_CMP);
      if (!i)
         return reject(ctx, "out of memory");
      i->writemask = (uint8_t)BITFIELD_MASK(alu->def.num_components);
      if (!get_alu_src(ctx, alu, 0, &i->src[0]))
         return false;
      const unsigned then_src = alu->op == nir_op_fcsel_ge ? 2 : 1;
      const unsigned else_src = alu->op == nir_op_fcsel_ge ? 1 : 2;
      if (!get_alu_src(ctx, alu, then_src, &i->src[1]) ||
          !get_alu_src(ctx, alu, else_src, &i->src[2]))
         return false;
      if (alu->op == nir_op_fcsel) {
         i->src[0].abs = true;
         i->src[0].negate = true;
      } else if (alu->op == nir_op_fcsel_gt) {
         i->src[0].negate = !i->src[0].negate;
      }
      map_def(ctx, &alu->def, (struct r300_classic_src){
         .file = R300C_FILE_SSA,
         .def = i,
         .swizzle = RC_SWIZZLE_XYZW,
      });
      return true;
   }

   /* FRC is exactly src - floor(src), so floor(x) = x - FRC(x) -- the same
    * two-instruction expansion ntr_emit_alu_special uses because RC has no
    * FLR opcode for an FP destination. */
   if (alu->op == nir_op_ffloor) {
      struct r300_classic_src src;
      if (!get_alu_src(ctx, alu, 0, &src))
         return false;
      struct r300_classic_instr *frc =
         r300_classic_instr_append(ctx->prog, R300C_OP_FRC);
      if (!frc)
         return reject(ctx, "out of memory");
      frc->writemask = (uint8_t)BITFIELD_MASK(alu->def.num_components);
      frc->src[0] = src;
      struct r300_classic_instr *sub =
         r300_classic_instr_append(ctx->prog, R300C_OP_ADD);
      if (!sub)
         return reject(ctx, "out of memory");
      sub->writemask = frc->writemask;
      sub->src[0] = src;
      sub->src[1] = (struct r300_classic_src){
         .file = R300C_FILE_SSA,
         .def = frc,
         .swizzle = RC_SWIZZLE_XYZW,
         .negate = true,
      };
      map_def(ctx, &alu->def, (struct r300_classic_src){
         .file = R300C_FILE_SSA,
         .def = sub,
         .swizzle = RC_SWIZZLE_XYZW,
      });
      return true;
   }

   /* fsat is a saturating MOV: RC carries the [0,1] clamp on the dst.
    * fsub is ADD with the second operand's sign folded. */
   const bool saturate = alu->op == nir_op_fsat;
   enum r300_classic_op op;
   if (saturate)
      op = R300C_OP_MOV;
   else if (alu->op == nir_op_fsub)
      op = R300C_OP_ADD;
   else if (!alu_op_map(alu->op, &op))
      return reject(ctx, "nir op '%s' outside the classic subset",
                    nir_op_infos[alu->op].name);

   /* The RC scalar ops replicate one source lane, so a vector-width
    * transcendental expands to one scalar instruction per destination
    * channel plus a collect -- the shape ntr_emit_scalar produces. */
   const bool is_scalar_op =
      op == R300C_OP_RCP || op == R300C_OP_RSQ || op == R300C_OP_EX2 ||
      op == R300C_OP_LG2 || op == R300C_OP_SIN || op == R300C_OP_COS ||
      op == R300C_OP_POW;
   if (is_scalar_op && alu->def.num_components > 1) {
      struct r300_classic_instr *chan[4] = {0};
      for (unsigned c = 0; c < alu->def.num_components; c++) {
         struct r300_classic_instr *s =
            r300_classic_instr_append(ctx->prog, op);
         if (!s)
            return reject(ctx, "out of memory");
         s->writemask = 0x1;
         for (unsigned si = 0; si < s->num_srcs; si++) {
            struct r300_classic_src src;
            if (!get_alu_src(ctx, alu, si, &src))
               return false;
            const unsigned lane = GET_SWZ(src.swizzle, c);
            src.swizzle = RC_MAKE_SWIZZLE(lane, lane, lane, lane);
            s->src[si] = src;
         }
         chan[c] = s;
      }
      struct r300_classic_instr *vec =
         r300_classic_instr_append(ctx->prog, R300C_OP_VEC);
      if (!vec)
         return reject(ctx, "out of memory");
      vec->num_srcs = alu->def.num_components;
      vec->writemask = (uint8_t)BITFIELD_MASK(alu->def.num_components);
      for (unsigned c = 0; c < alu->def.num_components; c++) {
         vec->src[c] = (struct r300_classic_src){
            .file = R300C_FILE_SSA,
            .def = chan[c],
            .swizzle = RC_SWIZZLE_XXXX,
         };
      }
      map_def(ctx, &alu->def, (struct r300_classic_src){
         .file = R300C_FILE_SSA,
         .def = vec,
         .swizzle = RC_SWIZZLE_XYZW,
      });
      return true;
   }

   struct r300_classic_instr *i = r300_classic_instr_append(ctx->prog, op);
   if (!i)
      return reject(ctx, "out of memory");
   i->saturate = saturate;
   i->writemask = (uint8_t)BITFIELD_MASK(alu->def.num_components);
   for (unsigned s = 0; s < i->num_srcs; s++)
      if (!get_alu_src(ctx, alu, s, &i->src[s]))
         return false;
   if (alu->op == nir_op_fsub)
      i->src[1].negate = !i->src[1].negate;

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
   /* txl and txd are r500-only at the TEX unit (r500_fragprog_emit.c);
    * the R300 encodings are LD, TXB, and TXP. */
   if (tex->op != nir_texop_tex && tex->op != nir_texop_txb)
      return reject(ctx, "texop %d outside the classic subset",
                    (int)tex->op);
   if (tex->is_shadow)
      return reject(ctx, "shadow comparison lowering lives in nir_to_rc");
   if (tex->is_array)
      return reject(ctx, "r300 has no array textures");

   /* The TX block samples 1D/2D/3D/CUBE/RECT targets; the same mapping
    * rc_texture_target_from_sampler_dim applies for nir_to_rc. */
   unsigned target;
   switch (tex->sampler_dim) {
   case GLSL_SAMPLER_DIM_1D:       target = RC_TEXTURE_1D; break;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_EXTERNAL: target = RC_TEXTURE_2D; break;
   case GLSL_SAMPLER_DIM_3D:       target = RC_TEXTURE_3D; break;
   case GLSL_SAMPLER_DIM_CUBE:     target = RC_TEXTURE_CUBE; break;
   case GLSL_SAMPLER_DIM_RECT:
      /* The entry's backend-tex lowering normalizes RECT coordinates by
       * the texrect factor and retargets to 2D on non-r500, so a RECT
       * sample reaching selection is the native r500 path. */
      target = RC_TEXTURE_RECT;
      break;
   default:
      return reject(ctx, "sampler dim %d outside the classic subset",
                    (int)tex->sampler_dim);
   }

   /* The entry lowering packed coordinate/bias/projector into backend1
    * (nir_to_rc_lower_tex); a projector that TXP cannot carry was divided
    * away before packing (nir_to_rc_lower_txp). */
   int coord_idx = nir_tex_instr_src_index(tex, nir_tex_src_backend1);
   if (coord_idx < 0)
      return reject(ctx, "tex without a packed backend source");
   for (unsigned s = 0; s < tex->num_srcs; s++) {
      switch (tex->src[s].src_type) {
      case nir_tex_src_backend1:
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

   /* A plain sample whose packed source is wider than the coordinate
    * carries a projector in the extra lane: the ntr_emit_texture width
    * rule that selects RC_OPCODE_TXP. */
   enum r300_classic_op op = R300C_OP_TEX;
   if (tex->op == nir_texop_txb) {
      op = R300C_OP_TXB;
   } else if ((unsigned)nir_tex_instr_src_size(tex, coord_idx) >
              MAX2(tex->coord_components, 2)) {
      op = R300C_OP_TXP;
   }

   struct r300_classic_instr *i = r300_classic_instr_append(ctx->prog, op);
   if (!i)
      return reject(ctx, "out of memory");
   /* R300/R400 emit_tex has no writemask field: the hardware TEX writes all
    * four channels of its destination register.  A narrowed declared mask
    * (from texture(s,uv).r and the nir_opt_shrink_vectors that produces it)
    * lets the register packer place an independent live value in a channel
    * the hardware write then clobbers, because the shared dataflow reads
    * this declared mask verbatim (writes_normal) and can only narrow, never
    * widen it.  Declare the full XYZW liveness on non-r500 so the packer
    * reserves the whole register; consumers still select their channel by
    * swizzle.  R500 emit honors DstReg.WriteMask, so keep the narrow mask
    * there -- the same split nir_to_rc's needs_mov workaround makes. */
   i->writemask = ctx->is_r500
                     ? (uint8_t)BITFIELD_MASK(tex->def.num_components)
                     : (uint8_t)RC_MASK_XYZW;
   i->tex_unit = tex->texture_index;
   i->tex_target = target;
   i->src[0] = coord;

   map_def(ctx, &tex->def, (struct r300_classic_src){
      .file = R300C_FILE_SSA,
      .def = i,
      .swizzle = RC_SWIZZLE_XYZW,
   });
   return true;
}

static unsigned
io_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

bool
r300_classic_select(void *mem_ctx, nir_shader *nir,
                    const struct r300_classic_target *target,
                    const struct r300_fragment_program_external_state *ext,
                    unsigned num_driver_consts,
                    enum r300_fs_input_semantics input_semantics,
                    struct r300_shader_semantics *semantics,
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
   /* The r300 varying-slot convention packs texcoords at generic 0-7,
    * pointcoord at 8, and user varyings from 9 (ntr_fixup_varying_slots);
    * the SW-TCL vertex path applies the same remap to its outputs
    * (r300_vs_draw.c), so input semantics recorded without it index the
    * rasterizer block nine slots low and the varying never routes. */
   ntr_fixup_varying_slots(nir, nir_var_shader_in);
   NIR_PASS(_, nir, nir_lower_samplers);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            io_type_size, (nir_lower_io_options)0);
   /* Divide away projectors TXP cannot carry, then pack coordinate, bias,
    * and the surviving projector into the backend tex source -- the same
    * two passes nir_to_rc runs so selection consumes identical tex
    * shapes. */
   NIR_PASS(_, nir, classic_lower_wpos, result);
   /* The sampler-state lowerings, in nir_to_rc's order: shadow comparison
    * first, then the coordinate rewrites (RECT normalization, NPOT wrap
    * emulation, 3D clamp-and-scale) drawing factors through the classic
    * state table, before the projector and packing passes consume the
    * final coordinates. */
   if (ext && ext->sampler_state_count > 0) {
      const unsigned n = (unsigned)ext->sampler_state_count;
      nir_lower_tex_shadow_swizzle tex_swizzle[PIPE_MAX_SHADER_SAMPLER_VIEWS];
      enum compare_func tex_compare_func[PIPE_MAX_SHADER_SAMPLER_VIEWS];
      for (unsigned i = 0; i < n; i++) {
         tex_compare_func[i] = ext->unit[i].texture_compare_func;
         tex_swizzle[i].swizzle_r = GET_SWZ(ext->unit[i].texture_swizzle, 0);
         tex_swizzle[i].swizzle_g = GET_SWZ(ext->unit[i].texture_swizzle, 1);
         tex_swizzle[i].swizzle_b = GET_SWZ(ext->unit[i].texture_swizzle, 2);
         tex_swizzle[i].swizzle_a = GET_SWZ(ext->unit[i].texture_swizzle, 3);
      }
      NIR_PASS(_, nir, nir_lower_tex_shadow, n, tex_compare_func, tex_swizzle,
               true);
   }
   {
      static const struct r300_fragment_program_external_state plain_state;
      const bool is_r500 =
         target->pfs_class == R300_CLASSIC_PFS_R500;
      NIR_PASS(_, nir, nir_to_rc_lower_backend_tex,
               ext ? ext : &plain_state, is_r500, classic_load_state_cb,
               result);
   }
   nir_to_rc_lower_txp(nir);
   NIR_PASS(_, nir, nir_to_rc_lower_tex);
   if (ext && ext->alpha_to_one)
      NIR_PASS(_, nir, r300_nir_lower_alpha_to_one);
   /* nir_lower_tex_shadow builds its ALWAYS/NEVER compare result from
    * nir_imm_int(~0)/nir_imm_int(0) fed through b2f32, so the b2f32 still
    * carries a raw 32-bit int source at this point.  nir_lower_int_to_float
    * converts that source by its int type, not by the boolean test the
    * shadow lowering intended, and turns the ALWAYS case into -1.0 instead
    * of 1.0 (texdepth EXT_shadow_func: GL_ALWAYS, depth-tex-compare).
    * nir_opt_constant_folding evaluates b2f32 by nir_op_b2f32's own
    * semantics (nonzero test) while the source is still typed int, the same
    * fixed-point loop nir_to_rc runs at its entry, so running it here before
    * the int/bool lowering below folds the shadow compare to the correct
    * float constant first. */
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
   } while (progress);
   /* Integer ops survive the production optimizer (dynamic-index select
    * ladders compare integer indices; GL uniforms arrive typed), and
    * nir_lower_bool_to_float treats operands as floats, so the integer
    * lowering must land first: bool lowering over raw integer bits turns
    * an index compare into a float compare of denormals and the ladder
    * constants fold to zero.  This is nir_to_rc's entry block in its
    * exact order. */
   bool int_unsupported = false;
   NIR_PASS(_, nir, r300_nir_lower_bitwise_to_arith, &int_unsupported);
   if (int_unsupported) {
      result->reject_reason = ralloc_strdup(
         mem_ctx, "integer bitwise op without an FP24-exact lowering");
      return true;
   }
   /* Apply the fragment stage's float-to-int conversion contract before
    * nir_lower_int_to_float lowers f2i32/f2u32 (to ftrunc/ffloor): an
    * interpolated fragment shader gets the smooth-varying epsilon correction, a
    * flat R2VB producer omits it.  This is the same helper nir_to_rc uses, so
    * both r300 fragment frontends honor one input-semantics contract. */
   NIR_PASS(_, nir, r300_nir_apply_fs_input_semantics, input_semantics);
   NIR_PASS(_, nir, nir_lower_int_to_float);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, r300_nir_post_integer_lowering);
   /* r300_nir_lower_bool_to_float_fs is pattern-based, so booleans can
    * survive the production optimizer (a bare flt feeding terminate_if);
    * the full lowering turns them into slt/sge/seq/sne float compares.
    * The R300 fragment US has no set-compare opcodes (radeonTransformALU
    * asserts on them; CMP and CND conditional selects are the hardware's
    * only comparison primitives), so the comparison lowering rewrites the
    * compares into the fcsel_ge shapes CMP carries -- the same two-pass
    * backstop nir_to_rc runs at its entry. */
   NIR_PASS(_, nir, nir_lower_bool_to_float, true);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, r300_nir_lower_comparison_fs);
   NIR_PASS(_, nir, nir_opt_cse);
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
      .semantics = semantics,
      .prog = r300_classic_program_create(mem_ctx, target),
      .value_map = _mesa_pointer_hash_table_create(mem_ctx),
      .is_r500 = target->pfs_class == R300_CLASSIC_PFS_R500,
   };
   if (!ctx.prog || !ctx.value_map)
      return false;

   /* Immediates append to Program.Constants after the external (UBO)
    * constants, so their indices start past the highest block-0 vec4 the
    * shader reads -- the same layout ntr_add_constants produces. */
   nir_block *block = nir_start_block(impl);
   nir_foreach_instr (instr, block) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
          !nir_src_is_const(intr->src[1]))
         continue;
      const unsigned extent = nir_intrinsic_base(intr) +
                              src_as_index(intr->src[1]) + 1;
      if (extent > result->immediates.first_index)
         result->immediates.first_index = extent;
   }

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
