/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "util/ralloc.h"

#include "classic/r300_classic_emit.h"
#include "classic/r300_classic_regalloc.h"
#include "classic/r300_classic_schedule.h"
#include "classic/r300_classic_select.h"
#include "classic/r300_classic_target.h"
#include "classic/r300_classic_validate_schedule.h"
#include "nir_to_rc.h"
#include "r300_fragprog_swizzle.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "r300_us_source_read.h"
#include "radeon_code.h"
#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_program_constants.h"
#include "radeon_program_pair.h"
#include "radeon_regalloc.h"

/* Oracle (a) on the SCHEDULED-AND-MERGED pair stream: r300_classic_schedule's
 * cross-instruction RGB+Alpha merge changes the pair multiset, so the
 * multiset-invariance proof r300_classic_new_sched_test.c uses for the
 * identity-permutation increment no longer stands in for value equality (see
 * classic-r300-fs-compiler-backend-scoping.md, Phase 1, item 6).  This file
 * adds a value-level CPU evaluator over the paired RGB/Alpha form itself and
 * checks it two ways: hand-built pair streams with a directly computable
 * expected result isolate one scheduler mechanism at a time (merge over
 * independent temporary writes, merge over disjoint output-channel writes, a
 * merge that legally refuses across mixed temp/output shapes, DP4's
 * raw-fourth-term read, DP3's three-term dot merged with an independent alpha
 * writer, the full RGB.DP3 + Alpha.DP3 pair whose Alpha lane broadcasts the
 * RGB pipe's dot, and the critical-path reorder tie-break); real-front-end
 * corpus shaders chain the pair evaluator to the existing nir_to_rc reference
 * oracle, the same two-front-end comparison r300_classic_parity_test.c runs at
 * the normal-IR level one stage later in the pipeline, covering the original
 * fract/scale merge and the RGB dot-product plus independent alpha writer
 * merge. */

static int failures;

#define CHECK(cond, what)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL: %s\n", what);                                \
         failures++;                                                        \
      }                                                                      \
   } while (0)

static bool
nearly_equal(float a, float b)
{
   return fabsf(a - b) <= 1e-4f;
}

/* ---------- pair-form CPU evaluator ----------
 *
 * Covers the opcode subset rc_pair_translate's final_rewrite ever leaves in
 * a pair (MAX, MAD, MIN, DP3, DP4 -- MOV/ADD/MUL always rewrite to MAX/MAD
 * before translation, and DP3/DP4 are the only opcodes classify_instruction
 * ever forces needrgb on regardless of write mask). */

struct pair_eval {
   float temps[64][4];
   float inputs[8][4];
   float outputs[4][4];
   float depth;
   const struct rc_constant_list *consts;
   const char *error;
   /* Execution profile for source-operand reads.  IDENTITY keeps the ideal
    * FP32 evaluator; RS48X_NEG_PREDECESSOR applies the measured RS482 US
    * read transform (r300_us_source_read.h) to input and temporary reads,
    * the two register files the sign-flip discriminator measured.  Constant
    * reads keep identity under both profiles: the constant-file read class
    * is unmeasured, and the model fails closed on conjecture. */
   enum r300_source_read_model src_read;
};

static bool
resolve_pair_reg(struct pair_eval *e, rc_register_file file, unsigned index, float *reg)
{
   switch (file) {
   case RC_FILE_TEMPORARY:
      memcpy(reg, e->temps[index & 63], sizeof(float[4]));
      return true;
   case RC_FILE_INPUT:
      memcpy(reg, e->inputs[index & 7], sizeof(float[4]));
      return true;
   case RC_FILE_CONSTANT:
      if (index >= e->consts->Count) {
         e->error = "constant index out of range";
         return false;
      }
      if (e->consts->Constants[index].Type != RC_CONSTANT_IMMEDIATE) {
         e->error = "non-immediate constant";
         return false;
      }
      memcpy(reg, e->consts->Constants[index].u.Immediate, sizeof(float[4]));
      return true;
   case RC_FILE_NONE:
      memset(reg, 0, sizeof(float[4]));
      return true;
   default:
      e->error = "unhandled pair source file";
      return false;
   }
}

static float
decode_pair_channel(const float *reg, unsigned swz)
{
   switch (swz) {
   case RC_SWIZZLE_X:    return reg[0];
   case RC_SWIZZLE_Y:    return reg[1];
   case RC_SWIZZLE_Z:    return reg[2];
   case RC_SWIZZLE_W:    return reg[3];
   case RC_SWIZZLE_ZERO: return 0.0f;
   case RC_SWIZZLE_ONE:  return 1.0f;
   case RC_SWIZZLE_HALF: return 0.5f;
   default:              return 0.0f;
   }
}

/* Resolve one Arg's source register.  rc_pair_get_src (radeon_program_pair.c)
 * is the canonical resolver: it picks pair->RGB.Src[] or pair->Alpha.Src[]
 * by walking the ARG'S OWN swizzle (rc_source_type_swz), not by which lane
 * owns the Arg -- a single instruction's RGB.Arg and Alpha.Arg can each
 * resolve into either Src[] array depending on whether their swizzle selects
 * an X/Y/Z-range channel or the W channel, since radeon_pair_translate.c's
 * set_pair_instruction shares source-slot allocation across both lanes for a
 * value one lane reads through the other pipe's read port.  A NULL return
 * means the swizzle only ever selects ZERO/ONE/HALF/UNUSED, so there is no
 * register to fetch at all. */
static bool
eval_arg_reg(struct pair_eval *e, struct rc_pair_instruction *pair, struct rc_pair_instruction_arg *arg,
            float reg[4])
{
   memset(reg, 0, sizeof(float[4]));
   struct rc_pair_instruction_source *src = rc_pair_get_src(pair, arg);
   if (!src || !src->Used)
      return true;
   if (!resolve_pair_reg(e, src->File, src->Index, reg))
      return false;
   /* The read transform lands between register resolution and the ABS/NEG
    * modifiers -- the ordering the sign-flip cell proved (NEG on an exact
    * positive exports exactly; a stored negative is already one ULP low
    * before its modifier applies).  Transforming the whole fetched register
    * equals transforming each swizzle-selected channel; ZERO/ONE/HALF
    * swizzle constants come from the swizzle hardware, not a register read,
    * and pass through untouched. */
   if (e->src_read == R300_SOURCE_READ_RS48X_NEG_PREDECESSOR &&
       (src->File == RC_FILE_INPUT || src->File == RC_FILE_TEMPORARY))
      for (unsigned ch = 0; ch < 4; ch++)
         reg[ch] = r300_us_source_read_f32(reg[ch], e->src_read);
   return true;
}

/* Resolve one RGB-lane argument's three channels.  DP3/DP4's fourth term
 * (channel 3) reads the resolved source's raw w component directly rather
 * than through Arg.Swizzle: rc_init_swizzle(swizzle, 3) always retires
 * channel 3 to RC_SWIZZLE_UNUSED for the RGB lane (radeon_pair_translate.c's
 * set_pair_instruction), matching r300FPTranslateRGBSwizzle's native-swizzle
 * lookup, which never encodes a channel-3 select for the RGB pipe -- the
 * hardware DP4 opcode dots the two selected registers' natural components,
 * it does not apply a fourth per-channel select. */
static bool
eval_rgb_arg(struct pair_eval *e, struct rc_pair_instruction *pair, struct rc_pair_sub_instruction *sub,
            unsigned arg_index, float out[4])
{
   struct rc_pair_instruction_arg *arg = &sub->Arg[arg_index];
   float reg[4];
   if (!eval_arg_reg(e, pair, arg, reg))
      return false;
   for (unsigned ch = 0; ch < 3; ch++) {
      float v = decode_pair_channel(reg, GET_SWZ(arg->Swizzle, ch));
      if (arg->Abs)
         v = fabsf(v);
      if (arg->Negate)
         v = -v;
      out[ch] = v;
   }
   float w = reg[3];
   if (arg->Abs)
      w = fabsf(w);
   if (arg->Negate)
      w = -w;
   out[3] = w;
   return true;
}

static bool
eval_alpha_arg(struct pair_eval *e, struct rc_pair_instruction *pair, struct rc_pair_sub_instruction *sub,
              unsigned arg_index, float *out_scalar)
{
   struct rc_pair_instruction_arg *arg = &sub->Arg[arg_index];
   float reg[4];
   if (!eval_arg_reg(e, pair, arg, reg))
      return false;
   float v = decode_pair_channel(reg, GET_SWZ(arg->Swizzle, 0));
   if (arg->Abs)
      v = fabsf(v);
   if (arg->Negate)
      v = -v;
   *out_scalar = v;
   return true;
}

