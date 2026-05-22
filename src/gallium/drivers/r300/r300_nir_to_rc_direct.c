/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Direct NIR->RC translation for r300, bypassing the ureg/TGSI serialization
 * round-trip.  r300_nir_lower_for_rc() must run on the NIR shader before
 * calling this function; it handles only the final RC instruction emission.
 *
 * The two-pass TGSI route was:
 *   nir_to_rc() -> ureg_get_tokens() -> malloc'd token array
 *              -> tgsi_scan_shader() (scan 1)
 *              -> r300_tgsi_to_rc()  (scan 2)
 *              -> FREE(tokens)
 *
 * This file collapses the two scans into one NIR walk, emitting RC instructions
 * directly via rc_insert_new_instruction().
 *
 * TODO: verify output on RS482/RS485 (1002:5974) hardware before removing the
 * tgsi_scan_shader + r300_tgsi_to_rc path from r300_translate_fragment_shader.
 */

#include "r300_nir_to_rc_direct.h"

#include <assert.h>
#include <stdio.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_legacy.h"
#include "compiler/radeon_compiler.h"
#include "compiler/radeon_program_constants.h"
#include "compiler/radeon_program.h"
#include "compiler/radeon_opcodes.h"
#include "compiler/radeon_code.h"
#include "pipe/p_screen.h"
#include "tgsi/tgsi_from_mesa.h"
#include "tgsi/tgsi_scan.h"
#include "util/bitscan.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "r300_screen.h"
#include "r300_fs.h"

/* -------------------------------------------------------------------------
 * Compile state
 * ------------------------------------------------------------------------- */

struct nrc_compile {
   const nir_shader *s;
   struct radeon_compiler *compiler;
   bool is_r500;
   bool lower_fabs;   /* VS on pre-R5xx: lower fabs to MAX(x, -x) */

   /* SSA def index -> RC temp register index; UINT32_MAX = not yet allocated */
   uint32_t *ssa_temp;  /* [impl->ssa_alloc] */

   /* decl_reg def index -> RC temp register index */
   uint32_t *reg_temp;  /* [nir_shader_get_entrypoint()->reg_alloc] */

   uint32_t num_temps;

   /* First UBO binding index (for indirect UBO addressing offset) */
   int first_ubo;

   bool error;
};

/* -------------------------------------------------------------------------
 * Temp register allocation
 * ------------------------------------------------------------------------- */

static uint32_t
nrc_alloc_temp(struct nrc_compile *c)
{
   return c->num_temps++;
}

static uint32_t
nrc_temp_for_ssa(struct nrc_compile *c, nir_def *def)
{
   if (c->ssa_temp[def->index] == UINT32_MAX)
      c->ssa_temp[def->index] = nrc_alloc_temp(c);
   return c->ssa_temp[def->index];
}

/* -------------------------------------------------------------------------
 * RC swizzle helpers
 * ------------------------------------------------------------------------- */

/* Map NIR channel index (0-3 = XYZW) to rc_swizzle (identical values). */
static rc_swizzle
nrc_swizzle_chan(unsigned chan)
{
   assert(chan < 4);
   return (rc_swizzle)chan;
}

/* Build 12-bit RC swizzle from a 4-element channel array (nir_alu_src style). */
static uint32_t
nrc_make_swizzle(const uint8_t swiz[4])
{
   return RC_MAKE_SWIZZLE(nrc_swizzle_chan(swiz[0]),
                          nrc_swizzle_chan(swiz[1]),
                          nrc_swizzle_chan(swiz[2]),
                          nrc_swizzle_chan(swiz[3]));
}

/* Build a scalar swizzle that replicates one channel to all four. */
static uint32_t
nrc_scalar_swizzle(uint8_t chan)
{
   rc_swizzle c = nrc_swizzle_chan(chan);
   return RC_MAKE_SWIZZLE(c, c, c, c);
}

/* Return the RC input slot index (== driver_location) for the given varying
 * slot, or -1 if not found.  Variables remain in the shader after nir_lower_io
 * runs, so this is safe to call from the emission phase. */
static int
nrc_find_input_slot(const nir_shader *s, gl_varying_slot slot)
{
   nir_foreach_shader_in_variable(var, s) {
      if ((gl_varying_slot)var->data.location == slot)
         return (int)var->data.driver_location;
   }
   return -1;
}

/* -------------------------------------------------------------------------
 * Instruction emission helpers
 * ------------------------------------------------------------------------- */

static struct rc_instruction *
nrc_emit(struct nrc_compile *c, rc_opcode opcode)
{
   struct rc_instruction *inst =
      rc_insert_new_instruction(c->compiler,
                                c->compiler->Program.Instructions.Prev);
   memset(&inst->U.I, 0, sizeof(inst->U.I));
   inst->U.I.Opcode = opcode;
   /* Default both-unused swizzle on all src slots */
   for (int i = 0; i < 3; i++) {
      inst->U.I.SrcReg[i].File   = RC_FILE_NONE;
      inst->U.I.SrcReg[i].Swizzle = RC_SWIZZLE_XYZW;
   }
   inst->U.I.DstReg.WriteMask = RC_MASK_XYZW;
   return inst;
}

/* -------------------------------------------------------------------------
 * Source register construction
 * ------------------------------------------------------------------------- */

