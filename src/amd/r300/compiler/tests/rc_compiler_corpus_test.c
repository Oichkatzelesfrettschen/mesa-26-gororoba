/*
 * SPDX-License-Identifier: MIT
 *
 * Compile-only correctness corpus for r300 radeon-compiler (RC) passes.
 *
 * Each case hand-builds a small rc_program, runs one pass in isolation, and
 * asserts on the resulting instruction list or stats result.  The first rows pin down
 * rc_inline_literals (compiler/radeon_inline_literals.c).  An R300 inline
 * constant is a 7-bit float: 3 mantissa bits and a 4-bit exponent biased by
 * 7, so ieee_754_to_r300_float only accepts an IEEE-754 value whose unbiased
 * exponent lands in [-7, 8] with the low 20 mantissa bits clear.  A
 * representable, channel-consistent immediate folds to RC_FILE_INLINE; an
 * out-of-range exponent, a set low-mantissa bit, per-channel disagreement, or
 * a non-constant source must leave the operand untouched.  The fold also has
 * to clear r300_swizzle_is_native, which accepts the WWWW alpha swizzle
 * (R300_ALU_ARGC_SRC0A) that the pass smears onto a non-ADD operand.
 *
 * The positive folds depend on r300_swizzle_caps; the negative cases return
 * before the IsNative check and so hold regardless of swizzle caps.  Both
 * directions are present so the corpus is calibrated on known-good and
 * known-bad inputs rather than one polarity.
 *
 * Scope is the compiler transform only.  The production fragment pipeline
 * wires this pass for r500 with optimization on (r3xx_fragprog.c gates it on
 * is_r500 && opt), so an r300-class part such as RS482 never runs it; the
 * 7-bit encoding it implements is nonetheless radeon-compiler-wide, and the
 * corpus exercises that encoding under the RS482-relevant r300 swizzle caps.
 *
 * The later rows pin down rc_get_stats (compiler/radeon_compiler.c), because
 * RS482-facing notes and admissions use its cycle model as the software-side
 * timing proxy.  The model counts one cycle per non-BEGIN_TEX instruction, then
 * layers the BEGIN_TEX penalty and the extra MAD cycle on top.  The corpus
 * keeps those rows on the r300-class path with is_r500 = 0, so the tests do
 * not accidentally depend on the r500-only SemWait credit.
 */

#include <stdbool.h>
#include <stdio.h>

#include "r300_fragprog_swizzle.h"
#include "radeon_code.h"
#include "radeon_compiler.h"
#include "radeon_dataflow.h"
#include "radeon_opcodes.h"
#include "radeon_program.h"
#include "radeon_program_constants.h"
#include "radeon_regalloc.h"

/* 2.0f has unbiased exponent 1 and zero mantissa, so the R300 exponent is
 * (1 + 7) = 8 in bits 6:3 and the byte is 0x40.  The encoder yields the same
 * byte for every channel, which makes a uniform vec4 the canonical
 * representable immediate. */
#define R300_INLINE_TWO 0x40

static unsigned g_failures;

#define CHECK(cond, name)                                                      \
   do {                                                                        \
      if (cond) {                                                              \
         printf("  ok   - %s\n", (name));                                      \
      } else {                                                                 \
         printf("  FAIL - %s\n", (name));                                      \
         g_failures++;                                                         \
      }                                                                        \
   } while (0)

struct corpus_compiler {
   struct radeon_compiler base;
   struct rc_regalloc_state regalloc;
};

/* RC_FRAGMENT_PROGRAM init mirroring r300_fs.c: the regalloc state is required
 * by rc_init even though rc_inline_literals never consults it, and SwizzleCaps
 * must be set before any pass that calls IsNative. */
static void
corpus_fs_init(struct corpus_compiler *cc)
{
   rc_init_regalloc_state(&cc->regalloc, RC_FRAGMENT_PROGRAM);
   rc_init(&cc->base, &cc->regalloc);
   cc->base.type = RC_FRAGMENT_PROGRAM;
   cc->base.is_r500 = 0;
   cc->base.has_presub = 1;
   cc->base.SwizzleCaps = &r300_swizzle_caps;
}