static bool
eval_rgb_op(struct pair_eval *e, struct rc_pair_instruction *pair, struct rc_pair_sub_instruction *sub,
           float result[3])
{
   float s0[4], s1[4], s2[4];
   switch (sub->Opcode) {
   case RC_OPCODE_MAX:
      if (!eval_rgb_arg(e, pair, sub, 0, s0) || !eval_rgb_arg(e, pair, sub, 1, s1))
         return false;
      for (unsigned ch = 0; ch < 3; ch++)
         result[ch] = fmaxf(s0[ch], s1[ch]);
      return true;
   case RC_OPCODE_MIN:
      if (!eval_rgb_arg(e, pair, sub, 0, s0) || !eval_rgb_arg(e, pair, sub, 1, s1))
         return false;
      for (unsigned ch = 0; ch < 3; ch++)
         result[ch] = fminf(s0[ch], s1[ch]);
      return true;
   case RC_OPCODE_MAD:
      if (!eval_rgb_arg(e, pair, sub, 0, s0) || !eval_rgb_arg(e, pair, sub, 1, s1) ||
          !eval_rgb_arg(e, pair, sub, 2, s2))
         return false;
      for (unsigned ch = 0; ch < 3; ch++)
         result[ch] = s0[ch] * s1[ch] + s2[ch];
      return true;
   case RC_OPCODE_DP3: {
      if (!eval_rgb_arg(e, pair, sub, 0, s0) || !eval_rgb_arg(e, pair, sub, 1, s1))
         return false;
      const float d = s0[0] * s1[0] + s0[1] * s1[1] + s0[2] * s1[2];
      result[0] = result[1] = result[2] = d;
      return true;
   }
   case RC_OPCODE_DP4: {
      if (!eval_rgb_arg(e, pair, sub, 0, s0) || !eval_rgb_arg(e, pair, sub, 1, s1))
         return false;
      const float d =
         s0[0] * s1[0] + s0[1] * s1[1] + s0[2] * s1[2] + s0[3] * s1[3];
      result[0] = result[1] = result[2] = d;
      return true;
   }
   case RC_OPCODE_FRC:
      if (!eval_rgb_arg(e, pair, sub, 0, s0))
         return false;
      for (unsigned ch = 0; ch < 3; ch++)
         result[ch] = s0[ch] - floorf(s0[ch]);
      return true;
   default:
      e->error = "unhandled RGB pair opcode";
      return false;
   }
}

static bool
eval_alpha_op(struct pair_eval *e, struct rc_pair_instruction *pair, struct rc_pair_sub_instruction *sub,
             float *result)
{
   float s0, s1, s2;
   switch (sub->Opcode) {
   case RC_OPCODE_MAX:
      if (!eval_alpha_arg(e, pair, sub, 0, &s0) || !eval_alpha_arg(e, pair, sub, 1, &s1))
         return false;
      *result = fmaxf(s0, s1);
      return true;
   case RC_OPCODE_MIN:
      if (!eval_alpha_arg(e, pair, sub, 0, &s0) || !eval_alpha_arg(e, pair, sub, 1, &s1))
         return false;
      *result = fminf(s0, s1);
      return true;
   case RC_OPCODE_MAD:
      if (!eval_alpha_arg(e, pair, sub, 0, &s0) || !eval_alpha_arg(e, pair, sub, 1, &s1) ||
          !eval_alpha_arg(e, pair, sub, 2, &s2))
         return false;
      *result = s0 * s1 + s2;
      return true;
   case RC_OPCODE_FRC:
      if (!eval_alpha_arg(e, pair, sub, 0, &s0))
         return false;
      *result = s0 - floorf(s0);
      return true;
   default:
      e->error = "unhandled Alpha pair opcode";
      return false;
   }
}

static float
clamp01(float v)
{
   return fminf(fmaxf(v, 0.0f), 1.0f);
}

static bool
eval_pair_program(struct pair_eval *e, struct radeon_compiler *c)
{
   for (struct rc_instruction *i = c->Program.Instructions.Next;
        i != &c->Program.Instructions; i = i->Next) {
      if (i->Type != RC_INSTRUCTION_PAIR) {
         e->error = "unhandled non-pair instruction";
         return false;
      }
      struct rc_pair_instruction *p = &i->U.P;

      /* The Alpha lane in DP mode broadcasts the RGB pipe's dot rather than
       * computing its own (R300_ALU_OUTA_DP4), so the RGB lane's pre-saturate
       * channel-0 result is the value the Alpha DP lane reads that same cycle. */
      float rgb_dot = 0.0f;

      if (p->RGB.Opcode != RC_OPCODE_NOP) {
         float result[3];
         if (!eval_rgb_op(e, p, &p->RGB, result))
            return false;
         rgb_dot = result[0];
         if (p->RGB.Saturate)
            for (unsigned ch = 0; ch < 3; ch++)
               result[ch] = clamp01(result[ch]);
         for (unsigned ch = 0; ch < 3; ch++) {
            if (p->RGB.WriteMask & (1u << ch))
               e->temps[p->RGB.DestIndex & 63][ch] = result[ch];
            if (p->RGB.OutputWriteMask & (1u << ch))
               e->outputs[p->RGB.Target & 3][ch] = result[ch];
         }
      }
      if (p->Alpha.Opcode != RC_OPCODE_NOP) {
         float result;
         if (p->Alpha.Opcode == RC_OPCODE_DP3 || p->Alpha.Opcode == RC_OPCODE_DP4) {
            result = rgb_dot;
         } else if (!eval_alpha_op(e, p, &p->Alpha, &result)) {
            return false;
         }
         if (p->Alpha.Saturate)
            result = clamp01(result);
         if (p->Alpha.WriteMask)
            e->temps[p->Alpha.DestIndex & 63][3] = result;
         if (p->Alpha.OutputWriteMask)
            e->outputs[p->Alpha.Target & 3][3] = result;
         if (p->Alpha.DepthWriteMask)
            e->depth = result;
      }
   }
   return true;
}

/* ---------- hand-built pair corpus helpers ----------
 *
 * These shapes engineer one scheduler mechanism at a time (a genuine RAW
 * cycle, an independent RGB-only/Alpha-only pair, a critical-path height
 * difference) that no real front end emits as-is, the same reason
 * r300_classic_new_sched_test.c's safety tests build RC_INSTRUCTION_PAIR
 * directly rather than compiling a shader. */

static struct rc_instruction *
append_pair(struct radeon_compiler *c, struct rc_instruction *tail,
           const struct rc_pair_instruction *p)
{
   struct rc_instruction *inst = rc_insert_new_instruction(c, tail);
   inst->Type = RC_INSTRUCTION_PAIR;
   inst->U.P = *p;
   return inst;
}

static unsigned
count_pairs(const struct radeon_compiler *c)
{
   unsigned n = 0;
   for (const struct rc_instruction *i = c->Program.Instructions.Next;
        i != &c->Program.Instructions; i = i->Next)
      if (i->Type == RC_INSTRUCTION_PAIR)
         n++;
   return n;
}

static void
fs_compiler_init(struct r300_fragment_program_compiler *fc, struct rc_regalloc_state *rs)
{
   rc_init_regalloc_state(rs, RC_FRAGMENT_PROGRAM);
   memset(fc, 0, sizeof(*fc));
   rc_init(&fc->Base, rs);
   fc->Base.type = RC_FRAGMENT_PROGRAM;
   fc->Base.is_r500 = false;
   fc->Base.has_half_swizzles = true;
   fc->Base.has_presub = true;
   fc->Base.has_omod = true;
   fc->Base.max_temp_regs = 32;
   fc->Base.max_constants = 32;
   fc->Base.max_alu_insts = 64;
   fc->Base.max_tex_insts = 32;
   fc->Base.SwizzleCaps = &r300_swizzle_caps;
}

static unsigned
add_const(struct r300_fragment_program_compiler *fc, float x, float y, float z, float w)
{
   const float v[4] = {x, y, z, w};
   return rc_constants_add_immediate_vec4(&fc->Base.Program.Constants, v);
}

/* Two independent half-full pairs writing DISTINCT temporaries: an RGB-only
 * MAD (temp 5, xyz) and an Alpha-only MAX (temp 6, w).  Neither reads the
 * other's destination, so both are ready from the start and
 * r300_classic_schedule's merge search finds them without any budget
 * pressure forcing the issue -- the plain, unforced case for the mission's
 * headline mechanism. */