/* Add a nir_load_const to the RC constant table and return the slot index. */
static unsigned
nrc_add_const(struct nrc_compile *c, nir_load_const_instr *lc)
{
   float vals[4] = {0.0f, 0.0f, 0.0f, 1.0f};
   for (int i = 0; i < MIN2((int)lc->def.num_components, 4); i++)
      vals[i] = uif(lc->value[i].u32);
   return rc_constants_add_immediate_vec4(&c->compiler->Program.Constants, vals);
}

/* Build an RC src register for a nir_legacy_src. */
static struct rc_src_register
nrc_src_from_legacy(struct nrc_compile *c, const nir_legacy_src *ls)
{
   struct rc_src_register src = {0};
   src.Swizzle = RC_SWIZZLE_XYZW;

   if (ls->is_ssa) {
      if (nir_def_is_const(ls->ssa)) {
         nir_load_const_instr *lc = nir_def_as_load_const(ls->ssa);
         src.File  = RC_FILE_CONSTANT;
         src.Index = nrc_add_const(c, lc);
      } else {
         src.File  = RC_FILE_TEMPORARY;
         src.Index = nrc_temp_for_ssa(c, ls->ssa);
      }
   } else {
      src.File  = RC_FILE_TEMPORARY;
      src.Index = c->reg_temp[ls->reg.handle->index] + ls->reg.base_offset;
   }

   return src;
}

/* Build an RC src from a nir_alu_instr src slot (with legacy modifier chase). */
static struct rc_src_register
nrc_alu_src(struct nrc_compile *c, nir_alu_instr *instr, int idx)
{
   nir_legacy_alu_src ls = nir_legacy_chase_alu_src(&instr->src[idx], !c->lower_fabs);
   struct rc_src_register src = nrc_src_from_legacy(c, &ls.src);

   src.Swizzle = nrc_make_swizzle(ls.swizzle);
   src.Abs     = ls.fabs ? 1 : 0;
   src.Negate  = ls.fneg ? RC_MASK_XYZW : 0;
   return src;
}

/* Build an RC src from a plain nir_src (no modifier folding). */
static struct rc_src_register
nrc_src(struct nrc_compile *c, nir_src src_in)
{
   nir_legacy_src ls = nir_legacy_chase_src(&src_in);
   return nrc_src_from_legacy(c, &ls);
}

/* -------------------------------------------------------------------------
 * Destination register construction
 * ------------------------------------------------------------------------- */

/* Build an RC dst register from a nir_alu_instr def (with legacy sat chase). */
static struct rc_dst_register
nrc_alu_dst(struct nrc_compile *c, nir_alu_instr *instr, bool *saturate_out)
{
   nir_legacy_alu_dest ld = nir_legacy_chase_alu_dest(&instr->def);
   struct rc_dst_register dst = {0};

   if (ld.dest.is_ssa) {
      dst.File      = RC_FILE_TEMPORARY;
      dst.Index     = nrc_temp_for_ssa(c, ld.dest.ssa);
      dst.WriteMask = BITSET_MASK(instr->def.num_components);
   } else {
      dst.File      = RC_FILE_TEMPORARY;
      dst.Index     = c->reg_temp[ld.dest.reg.handle->index]
                      + ld.dest.reg.base_offset;
      dst.WriteMask = ld.write_mask;
   }

   if (saturate_out)
      *saturate_out = ld.fsat;
   return dst;
}

/* Build a plain RC dst from a nir_def (no saturate). */
static struct rc_dst_register
nrc_dst_for_def(struct nrc_compile *c, nir_def *def)
{
   struct rc_dst_register dst = {0};
   nir_legacy_dest ld = nir_legacy_chase_dest(def);

   if (ld.is_ssa) {
      dst.File      = RC_FILE_TEMPORARY;
      dst.Index     = nrc_temp_for_ssa(c, ld.ssa);
      dst.WriteMask = BITSET_MASK(def->num_components);
   } else {
      dst.File      = RC_FILE_TEMPORARY;
      dst.Index     = c->reg_temp[ld.reg.handle->index] + ld.reg.base_offset;
      dst.WriteMask = BITSET_MASK(def->num_components);
   }

   return dst;
}

/* -------------------------------------------------------------------------
 * Scalar op emission (one RC instruction per destination channel)
 * RCP, RSQ, EX2, LG2, SIN, COS, POW replicate a single src channel.
 * -------------------------------------------------------------------------*/

static void
nrc_emit_scalar(struct nrc_compile *c, rc_opcode opcode,
                struct rc_dst_register dst, struct rc_src_register src0,
                struct rc_src_register src1, bool saturate)
{
   /* One RC instruction per destination channel.  For each channel i, extract
    * the source channel that feeds it from src0.Swizzle and replicate to all
    * four positions, satisfying the scalar-read rule.  Saturation is applied
    * per-instruction to match the original NIR ALU destination. */
   for (int i = 0; i < 4; i++) {
      if (!(dst.WriteMask & (1u << i)))
         continue;
      rc_swizzle src_chan = (rc_swizzle)((src0.Swizzle >> (i * 3)) & 0x7u);
      struct rc_src_register scalar_src = src0;
      scalar_src.Swizzle = RC_MAKE_SWIZZLE(src_chan, src_chan, src_chan, src_chan);
      struct rc_instruction *inst = nrc_emit(c, opcode);
      inst->U.I.DstReg           = dst;
      inst->U.I.DstReg.WriteMask = 1u << i;
      inst->U.I.SrcReg[0]        = scalar_src;
      inst->U.I.SrcReg[1]        = src1;
      inst->U.I.SaturateMode     = saturate ? RC_SATURATE_ZERO_ONE : RC_SATURATE_NONE;
   }
}

