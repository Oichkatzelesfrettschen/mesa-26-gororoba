/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_classic_emit.h"

#include "../radeon_compiler.h"
#include "../radeon_code.h"
#include "../radeon_program.h"
#include "../radeon_program_constants.h"

static rc_opcode
rc_opcode_for(enum r300_classic_op op)
{
   switch (op) {
   case R300C_OP_MOV: return RC_OPCODE_MOV;
   case R300C_OP_ADD: return RC_OPCODE_ADD;
   case R300C_OP_MUL: return RC_OPCODE_MUL;
   case R300C_OP_MAD: return RC_OPCODE_MAD;
   case R300C_OP_DP2: return RC_OPCODE_DP2;
   case R300C_OP_DP3: return RC_OPCODE_DP3;
   case R300C_OP_DP4: return RC_OPCODE_DP4;
   case R300C_OP_MIN: return RC_OPCODE_MIN;
   case R300C_OP_MAX: return RC_OPCODE_MAX;
   case R300C_OP_FRC: return RC_OPCODE_FRC;
   case R300C_OP_ROUND: return RC_OPCODE_ROUND;
   case R300C_OP_RCP: return RC_OPCODE_RCP;
   case R300C_OP_RSQ: return RC_OPCODE_RSQ;
   case R300C_OP_EX2: return RC_OPCODE_EX2;
   case R300C_OP_LG2: return RC_OPCODE_LG2;
   case R300C_OP_SIN: return RC_OPCODE_SIN;
   case R300C_OP_COS: return RC_OPCODE_COS;
   case R300C_OP_POW: return RC_OPCODE_POW;
   case R300C_OP_CMP: return RC_OPCODE_CMP;
   case R300C_OP_DDX: return RC_OPCODE_DDX;
   case R300C_OP_DDY: return RC_OPCODE_DDY;
   case R300C_OP_TEX: return RC_OPCODE_TEX;
   case R300C_OP_TXB: return RC_OPCODE_TXB;
   case R300C_OP_TXP: return RC_OPCODE_TXP;
   case R300C_OP_KIL: return RC_OPCODE_KIL;
   case R300C_OP_KILP: return RC_OPCODE_KILP;
   /* The collect expands to MOVs, one per channel group. */
   case R300C_OP_VEC: return RC_OPCODE_MOV;
   /* Exports emit as MOVs into RC_FILE_OUTPUT. */
   case R300C_OP_EXPORT_COLOR:
   case R300C_OP_EXPORT_DEPTH:
      return RC_OPCODE_MOV;
   default:
      return RC_OPCODE_ILLEGAL_OPCODE;
   }
}

static bool
convert_src(const struct r300_classic_src *src,
            const struct r300_classic_immediates *imm,
            const unsigned *imm_rc_index,
            const unsigned *state_rc_index, unsigned num_states,
            struct rc_src_register *out)
{
   memset(out, 0, sizeof(*out));
   switch (src->file) {
   case R300C_FILE_SSA:
      out->File = RC_FILE_TEMPORARY;
      out->Index = src->def->ssa_id;
      break;
   case R300C_FILE_STATE:
      if (src->index >= num_states)
         return false;
      out->File = RC_FILE_CONSTANT;
      out->Index = state_rc_index[src->index];
      break;
   case R300C_FILE_INPUT:
      out->File = RC_FILE_INPUT;
      out->Index = src->index;
      break;
   case R300C_FILE_CONST:
      out->File = RC_FILE_CONSTANT;
      if (src->index >= imm->first_index) {
         const unsigned slot = src->index - imm->first_index;
         if (slot >= imm->count)
            return false;
         out->Index = imm_rc_index[slot];
      } else {
         out->Index = src->index;
      }
      break;
   default:
      return false;
   }
   out->Swizzle = src->swizzle;
   out->Negate = src->negate ? RC_MASK_XYZW : 0;
   out->Abs = src->abs;
   return true;
}

bool
r300_classic_emit(const struct r300_classic_program *p,
                  const struct r300_classic_immediates *imm,
                  const struct r300_classic_state_constants *states,
                  struct r300_fragment_program_compiler *fc)
{
   struct radeon_compiler *c = &fc->Base;

   /* External (UBO) constants occupy list positions 0..first_index-1 so a
    * classic FILE_CONST index equals its Program.Constants position, the
    * layout ntr_add_constants produces and the driver's constant upload
    * expects. */
   for (unsigned n = 0; n < imm->first_index; n++) {
      struct rc_constant constant;
      memset(&constant, 0, sizeof(constant));
      constant.Type = RC_CONSTANT_EXTERNAL;
      constant.UseMask = RC_MASK_XYZW;
      constant.u.External = n;
      rc_constants_add(&c->Program.Constants, &constant);
   }

   /* Driver-updated state constants precede the immediates, the position
    * nir_to_rc's table takes; the driver's constant upload walks
    * Program.Constants by type, so only the reference remap matters. */
   unsigned state_rc_index[R300_CLASSIC_MAX_STATE_CONSTANTS];
   for (unsigned n = 0; n < states->count; n++) {
      struct rc_constant constant;
      memset(&constant, 0, sizeof(constant));
      constant.Type = RC_CONSTANT_STATE;
      constant.UseMask = RC_MASK_XYZW;
      constant.u.State[0] = states->entries[n].rc_state;
      constant.u.State[1] = states->entries[n].sampler;
      state_rc_index[n] = rc_constants_add(&c->Program.Constants, &constant);
   }

   unsigned imm_rc_index[R300_CLASSIC_MAX_IMMEDIATES];
   for (unsigned n = 0; n < imm->count; n++)
      imm_rc_index[n] =
         rc_constants_add_immediate_vec4(&c->Program.Constants,
                                         imm->values[n]);