static void
test_merge_combines_independent_temp_writes(void)
{
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);

   const unsigned c0 = add_const(&fc, 2.0f, 3.0f, 4.0f, 0.0f);
   const unsigned c1 = add_const(&fc, 1.0f, 1.0f, 1.0f, 0.0f);
   const unsigned c2 = add_const(&fc, 0.0f, 0.0f, 0.0f, 0.7f);

   struct rc_pair_instruction rgb_only;
   memset(&rgb_only, 0, sizeof(rgb_only));
   rgb_only.RGB.Opcode = RC_OPCODE_MAD;
   rgb_only.RGB.WriteMask = RC_MASK_XYZ;
   rgb_only.RGB.DestIndex = 5;
   rgb_only.RGB.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c0};
   rgb_only.RGB.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c1};
   rgb_only.RGB.Src[2] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 0};
   rgb_only.RGB.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.RGB.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.RGB.Arg[2] = (struct rc_pair_instruction_arg){.Source = 2, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.Alpha.Opcode = RC_OPCODE_NOP;

   struct rc_pair_instruction alpha_only;
   memset(&alpha_only, 0, sizeof(alpha_only));
   alpha_only.RGB.Opcode = RC_OPCODE_NOP;
   alpha_only.Alpha.Opcode = RC_OPCODE_MAX;
   alpha_only.Alpha.WriteMask = RC_MASK_W;
   alpha_only.Alpha.DestIndex = 6;
   alpha_only.Alpha.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 1};
   alpha_only.Alpha.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c2};
   alpha_only.Alpha.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};
   alpha_only.Alpha.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_WWWW};

   struct rc_instruction *tail =
      append_pair(&fc.Base, &fc.Base.Program.Instructions, &rgb_only);
   append_pair(&fc.Base, tail, &alpha_only);

   CHECK(count_pairs(&fc.Base) == 2, "merge_temp: translated stream starts at two half-full pairs");

   int opt = 0;
   r300_classic_schedule(&fc.Base, &opt);

   CHECK(!fc.Base.Error, "merge_temp: schedule raises no error");
   CHECK(count_pairs(&fc.Base) == 1,
        "merge_temp: r300_classic_schedule merges the independent RGB-only and Alpha-only pairs itself");

   const char *reject = NULL;
   CHECK(r300_classic_validate_schedule(&fc.Base, &reject), "merge_temp: merged pair passes the legality oracle");
   if (reject)
      fprintf(stderr, "  legality: %s\n", reject);

   struct pair_eval e;
   memset(&e, 0, sizeof(e));
   e.consts = &fc.Base.Program.Constants;
   e.inputs[0][0] = 0.5f;
   e.inputs[0][1] = 0.5f;
   e.inputs[0][2] = 0.5f;
   e.inputs[1][3] = 0.2f;

   CHECK(eval_pair_program(&e, &fc.Base), "merge_temp: merged pair evaluates");
   if (e.error)
      fprintf(stderr, "  eval: %s\n", e.error);

   /* Expected: temp5.xyz = c0.xyz * c1.xyz + in0.xyz = (2,3,4)*1 + 0.5;
    * temp6.w = max(in1.w, c2.w) = max(0.2, 0.7). */
   CHECK(nearly_equal(e.temps[5][0], 2.5f) && nearly_equal(e.temps[5][1], 3.5f) &&
        nearly_equal(e.temps[5][2], 4.5f),
        "merge_temp: merged RGB lane still computes the RGB-only pair's own value");
   CHECK(nearly_equal(e.temps[6][3], 0.7f),
        "merge_temp: merged Alpha lane still computes the Alpha-only pair's own value");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* Two independent half-full pairs writing DISJOINT channels of the SAME
 * render target: an RGB-only DP4 (Target 0, xyz) and an Alpha-only MIN
 * (Target 0, w) -- the realistic "color from one path, alpha from another"
 * shape.  Per-(target, channel) output write-after-write tracking is what
 * makes both simultaneously ready despite writing the same export; a
 * whole-target chain (the pre-increment behavior) would have serialized
 * them and hidden this merge entirely. */
static void
test_merge_combines_disjoint_output_writers(void)
{
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);

   const unsigned c0 = add_const(&fc, 1.0f, 2.0f, 3.0f, 4.0f);
   const unsigned c1 = add_const(&fc, 0.0f, 0.0f, 0.0f, 0.9f);

   struct rc_pair_instruction rgb_only;
   memset(&rgb_only, 0, sizeof(rgb_only));
   rgb_only.RGB.Opcode = RC_OPCODE_DP4;
   rgb_only.RGB.Target = 0;
   rgb_only.RGB.OutputWriteMask = RC_MASK_XYZ;
   rgb_only.RGB.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c0};
   rgb_only.RGB.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 0};
   rgb_only.RGB.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.RGB.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.Alpha.Opcode = RC_OPCODE_NOP;

   struct rc_pair_instruction alpha_only;
   memset(&alpha_only, 0, sizeof(alpha_only));
   alpha_only.RGB.Opcode = RC_OPCODE_NOP;
   alpha_only.Alpha.Opcode = RC_OPCODE_MIN;
   alpha_only.Alpha.Target = 0;
   alpha_only.Alpha.OutputWriteMask = 1;
   alpha_only.Alpha.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 1};
   alpha_only.Alpha.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c1};
   alpha_only.Alpha.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};
   alpha_only.Alpha.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_WWWW};

   struct rc_instruction *tail =
      append_pair(&fc.Base, &fc.Base.Program.Instructions, &rgb_only);
   append_pair(&fc.Base, tail, &alpha_only);

   int opt = 0;
   r300_classic_schedule(&fc.Base, &opt);

   CHECK(!fc.Base.Error, "merge_output: schedule raises no error");
   CHECK(count_pairs(&fc.Base) == 1,
        "merge_output: disjoint-channel output writers to the same target merge into one pair");

   const char *reject = NULL;
   CHECK(r300_classic_validate_schedule(&fc.Base, &reject), "merge_output: merged pair passes the legality oracle");
   if (reject)
      fprintf(stderr, "  legality: %s\n", reject);

   struct pair_eval e;
   memset(&e, 0, sizeof(e));
   e.consts = &fc.Base.Program.Constants;
   e.inputs[0][0] = 1.0f;
   e.inputs[0][1] = 1.0f;
   e.inputs[0][2] = 1.0f;
   e.inputs[0][3] = 1.0f;
   e.inputs[1][3] = 0.3f;

   CHECK(eval_pair_program(&e, &fc.Base), "merge_output: merged pair evaluates");
   if (e.error)
      fprintf(stderr, "  eval: %s\n", e.error);

   /* Expected: OUTPUT[0].xyz = dp4(c0, in0) replicated = 1+2+3+4 = 10;
    * OUTPUT[0].w = min(in1.w, c1.w) = min(0.3, 0.9). */
   CHECK(nearly_equal(e.outputs[0][0], 10.0f) && nearly_equal(e.outputs[0][1], 10.0f) &&
        nearly_equal(e.outputs[0][2], 10.0f),
        "merge_output: merged RGB lane still computes the DP4-only pair's own value");
   CHECK(nearly_equal(e.outputs[0][3], 0.3f),
        "merge_output: merged Alpha lane still computes the MIN-only pair's own value");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* The DP3 sibling of test_merge_combines_disjoint_output_writers: an RGB-only
 * DP3 (Target 0, xyz) and an Alpha-only MIN (Target 0, w).  RC_OPCODE_DP3
 * takes eval_rgb_op's three-term dot path, distinct from the four-term DP4
 * case its sibling drives; classify_instruction leaves needalpha clear for a
 * DP3 that writes only xyz (radeon_pair_translate.c), so a dot3-to-color pass
 * translates to exactly this RGB-only DP3 and merges with an independent
 * alpha writer.  The merged pair's Alpha lane is MIN, not a dot, so
 * check_alpha_reads_rgb_dot stays inert here -- the full RGB.DP3 + Alpha.DP3
 * broadcast pairing is exercised by test_dp3_full_pair_broadcast. */
static void
test_merge_combines_dp3_output_writers(void)
{
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);

   const unsigned c0 = add_const(&fc, 1.0f, 2.0f, 3.0f, 4.0f);
   const unsigned c1 = add_const(&fc, 0.0f, 0.0f, 0.0f, 0.9f);

   struct rc_pair_instruction rgb_only;
   memset(&rgb_only, 0, sizeof(rgb_only));
   rgb_only.RGB.Opcode = RC_OPCODE_DP3;
   rgb_only.RGB.Target = 0;
   rgb_only.RGB.OutputWriteMask = RC_MASK_XYZ;
   rgb_only.RGB.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c0};
   rgb_only.RGB.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 0};
   rgb_only.RGB.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.RGB.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.Alpha.Opcode = RC_OPCODE_NOP;

   struct rc_pair_instruction alpha_only;
   memset(&alpha_only, 0, sizeof(alpha_only));
   alpha_only.RGB.Opcode = RC_OPCODE_NOP;
   alpha_only.Alpha.Opcode = RC_OPCODE_MIN;
   alpha_only.Alpha.Target = 0;
   alpha_only.Alpha.OutputWriteMask = 1;
   alpha_only.Alpha.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 1};
   alpha_only.Alpha.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c1};
   alpha_only.Alpha.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};
   alpha_only.Alpha.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_WWWW};

   struct rc_instruction *tail =
      append_pair(&fc.Base, &fc.Base.Program.Instructions, &rgb_only);
   append_pair(&fc.Base, tail, &alpha_only);

   int opt = 0;
   r300_classic_schedule(&fc.Base, &opt);

   CHECK(!fc.Base.Error, "merge_dp3: schedule raises no error");
   CHECK(count_pairs(&fc.Base) == 1,
        "merge_dp3: the RGB-only DP3 and the independent Alpha-only MIN merge into one pair");

   const char *reject = NULL;
   CHECK(r300_classic_validate_schedule(&fc.Base, &reject), "merge_dp3: merged pair passes the legality oracle");
   if (reject)
      fprintf(stderr, "  legality: %s\n", reject);

   struct pair_eval e;
   memset(&e, 0, sizeof(e));
   e.consts = &fc.Base.Program.Constants;
   e.inputs[0][0] = 1.0f;
   e.inputs[0][1] = 1.0f;
   e.inputs[0][2] = 1.0f;
   e.inputs[0][3] = 1.0f;
   e.inputs[1][3] = 0.3f;

   CHECK(eval_pair_program(&e, &fc.Base), "merge_dp3: merged pair evaluates");
   if (e.error)
      fprintf(stderr, "  eval: %s\n", e.error);

   /* Expected: OUTPUT[0].xyz = dp3(c0.xyz, in0.xyz) replicated = 1+2+3 = 6;
    * OUTPUT[0].w = min(in1.w, c1.w) = min(0.3, 0.9). */
   CHECK(nearly_equal(e.outputs[0][0], 6.0f) && nearly_equal(e.outputs[0][1], 6.0f) &&
        nearly_equal(e.outputs[0][2], 6.0f),
        "merge_dp3: merged RGB lane still computes the DP3-only pair's three-term dot");
   CHECK(nearly_equal(e.outputs[0][3], 0.3f),
        "merge_dp3: merged Alpha lane still computes the MIN-only pair's own value");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* An RGB-only pair that writes OUTPUT and an Alpha-only pair that writes a
 * TEMPORARY must NOT merge: rc_pair_try_merge (radeon_pair_schedule.c's
 * merge_instructions) refuses pairing an output write with a temp write.
 * Both stay ready simultaneously (no data or output dependency links them),
 * so this exercises the refusal path rather than assuming it. */