/* -------------------------------------------------------------------------
 * ALU instruction emission
 * ------------------------------------------------------------------------- */

static void
nrc_emit_alu(struct nrc_compile *c, nir_alu_instr *instr)
{
   bool saturate = false;
   struct rc_dst_register dst = nrc_alu_dst(c, instr, &saturate);
   struct rc_src_register src[3] = {{0}, {0}, {0}};

   /* Fetch sources (up to 3; not all opcodes use all three). */
   int nsrc = nir_op_infos[instr->op].num_inputs;
   for (int i = 0; i < MIN2(nsrc, 3); i++)
      src[i] = nrc_alu_src(c, instr, i);

   /* Primary opcode table (direct one-to-one mappings). */
   static const rc_opcode op_map[] = {
      [nir_op_mov]             = RC_OPCODE_MOV,
      [nir_op_fadd]            = RC_OPCODE_ADD,
      [nir_op_fmul]            = RC_OPCODE_MUL,
      [nir_op_ffma]            = RC_OPCODE_MAD,
      [nir_op_fmin]            = RC_OPCODE_MIN,
      [nir_op_fmax]            = RC_OPCODE_MAX,
      [nir_op_fdot2_replicated] = RC_OPCODE_DP2,
      [nir_op_fdot3_replicated] = RC_OPCODE_DP3,
      [nir_op_fdot4_replicated] = RC_OPCODE_DP4,
      [nir_op_ffract]          = RC_OPCODE_FRC,
      [nir_op_fround_even]     = RC_OPCODE_ROUND,
      [nir_op_slt]             = RC_OPCODE_SLT,
      [nir_op_sge]             = RC_OPCODE_SGE,
      [nir_op_seq]             = RC_OPCODE_SEQ,
      [nir_op_sne]             = RC_OPCODE_SNE,
   };

   if (instr->op < ARRAY_SIZE(op_map) && op_map[instr->op] != 0) {
      struct rc_instruction *inst = nrc_emit(c, op_map[instr->op]);
      inst->U.I.DstReg         = dst;
      inst->U.I.SaturateMode   = saturate ? RC_SATURATE_ZERO_ONE : RC_SATURATE_NONE;
      inst->U.I.SrcReg[0]      = src[0];
      inst->U.I.SrcReg[1]      = src[1];
      inst->U.I.SrcReg[2]      = src[2];
      return;
   }

   /* Special cases */
   switch (instr->op) {

   case nir_op_fabs:
      if (c->lower_fabs) {
         /* MAX(x, -x) expansion for pre-R5xx VS */
         struct rc_src_register neg_src = src[0];
         neg_src.Negate ^= RC_MASK_XYZW;
         struct rc_instruction *inst = nrc_emit(c, RC_OPCODE_MAX);
         inst->U.I.DstReg       = dst;
         inst->U.I.SaturateMode = saturate ? RC_SATURATE_ZERO_ONE : RC_SATURATE_NONE;
         inst->U.I.SrcReg[0]    = src[0];
         inst->U.I.SrcReg[1]    = neg_src;
      } else {
         struct rc_src_register abs_src = src[0];
         abs_src.Abs = 1;
         abs_src.Negate = 0;
         struct rc_instruction *inst = nrc_emit(c, RC_OPCODE_MOV);
         inst->U.I.DstReg       = dst;
         inst->U.I.SaturateMode = saturate ? RC_SATURATE_ZERO_ONE : RC_SATURATE_NONE;
         inst->U.I.SrcReg[0]    = abs_src;
      }
      break;

   case nir_op_fneg: {
      struct rc_src_register neg_src = src[0];
      neg_src.Negate ^= RC_MASK_XYZW;
      struct rc_instruction *inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg       = dst;
      inst->U.I.SaturateMode = saturate ? RC_SATURATE_ZERO_ONE : RC_SATURATE_NONE;
      inst->U.I.SrcReg[0]    = neg_src;
      break;
   }

   case nir_op_fsat: {
      struct rc_instruction *inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg       = dst;
      inst->U.I.SaturateMode = RC_SATURATE_ZERO_ONE;
      inst->U.I.SrcReg[0]    = src[0];
      break;
   }

   case nir_op_fsub: {
      struct rc_src_register neg1 = src[1];
      neg1.Negate ^= RC_MASK_XYZW;
      struct rc_instruction *inst = nrc_emit(c, RC_OPCODE_ADD);
      inst->U.I.DstReg       = dst;
      inst->U.I.SaturateMode = saturate ? RC_SATURATE_ZERO_ONE : RC_SATURATE_NONE;
      inst->U.I.SrcReg[0]    = src[0];
      inst->U.I.SrcReg[1]    = neg1;
      break;
   }

   case nir_op_frcp:
      nrc_emit_scalar(c, RC_OPCODE_RCP, dst, src[0], (struct rc_src_register){0}, saturate);
      break;

   case nir_op_frsq:
      nrc_emit_scalar(c, RC_OPCODE_RSQ, dst, src[0], (struct rc_src_register){0}, saturate);
      break;

   case nir_op_fexp2:
      nrc_emit_scalar(c, RC_OPCODE_EX2, dst, src[0], (struct rc_src_register){0}, saturate);
      break;

   case nir_op_flog2:
      nrc_emit_scalar(c, RC_OPCODE_LG2, dst, src[0], (struct rc_src_register){0}, saturate);
      break;

   case nir_op_fsin:
      nrc_emit_scalar(c, RC_OPCODE_SIN, dst, src[0], (struct rc_src_register){0}, saturate);
      break;

   case nir_op_fcos:
      nrc_emit_scalar(c, RC_OPCODE_COS, dst, src[0], (struct rc_src_register){0}, saturate);
      break;

   case nir_op_fpow:
      nrc_emit_scalar(c, RC_OPCODE_POW, dst, src[0], src[1], saturate);
      break;

   case nir_op_flrp: {
      /* TODO: r300_nir_lower_flrp should have eliminated this for VS; for FS
       * the RC compiler has LRP in its internal opcode set via the CMP path.
       * Expand to MAD: a*(1-c) + b*c = MAD(a, (1-c), b*c) is complex;
       * simpler: use the CMP/MAD form:  lerp = src2*src1 + src0*(1-src2).
       * For now, fail gracefully and fall back to TGSI path on error. */
      rc_error(c->compiler, "r300_nir_to_rc_direct: flrp not lowered; run r300_nir_lower_flrp first\n");
      c->error = true;
      break;
   }

   case nir_op_fcsel:
      /* CMP(dst, -abs(src0), src1, src2) */
      {
         struct rc_src_register neg_abs = src[0];
         neg_abs.Abs    = 1;
         neg_abs.Negate = RC_MASK_XYZW;
         struct rc_instruction *inst = nrc_emit(c, RC_OPCODE_CMP);
         inst->U.I.DstReg       = dst;
         inst->U.I.SaturateMode = saturate ? RC_SATURATE_ZERO_ONE : RC_SATURATE_NONE;
         inst->U.I.SrcReg[0]    = neg_abs;
         inst->U.I.SrcReg[1]    = src[1];
         inst->U.I.SrcReg[2]    = src[2];
      }
      break;

   case nir_op_fcsel_gt: {
      struct rc_src_register neg = src[0];
      neg.Negate ^= RC_MASK_XYZW;
      struct rc_instruction *inst = nrc_emit(c, RC_OPCODE_CMP);
      inst->U.I.DstReg       = dst;
      inst->U.I.SaturateMode = saturate ? RC_SATURATE_ZERO_ONE : RC_SATURATE_NONE;
      inst->U.I.SrcReg[0]    = neg;
      inst->U.I.SrcReg[1]    = src[1];
      inst->U.I.SrcReg[2]    = src[2];
      break;
   }

   case nir_op_fcsel_ge: {
      struct rc_instruction *inst = nrc_emit(c, RC_OPCODE_CMP);
      inst->U.I.DstReg       = dst;
      inst->U.I.SaturateMode = saturate ? RC_SATURATE_ZERO_ONE : RC_SATURATE_NONE;
      inst->U.I.SrcReg[0]    = src[0];
      inst->U.I.SrcReg[1]    = src[2];  /* swapped per the CMP semantics */
      inst->U.I.SrcReg[2]    = src[1];
      break;
   }

   case nir_op_vec4:
   case nir_op_vec3:
   case nir_op_vec2:
      UNREACHABLE("vec ops should be lowered by nir_lower_vec_to_regs");

   default:
      rc_error(c->compiler, "r300_nir_to_rc_direct: unknown NIR opcode %s\n",
               nir_op_infos[instr->op].name);
      c->error = true;
      break;
   }
}