static void
corpus_fs_destroy(struct corpus_compiler *cc)
{
   rc_destroy(&cc->base);
   rc_destroy_regalloc_state(&cc->regalloc);
}

/* Append an ALU instruction at the tail of the program with a temporary
 * destination.  rc_alloc_instruction already seeds XYZW swizzles and a full
 * write mask, so callers only override the source they want to exercise. */
static struct rc_instruction *
corpus_append_alu(struct radeon_compiler *c, rc_opcode opcode)
{
   struct rc_instruction *inst =
      rc_insert_new_instruction(c, c->Program.Instructions.Prev);
   inst->U.I.Opcode = opcode;
   inst->U.I.DstReg.File = RC_FILE_TEMPORARY;
   inst->U.I.DstReg.Index = 0;
   inst->U.I.DstReg.WriteMask = RC_MASK_XYZW;
   return inst;
}

static void
corpus_init_pair_instruction(struct rc_instruction *inst)
{
   inst->Type = RC_INSTRUCTION_PAIR;
   memset(&inst->U.P, 0, sizeof(inst->U.P));
   inst->U.P.RGB.Opcode = RC_OPCODE_NOP;
   inst->U.P.Alpha.Opcode = RC_OPCODE_NOP;
}

static struct rc_instruction *
corpus_append_pair(struct radeon_compiler *c)
{
   struct rc_instruction *inst =
      rc_insert_new_instruction(c, c->Program.Instructions.Prev);
   corpus_init_pair_instruction(inst);
   return inst;
}

static void
corpus_set_constant_src0(struct rc_instruction *inst, unsigned const_index)
{
   inst->U.I.SrcReg[0].File = RC_FILE_CONSTANT;
   inst->U.I.SrcReg[0].Index = const_index;
   inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;
}

static unsigned
corpus_add_uniform_immediate(struct radeon_compiler *c, float value)
{
   const float vec[4] = {value, value, value, value};
   return rc_constants_add_immediate_vec4(&c->Program.Constants, vec);
}

static void
case_representable_folds_to_inline(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   unsigned idx = corpus_add_uniform_immediate(&cc.base, 2.0f);
   struct rc_instruction *inst = corpus_append_alu(&cc.base, RC_OPCODE_MOV);
   corpus_set_constant_src0(inst, idx);

   rc_inline_literals(&cc.base, NULL);

   CHECK(!cc.base.Error, "representable immediate: no compiler error");
   CHECK(inst->U.I.SrcReg[0].File == RC_FILE_INLINE,
         "representable immediate folds to RC_FILE_INLINE");
   CHECK(inst->U.I.SrcReg[0].Index == R300_INLINE_TWO,
         "2.0f encodes to the 7-bit inline value 0x40");

   corpus_fs_destroy(&cc);
}

static void
case_negated_representable_folds_with_negate(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   unsigned idx = corpus_add_uniform_immediate(&cc.base, -2.0f);
   struct rc_instruction *inst = corpus_append_alu(&cc.base, RC_OPCODE_MOV);
   corpus_set_constant_src0(inst, idx);

   rc_inline_literals(&cc.base, NULL);

   CHECK(inst->U.I.SrcReg[0].File == RC_FILE_INLINE,
         "negated representable immediate folds to RC_FILE_INLINE");
   CHECK(inst->U.I.SrcReg[0].Index == R300_INLINE_TWO,
         "-2.0f encodes the magnitude 0x40 and carries the sign in Negate");
   CHECK(inst->U.I.SrcReg[0].Negate != 0,
         "negative immediate sets the per-channel Negate mask");

   corpus_fs_destroy(&cc);
}