static void
test_merge_refuses_mixed_output_and_temp(void)
{
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);

   struct rc_pair_instruction rgb_only;
   memset(&rgb_only, 0, sizeof(rgb_only));
   rgb_only.RGB.Opcode = RC_OPCODE_MAX;
   rgb_only.RGB.Target = 0;
   rgb_only.RGB.OutputWriteMask = RC_MASK_XYZ;
   rgb_only.RGB.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 0};
   rgb_only.RGB.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.RGB.Arg[1] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.Alpha.Opcode = RC_OPCODE_NOP;

   struct rc_pair_instruction alpha_only;
   memset(&alpha_only, 0, sizeof(alpha_only));
   alpha_only.RGB.Opcode = RC_OPCODE_NOP;
   alpha_only.Alpha.Opcode = RC_OPCODE_MAX;
   alpha_only.Alpha.WriteMask = RC_MASK_W;
   alpha_only.Alpha.DestIndex = 9;
   alpha_only.Alpha.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 1};
   alpha_only.Alpha.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};
   alpha_only.Alpha.Arg[1] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};

   struct rc_instruction *tail =
      append_pair(&fc.Base, &fc.Base.Program.Instructions, &rgb_only);
   append_pair(&fc.Base, tail, &alpha_only);

   int opt = 0;
   r300_classic_schedule(&fc.Base, &opt);

   CHECK(!fc.Base.Error, "merge_refuse: schedule raises no error");
   CHECK(count_pairs(&fc.Base) == 2,
        "merge_refuse: an output writer and a temp writer stay two separate pairs");

   const char *reject = NULL;
   CHECK(r300_classic_validate_schedule(&fc.Base, &reject), "merge_refuse: unmerged stream still passes the legality oracle");
   if (reject)
      fprintf(stderr, "  legality: %s\n", reject);

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* Two independent half-full pairs under a one-slot budget: merging is what
 * lets r300_classic_schedule fit them itself, without ever calling
 * rc_pair_schedule. */
static void
test_compaction_merges_within_budget(void)
{
   struct rc_pair_instruction rgb_only, alpha_only;

   memset(&rgb_only, 0, sizeof(rgb_only));
   rgb_only.RGB.Opcode = RC_OPCODE_MAX;
   rgb_only.RGB.WriteMask = RC_MASK_XYZ;
   rgb_only.RGB.DestIndex = 0;
   rgb_only.RGB.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 0};
   rgb_only.RGB.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.RGB.Arg[1] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   rgb_only.Alpha.Opcode = RC_OPCODE_NOP;

   memset(&alpha_only, 0, sizeof(alpha_only));
   alpha_only.RGB.Opcode = RC_OPCODE_NOP;
   alpha_only.Alpha.Opcode = RC_OPCODE_MAX;
   alpha_only.Alpha.WriteMask = RC_MASK_W;
   alpha_only.Alpha.DestIndex = 1;
   alpha_only.Alpha.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 1};
   alpha_only.Alpha.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};
   alpha_only.Alpha.Arg[1] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};

   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);
   fc.Base.max_alu_insts = 1;
   struct rc_instruction *tail =
      append_pair(&fc.Base, &fc.Base.Program.Instructions, &rgb_only);
   append_pair(&fc.Base, tail, &alpha_only);

   CHECK(count_pairs(&fc.Base) == 2, "compaction: the translated stream starts at two half-full pairs");

   int opt = 0;
   r300_classic_schedule(&fc.Base, &opt);

   CHECK(!fc.Base.Error, "compaction: schedule raises no error");
   CHECK(count_pairs(&fc.Base) == 1,
        "compaction: r300_classic_schedule's own merge fits the one-slot budget without deferring");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* Four hand-built pairs in program order [b0, a0, a1, a2]: b0 is an
 * independent Alpha-only write to an unrelated temporary with no
 * dependents (height 1); a0 -> a1 -> a2 is a three-deep dependent chain
 * (heights 3, 2, 1).  At the very first decision, b0 and a0 are both ready;
 * program-order tie-breaking would place b0 first (smaller original index),
 * but critical-path height prioritizes a0 (height 3 against b0's 1) --
 * scheduling the longer remaining chain of work first.  a2 writes OUTPUT
 * while b0 writes a TEMPORARY, so rc_pair_try_merge refuses to combine them
 * (test_merge_refuses_mixed_output_and_temp proves that refusal directly);
 * this corpus stays a pure-reorder proof with the pair count unchanged. */
static void
test_reorder_prioritizes_critical_path(void)
{
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);

   const unsigned c_mad0 = add_const(&fc, 2.0f, 2.0f, 2.0f, 2.0f);
   const unsigned c_mad1 = add_const(&fc, 1.0f, 1.0f, 1.0f, 1.0f);
   const unsigned c_mad2 = add_const(&fc, 3.0f, 3.0f, 3.0f, 3.0f);

   struct rc_pair_instruction b0;
   memset(&b0, 0, sizeof(b0));
   b0.RGB.Opcode = RC_OPCODE_NOP;
   b0.Alpha.Opcode = RC_OPCODE_MAX;
   b0.Alpha.WriteMask = RC_MASK_W;
   b0.Alpha.DestIndex = 20;
   b0.Alpha.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 2};
   b0.Alpha.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};
   b0.Alpha.Arg[1] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};

   /* a0: temp21 = in3 * c_mad0 + c_mad1 (full pair, both lanes). */
   struct rc_pair_instruction a0;
   memset(&a0, 0, sizeof(a0));
   a0.RGB.Opcode = RC_OPCODE_MAD;
   a0.RGB.WriteMask = RC_MASK_XYZ;
   a0.RGB.DestIndex = 21;
   a0.RGB.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 3};
   a0.RGB.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c_mad0};
   a0.RGB.Src[2] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c_mad1};
   a0.RGB.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   a0.RGB.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_XYZW};
   a0.RGB.Arg[2] = (struct rc_pair_instruction_arg){.Source = 2, .Swizzle = RC_SWIZZLE_XYZW};
   a0.Alpha.Opcode = RC_OPCODE_MAD;
   a0.Alpha.WriteMask = RC_MASK_W;
   a0.Alpha.DestIndex = 21;
   a0.Alpha.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 3};
   a0.Alpha.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c_mad0};
   a0.Alpha.Src[2] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c_mad1};
   a0.Alpha.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};
   a0.Alpha.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_WWWW};
   a0.Alpha.Arg[2] = (struct rc_pair_instruction_arg){.Source = 2, .Swizzle = RC_SWIZZLE_WWWW};

   /* a1: temp22 = temp21 + c_mad2 (full pair, both lanes; MAD form of ADD). */
   struct rc_pair_instruction a1;
   memset(&a1, 0, sizeof(a1));
   a1.RGB.Opcode = RC_OPCODE_MAD;
   a1.RGB.WriteMask = RC_MASK_XYZ;
   a1.RGB.DestIndex = 22;
   a1.RGB.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_TEMPORARY, .Index = 21};
   a1.RGB.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_NONE};
   a1.RGB.Src[2] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c_mad2};
   a1.RGB.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   a1.RGB.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_1111};
   a1.RGB.Arg[2] = (struct rc_pair_instruction_arg){.Source = 2, .Swizzle = RC_SWIZZLE_XYZW};
   a1.Alpha.Opcode = RC_OPCODE_MAD;
   a1.Alpha.WriteMask = RC_MASK_W;
   a1.Alpha.DestIndex = 22;
   a1.Alpha.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_TEMPORARY, .Index = 21};
   a1.Alpha.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_NONE};
   a1.Alpha.Src[2] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c_mad2};
   a1.Alpha.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};
   a1.Alpha.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_1111};
   a1.Alpha.Arg[2] = (struct rc_pair_instruction_arg){.Source = 2, .Swizzle = RC_SWIZZLE_WWWW};

   /* a2: OUTPUT[0].xyz = temp22.xyz (RGB-only passthrough). */
   struct rc_pair_instruction a2;
   memset(&a2, 0, sizeof(a2));
   a2.RGB.Opcode = RC_OPCODE_MAX;
   a2.RGB.Target = 0;
   a2.RGB.OutputWriteMask = RC_MASK_XYZ;
   a2.RGB.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_TEMPORARY, .Index = 22};
   a2.RGB.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   a2.RGB.Arg[1] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   a2.Alpha.Opcode = RC_OPCODE_NOP;

   struct rc_instruction *tail = append_pair(&fc.Base, &fc.Base.Program.Instructions, &b0);
   tail = append_pair(&fc.Base, tail, &a0);
   tail = append_pair(&fc.Base, tail, &a1);
   append_pair(&fc.Base, tail, &a2);

   int opt = 0;
   r300_classic_schedule(&fc.Base, &opt);

   CHECK(!fc.Base.Error, "reorder: schedule raises no error");
   CHECK(count_pairs(&fc.Base) == 4,
        "reorder: no merge is available (a2 exports OUTPUT, b0 writes a TEMPORARY), pure reorder");

   int pos_b0 = -1, pos_a0 = -1, pos = 0;
   for (struct rc_instruction *i = fc.Base.Program.Instructions.Next;
        i != &fc.Base.Program.Instructions; i = i->Next, pos++) {
      if (i->Type != RC_INSTRUCTION_PAIR)
         continue;
      if (i->U.P.Alpha.Opcode == RC_OPCODE_MAX && i->U.P.Alpha.DestIndex == 20)
         pos_b0 = pos;
      if (i->U.P.RGB.Opcode == RC_OPCODE_MAD && i->U.P.RGB.DestIndex == 21)
         pos_a0 = pos;
   }
   CHECK(pos_b0 >= 0 && pos_a0 >= 0, "reorder: both b0 and a0 survive scheduling");
   CHECK(pos_a0 < pos_b0,
        "reorder: critical-path height schedules the three-deep chain's head (a0) before the "
        "independent, dependent-free b0, reversing their original program order");

   const char *reject = NULL;
   CHECK(r300_classic_validate_schedule(&fc.Base, &reject), "reorder: reordered stream passes the legality oracle");
   if (reject)
      fprintf(stderr, "  legality: %s\n", reject);

   struct pair_eval e;
   memset(&e, 0, sizeof(e));
   e.consts = &fc.Base.Program.Constants;
   e.inputs[2][3] = 0.42f;
   e.inputs[3][0] = 0.25f;
   e.inputs[3][1] = 0.25f;
   e.inputs[3][2] = 0.25f;
   e.inputs[3][3] = 0.25f;

   CHECK(eval_pair_program(&e, &fc.Base), "reorder: reordered stream evaluates");
   if (e.error)
      fprintf(stderr, "  eval: %s\n", e.error);

   /* temp21 = in3*2 + 1 = 1.5; temp22 = temp21 + 3 = 4.5;
    * OUTPUT[0].xyz = temp22 = 4.5; temp20.w = in2.w = 0.42 (unread but
    * still computed, proving the reorder did not drop it). */
   CHECK(nearly_equal(e.outputs[0][0], 4.5f) && nearly_equal(e.outputs[0][1], 4.5f) &&
        nearly_equal(e.outputs[0][2], 4.5f),
        "reorder: the reordered chain still computes the same value the program order would have");
   CHECK(nearly_equal(e.temps[20][3], 0.42f),
        "reorder: the reordered independent write still lands");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* ---------- real-front-end corpus: merge chained to the nir_to_rc oracle ----------
 *
 * gl_FragColor.rgb from one computation and gl_FragColor.a from an
 * independent one, mirroring r300_classic_parity_test.c's
 * build_masked_output_store shape but split XYZ/W so classic-select's two
 * export instructions land as an RGB-only pair and an Alpha-only pair after
 * rc_pair_translate -- the natural, unforced occurrence of this increment's
 * headline merge in real compiled code, not just a hand-built corpus. */