/* -------------------------------------------------------------------------
 * Texture instruction emission
 * ------------------------------------------------------------------------- */

static rc_texture_target
nrc_tex_target(enum glsl_sampler_dim dim, bool is_array)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:   return is_array ? RC_TEXTURE_1D_ARRAY : RC_TEXTURE_1D;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_EXTERNAL:
                                return is_array ? RC_TEXTURE_2D_ARRAY : RC_TEXTURE_2D;
   case GLSL_SAMPLER_DIM_3D:   return RC_TEXTURE_3D;
   case GLSL_SAMPLER_DIM_CUBE: return RC_TEXTURE_CUBE;
   case GLSL_SAMPLER_DIM_RECT: return RC_TEXTURE_RECT;
   default:
      UNREACHABLE("unsupported sampler dim for RC texture target");
   }
}

static void
nrc_emit_texture(struct nrc_compile *c, nir_tex_instr *instr)
{
   struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
   assert(!instr->is_shadow);

   int backend1 = nir_tex_instr_src_index(instr, nir_tex_src_backend1);
   int backend2 = nir_tex_instr_src_index(instr, nir_tex_src_backend2);
   int ddx_idx  = nir_tex_instr_src_index(instr, nir_tex_src_ddx);
   int ddy_idx  = nir_tex_instr_src_index(instr, nir_tex_src_ddy);

   rc_opcode opcode;
   switch (instr->op) {
   case nir_texop_tex:
      opcode = RC_OPCODE_TEX;
      break;
   case nir_texop_txl:
      opcode = RC_OPCODE_TXL;
      break;
   case nir_texop_txb:
      opcode = RC_OPCODE_TXB;
      break;
   case nir_texop_txd:
      opcode = RC_OPCODE_TXD;
      break;
   case nir_texop_txs:
   case nir_texop_query_levels:
   case nir_texop_lod:
   case nir_texop_texture_samples:
   case nir_texop_tg4:
      /* These must be lowered by nir_to_rc_lower_tex before reaching here. */
      rc_error(c->compiler, "r300_nir_to_rc_direct: texop %d not lowered\n", instr->op);
      c->error = true;
      return;
   default:
      UNREACHABLE("unsupported tex op");
   }

   struct rc_instruction *inst = nrc_emit(c, opcode);
   inst->U.I.DstReg        = dst;
   inst->U.I.TexSrcUnit    = instr->sampler_index;
   inst->U.I.TexSrcTarget  = nrc_tex_target(instr->sampler_dim, instr->is_array);
   inst->U.I.TexSwizzle    = RC_SWIZZLE_XYZW;

   if (backend1 >= 0)
      inst->U.I.SrcReg[0] = nrc_src(c, instr->src[backend1].src);
   if (backend2 >= 0)
      inst->U.I.SrcReg[1] = nrc_src(c, instr->src[backend2].src);

   if (instr->op == nir_texop_txd) {
      if (ddx_idx >= 0) inst->U.I.SrcReg[1] = nrc_src(c, instr->src[ddx_idx].src);
      if (ddy_idx >= 0) inst->U.I.SrcReg[2] = nrc_src(c, instr->src[ddy_idx].src);
   }
}

