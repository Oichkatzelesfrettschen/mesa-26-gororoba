/*
 * SPDX-License-Identifier: MIT
 *
 * Pair-form CPU evaluator and deterministic schedule serializer.  The
 * evaluator covers the opcode subset rc_pair_translate's final_rewrite ever
 * leaves in a pair (MAX, MAD, MIN, DP3, DP4, FRC -- MOV/ADD/MUL always
 * rewrite to MAX/MAD before translation, and DP3/DP4 are the only opcodes
 * classify_instruction ever forces needrgb on regardless of write mask).
 */

#include "r300_pair_eval.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct r300_pair_eval_profile
r300_pair_eval_profile_identity(void)
{
   struct r300_pair_eval_profile p;
   memset(&p, 0, sizeof(p));
   return p;
}

struct r300_pair_eval_profile
r300_pair_eval_profile_rs48x_measured(void)
{
   struct r300_pair_eval_profile p = {
      .input = R300_SOURCE_READ_RS48X_NEG_PREDECESSOR,
      .temporary = R300_SOURCE_READ_RS48X_NEG_PREDECESSOR,
      .input_alpha = R300_SOURCE_READ_UNMODELED,
      .temporary_alpha = R300_SOURCE_READ_UNMODELED,
      .constant = R300_SOURCE_READ_UNMODELED,
      .texture = R300_SOURCE_READ_UNMODELED,
      .presubtract = R300_SOURCE_READ_UNMODELED,
   };
   return p;
}

static enum r300_source_read_model
class_model(const struct r300_pair_eval_profile *p, rc_register_file file,
            bool alpha_port)
{
   switch (file) {
   case RC_FILE_INPUT:     return alpha_port ? p->input_alpha : p->input;
   case RC_FILE_TEMPORARY:
      return alpha_port ? p->temporary_alpha : p->temporary;
   case RC_FILE_CONSTANT:  return p->constant;
   case RC_FILE_PRESUB:    return p->presubtract;
   default:                return R300_SOURCE_READ_IDENTITY;
   }
}

static bool
resolve_pair_reg(struct r300_pair_eval *e, rc_register_file file,
                 unsigned index, float *reg)
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

static bool
apply_source_read(struct r300_pair_eval *e,
                  enum r300_source_read_model model,
                  float reg[4], unsigned consumed_mask)
{
   if (model == R300_SOURCE_READ_UNMODELED) {
      for (unsigned ch = 0; ch < 4; ch++) {
         if ((consumed_mask & (1u << ch)) && signbit(reg[ch])) {
            e->error = "unmodeled negative source read (indeterminate)";
            return false;
         }
      }
   } else if (model == R300_SOURCE_READ_RS48X_NEG_PREDECESSOR) {
      for (unsigned ch = 0; ch < 4; ch++) {
         if (consumed_mask & (1u << ch))
            reg[ch] = r300_us_source_read_f32(reg[ch], model);
      }
   }
   return true;
}

static bool
resolve_presub_reg(struct r300_pair_eval *e,
                   const struct rc_pair_sub_instruction *lane,
                   unsigned presub_opcode, float reg[4],
                   unsigned consumed_mask, bool alpha_port)
{
   const unsigned source_count =
      rc_presubtract_src_reg_count(presub_opcode);
   if (!source_count) {
      e->error = "invalid presubtract opcode";
      return false;
   }

   memset(reg, 0, sizeof(float[4]));
   for (unsigned ch = 0; ch < 4; ch++) {
      if (!(consumed_mask & (1u << ch)))
         continue;

      float source_value[2] = {0.0f, 0.0f};
      for (unsigned source = 0; source < source_count; source++) {
         const struct rc_pair_instruction_source *src = &lane->Src[source];
         float source_reg[4];
         if (!src->Used || !resolve_pair_reg(e, src->File, src->Index,
                                             source_reg) ||
             !apply_source_read(e, class_model(&e->profile, src->File,
                                               alpha_port),
                                source_reg, 1u << ch))
            return false;
         source_value[source] = source_reg[ch];
      }

      switch (presub_opcode) {
      case RC_PRESUB_BIAS:
         reg[ch] = 1.0f - 2.0f * source_value[0];
         break;
      case RC_PRESUB_SUB:
         reg[ch] = source_value[1] - source_value[0];
         break;
      case RC_PRESUB_ADD:
         reg[ch] = source_value[1] + source_value[0];
         break;
      case RC_PRESUB_INV:
         reg[ch] = 1.0f - source_value[0];
         break;
      default:
         e->error = "invalid presubtract opcode";
         return false;
      }
   }

   return apply_source_read(e,
                            class_model(&e->profile, RC_FILE_PRESUB,
                                        alpha_port),
                            reg, consumed_mask);
}