static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   memset(s, 0, sizeof(*s));
   s->caps.has_tcl = true;
   return (struct pipe_screen *)s;
}

static bool
compile_classic(void *ctx, nir_shader *s, struct r300_fragment_program_compiler *fc,
                struct rc_regalloc_state *rs, const char **why)
{
   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(s, r300_screen(ps));

   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result sel;
   if (!r300_classic_select(ctx, s, t, NULL, 0, R300_FS_INPUT_INTERPOLATED, NULL, &sel) || !sel.program) {
      *why = sel.reject_reason ? sel.reject_reason : "selection failed";
      return false;
   }
   struct r300_classic_regalloc_result ra;
   if (!r300_classic_regalloc(ctx, sel.program, &ra) || !ra.temp_of_ssa) {
      *why = ra.reject_reason ? ra.reject_reason : "allocation failed";
      return false;
   }
   fs_compiler_init(fc, rs);
   if (!r300_classic_emit(sel.program, &sel.immediates, &sel.states, fc)) {
      *why = "emission failed";
      return false;
   }
   return true;
}

static bool
compile_reference(nir_shader *s, struct r300_fragment_program_compiler *fc,
                  struct rc_regalloc_state *rs, struct r300_fragment_shader_code *fs_code)
{
   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state zero_ext = {0};
   union r300_shader_code code = {.f = fs_code};

   fs_compiler_init(fc, rs);
   r300_optimize_nir(s, r300_screen(ps));
   nir_to_rc(s, ps, zero_ext, code, &fc->Base);
   return !fc->Base.Error;
}

/* Small normal-IR reference evaluator, scoped to this file's corpus (fmov,
 * fmad, and the constant/varying loads r300_optimize_nir leaves after
 * lowering) -- a fresh, self-contained copy, matching this test tree's
 * existing convention of not sharing an evaluator across test binaries
 * (r300_classic_new_sched_test.c and r300_classic_parity_test.c each keep
 * their own compile helpers rather than importing one another's). */
struct normal_eval {
   float temps[128][4];
   float inputs[8][4];
   float outputs[8][4];
   const struct rc_constant_list *consts;
   const char *error;
};

static bool
eval_normal_src(struct normal_eval *e, const struct rc_src_register *src, float *out)
{
   float reg[4] = {0, 0, 0, 0};
   switch (src->File) {
   case RC_FILE_TEMPORARY:
      memcpy(reg, e->temps[src->Index & 127], sizeof(reg));
      break;
   case RC_FILE_INPUT:
      memcpy(reg, e->inputs[src->Index & 7], sizeof(reg));
      break;
   case RC_FILE_CONSTANT: {
      if (src->Index >= e->consts->Count) {
         e->error = "constant index out of range";
         return false;
      }
      const struct rc_constant *c = &e->consts->Constants[src->Index];
      if (c->Type != RC_CONSTANT_IMMEDIATE) {
         e->error = "non-immediate constant";
         return false;
      }
      memcpy(reg, c->u.Immediate, sizeof(reg));
      break;
   }
   case RC_FILE_NONE:
      break;
   default:
      e->error = "unhandled source file";
      return false;
   }
   for (unsigned ch = 0; ch < 4; ch++) {
      out[ch] = decode_pair_channel(reg, GET_SWZ(src->Swizzle, ch));
      if (src->Abs)
         out[ch] = fabsf(out[ch]);
      if (src->Negate & (1u << ch))
         out[ch] = -out[ch];
   }
   return true;
}

static bool
eval_normal_program(struct normal_eval *e, struct radeon_compiler *c)
{
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      const struct rc_opcode_info *info = rc_get_opcode_info(inst->U.I.Opcode);
      float s[3][4];
      for (unsigned n = 0; n < info->NumSrcRegs; n++)
         if (!eval_normal_src(e, &inst->U.I.SrcReg[n], s[n]))
            return false;

      float r[4] = {0, 0, 0, 0};
      switch (inst->U.I.Opcode) {
      case RC_OPCODE_MOV:
         memcpy(r, s[0], sizeof(r));
         break;
      case RC_OPCODE_MUL:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = s[0][ch] * s[1][ch];
         break;
      case RC_OPCODE_ADD:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = s[0][ch] + s[1][ch];
         break;
      case RC_OPCODE_MAD:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = s[0][ch] * s[1][ch] + s[2][ch];
         break;
      case RC_OPCODE_DP3: {
         const float d = s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[0][2] * s[1][2];
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = d;
         break;
      }
      case RC_OPCODE_DP4: {
         const float d =
            s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[0][2] * s[1][2] + s[0][3] * s[1][3];
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = d;
         break;
      }
      case RC_OPCODE_FRC:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = s[0][ch] - floorf(s[0][ch]);
         break;
      default:
         e->error = info->Name;
         return false;
      }

      if (!info->HasDstReg)
         continue;

      float *dst;
      if (inst->U.I.DstReg.File == RC_FILE_TEMPORARY)
         dst = e->temps[inst->U.I.DstReg.Index & 127];
      else if (inst->U.I.DstReg.File == RC_FILE_OUTPUT)
         dst = e->outputs[inst->U.I.DstReg.Index & 7];
      else {
         e->error = "unhandled destination file";
         return false;
      }
      for (unsigned ch = 0; ch < 4; ch++)
         if (inst->U.I.DstReg.WriteMask & (1u << ch))
            dst[ch] = r[ch];
   }
   return true;
}