/* -------------------------------------------------------------------------
 * Intrinsic emission
 * ------------------------------------------------------------------------- */

static void
nrc_emit_intrinsic(struct nrc_compile *c, nir_intrinsic_instr *instr)
{
   struct rc_instruction *inst;

   switch (instr->intrinsic) {

   case nir_intrinsic_load_input:
   case nir_intrinsic_load_per_vertex_input: {
      int base = nir_intrinsic_base(instr);
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg         = dst;
      inst->U.I.SrcReg[0].File  = RC_FILE_INPUT;
      inst->U.I.SrcReg[0].Index = base;
      inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
      /* Shift by component fraction */
      unsigned frac = nir_intrinsic_component(instr);
      if (frac) {
         inst->U.I.SrcReg[0].Swizzle =
            RC_MAKE_SWIZZLE(nrc_swizzle_chan(frac + 0),
                            nrc_swizzle_chan(MIN2(frac + 1, 3)),
                            nrc_swizzle_chan(MIN2(frac + 2, 3)),
                            nrc_swizzle_chan(MIN2(frac + 3, 3)));
      }
      break;
   }

   case nir_intrinsic_load_interpolated_input: {
      int base = nir_intrinsic_base(instr);
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg         = dst;
      inst->U.I.SrcReg[0].File  = RC_FILE_INPUT;
      inst->U.I.SrcReg[0].Index = base;
      inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
      unsigned frac = nir_intrinsic_component(instr);
      if (frac) {
         inst->U.I.SrcReg[0].Swizzle =
            RC_MAKE_SWIZZLE(nrc_swizzle_chan(frac + 0),
                            nrc_swizzle_chan(MIN2(frac + 1, 3)),
                            nrc_swizzle_chan(MIN2(frac + 2, 3)),
                            nrc_swizzle_chan(MIN2(frac + 3, 3)));
      }
      break;
   }

   case nir_intrinsic_store_output:
   case nir_intrinsic_store_per_vertex_output: {
      int base = nir_intrinsic_base(instr);
      unsigned writemask = nir_intrinsic_write_mask(instr);
      unsigned frac = nir_intrinsic_component(instr);
      struct rc_src_register value = nrc_src(c, instr->src[0]);

      /* RC executes channel i of dst from channel i of src.  When frac > 0,
       * the source's channel 0 should land at output channel frac, so shift
       * the source swizzle up by frac positions to match the shifted WriteMask. */
      if (frac) {
         uint32_t orig = value.Swizzle;
         uint32_t new_swiz = orig;
         for (int i = frac; i < 4; i++) {
            rc_swizzle sc = (rc_swizzle)((orig >> ((i - (int)frac) * 3)) & 0x7u);
            new_swiz = (new_swiz & ~(0x7u << (i * 3))) | ((uint32_t)sc << (i * 3));
         }
         value.Swizzle = new_swiz;
      }

      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg.File      = RC_FILE_OUTPUT;
      inst->U.I.DstReg.Index     = base;
      inst->U.I.DstReg.WriteMask = writemask << frac;
      inst->U.I.SrcReg[0]        = value;
      break;
   }

   case nir_intrinsic_load_output:
   case nir_intrinsic_load_per_vertex_output: {
      int base = nir_intrinsic_base(instr);
      unsigned frac = nir_intrinsic_component(instr);
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg            = dst;
      inst->U.I.SrcReg[0].File    = RC_FILE_OUTPUT;
      inst->U.I.SrcReg[0].Index   = base;
      inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
      if (frac) {
         inst->U.I.SrcReg[0].Swizzle =
            RC_MAKE_SWIZZLE(nrc_swizzle_chan(frac + 0),
                            nrc_swizzle_chan(MIN2(frac + 1, 3)),
                            nrc_swizzle_chan(MIN2(frac + 2, 3)),
                            nrc_swizzle_chan(MIN2(frac + 3, 3)));
      }
      break;
   }

   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ubo_vec4: {
      /* UBO loads must be constant-indexed on r300; indirect access is unsupported. */
      if (!nir_src_is_const(instr->src[0]) || !nir_src_is_const(instr->src[1])) {
         rc_error(c->compiler, "r300_nir_to_rc_direct: indirect UBO access not supported\n");
         c->error = true;
         return;
      }
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      /* src[1] is a vec4 index at this stage: the Gallium/Mesa UBO lowering
       * pipeline converts byte offsets to vec4 units before the shader
       * reaches emission.  The TGSI path (ntr_emit_load_ubo, nir_to_rc.c)
       * adds src[1] directly without dividing by 16; align with that. */
      unsigned vec4_offset = nir_src_as_uint(instr->src[1]);
      /* nir_intrinsic_base gives the starting constant slot for this UBO
       * binding in the flat RC constant table (RC_CONSTANT_EXTERNAL). */
      int slot = nir_intrinsic_base(instr) + (int)vec4_offset;

      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg = dst;
      inst->U.I.SrcReg[0].File    = RC_FILE_CONSTANT;
      inst->U.I.SrcReg[0].Index   = slot;
      inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;

      unsigned frac = nir_intrinsic_component(instr);
      if (frac) {
         inst->U.I.SrcReg[0].Swizzle =
            RC_MAKE_SWIZZLE(nrc_swizzle_chan(frac + 0),
                            nrc_swizzle_chan(MIN2(frac + 1, 3)),
                            nrc_swizzle_chan(MIN2(frac + 2, 3)),
                            nrc_swizzle_chan(MIN2(frac + 3, 3)));
      }
      break;
   }

   case nir_intrinsic_load_frag_coord: {
      /* WPOS input slot assigned by nir_lower_io from the VARYING_SLOT_POS
       * input variable; rc_transform_fragment_wpos() transforms it later. */
      int idx = nrc_find_input_slot(c->s, VARYING_SLOT_POS);
      if (idx < 0) {
         rc_error(c->compiler, "r300_nir_to_rc_direct: no VARYING_SLOT_POS input for frag_coord\n");
         c->error = true;
         return;
      }
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg            = dst;
      inst->U.I.SrcReg[0].File    = RC_FILE_INPUT;
      inst->U.I.SrcReg[0].Index   = idx;
      inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
      break;
   }

   case nir_intrinsic_load_front_face: {
      /* FACE input slot assigned by nir_lower_io from VARYING_SLOT_FACE;
       * rc_transform_fragment_face() transforms it after compilation. */
      int idx = nrc_find_input_slot(c->s, VARYING_SLOT_FACE);
      if (idx < 0) {
         rc_error(c->compiler, "r300_nir_to_rc_direct: no VARYING_SLOT_FACE input for front_face\n");
         c->error = true;
         return;
      }
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg            = dst;
      inst->U.I.SaturateMode      = RC_SATURATE_ZERO_ONE;
      inst->U.I.SrcReg[0].File    = RC_FILE_INPUT;
      inst->U.I.SrcReg[0].Index   = idx;
      inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
      break;
   }

   case nir_intrinsic_load_point_coord: {
      /* r300_nir_lower_for_rc() runs ntr_fixup_varying_slots() before
       * emission.  That pass remaps VARYING_SLOT_PNTC -> VARYING_SLOT_VAR8
       * (nir_to_rc.c:ntr_fixup_varying_slots).  Searching for PNTC after
       * the remap always returns -1 even when point coord is present. */
      int idx = nrc_find_input_slot(c->s, VARYING_SLOT_VAR8);
      if (idx < 0) {
         rc_error(c->compiler, "r300_nir_to_rc_direct: no VARYING_SLOT_VAR8 input for point_coord\n");
         c->error = true;
         return;
      }
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg            = dst;
      inst->U.I.SrcReg[0].File    = RC_FILE_INPUT;
      inst->U.I.SrcReg[0].Index   = idx;
      inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
      break;
   }

   case nir_intrinsic_terminate:
      nrc_emit(c, RC_OPCODE_KILP);
      break;

   case nir_intrinsic_terminate_if: {
      struct rc_src_register cond = nrc_src(c, instr->src[0]);
      inst = nrc_emit(c, RC_OPCODE_KIL);
      inst->U.I.SrcReg[0] = cond;
      /* KIL kills fragments where src < 0; negate to match GLSL discard_if(cond) */
      inst->U.I.SrcReg[0].Negate = RC_MASK_XYZW;
      break;
   }

   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_coarse: {
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      inst = nrc_emit(c, RC_OPCODE_DDX);
      inst->U.I.DstReg    = dst;
      inst->U.I.SrcReg[0] = nrc_src(c, instr->src[0]);
      break;
   }

   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_coarse: {
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      inst = nrc_emit(c, RC_OPCODE_DDY);
      inst->U.I.DstReg    = dst;
      inst->U.I.SrcReg[0] = nrc_src(c, instr->src[0]);
      break;
   }

   case nir_intrinsic_decl_reg: {
      unsigned idx = instr->def.index;
      c->reg_temp[idx] = nrc_alloc_temp(c);
      break;
   }

   case nir_intrinsic_load_reg: {
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      unsigned ridx = nir_intrinsic_base(instr);
      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg            = dst;
      inst->U.I.SrcReg[0].File    = RC_FILE_TEMPORARY;
      inst->U.I.SrcReg[0].Index   = c->reg_temp[ridx];
      inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
      break;
   }

   case nir_intrinsic_store_reg: {
      unsigned ridx   = nir_intrinsic_base(instr);
      unsigned wmask  = nir_intrinsic_write_mask(instr);
      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg.File      = RC_FILE_TEMPORARY;
      inst->U.I.DstReg.Index     = c->reg_temp[ridx];
      inst->U.I.DstReg.WriteMask = wmask;
      inst->U.I.SrcReg[0]        = nrc_src(c, instr->src[0]);
      break;
   }

   case nir_intrinsic_load_reg_indirect:
   case nir_intrinsic_store_reg_indirect:
      rc_error(c->compiler, "r300_nir_to_rc_direct: indirect register access not supported\n");
      c->error = true;
      break;

   /* VS system values */
   case nir_intrinsic_load_vertex_id:
   case nir_intrinsic_load_instance_id:
   case nir_intrinsic_load_draw_id:
   case nir_intrinsic_load_invocation_id: {
      /* VS system values come from dedicated RC input slots; index assignment
       * matches the TGSI SYSTEM_VALUE slot set up by the VS input lowering. */
      struct rc_dst_register dst = nrc_dst_for_def(c, &instr->def);
      inst = nrc_emit(c, RC_OPCODE_MOV);
      inst->U.I.DstReg = dst;
      inst->U.I.SrcReg[0].File   = RC_FILE_INPUT;
      inst->U.I.SrcReg[0].Index  = 0;  /* TODO: derive correct system-value slot */
      inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
      break;
   }

   /* Barycentric load_barycentric_* are only sources for interpolated_input;
    * they carry no RC instruction and are handled by the caller. */
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_sample:
   case nir_intrinsic_load_barycentric_at_sample:
   case nir_intrinsic_load_barycentric_at_offset:
      break;

   default:
      rc_error(c->compiler, "r300_nir_to_rc_direct: unhandled intrinsic %s\n",
               nir_intrinsic_infos[instr->intrinsic].name);
      c->error = true;
      break;
   }
}