static void
case_out_of_range_exponent_stays_constant(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   unsigned idx = corpus_add_uniform_immediate(&cc.base, 1.0e30f);
   struct rc_instruction *inst = corpus_append_alu(&cc.base, RC_OPCODE_MOV);
   corpus_set_constant_src0(inst, idx);

   rc_inline_literals(&cc.base, NULL);

   CHECK(inst->U.I.SrcReg[0].File == RC_FILE_CONSTANT,
         "exponent above +8 leaves the operand as RC_FILE_CONSTANT");
   CHECK(inst->U.I.SrcReg[0].Index == idx,
         "rejected immediate keeps its constant index");

   corpus_fs_destroy(&cc);
}

static void
case_low_mantissa_bits_stay_constant(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   /* 0.1f has an in-range exponent but sets low mantissa bits, so the
    * mantissa mask rejects it even though the exponent would fit. */
   unsigned idx = corpus_add_uniform_immediate(&cc.base, 0.1f);
   struct rc_instruction *inst = corpus_append_alu(&cc.base, RC_OPCODE_MOV);
   corpus_set_constant_src0(inst, idx);

   rc_inline_literals(&cc.base, NULL);

   CHECK(inst->U.I.SrcReg[0].File == RC_FILE_CONSTANT,
         "a set low-mantissa bit leaves the operand as RC_FILE_CONSTANT");

   corpus_fs_destroy(&cc);
}

static void
case_inconsistent_channels_stay_constant(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   /* 2.0f encodes 0x40 and 4.0f encodes 0x48; channels disagreeing on the
    * inline byte cannot share one literal, so the fold is abandoned. */
   const float vec[4] = {2.0f, 4.0f, 2.0f, 2.0f};
   unsigned idx = rc_constants_add_immediate_vec4(&cc.base.Program.Constants, vec);
   struct rc_instruction *inst = corpus_append_alu(&cc.base, RC_OPCODE_MOV);
   corpus_set_constant_src0(inst, idx);

   rc_inline_literals(&cc.base, NULL);

   CHECK(inst->U.I.SrcReg[0].File == RC_FILE_CONSTANT,
         "channels encoding different inline bytes are not folded");

   corpus_fs_destroy(&cc);
}

static void
case_non_constant_source_untouched(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   struct rc_instruction *inst = corpus_append_alu(&cc.base, RC_OPCODE_MOV);
   inst->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
   inst->U.I.SrcReg[0].Index = 3;
   inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_XYZW;

   rc_inline_literals(&cc.base, NULL);

   CHECK(inst->U.I.SrcReg[0].File == RC_FILE_TEMPORARY,
         "a non-constant source is left untouched");
   CHECK(inst->U.I.SrcReg[0].Index == 3,
         "a non-constant source keeps its index");

   corpus_fs_destroy(&cc);
}

static unsigned
corpus_count_instructions(struct radeon_compiler *c)
{
   unsigned count = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next)
      count++;
   return count;
}

static void
corpus_set_src(struct rc_instruction *inst, unsigned idx, unsigned file, unsigned index)
{
   inst->U.I.SrcReg[idx].File = file;
   inst->U.I.SrcReg[idx].Index = index;
   inst->U.I.SrcReg[idx].Swizzle = RC_SWIZZLE_XYZW;
}

/* True if any instruction still reads the constant file.  rc_optimize may
 * rewrite an ADD into a presubtract form, so scanning the program is safer
 * than holding a pointer to the original instruction across the pass. */
static bool
corpus_reads_constant_file(struct radeon_compiler *c)
{
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      const struct rc_opcode_info *info = rc_get_opcode_info(inst->U.I.Opcode);
      for (unsigned s = 0; s < info->NumSrcRegs; s++)
         if (inst->U.I.SrcReg[s].File == RC_FILE_CONSTANT)
            return true;
   }
   return false;
}

static struct rc_program_stats
corpus_get_stats(struct radeon_compiler *c)
{
   struct rc_program_stats stats = {0};
   rc_get_stats(c, &stats);
   return stats;
}

/* rc_optimize copy-propagates a MOV whose destination is a temporary into its
 * readers and then removes the MOV (radeon_optimize.c copy_propagate). */