static nir_builder
fs_builder(const char *name)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
      .fdot_replicates = true,
   };
   return nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "%s", name);
}

static nir_variable *
add_varying(nir_builder *b)
{
   nir_variable *in = nir_variable_create(b->shader, nir_var_shader_in, glsl_vec4_type(), "in0");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   return in;
}

static nir_variable *
add_color_output(nir_builder *b)
{
   nir_variable *out =
      nir_variable_create(b->shader, nir_var_shader_out, glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;
   return out;
}

/* rgb = fract(in0.xyz * 2); a = in0.w * 0.5 -- two data-independent stores to
 * the same output variable with the XYZ/W split. */
static nir_shader *
build_rgb_alpha_independent(void)
{
   nir_builder b = fs_builder("pairvalue_rgba_indep");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *rgb = nir_ffract(&b, nir_fmul_imm(&b, nir_trim_vector(&b, v, 3), 2.0f));
   nir_def *a = nir_fmul_imm(&b, nir_channel(&b, v, 3), 0.5f);
   nir_def *unused = nir_imm_float(&b, 0.0f);

   nir_store_var(&b, out, nir_vec4(&b, nir_channel(&b, rgb, 0), nir_channel(&b, rgb, 1),
                                   nir_channel(&b, rgb, 2), unused),
                0x7);
   nir_store_var(&b, out, nir_vec4(&b, unused, unused, unused, a), 0x8);
   return b.shader;
}

static void
test_real_corpus_merge_matches_nir_to_rc(void)
{
   void *ctx = ralloc_context(NULL);
   nir_shader *for_ref = build_rgb_alpha_independent();
   nir_shader *for_classic = build_rgb_alpha_independent();

   struct r300_fragment_program_compiler ref_fc, cls_fc;
   struct rc_regalloc_state ref_rs, cls_rs;
   struct r300_fragment_shader_code ref_code;
   memset(&ref_code, 0, sizeof(ref_code));

   const bool ref_ok = compile_reference(for_ref, &ref_fc, &ref_rs, &ref_code);
   CHECK(ref_ok, "real_corpus: reference front end compiles");

   const char *why = NULL;
   const bool cls_ok = compile_classic(ctx, for_classic, &cls_fc, &cls_rs, &why);
   CHECK(cls_ok, "real_corpus: classic front end compiles");
   if (!cls_ok && why)
      fprintf(stderr, "  classic: %s\n", why);

   if (ref_ok && cls_ok) {
      struct normal_eval ref_eval;
      memset(&ref_eval, 0, sizeof(ref_eval));
      ref_eval.consts = &ref_fc.Base.Program.Constants;
      for (unsigned ch = 0; ch < 4; ch++)
         ref_eval.inputs[0][ch] = 0.6f + (float)ch * 0.1f;
      CHECK(eval_normal_program(&ref_eval, &ref_fc.Base), "real_corpus: reference program evaluates");
      if (ref_eval.error)
         fprintf(stderr, "  reference eval: %s\n", ref_eval.error);

      int opt = 0;
      rc_pair_translate(&cls_fc.Base, &opt);
      const unsigned pre_schedule_count = count_pairs(&cls_fc.Base);

      r300_classic_schedule(&cls_fc.Base, &opt);
      const unsigned post_schedule_count = count_pairs(&cls_fc.Base);

      CHECK(!cls_fc.Base.Error, "real_corpus: schedule raises no error");
      CHECK(post_schedule_count < pre_schedule_count,
           "real_corpus: the independent XYZ/W stores merge into fewer pairs than translate produced");

      const char *reject = NULL;
      CHECK(r300_classic_validate_schedule(&cls_fc.Base, &reject),
           "real_corpus: merged classic schedule passes the legality oracle");
      if (reject)
         fprintf(stderr, "  legality: %s\n", reject);

      struct pair_eval cls_eval;
      memset(&cls_eval, 0, sizeof(cls_eval));
      cls_eval.consts = &cls_fc.Base.Program.Constants;
      for (unsigned ch = 0; ch < 4; ch++)
         cls_eval.inputs[0][ch] = 0.6f + (float)ch * 0.1f;
      CHECK(eval_pair_program(&cls_eval, &cls_fc.Base), "real_corpus: merged classic schedule evaluates");
      if (cls_eval.error)
         fprintf(stderr, "  classic eval: %s\n", cls_eval.error);

      for (unsigned ch = 0; ch < 4; ch++) {
         const float d = fabsf(ref_eval.outputs[cls_fc.OutputColor[0] & 7][ch] -
                               cls_eval.outputs[0][ch]);
         CHECK(d <= 1e-4f, "real_corpus: merged-and-scheduled classic output matches the nir_to_rc reference");
         if (d > 1e-4f)
            fprintf(stderr, "  channel %u: reference %g classic %g\n", ch,
                   ref_eval.outputs[cls_fc.OutputColor[0] & 7][ch], cls_eval.outputs[0][ch]);
      }
   }

   if (ref_ok) {
      rc_destroy(&ref_fc.Base);
      rc_destroy_regalloc_state(&ref_rs);
   }
   if (cls_ok) {
      rc_destroy(&cls_fc.Base);
      rc_destroy_regalloc_state(&cls_rs);
   }
   ralloc_free(ctx);
}

/* rgb = dot3(in0.xyz, const.xyz) broadcast; a = in0.w * 0.5.  The dot3 writes
 * only xyz, so classify_instruction leaves needalpha clear (radeon_pair_translate.c)
 * and pair translate emits an RGB-only DP3; the alpha store becomes an
 * independent Alpha-only pair, and r300_classic_schedule merges the two.  This
 * is the real-front-end occurrence of the RGB dot-product + independent alpha
 * writer merge, chained to the nir_to_rc reference the same way
 * test_real_corpus_merge_matches_nir_to_rc chains its fract/scale shader. */
static nir_shader *
build_rgb_dot3_alpha_independent(void)
{
   nir_builder b = fs_builder("pairvalue_dp3_indep");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c = nir_imm_vec4(&b, 0.5f, 0.25f, 0.75f, 0.0f);
   nir_def *d = nir_fdot(&b, nir_trim_vector(&b, v, 3), nir_trim_vector(&b, c, 3));
   nir_def *a = nir_fmul_imm(&b, nir_channel(&b, v, 3), 0.5f);
   nir_def *unused = nir_imm_float(&b, 0.0f);

   nir_store_var(&b, out, nir_vec4(&b, d, d, d, unused), 0x7);
   nir_store_var(&b, out, nir_vec4(&b, unused, unused, unused, a), 0x8);
   return b.shader;
}

static void
test_real_corpus_dp3_merge_matches_nir_to_rc(void)
{
   void *ctx = ralloc_context(NULL);
   nir_shader *for_ref = build_rgb_dot3_alpha_independent();
   nir_shader *for_classic = build_rgb_dot3_alpha_independent();

   struct r300_fragment_program_compiler ref_fc, cls_fc;
   struct rc_regalloc_state ref_rs, cls_rs;
   struct r300_fragment_shader_code ref_code;
   memset(&ref_code, 0, sizeof(ref_code));

   const bool ref_ok = compile_reference(for_ref, &ref_fc, &ref_rs, &ref_code);
   CHECK(ref_ok, "dp3_corpus: reference front end compiles");

   const char *why = NULL;
   const bool cls_ok = compile_classic(ctx, for_classic, &cls_fc, &cls_rs, &why);
   CHECK(cls_ok, "dp3_corpus: classic front end compiles");
   if (!cls_ok && why)
      fprintf(stderr, "  classic: %s\n", why);

   if (ref_ok && cls_ok) {
      struct normal_eval ref_eval;
      memset(&ref_eval, 0, sizeof(ref_eval));
      ref_eval.consts = &ref_fc.Base.Program.Constants;
      for (unsigned ch = 0; ch < 4; ch++)
         ref_eval.inputs[0][ch] = 0.6f + (float)ch * 0.1f;
      CHECK(eval_normal_program(&ref_eval, &ref_fc.Base), "dp3_corpus: reference program evaluates");
      if (ref_eval.error)
         fprintf(stderr, "  reference eval: %s\n", ref_eval.error);

      int opt = 0;
      rc_pair_translate(&cls_fc.Base, &opt);
      const unsigned pre_schedule_count = count_pairs(&cls_fc.Base);

      r300_classic_schedule(&cls_fc.Base, &opt);
      const unsigned post_schedule_count = count_pairs(&cls_fc.Base);

      CHECK(!cls_fc.Base.Error, "dp3_corpus: schedule raises no error");
      CHECK(post_schedule_count < pre_schedule_count,
           "dp3_corpus: the RGB-only DP3 and the independent alpha store merge into fewer pairs");

      const char *reject = NULL;
      CHECK(r300_classic_validate_schedule(&cls_fc.Base, &reject),
           "dp3_corpus: merged classic schedule passes the legality oracle");
      if (reject)
         fprintf(stderr, "  legality: %s\n", reject);

      struct pair_eval cls_eval;
      memset(&cls_eval, 0, sizeof(cls_eval));
      cls_eval.consts = &cls_fc.Base.Program.Constants;
      for (unsigned ch = 0; ch < 4; ch++)
         cls_eval.inputs[0][ch] = 0.6f + (float)ch * 0.1f;
      CHECK(eval_pair_program(&cls_eval, &cls_fc.Base), "dp3_corpus: merged classic schedule evaluates");
      if (cls_eval.error)
         fprintf(stderr, "  classic eval: %s\n", cls_eval.error);

      for (unsigned ch = 0; ch < 4; ch++) {
         const float d = fabsf(ref_eval.outputs[cls_fc.OutputColor[0] & 7][ch] -
                               cls_eval.outputs[0][ch]);
         CHECK(d <= 1e-4f, "dp3_corpus: merged-and-scheduled classic output matches the nir_to_rc reference");
         if (d > 1e-4f)
            fprintf(stderr, "  channel %u: reference %g classic %g\n", ch,
                   ref_eval.outputs[cls_fc.OutputColor[0] & 7][ch], cls_eval.outputs[0][ch]);
      }
   }

   if (ref_ok) {
      rc_destroy(&ref_fc.Base);
      rc_destroy_regalloc_state(&ref_rs);
   }
   if (cls_ok) {
      rc_destroy(&cls_fc.Base);
      rc_destroy_regalloc_state(&cls_rs);
   }
   ralloc_free(ctx);
}

/* A full RGB.DP3 + Alpha.DP3 pair reading the same two source registers, the
 * shape radeon_pair_translate.c emits for a dot3 that also writes W: the Alpha
 * lane runs in R300_ALU_OUTA_DP4 DP mode, broadcasting the RGB pipe's
 * three-term dot rather than computing an independent one.  The classic front
 * end materializes a dot3-to-vec4 as a single-channel DP3 into a temp followed
 * by a MAX replicate instead of this fused pairing, so the fused shape is
 * hand-built, the same reason r300_classic_validate_schedule_test.c's
 * fill_alpha_dp_valid hand-builds it.  This drives the DP3 arm of
 * check_alpha_reads_rgb_dot (matching opcodes and shared operands accepted)
 * and eval_pair_program's Alpha-lane dot broadcast, both untouched by the
 * DP4-only coverage. */
static void
test_dp3_full_pair_broadcast(void)
{
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);

   const unsigned c0 = add_const(&fc, 2.0f, 3.0f, 4.0f, 0.0f);

   struct rc_pair_instruction dp3;
   memset(&dp3, 0, sizeof(dp3));
   dp3.RGB.Opcode = RC_OPCODE_DP3;
   dp3.RGB.WriteMask = RC_MASK_XYZ;
   dp3.RGB.DestIndex = 7;
   dp3.RGB.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c0};
   dp3.RGB.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 0};
   dp3.RGB.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   dp3.RGB.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_XYZW};
   dp3.Alpha.Opcode = RC_OPCODE_DP3;
   dp3.Alpha.WriteMask = RC_MASK_W;
   dp3.Alpha.DestIndex = 7;
   dp3.Alpha.Src[0] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_CONSTANT, .Index = c0};
   dp3.Alpha.Src[1] = (struct rc_pair_instruction_source){.Used = 1, .File = RC_FILE_INPUT, .Index = 0};
   dp3.Alpha.Arg[0] = (struct rc_pair_instruction_arg){.Source = 0, .Swizzle = RC_SWIZZLE_WWWW};
   dp3.Alpha.Arg[1] = (struct rc_pair_instruction_arg){.Source = 1, .Swizzle = RC_SWIZZLE_WWWW};

   append_pair(&fc.Base, &fc.Base.Program.Instructions, &dp3);

   int opt = 0;
   r300_classic_schedule(&fc.Base, &opt);

   CHECK(!fc.Base.Error, "dp3_bcast: schedule raises no error");
   CHECK(count_pairs(&fc.Base) == 1,
        "dp3_bcast: the full DP3 pair stays a single pair (no half-full lane to merge)");

   bool has_alpha_dp3 = false;
   for (const struct rc_instruction *i = fc.Base.Program.Instructions.Next;
        i != &fc.Base.Program.Instructions; i = i->Next)
      if (i->Type == RC_INSTRUCTION_PAIR && i->U.P.Alpha.Opcode == RC_OPCODE_DP3)
         has_alpha_dp3 = true;
   CHECK(has_alpha_dp3, "dp3_bcast: the scheduled pair still carries Alpha.DP3");

   const char *reject = NULL;
   CHECK(r300_classic_validate_schedule(&fc.Base, &reject),
        "dp3_bcast: check_alpha_reads_rgb_dot accepts the matching RGB.DP3/Alpha.DP3 pair");
   if (reject)
      fprintf(stderr, "  legality: %s\n", reject);

   struct pair_eval e;
   memset(&e, 0, sizeof(e));
   e.consts = &fc.Base.Program.Constants;
   e.inputs[0][0] = 1.0f;
   e.inputs[0][1] = 1.0f;
   e.inputs[0][2] = 1.0f;

   CHECK(eval_pair_program(&e, &fc.Base), "dp3_bcast: DP3 broadcast pair evaluates");
   if (e.error)
      fprintf(stderr, "  eval: %s\n", e.error);

   /* dp3(c0.xyz, in0.xyz) = 2+3+4 = 9; RGB writes it to xyz, Alpha.DP3
    * broadcasts the same dot to w. */
   CHECK(nearly_equal(e.temps[7][0], 9.0f) && nearly_equal(e.temps[7][1], 9.0f) &&
        nearly_equal(e.temps[7][2], 9.0f),
        "dp3_bcast: RGB lane writes the three-term dot to xyz");
   CHECK(nearly_equal(e.temps[7][3], 9.0f),
        "dp3_bcast: Alpha lane broadcasts the RGB dot to w rather than computing its own");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* ---------- RS48x source-read profile calibration ----------
 *
 * The profile earns trust the standard way: known-good inputs are the
 * retained RS482 silicon lanes (sign-flip mov discriminator, MAD product
 * read, double-truncation chain), which the RS48X_NEG_PREDECESSOR profile
 * must reproduce bit-for-bit; the known-bad control is the IDENTITY
 * profile, which must diverge on exactly the negative-read lanes and agree
 * on the exact ones. */