/* -------------------------------------------------------------------------
 * Control flow emission
 * ------------------------------------------------------------------------- */

static void nrc_emit_block(struct nrc_compile *c, nir_block *block);
static void nrc_emit_cf_list(struct nrc_compile *c, struct exec_list *list);

static void
nrc_emit_if(struct nrc_compile *c, nir_if *nif)
{
   /* R3xx/R4xx do not support branching; the RC compiler emits an error. */
   if (!c->is_r500) {
      rc_error(c->compiler, "r300_nir_to_rc_direct: branches not supported on R3xx/R4xx\n");
      c->error = true;
      return;
   }

   struct rc_src_register cond = nrc_src(c, nif->condition);
   struct rc_instruction *if_inst = nrc_emit(c, RC_OPCODE_IF);
   if_inst->U.I.SrcReg[0] = cond;

   nrc_emit_cf_list(c, &nif->then_list);

   if (!exec_list_is_empty(&nif->else_list)) {
      nrc_emit(c, RC_OPCODE_ELSE);
      nrc_emit_cf_list(c, &nif->else_list);
   }

   nrc_emit(c, RC_OPCODE_ENDIF);
}

static void
nrc_emit_loop(struct nrc_compile *c, nir_loop *loop)
{
   if (!c->is_r500) {
      rc_error(c->compiler, "r300_nir_to_rc_direct: loops not supported on R3xx/R4xx\n");
      c->error = true;
      return;
   }

   nrc_emit(c, RC_OPCODE_BGNLOOP);
   nrc_emit_cf_list(c, &loop->body);
   nrc_emit(c, RC_OPCODE_ENDLOOP);
}

