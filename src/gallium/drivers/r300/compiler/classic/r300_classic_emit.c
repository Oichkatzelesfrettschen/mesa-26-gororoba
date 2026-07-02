/*
 * Copyright (c) 2026 Terascale Functionalists
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
   case R300C_OP_DP3: return RC_OPCODE_DP3;
   case R300C_OP_DP4: return RC_OPCODE_DP4;
   case R300C_OP_MIN: return RC_OPCODE_MIN;
   case R300C_OP_MAX: return RC_OPCODE_MAX;
   case R300C_OP_FRC: return RC_OPCODE_FRC;
   case R300C_OP_RCP: return RC_OPCODE_RCP;
   case R300C_OP_RSQ: return RC_OPCODE_RSQ;
   case R300C_OP_TEX: return RC_OPCODE_TEX;
   case R300C_OP_KIL: return RC_OPCODE_KIL;
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
            const struct r300_classic_regalloc_result *ra,
            const struct r300_classic_immediates *imm,
            const unsigned *imm_rc_index,
            struct rc_src_register *out)
{
   memset(out, 0, sizeof(*out));
   switch (src->file) {
   case R300C_FILE_SSA: {
      const unsigned temp = ra->temp_of_ssa[src->def->ssa_id];
      if (temp == R300_CLASSIC_NO_TEMP)
         return false;
      out->File = RC_FILE_TEMPORARY;
      out->Index = temp;
      break;
   }
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
                  const struct r300_classic_regalloc_result *ra,
                  const struct r300_classic_immediates *imm,
                  struct r300_fragment_program_compiler *fc)
{
   struct radeon_compiler *c = &fc->Base;

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
   unsigned color_index = ~0u;
   unsigned depth_index = ~0u;
   list_for_each_entry (struct r300_classic_instr, i, &p->instrs, link) {
      if (i->op == R300C_OP_EXPORT_COLOR && color_index == ~0u)
         color_index = num_outputs++;
      if (i->op == R300C_OP_EXPORT_DEPTH && depth_index == ~0u)
         depth_index = num_outputs++;
   }
   fc->OutputColor[0] = color_index != ~0u ? color_index : num_outputs;
   for (unsigned n = 1; n < 4; n++)
      fc->OutputColor[n] = num_outputs;
   fc->OutputDepth = depth_index != ~0u ? depth_index : num_outputs;

   list_for_each_entry (struct r300_classic_instr, i, &p->instrs, link) {
      const rc_opcode op = rc_opcode_for(i->op);
      if (op == RC_OPCODE_ILLEGAL_OPCODE)
         return false;

      struct rc_instruction *inst =
         rc_insert_new_instruction(c, c->Program.Instructions.Prev);
      inst->U.I.Opcode = op;

      for (unsigned s = 0; s < i->num_srcs; s++)
         if (!convert_src(&i->src[s], ra, imm, imm_rc_index,
                          &inst->U.I.SrcReg[s]))
            return false;

      switch (i->op) {
      case R300C_OP_EXPORT_COLOR:
         inst->U.I.DstReg.File = RC_FILE_OUTPUT;
         inst->U.I.DstReg.Index = color_index;
         inst->U.I.DstReg.WriteMask = RC_MASK_XYZW;
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
         /* KIL has no destination. */
         break;
      case R300C_OP_TEX:
         inst->U.I.TexSrcUnit = i->tex_unit;
         inst->U.I.TexSrcTarget = RC_TEXTURE_2D;
         FALLTHROUGH;
      default: {
         const unsigned temp = ra->temp_of_ssa[i->ssa_id];
         if (temp == R300_CLASSIC_NO_TEMP)
            return false;
         inst->U.I.DstReg.File = RC_FILE_TEMPORARY;
         inst->U.I.DstReg.Index = temp;
         inst->U.I.DstReg.WriteMask = i->writemask;
         break;
      }
      }
   }

   rc_calculate_inputs_outputs(c);
   return true;
}