static uint32_t
f32_bits_of(float v)
{
   return r300_f32_to_bits(v);
}

/* One RGB-only mov (MAX of a source with itself, the final_rewrite form) of
 * input 0, with optional NEG on the argument. */
static void
append_input_mov(struct r300_fragment_program_compiler *fc,
                 struct rc_instruction **tail, unsigned dest_temp,
                 unsigned negate)
{
   struct rc_pair_instruction mov;
   memset(&mov, 0, sizeof(mov));
   mov.RGB.Opcode = RC_OPCODE_MAX;
   mov.RGB.WriteMask = RC_MASK_XYZ;
   mov.RGB.DestIndex = dest_temp;
   mov.RGB.Src[0] = (struct rc_pair_instruction_source){
      .Used = 1, .File = RC_FILE_INPUT, .Index = 0};
   mov.RGB.Arg[0] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_XYZW, .Negate = negate};
   mov.RGB.Arg[1] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_XYZW, .Negate = negate};
   mov.Alpha.Opcode = RC_OPCODE_NOP;
   *tail = append_pair(&fc->Base, *tail, &mov);
}

/* The sign-flip discriminator lanes on the input file.  Stored -1 reads as
 * -pred24(1) = -0.99999237 (FP32 0xBF7FFF80); NEG of stored +1 exports -1
 * exactly; NEG of stored -1 gives +pred24(1); stored -0 canonicalizes to
 * +0.  The identity profile returns the stored values unchanged. */