static void
nrc_emit_block(struct nrc_compile *c, nir_block *block)
{
   nir_foreach_instr (instr, block) {
      switch (instr->type) {
      case nir_instr_type_alu:
         nrc_emit_alu(c, nir_instr_as_alu(instr));
         break;
      case nir_instr_type_tex:
         nrc_emit_texture(c, nir_instr_as_tex(instr));
         break;
      case nir_instr_type_intrinsic:
         nrc_emit_intrinsic(c, nir_instr_as_intrinsic(instr));
         break;
      case nir_instr_type_load_const:
         /* load_const values are inlined at their use sites via nrc_add_const;
          * no separate instruction is emitted here. */
         break;
      case nir_instr_type_jump: {
         nir_jump_instr *jump = nir_instr_as_jump(instr);
         switch (jump->type) {
         case nir_jump_break:
            nrc_emit(c, RC_OPCODE_BRK);
            break;
         case nir_jump_continue:
            nrc_emit(c, RC_OPCODE_CONT);
            break;
         case nir_jump_return:
            /* fall through to end of function; no explicit RC return needed */
            break;
         default:
            UNREACHABLE("unexpected jump type");
         }
         break;
      }
      case nir_instr_type_undef:
         /* Undefined defs don't generate code. */
         break;
      default:
         rc_error(c->compiler, "r300_nir_to_rc_direct: unhandled instr type %d\n",
                  instr->type);
         c->error = true;
         break;
      }

      if (c->error)
         return;
   }
}