   /* Output register indices follow nir_to_rc's convention: allocated in
    * first-export order, with the unused kind parked one past the used
    * range so the backend's OutputColor/OutputDepth comparisons never
    * alias a real register. */
   unsigned num_outputs = 0;
   unsigned color_reg[4] = {~0u, ~0u, ~0u, ~0u};
   unsigned depth_index = ~0u;
   list_for_each_entry (struct r300_classic_instr, i, &p->instrs, link) {
      if (i->op == R300C_OP_EXPORT_COLOR &&
          color_reg[i->export_index & 3] == ~0u)
         color_reg[i->export_index & 3] = num_outputs++;
      if (i->op == R300C_OP_EXPORT_DEPTH && depth_index == ~0u)
         depth_index = num_outputs++;
   }
   for (unsigned n = 0; n < 4; n++)
      fc->OutputColor[n] = color_reg[n] != ~0u ? color_reg[n] : num_outputs;
   fc->OutputDepth = depth_index != ~0u ? depth_index : num_outputs;

   list_for_each_entry (struct r300_classic_instr, i, &p->instrs, link) {
      const rc_opcode op = rc_opcode_for(i->op);
      if (op == RC_OPCODE_ILLEGAL_OPCODE)
         return false;

      /* The collect writes one destination temp channel by channel; adjacent
       * channels reading the same register merge into one MOV whose swizzle
       * carries each channel's own select.  SSA-indexed temps make the
       * destination inherently disjoint from every source, so the MOV order
       * is free. */
      if (i->op == R300C_OP_VEC) {
         unsigned ch = 0;
         while (ch < i->num_srcs) {
            unsigned end = ch + 1;
            while (end < i->num_srcs &&
                   i->src[end].file == i->src[ch].file &&
                   i->src[end].def == i->src[ch].def &&
                   i->src[end].index == i->src[ch].index &&
                   i->src[end].negate == i->src[ch].negate &&
                   i->src[end].abs == i->src[ch].abs)
               end++;
            struct rc_instruction *mov =
               rc_insert_new_instruction(c, c->Program.Instructions.Prev);
            mov->U.I.Opcode = RC_OPCODE_MOV;
            if (!convert_src(&i->src[ch], imm, imm_rc_index,
                             state_rc_index, states->count,
                             &mov->U.I.SrcReg[0]))
               return false;
            unsigned swz = mov->U.I.SrcReg[0].Swizzle;
            for (unsigned k = ch; k < end; k++) {
               swz &= ~(0x7u << (3 * k));
               swz |= GET_SWZ(i->src[k].swizzle, k) << (3 * k);
            }
            mov->U.I.SrcReg[0].Swizzle = swz;
            mov->U.I.DstReg.File = RC_FILE_TEMPORARY;
            mov->U.I.DstReg.Index = i->ssa_id;
            mov->U.I.DstReg.WriteMask = BITFIELD_RANGE(ch, end - ch);
            ch = end;
         }
         continue;
      }

      struct rc_instruction *inst =
         rc_insert_new_instruction(c, c->Program.Instructions.Prev);
      inst->U.I.Opcode = op;

      for (unsigned s = 0; s < i->num_srcs; s++)
         if (!convert_src(&i->src[s], imm, imm_rc_index,
                          state_rc_index, states->count,
                          &inst->U.I.SrcReg[s]))
            return false;

      /* r500 MDH/MDV computes A*B+C; B = -1 encodes as an all-ones swizzle
       * with full negate, the constant second source nir_to_rc always
       * supplies. */
      if (i->op == R300C_OP_DDX || i->op == R300C_OP_DDY) {
         inst->U.I.SrcReg[1].Swizzle = RC_SWIZZLE_1111;
         inst->U.I.SrcReg[1].Negate = RC_MASK_XYZW;
      }

      switch (i->op) {
      case R300C_OP_EXPORT_COLOR:
         inst->U.I.DstReg.File = RC_FILE_OUTPUT;
         inst->U.I.DstReg.Index = color_reg[i->export_index & 3];
         inst->U.I.DstReg.WriteMask = i->writemask;
         break;
      case R300C_OP_EXPORT_DEPTH: {
         /* The backend reads fragment depth from the .w channel; route the
          * value's first component into the W lane and write W only. */
         inst->U.I.DstReg.File = RC_FILE_OUTPUT;
         inst->U.I.DstReg.Index = depth_index;
         inst->U.I.DstReg.WriteMask = RC_MASK_W;
         unsigned swz = inst->U.I.SrcReg[0].Swizzle;
         swz = (swz & ~(0x7u << 9)) | (GET_SWZ(swz, 0) << 9);
         inst->U.I.SrcReg[0].Swizzle = swz;
         break;
      }
      case R300C_OP_KIL:
      case R300C_OP_KILP:
         /* Discards have no destination. */
         break;
      case R300C_OP_TEX:
      case R300C_OP_TXB:
      case R300C_OP_TXP:
         inst->U.I.TexSrcUnit = i->tex_unit;
         inst->U.I.TexSrcTarget = i->tex_target;
         /* A fresh rc_instruction zeroes TexSwizzle to .xxxx;
          * rc_normal_rewrite_writemask requires the identity swizzle
          * nir_to_rc always sets before it will trim a TEX writemask. */
         inst->U.I.TexSwizzle = RC_SWIZZLE_XYZW;
         FALLTHROUGH;
      default: {
         inst->U.I.DstReg.File = RC_FILE_TEMPORARY;
         inst->U.I.DstReg.Index = i->ssa_id;
         inst->U.I.DstReg.WriteMask = i->writemask;
         if (i->saturate)
            inst->U.I.SaturateMode = RC_SATURATE_ZERO_ONE;
         break;
      }
      }
   }

   rc_calculate_inputs_outputs(c);
   return true;
}