static void
case_optimize_copy_propagates_mov(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   struct rc_instruction *mov = corpus_append_alu(&cc.base, RC_OPCODE_MOV);
   mov->U.I.DstReg.Index = 1;
   corpus_set_src(mov, 0, RC_FILE_INPUT, 0);

   struct rc_instruction *add = corpus_append_alu(&cc.base, RC_OPCODE_ADD);
   add->U.I.DstReg.Index = 2;
   corpus_set_src(add, 0, RC_FILE_TEMPORARY, 1);   /* reads the MOV destination */
   corpus_set_src(add, 1, RC_FILE_INPUT, 1);

   unsigned before = corpus_count_instructions(&cc.base);
   rc_optimize(&cc.base, NULL);
   unsigned after = corpus_count_instructions(&cc.base);

   CHECK(!cc.base.Error, "copy-propagate: no compiler error");
   CHECK(after == before - 1, "the redundant temporary MOV is copy-propagated away");
   struct rc_instruction *only = cc.base.Program.Instructions.Next;
   CHECK(only->U.I.Opcode == RC_OPCODE_ADD &&
         only->U.I.SrcReg[0].File == RC_FILE_INPUT &&
         only->U.I.SrcReg[0].Index == 0,
         "the reader's source is chained to the MOV's input");

   corpus_fs_destroy(&cc);
}

/* copy_propagate returns early unless the MOV destination is a temporary
 * (radeon_optimize.c:129), so an output MOV must survive. */
static void
case_optimize_keeps_output_mov(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   struct rc_instruction *mov = corpus_append_alu(&cc.base, RC_OPCODE_MOV);
   mov->U.I.DstReg.File = RC_FILE_OUTPUT;
   mov->U.I.DstReg.Index = 0;
   corpus_set_src(mov, 0, RC_FILE_TEMPORARY, 0);

   unsigned before = corpus_count_instructions(&cc.base);
   rc_optimize(&cc.base, NULL);
   unsigned after = corpus_count_instructions(&cc.base);

   CHECK(after == before,
         "an output MOV is not removed (copy_propagate requires a temporary dst)");
   CHECK(cc.base.Program.Instructions.Next->U.I.DstReg.File == RC_FILE_OUTPUT,
         "the surviving instruction still writes the output");

   corpus_fs_destroy(&cc);
}

/* constant_folding replaces an all-ones immediate with the ONE swizzle and
 * drops it out of the constant file (radeon_optimize.c:214). */
static void
case_optimize_folds_one_constant_to_none(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   unsigned idx = corpus_add_uniform_immediate(&cc.base, 1.0f);
   struct rc_instruction *add = corpus_append_alu(&cc.base, RC_OPCODE_ADD);
   corpus_set_constant_src0(add, idx);
   corpus_set_src(add, 1, RC_FILE_TEMPORARY, 0);

   rc_optimize(&cc.base, NULL);

   CHECK(!corpus_reads_constant_file(&cc.base),
         "an all-ones immediate is folded out of the constant file");

   corpus_fs_destroy(&cc);
}

/* A non-special immediate (2.0) has no constant swizzle, so constant_folding
 * leaves it in the constant file. */
static void
case_optimize_keeps_nonspecial_constant(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   unsigned idx = corpus_add_uniform_immediate(&cc.base, 2.0f);
   struct rc_instruction *add = corpus_append_alu(&cc.base, RC_OPCODE_ADD);
   corpus_set_constant_src0(add, idx);
   corpus_set_src(add, 1, RC_FILE_TEMPORARY, 0);

   rc_optimize(&cc.base, NULL);

   CHECK(corpus_reads_constant_file(&cc.base),
         "a non-special (2.0) immediate stays in the constant file");

   corpus_fs_destroy(&cc);
}