static void
nrc_emit_cf_list(struct nrc_compile *c, struct exec_list *list)
{
   foreach_list_typed (nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block:
         nrc_emit_block(c, nir_cf_node_as_block(node));
         break;
      case nir_cf_node_if:
         nrc_emit_if(c, nir_cf_node_as_if(node));
         break;
      case nir_cf_node_loop:
         nrc_emit_loop(c, nir_cf_node_as_loop(node));
         break;
      default:
         UNREACHABLE("unexpected CF node type");
      }

      if (c->error)
         return;
   }
}

/* -------------------------------------------------------------------------
 * r300_nir_fill_shader_info
 * Populates tgsi_shader_info fields needed by r300_shader_read_fs_inputs()
 * and find_output_registers(), replacing tgsi_scan_shader() for NIR path.
 * ------------------------------------------------------------------------- */

void
r300_nir_fill_shader_info(const struct nir_shader *s, struct tgsi_shader_info *info)
{
   memset(info, 0, sizeof(*info));

   int ni = 0;
   nir_foreach_shader_in_variable (var, s) {
      unsigned sem_name, sem_index;
      /* Apply the same TEX0-7 -> VAR0+ fixup that ntr_fixup_varying_slots does
       * in the TGSI path, so the semantic names match tgsi_scan_shader output. */
      int loc = var->data.location;
      if (loc >= VARYING_SLOT_TEX0 && loc <= VARYING_SLOT_TEX7)
         loc = VARYING_SLOT_VAR0 + (loc - VARYING_SLOT_TEX0);
      tgsi_get_gl_varying_semantic(loc, true, &sem_name, &sem_index);
      info->input_semantic_name[ni]  = sem_name;
      info->input_semantic_index[ni] = sem_index;
      ni++;
   }
   info->num_inputs = ni;

   int no = 0;
   nir_foreach_shader_out_variable (var, s) {
      if (s->info.stage == MESA_SHADER_FRAGMENT) {
         unsigned sem_name, sem_index;
         tgsi_get_gl_frag_result_semantic(var->data.location, &sem_name, &sem_index);
         info->output_semantic_name[no]  = sem_name;
         info->output_semantic_index[no] = sem_index;

         if (var->data.location == FRAG_RESULT_COLOR)
            info->properties[TGSI_PROPERTY_FS_COLOR0_WRITES_ALL_CBUFS] = 1;
      } else {
         unsigned sem_name, sem_index;
         tgsi_get_gl_varying_semantic(var->data.location, true, &sem_name, &sem_index);
         info->output_semantic_name[no]  = sem_name;
         info->output_semantic_index[no] = sem_index;
      }
      no++;
   }
   info->num_outputs = no;
}

/* -------------------------------------------------------------------------
 * Main entry point
 * ------------------------------------------------------------------------- */

void
r300_nir_to_rc_direct(struct radeon_compiler *compiler,
                       const struct nir_shader *s,
                       struct pipe_screen *screen,
                       struct r300_fragment_program_external_state state)
{
   (void)state;  /* used by the NIR lowering passes, not by RC emission */

   struct nrc_compile c = {
      .s         = s,
      .compiler  = compiler,
      .is_r500   = compiler->is_r500,
      .lower_fabs = !compiler->is_r500 && s->info.stage == MESA_SHADER_VERTEX,
      .first_ubo = ~0,
      .error     = false,
   };

   nir_function_impl *impl = nir_shader_get_entrypoint(s);

   c.ssa_temp = MALLOC(impl->ssa_alloc * sizeof(uint32_t));
   memset(c.ssa_temp, 0xFF, impl->ssa_alloc * sizeof(uint32_t));  /* UINT32_MAX */

   /* Count decl_regs for reg_temp allocation */
   unsigned reg_alloc = 0;
   nir_foreach_reg_decl (reg, impl)
      reg_alloc = MAX2(reg_alloc, reg->def.index + 1);

   c.reg_temp = reg_alloc ? MALLOC(reg_alloc * sizeof(uint32_t)) : NULL;
   if (reg_alloc)
      memset(c.reg_temp, 0xFF, reg_alloc * sizeof(uint32_t));

   /* Pre-allocate external constants for uniforms.  The number of external
    * constant slots must match what r300_translate_fragment_shader expects,
    * which is ttr->info->file_max[TGSI_FILE_CONSTANT] + 1.  For the direct
    * path we read num_uniforms from the NIR shader instead. */
   if (s->num_uniforms > 0) {
      for (unsigned i = 0; i < s->num_uniforms; i++) {
         struct rc_constant constant = {0};
         constant.Type     = RC_CONSTANT_EXTERNAL;
         constant.UseMask  = RC_MASK_XYZW;
         constant.u.External = i;
         rc_constants_add(&compiler->Program.Constants, &constant);
      }
   }

   /* Walk and emit */
   nrc_emit_cf_list(&c, &impl->body);

   /* rc_calculate_inputs_outputs reads the instruction stream; skip on error
    * to avoid operating on a partial or inconsistent program. */
   if (!c.error)
      rc_calculate_inputs_outputs(compiler);

   FREE(c.ssa_temp);
   FREE(c.reg_temp);
}