float
r300_pair_decode_channel(const float *reg, unsigned swz)
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
eval_arg_reg(struct r300_pair_eval *e, struct rc_pair_instruction *pair,
             struct rc_pair_instruction_arg *arg, float reg[4],
             unsigned consumed_mask, bool alpha_port)
{
   memset(reg, 0, sizeof(float[4]));
   struct rc_pair_instruction_source *src = rc_pair_get_src(pair, arg);
   if (!src || !src->Used)
      return true;
   if (src->File == RC_FILE_PRESUB) {
      const struct rc_pair_sub_instruction *lane =
         alpha_port ? &pair->Alpha : &pair->RGB;
      return resolve_presub_reg(e, lane, src->Index, reg, consumed_mask,
                                alpha_port);
   }

   return resolve_pair_reg(e, src->File, src->Index, reg) &&
          apply_source_read(e, class_model(&e->profile, src->File,
                                           alpha_port),
                            reg, consumed_mask);
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
eval_rgb_arg(struct r300_pair_eval *e, struct rc_pair_instruction *pair,
             struct rc_pair_sub_instruction *sub, unsigned arg_index,
             bool include_w, float out[4])
{
   struct rc_pair_instruction_arg *arg = &sub->Arg[arg_index];
   unsigned consumed_mask = 0;
   for (unsigned ch = 0; ch < 3; ch++) {
      const unsigned swz = GET_SWZ(arg->Swizzle, ch);
      if (swz <= RC_SWIZZLE_W)
         consumed_mask |= 1u << swz;
   }
   if (include_w)
      consumed_mask |= 1u << 3;

   float reg[4];
   if (!eval_arg_reg(e, pair, arg, reg, consumed_mask, false))
      return false;
   for (unsigned ch = 0; ch < 3; ch++) {
      float v = r300_pair_decode_channel(reg, GET_SWZ(arg->Swizzle, ch));
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
eval_alpha_arg(struct r300_pair_eval *e, struct rc_pair_instruction *pair,
               struct rc_pair_sub_instruction *sub, unsigned arg_index,
               float *out_scalar)
{
   struct rc_pair_instruction_arg *arg = &sub->Arg[arg_index];
   const unsigned swz = GET_SWZ(arg->Swizzle, 0);
   const unsigned consumed_mask = swz <= RC_SWIZZLE_W ? 1u << swz : 0;
   float reg[4];
   if (!eval_arg_reg(e, pair, arg, reg, consumed_mask, true))
      return false;
   float v = r300_pair_decode_channel(reg, GET_SWZ(arg->Swizzle, 0));
   if (arg->Abs)
      v = fabsf(v);
   if (arg->Negate)
      v = -v;
   *out_scalar = v;
   return true;
}

static bool
eval_rgb_op(struct r300_pair_eval *e, struct rc_pair_instruction *pair,
            struct rc_pair_sub_instruction *sub, float result[3])
{
   float s0[4], s1[4], s2[4];
   switch (sub->Opcode) {
   case RC_OPCODE_MAX:
      if (!eval_rgb_arg(e, pair, sub, 0, false, s0) ||
          !eval_rgb_arg(e, pair, sub, 1, false, s1))
         return false;
      for (unsigned ch = 0; ch < 3; ch++)
         result[ch] = fmaxf(s0[ch], s1[ch]);
      return true;
   case RC_OPCODE_MIN:
      if (!eval_rgb_arg(e, pair, sub, 0, false, s0) ||
          !eval_rgb_arg(e, pair, sub, 1, false, s1))
         return false;
      for (unsigned ch = 0; ch < 3; ch++)
         result[ch] = fminf(s0[ch], s1[ch]);
      return true;
   case RC_OPCODE_MAD:
      if (!eval_rgb_arg(e, pair, sub, 0, false, s0) ||
          !eval_rgb_arg(e, pair, sub, 1, false, s1) ||
          !eval_rgb_arg(e, pair, sub, 2, false, s2))
         return false;
      for (unsigned ch = 0; ch < 3; ch++)
         result[ch] = s0[ch] * s1[ch] + s2[ch];
      return true;
   case RC_OPCODE_DP3: {
      if (!eval_rgb_arg(e, pair, sub, 0, false, s0) ||
          !eval_rgb_arg(e, pair, sub, 1, false, s1))
         return false;
      const float d = s0[0] * s1[0] + s0[1] * s1[1] + s0[2] * s1[2];
      result[0] = result[1] = result[2] = d;
      return true;
   }
   case RC_OPCODE_DP4: {
      if (!eval_rgb_arg(e, pair, sub, 0, true, s0) ||
          !eval_rgb_arg(e, pair, sub, 1, true, s1))
         return false;
      const float d =
         s0[0] * s1[0] + s0[1] * s1[1] + s0[2] * s1[2] + s0[3] * s1[3];
      result[0] = result[1] = result[2] = d;
      return true;
   }
   case RC_OPCODE_FRC:
      if (!eval_rgb_arg(e, pair, sub, 0, false, s0))
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
eval_alpha_op(struct r300_pair_eval *e, struct rc_pair_instruction *pair,
              struct rc_pair_sub_instruction *sub, float *result)
{
   float s0, s1, s2;
   switch (sub->Opcode) {
   case RC_OPCODE_MAX:
      if (!eval_alpha_arg(e, pair, sub, 0, &s0) ||
          !eval_alpha_arg(e, pair, sub, 1, &s1))
         return false;
      *result = fmaxf(s0, s1);
      return true;
   case RC_OPCODE_MIN:
      if (!eval_alpha_arg(e, pair, sub, 0, &s0) ||
          !eval_alpha_arg(e, pair, sub, 1, &s1))
         return false;
      *result = fminf(s0, s1);
      return true;
   case RC_OPCODE_MAD:
      if (!eval_alpha_arg(e, pair, sub, 0, &s0) ||
          !eval_alpha_arg(e, pair, sub, 1, &s1) ||
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

bool
r300_pair_eval_program(struct r300_pair_eval *e, struct radeon_compiler *c)
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
       * channel-0 result is the value the Alpha DP lane reads that same
       * cycle. */
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
         if (p->Alpha.Opcode == RC_OPCODE_DP3 ||
             p->Alpha.Opcode == RC_OPCODE_DP4) {
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

/* deterministic schedule serialization */
static const char *
model_name(enum r300_source_read_model m)
{
   switch (m) {
   case R300_SOURCE_READ_IDENTITY:             return "identity";
   case R300_SOURCE_READ_RS48X_NEG_PREDECESSOR:
      return "rs48x-negative-predecessor";
   default:                                    return "unmodeled";
   }
}

static const char *
file_name(rc_register_file file)
{
   switch (file) {
   case RC_FILE_TEMPORARY: return "temporary";
   case RC_FILE_INPUT:     return "input";
   case RC_FILE_OUTPUT:    return "output";
   case RC_FILE_ADDRESS:   return "address";
   case RC_FILE_CONSTANT:  return "constant";
   case RC_FILE_SPECIAL:   return "special";
   case RC_FILE_PRESUB:    return "presub";
   case RC_FILE_INLINE:    return "inline";
   case RC_FILE_NONE:      return "none";
   default:                return "other";
   }
}

static void
swizzle_string(unsigned swizzle, unsigned channels, char out[5])
{
   static const char map[] = "xyzw01h_";
   for (unsigned ch = 0; ch < channels; ch++) {
      unsigned swz = GET_SWZ(swizzle, ch);
      out[ch] = swz < 8 ? map[swz] : '?';
   }
   out[channels] = '\0';
}

static void
serialize_lane(FILE *out, struct rc_pair_instruction *pair,
               struct rc_pair_sub_instruction *sub, const char *lane,
               unsigned channels, bool alpha_port,
               const struct r300_pair_eval_profile *p)
{
   fprintf(out,
           "    {\"lane\":\"%s\",\"opcode\":\"%s\",\"saturate\":%u,"
           "\"write_mask\":%u,\"dest_index\":%u,\"output_write_mask\":%u,"
           "\"depth_write_mask\":%u,\"target\":%u,\"omod\":%u,"
           "\"sources\":[",
           lane, rc_get_opcode_info(sub->Opcode)->Name, sub->Saturate,
           sub->WriteMask, sub->DestIndex, sub->OutputWriteMask,
           sub->DepthWriteMask, sub->Target, sub->Omod);
   const unsigned nargs =
      rc_get_opcode_info(sub->Opcode)->NumSrcRegs;
   for (unsigned a = 0; a < 3; a++) {
      struct rc_pair_instruction_arg *arg = &sub->Arg[a];
      struct rc_pair_instruction_source *src =
         a < nargs ? rc_pair_get_src(pair, arg) : NULL;
      char swz[5];
      swizzle_string(arg->Swizzle, channels, swz);
      fprintf(out,
              "%s{\"slot\":%u,\"source_slot\":%u,\"used\":%u,"
              "\"file\":\"%s\",\"index\":%u,"
              "\"swizzle\":\"%s\",\"absolute\":%u,\"negate\":%u,"
              "\"source_read_model\":\"%s\"}",
              a ? "," : "", a, a < nargs ? arg->Source : 0u,
              src && src->Used ? 1u : 0u,
              src && src->Used ? file_name(src->File) : "none",
              src && src->Used ? src->Index : 0u,
              a < nargs ? swz : "",
              a < nargs ? (unsigned)arg->Abs : 0u,
              a < nargs ? (unsigned)arg->Negate : 0u,
              src && src->Used
                 ? model_name(class_model(p, src->File, alpha_port))
                 : "identity");
   }
   fprintf(out, "],\"source_slots\":[");
   for (unsigned slot = 0; slot < 4; slot++) {
      const struct rc_pair_instruction_source *src = &sub->Src[slot];
      fprintf(out,
              "%s{\"slot\":%u,\"used\":%u,\"file\":\"%s\","
              "\"index\":%u,\"source_read_model\":\"%s\"}",
              slot ? "," : "", slot, src->Used,
              src->Used ? file_name(src->File) : "none", src->Used ? src->Index : 0u,
              src->Used ? model_name(class_model(p, src->File, alpha_port))
                        : "identity");
   }
   fprintf(out, "]}");
}

void
r300_pair_schedule_serialize(FILE *out, struct radeon_compiler *c,
                             const struct r300_pair_eval_profile *p)
{
   fprintf(out, "{\"schema\":2,\"instructions\":[\n");
   unsigned ordinal = 0;
   for (struct rc_instruction *i = c->Program.Instructions.Next;
        i != &c->Program.Instructions; i = i->Next) {
      if (i->Type != RC_INSTRUCTION_PAIR)
         continue;
      struct rc_pair_instruction *pair = &i->U.P;
      fprintf(out,
              "%s  {\"ordinal\":%u,\"write_alu_result\":%u,"
              "\"alu_result_compare\":%u,\"nop\":%u,\"sem_wait\":%u,"
              "\"lanes\":[\n",
              ordinal ? ",\n" : "", ordinal, pair->WriteALUResult,
              pair->ALUResultCompare, pair->Nop, pair->SemWait);
      serialize_lane(out, pair, &pair->RGB, "rgb", 3, false, p);
      fprintf(out, ",\n");
      serialize_lane(out, pair, &pair->Alpha, "alpha", 1, true, p);
      fprintf(out, "\n  ],\"alpha_depth_write\":%u}",
              pair->Alpha.DepthWriteMask);
      ordinal++;
   }
   fprintf(out, "\n]}\n");
}

static uint64_t
fnv1a64(uint64_t h, const void *data, size_t len)
{
   const unsigned char *bytes = data;
   for (size_t i = 0; i < len; i++) {
      h ^= bytes[i];
      h *= 0x00000100000001B3ull;
   }
   return h;
}

#define FNV1A64_BASIS 0xCBF29CE484222325ull

uint64_t
r300_pair_schedule_semantic_hash(struct radeon_compiler *c,
                                 const struct r300_pair_eval_profile *p)
{
   char *buf = NULL;
   size_t len = 0;
   FILE *mem = open_memstream(&buf, &len);
   if (!mem)
      return 0;
   r300_pair_schedule_serialize(mem, c, p);
   fclose(mem);
   const uint64_t h = fnv1a64(FNV1A64_BASIS, buf, len);
   free(buf);
   return h;
}

uint64_t
r300_pair_constant_list_hash(const struct rc_constant_list *consts)
{
   uint64_t h = FNV1A64_BASIS;
   const uint32_t count = consts->Count;
   h = fnv1a64(h, &count, sizeof(count));
   for (unsigned i = 0; i < consts->Count; i++) {
      const struct rc_constant *k = &consts->Constants[i];
      const uint32_t type = k->Type;
      const uint32_t use_mask = k->UseMask;
      h = fnv1a64(h, &type, sizeof(type));
      h = fnv1a64(h, &use_mask, sizeof(use_mask));
      switch (k->Type) {
      case RC_CONSTANT_EXTERNAL:
         h = fnv1a64(h, &k->u.External, sizeof(k->u.External));
         break;
      case RC_CONSTANT_IMMEDIATE:
         for (unsigned channel = 0; channel < 4; channel++) {
            const uint32_t bits = r300_f32_to_bits(k->u.Immediate[channel]);
            h = fnv1a64(h, &bits, sizeof(bits));
         }
         break;
      case RC_CONSTANT_STATE:
         h = fnv1a64(h, k->u.State, sizeof(k->u.State));
         break;
      default:
         /* The type and use mask still identify a malformed entry. */
         break;
      }
   }
   return h;
}

uint64_t
r300_pair_program_words_hash(const uint32_t *words, size_t count)
{
   return fnv1a64(FNV1A64_BASIS, words, count * sizeof(*words));
}