static void
test_source_read_signflip_lanes(void)
{
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);

   struct rc_instruction *tail = &fc.Base.Program.Instructions;
   append_input_mov(&fc, &tail, 1, 0); /* temp1 = in0        */
   append_input_mov(&fc, &tail, 2, 1); /* temp2 = -in0       */

   const float pred_one = r300_bits_to_f32(0x3F7FFF80u);

   struct pair_eval e;
   memset(&e, 0, sizeof(e));
   e.consts = &fc.Base.Program.Constants;
   e.src_read = R300_SOURCE_READ_RS48X_NEG_PREDECESSOR;
   e.inputs[0][0] = -1.0f;
   e.inputs[0][1] = 1.0f;
   e.inputs[0][2] = -0.0f;

   CHECK(eval_pair_program(&e, &fc.Base), "src_read: signflip stream evaluates");
   CHECK(f32_bits_of(e.temps[1][0]) == 0xBF7FFF80u,
        "src_read: stored -1 reads as -pred24(1), one FP24 ULP toward zero");
   CHECK(f32_bits_of(e.temps[1][1]) == f32_bits_of(1.0f),
        "src_read: stored +1 reads exactly");
   CHECK(f32_bits_of(e.temps[1][2]) == 0,
        "src_read: stored -0 canonicalizes to +0");
   CHECK(f32_bits_of(e.temps[2][0]) == f32_bits_of(pred_one),
        "src_read: NEG of stored -1 gives +pred24(1), truncation precedes the modifier");
   CHECK(f32_bits_of(e.temps[2][1]) == f32_bits_of(-1.0f),
        "src_read: NEG of stored +1 exports -1 exactly, the proven-safe carrier");

   struct pair_eval ident;
   memset(&ident, 0, sizeof(ident));
   ident.consts = &fc.Base.Program.Constants;
   ident.src_read = R300_SOURCE_READ_IDENTITY;
   ident.inputs[0][0] = -1.0f;
   ident.inputs[0][1] = 1.0f;
   ident.inputs[0][2] = -0.0f;

   CHECK(eval_pair_program(&ident, &fc.Base), "src_read: identity control evaluates");
   CHECK(f32_bits_of(ident.temps[1][0]) == f32_bits_of(-1.0f),
        "src_read: identity control keeps stored -1, diverging from the silicon lane");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* Temporary-file read of a negative product.  The chain writes -64 exactly
 * (NEG modifier on the stored +8 input times the swizzle-encoded operand
 * chain), then a mov reads the negative temporary: the retained MAD lane
 * value is -63.99902 (FP32 0xC27FFF80), one predecessor step on 64. */
static void
test_source_read_temp_product_lane(void)
{
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);

   /* temp1 = (-in0) * in0 + 0 = -64 for in0 = 8: both reads see the stored
    * positive 8 (exact), the NEG modifier creates the negative product at
    * the destination, so the write itself is exact. */
   struct rc_pair_instruction mad;
   memset(&mad, 0, sizeof(mad));
   mad.RGB.Opcode = RC_OPCODE_MAD;
   mad.RGB.WriteMask = RC_MASK_XYZ;
   mad.RGB.DestIndex = 1;
   mad.RGB.Src[0] = (struct rc_pair_instruction_source){
      .Used = 1, .File = RC_FILE_INPUT, .Index = 0};
   mad.RGB.Arg[0] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_XYZW, .Negate = 1};
   mad.RGB.Arg[1] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   mad.RGB.Arg[2] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_0000};
   mad.Alpha.Opcode = RC_OPCODE_NOP;

   struct rc_pair_instruction mov;
   memset(&mov, 0, sizeof(mov));
   mov.RGB.Opcode = RC_OPCODE_MAX;
   mov.RGB.WriteMask = RC_MASK_XYZ;
   mov.RGB.DestIndex = 2;
   mov.RGB.Src[0] = (struct rc_pair_instruction_source){
      .Used = 1, .File = RC_FILE_TEMPORARY, .Index = 1};
   mov.RGB.Arg[0] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   mov.RGB.Arg[1] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   mov.Alpha.Opcode = RC_OPCODE_NOP;

   struct rc_instruction *tail =
      append_pair(&fc.Base, &fc.Base.Program.Instructions, &mad);
   append_pair(&fc.Base, tail, &mov);

   struct pair_eval e;
   memset(&e, 0, sizeof(e));
   e.consts = &fc.Base.Program.Constants;
   e.src_read = R300_SOURCE_READ_RS48X_NEG_PREDECESSOR;
   e.inputs[0][0] = 8.0f;

   CHECK(eval_pair_program(&e, &fc.Base), "src_read: product chain evaluates");
   CHECK(f32_bits_of(e.temps[1][0]) == f32_bits_of(-64.0f),
        "src_read: the negative product writes -64 exactly");
   CHECK(f32_bits_of(e.temps[2][0]) == 0xC27FFF80u,
        "src_read: the temp read of -64 delivers -63.99902, the retained MAD lane value");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* Two negative reads across two instructions: temp1 = -0.5 (exact write via
 * NEG on the stored +0.5 input); temp2 = temp1 * 0.5 + 0 reads the negative
 * temp once (-pred24(0.5) * 0.5 = -0.24999809, written exactly); the final
 * mad reads the negative temp2 once more and adds 0.5, delivering the
 * retained double-truncation value 0.2500038 (FP32 0x3E800080).  The HALF
 * and ZERO operands ride swizzle constants, so exactly two register reads
 * are negative. */
static void
test_source_read_double_truncation_chain(void)
{
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);

   struct rc_instruction *tail = &fc.Base.Program.Instructions;
   append_input_mov(&fc, &tail, 1, 1); /* temp1 = -in0 = -0.5, exact */

   struct rc_pair_instruction scale;
   memset(&scale, 0, sizeof(scale));
   scale.RGB.Opcode = RC_OPCODE_MAD;
   scale.RGB.WriteMask = RC_MASK_XYZ;
   scale.RGB.DestIndex = 2;
   scale.RGB.Src[0] = (struct rc_pair_instruction_source){
      .Used = 1, .File = RC_FILE_TEMPORARY, .Index = 1};
   scale.RGB.Arg[0] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   scale.RGB.Arg[1] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_HHHH};
   scale.RGB.Arg[2] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_0000};
   scale.Alpha.Opcode = RC_OPCODE_NOP;

   struct rc_pair_instruction add_half;
   memset(&add_half, 0, sizeof(add_half));
   add_half.RGB.Opcode = RC_OPCODE_MAD;
   add_half.RGB.WriteMask = RC_MASK_XYZ;
   add_half.RGB.DestIndex = 3;
   add_half.RGB.Src[0] = (struct rc_pair_instruction_source){
      .Used = 1, .File = RC_FILE_TEMPORARY, .Index = 2};
   add_half.RGB.Arg[0] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_XYZW};
   add_half.RGB.Arg[1] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_1111};
   add_half.RGB.Arg[2] = (struct rc_pair_instruction_arg){
      .Source = 0, .Swizzle = RC_SWIZZLE_HHHH};
   add_half.Alpha.Opcode = RC_OPCODE_NOP;

   tail = append_pair(&fc.Base, tail, &scale);
   append_pair(&fc.Base, tail, &add_half);

   struct pair_eval e;
   memset(&e, 0, sizeof(e));
   e.consts = &fc.Base.Program.Constants;
   e.src_read = R300_SOURCE_READ_RS48X_NEG_PREDECESSOR;
   e.inputs[0][0] = 0.5f;

   CHECK(eval_pair_program(&e, &fc.Base), "src_read: double-truncation chain evaluates");
   CHECK(f32_bits_of(e.temps[1][0]) == f32_bits_of(-0.5f),
        "src_read: NEG on the stored +0.5 writes -0.5 exactly");
   CHECK(f32_bits_of(e.temps[2][0]) == 0xBE7FFF80u,
        "src_read: first negative read scales to a stored -0.24999809");
   CHECK(f32_bits_of(e.temps[3][0]) == 0x3E800080u,
        "src_read: second negative read plus 0.5 delivers 0.2500038, the retained chain value");

   struct pair_eval ident;
   memset(&ident, 0, sizeof(ident));
   ident.consts = &fc.Base.Program.Constants;
   ident.src_read = R300_SOURCE_READ_IDENTITY;
   ident.inputs[0][0] = 0.5f;

   CHECK(eval_pair_program(&ident, &fc.Base), "src_read: identity chain evaluates");
   CHECK(f32_bits_of(ident.temps[3][0]) == f32_bits_of(0.25f),
        "src_read: identity control delivers the ideal 0.25");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();

   test_merge_combines_independent_temp_writes();
   test_merge_combines_disjoint_output_writers();
   test_merge_combines_dp3_output_writers();
   test_merge_refuses_mixed_output_and_temp();
   test_compaction_merges_within_budget();
   test_reorder_prioritizes_critical_path();
   test_real_corpus_merge_matches_nir_to_rc();
   test_real_corpus_dp3_merge_matches_nir_to_rc();
   test_dp3_full_pair_broadcast();
   test_source_read_signflip_lanes();
   test_source_read_temp_product_lane();
   test_source_read_double_truncation_chain();

   glsl_type_singleton_decref();
   if (failures) {
      fprintf(stderr, "r300_classic_pair_value_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_classic_pair_value_test: all checks passed\n");
   return 0;
}