static void
case_stats_begin_tex_adds_penalty_for_texture_block(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   corpus_append_alu(&cc.base, RC_OPCODE_BEGIN_TEX);
   corpus_append_alu(&cc.base, RC_OPCODE_TEX);
   struct rc_program_stats stats = corpus_get_stats(&cc.base);

   CHECK(stats.type == RC_FRAGMENT_PROGRAM,
         "stats inherit the fragment-program type");
   CHECK(stats.num_tex_insts == 1,
         "BEGIN_TEX + TEX counts one texture instruction");
   CHECK(stats.num_cycles == 31,
         "BEGIN_TEX before a real texture block adds the 30-cycle penalty plus TEX");

   corpus_fs_destroy(&cc);
}

static void
case_stats_tex_without_begintex_keeps_base_cost(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   corpus_append_alu(&cc.base, RC_OPCODE_TEX);
   struct rc_program_stats stats = corpus_get_stats(&cc.base);

   CHECK(stats.num_tex_insts == 1,
         "a lone TEX counts one texture instruction without BEGIN_TEX");
   CHECK(stats.num_cycles == 1,
         "a lone TEX keeps its one-cycle base cost without the BEGIN_TEX penalty");

   corpus_fs_destroy(&cc);
}

static void
case_stats_kil_only_texblock_skips_penalty(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   corpus_append_alu(&cc.base, RC_OPCODE_BEGIN_TEX);
   corpus_append_alu(&cc.base, RC_OPCODE_KIL);
   corpus_append_alu(&cc.base, RC_OPCODE_NOP);
   struct rc_program_stats stats = corpus_get_stats(&cc.base);

   CHECK(stats.num_tex_insts == 0,
         "BEGIN_TEX + KIL + NOP keeps the tex count at zero");
   CHECK(stats.num_cycles == 2,
         "BEGIN_TEX before a kill-only block skips the penalty but still counts KIL and NOP");

   corpus_fs_destroy(&cc);
}

static void
case_stats_mad_three_temp_sources_adds_cycle(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   struct rc_instruction *mad = corpus_append_alu(&cc.base, RC_OPCODE_MAD);
   corpus_set_src(mad, 0, RC_FILE_TEMPORARY, 0);
   corpus_set_src(mad, 1, RC_FILE_TEMPORARY, 1);
   corpus_set_src(mad, 2, RC_FILE_TEMPORARY, 2);

   struct rc_program_stats stats = corpus_get_stats(&cc.base);

   CHECK(stats.num_cycles == 2,
         "MAD with three different temporaries pays the extra cycle on top of the base instruction");

   corpus_fs_destroy(&cc);
}

static void
case_stats_r300_semwait_credit_stays_disabled(void)
{
   struct corpus_compiler cc;
   corpus_fs_init(&cc);

   corpus_append_alu(&cc.base, RC_OPCODE_BEGIN_TEX);
   corpus_append_alu(&cc.base, RC_OPCODE_TEX);
   struct rc_instruction *pair = corpus_append_pair(&cc.base);
   pair->U.P.SemWait = 1;

   struct rc_program_stats stats = corpus_get_stats(&cc.base);

   CHECK(!cc.base.is_r500, "corpus stats run on the r300-class path");
   CHECK(stats.num_cycles == 32,
         "SemWait does not credit cycles back on r300-class hardware");

   corpus_fs_destroy(&cc);
}

int
main(void)
{
   printf("r300 compiler correctness corpus: rc_inline_literals\n");

   case_representable_folds_to_inline();
   case_negated_representable_folds_with_negate();
   case_out_of_range_exponent_stays_constant();
   case_low_mantissa_bits_stay_constant();
   case_inconsistent_channels_stay_constant();
   case_non_constant_source_untouched();

   printf("r300 compiler correctness corpus: rc_optimize\n");
   case_optimize_copy_propagates_mov();
   case_optimize_keeps_output_mov();
   case_optimize_folds_one_constant_to_none();
   case_optimize_keeps_nonspecial_constant();

   printf("r300 compiler correctness corpus: rc_get_stats\n");
   case_stats_begin_tex_adds_penalty_for_texture_block();
   case_stats_tex_without_begintex_keeps_base_cost();
   case_stats_kil_only_texblock_skips_penalty();
   case_stats_mad_three_temp_sources_adds_cycle();
   case_stats_r300_semwait_credit_stays_disabled();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
