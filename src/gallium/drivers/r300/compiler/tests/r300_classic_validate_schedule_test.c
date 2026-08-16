/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "util/ralloc.h"

#include "classic/r300_classic_validate_schedule.h"
#include "nir_to_rc.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "radeon_code.h"
#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_program_constants.h"
#include "radeon_program_pair.h"

/* Oracle (b), schedule legality: the validator must PASS on the unmodified
 * legacy path (nir_to_rc -> r3xx_compile_fragment_program, the
 * pairer/scheduler/regalloc this file does not touch) before it can be
 * trusted to gate a from-scratch scheduler.  Two halves:
 *
 *   - a corpus of shaders compiled through the full legacy backend pass
 *     chain, including ones shaped to force RGB.REPL_ALPHA and Alpha.DP
 *     pairing, all of which the validator must accept;
 *   - synthetic rc_pair_instruction sequences that isolate each of the
 *     four checks and confirm the validator actually rejects the violation
 *     it claims to detect, not just that it accepts real compiler output. */

static int failures;

#define CHECK(cond, what)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL: %s\n", what);                                \
         failures++;                                                         \
      }                                                                      \
   } while (0)

/* corpus half: full legacy pipeline, real compiler output */
static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   memset(s, 0, sizeof(*s));
   s->caps.has_tcl = true;
   return (struct pipe_screen *)s;
}

static void
allocate_identity_inputs(struct r300_fragment_program_compiler *c,
                         void (*allocate)(void *data, unsigned input,
                                          unsigned hwreg),
                         void *mydata)
{
   for (unsigned i = 0; i < 8; i++)
      allocate(mydata, i, i);
}

/* Compile through the unmodified legacy front end (nir_to_rc) and the real
 * backend pass chain (r3xx_compile_fragment_program): native rewrite,
 * pair translate, pair scheduling, dead sources, register allocation,
 * final validation, machine code generation.  Consumes s. */
static bool
compile_legacy_full(nir_shader *s, struct r300_fragment_program_compiler *fc,
                    struct rc_regalloc_state *rs,
                    struct r300_fragment_shader_code *fs_code)
{
   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state ext = {0};
   union r300_shader_code code = {.f = fs_code};

   rc_init_regalloc_state(rs, RC_FRAGMENT_PROGRAM);
   memset(fc, 0, sizeof(*fc));
   rc_init(&fc->Base, rs);
   fc->Base.type = RC_FRAGMENT_PROGRAM;
   fc->Base.has_half_swizzles = true;
   fc->Base.has_presub = true;
   fc->Base.has_omod = true;
   fc->Base.max_temp_regs = 32;
   fc->Base.max_constants = 32;
   fc->Base.max_alu_insts = 64;
   fc->Base.max_tex_insts = 32;
   fc->code = &fs_code->code;
   fc->AllocateHwInputs = allocate_identity_inputs;

   r300_optimize_nir(s, &r300_screen(ps)->caps);
   nir_to_rc(s, &r300_screen(ps)->caps, ext, code, &fc->Base);
   if (fc->Base.Error)
      return false;

   r3xx_compile_fragment_program(fc);
   return !fc->Base.Error;
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
   return nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "%s",
                                         name);
}

static nir_variable *
add_varying(nir_builder *b)
{
   nir_variable *in = nir_variable_create(b->shader, nir_var_shader_in,
                                          glsl_vec4_type(), "in0");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   return in;
}

static nir_variable *
add_color_output(nir_builder *b)
{
   nir_variable *out = nir_variable_create(b->shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;
   return out;
}

static nir_shader *
build_fmad(void)
{
   nir_builder b = fs_builder("validate_fmad");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c1 = nir_imm_vec4(&b, 0.5f, 0.25f, 2.0f, 1.0f);
   nir_def *c2 = nir_imm_vec4(&b, 0.125f, -0.5f, 0.0f, 0.75f);
   nir_store_var(&b, out, nir_build_alu3(&b, nir_op_fmad, v, c1, c2), 0xf);
   return b.shader;
}

/* Four DIFFERENT per-channel scalar producers (EX2/LG2/RCP/RSQ) collected
 * into one vec4 output write.  This shape matters: a single broadcast
 * scalar (all four channels reading the same def) gets caught by
 * r3xx_fragprog.c's rc_convert_rgb_alpha pass, which demotes any standalone
 * (non-Friended) scalar-opcode instruction's destination to W-only before
 * pairing -- a broadcast EX2 never reaches pair translate with an xyz-
 * carrying mask.  Four DIFFERENT channel writers of the same destination
 * temp, all consumed by the same later vec4-read instruction, make
 * rc_get_variables() Friend them together (radeon_variable.c: "Any two
 * variables that share a reader are considered 'friends'"), which exempts
 * them from the demotion.  classify_instruction then sees EX2/LG2/RCP
 * writing an xyz channel (needrgb) with the unconditional needalpha the
 * transcendent case sets, so pair translate emits RGB.REPL_ALPHA paired
 * with the matching Alpha opcode -- a real positive instance of the
 * RGB-reads-alpha coupling in actual compiled output, confirmed by the
 * corpus's own opcode count, not asserted on faith. */
static nir_shader *
build_sop_repl_alpha(void)
{
   nir_builder b = fs_builder("validate_sop");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *safe = nir_fadd_imm(&b, nir_fabs(&b, v), 0.5f);
   nir_def *x = nir_fexp2(&b, nir_channel(&b, safe, 0));
   nir_def *y = nir_flog2(&b, nir_channel(&b, safe, 1));
   nir_def *z = nir_frcp(&b, nir_channel(&b, safe, 2));
   nir_def *w = nir_frsq(&b, nir_channel(&b, safe, 3));
   nir_store_var(&b, out, nir_vec4(&b, x, y, z, w), 0xf);
   return b.shader;
}

/* dot4(v, c) broadcast to all four channels: classify_instruction sets
 * needalpha unconditionally for DP4 and needrgb from the xyz write mask,
 * so pair translate emits RGB.DP4 paired with Alpha.DP4 in the same
 * instruction -- a real positive instance of the alpha-reads-rgb-dot
 * coupling. */
static nir_shader *
build_alpha_dp_dot(void)
{
   nir_builder b = fs_builder("validate_dp");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c4 = nir_imm_vec4(&b, 0.5f, -0.25f, 1.5f, 0.125f);
   nir_def *d = nir_fdot(&b, v, c4);
   nir_store_var(&b, out, nir_vec4(&b, d, d, d, d), 0xf);
   return b.shader;
}

static nir_shader *
build_tex_modulate(void)
{
   nir_builder b = fs_builder("validate_tex");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);

   const struct glsl_type *sampler2d =
      glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform,
                                               sampler2d, "tex0");
   sampler->data.binding = 0;
   nir_deref_instr *deref = nir_build_deref_var(&b, sampler);

   nir_def *v = nir_load_var(&b, in);
   nir_def *uv = nir_trim_vector(&b, v, 2);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 3);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &deref->def);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &deref->def);
   tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_coord, uv);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);

   nir_store_var(&b, out, nir_fmul(&b, &tex->def, v), 0xf);
   return b.shader;
}

/* Count how many emitted pairs actually exercise the two cross-pipe
 * couplings, so a corpus case that claims to force RGB.REPL_ALPHA or
 * Alpha.DP can be held to actually containing one -- a validator that
 * always returns true on a corpus that never produces the shape it checks
 * would pass vacuously, proving nothing about that check. */
static void
count_cross_pipe_pairs(const struct radeon_compiler *c, unsigned *repl_alpha_count,
                       unsigned *alpha_dp_count)
{
   *repl_alpha_count = 0;
   *alpha_dp_count = 0;
   for (const struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      if (inst->Type != RC_INSTRUCTION_PAIR)
         continue;
      if (inst->U.P.RGB.Opcode == RC_OPCODE_REPL_ALPHA)
         (*repl_alpha_count)++;
      if (inst->U.P.Alpha.Opcode == RC_OPCODE_DP3 ||
          inst->U.P.Alpha.Opcode == RC_OPCODE_DP4)
         (*alpha_dp_count)++;
   }
}

static void
corpus_case(const char *name, nir_shader *(*build)(void),
           unsigned min_repl_alpha, unsigned min_alpha_dp)
{
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   struct r300_fragment_shader_code fs_code;
   memset(&fs_code, 0, sizeof(fs_code));

   char what[256];
   nir_shader *s = build();

   snprintf(what, sizeof(what), "%s: legacy pipeline compiles", name);
   const bool ok = compile_legacy_full(s, &fc, &rs, &fs_code);
   CHECK(ok, what);
   if (!ok) {
      if (fc.Base.ErrorMsg)
         fprintf(stderr, "  backend said: %s\n", fc.Base.ErrorMsg);
      return;
   }

   unsigned repl_alpha_count, alpha_dp_count;
   count_cross_pipe_pairs(&fc.Base, &repl_alpha_count, &alpha_dp_count);
   snprintf(what, sizeof(what),
           "%s: compiled output actually contains >=%u RGB.REPL_ALPHA pair(s)",
           name, min_repl_alpha);
   CHECK(repl_alpha_count >= min_repl_alpha, what);
   snprintf(what, sizeof(what),
           "%s: compiled output actually contains >=%u Alpha.DP pair(s)",
           name, min_alpha_dp);
   CHECK(alpha_dp_count >= min_alpha_dp, what);

   const char *reject_reason = NULL;
   snprintf(what, sizeof(what), "%s: schedule legality validator accepts",
           name);
   const bool legal = r300_classic_validate_schedule(&fc.Base, &reject_reason);
   CHECK(legal, what);
   if (!legal && reject_reason)
      fprintf(stderr, "  rejected: %s\n", reject_reason);

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* synthetic half: isolate each check against a hand-built pair */
static struct rc_instruction *
append_pair(struct radeon_compiler *c, struct rc_instruction *tail,
           const struct rc_pair_instruction *p)
{
   struct rc_instruction *inst = rc_insert_new_instruction(c, tail);
   inst->Type = RC_INSTRUCTION_PAIR;
   inst->U.P = *p;
   return inst;
}

static void
synth_case(const char *name, void (*fill)(struct radeon_compiler *c,
                                          struct rc_instruction *tail),
          bool expect_legal, const char *expect_reason_substr)
{
   struct rc_regalloc_state rs;
   rc_init_regalloc_state(&rs, RC_FRAGMENT_PROGRAM);
   struct radeon_compiler c;
   memset(&c, 0, sizeof(c));
   rc_init(&c, &rs);

   fill(&c, &c.Program.Instructions);

   const char *reason = NULL;
   const bool legal = r300_classic_validate_schedule(&c, &reason);

   char what[256];
   snprintf(what, sizeof(what), "%s: legality matches expectation", name);
   CHECK(legal == expect_legal, what);
   if (legal != expect_legal)
      fprintf(stderr, "  got legal=%d reason=%s\n", legal,
              reason ? reason : "(none)");
   if (!legal && expect_reason_substr) {
      snprintf(what, sizeof(what), "%s: reject reason names the constraint",
              name);
      CHECK(reason && strstr(reason, expect_reason_substr), what);
   }

   rc_destroy(&c);
   rc_destroy_regalloc_state(&rs);
}

static void
fill_repl_alpha_valid(struct radeon_compiler *c, struct rc_instruction *tail)
{
   struct rc_pair_instruction p;
   memset(&p, 0, sizeof(p));
   p.RGB.Opcode = RC_OPCODE_REPL_ALPHA;
   p.RGB.WriteMask = RC_MASK_XYZ;
   p.RGB.DestIndex = 0;
   p.Alpha.Opcode = RC_OPCODE_EX2;
   p.Alpha.WriteMask = RC_MASK_W;
   p.Alpha.DestIndex = 0;
   append_pair(c, tail, &p);
}

static void
fill_repl_alpha_reads_nop(struct radeon_compiler *c, struct rc_instruction *tail)
{
   struct rc_pair_instruction p;
   memset(&p, 0, sizeof(p));
   p.RGB.Opcode = RC_OPCODE_REPL_ALPHA;
   p.RGB.WriteMask = RC_MASK_XYZ;
   p.RGB.DestIndex = 0;
   p.Alpha.Opcode = RC_OPCODE_NOP;
   append_pair(c, tail, &p);
}

static void
fill_alpha_dp_valid(struct radeon_compiler *c, struct rc_instruction *tail)
{
   struct rc_pair_instruction p;
   memset(&p, 0, sizeof(p));
   p.RGB.Opcode = RC_OPCODE_DP4;
   p.RGB.WriteMask = RC_MASK_XYZ;
   p.RGB.DestIndex = 0;
   p.Alpha.Opcode = RC_OPCODE_DP4;
   p.Alpha.WriteMask = RC_MASK_W;
   p.Alpha.DestIndex = 0;
   append_pair(c, tail, &p);
}

static void
fill_alpha_dp_orphaned(struct radeon_compiler *c, struct rc_instruction *tail)
{
   struct rc_pair_instruction p;
   memset(&p, 0, sizeof(p));
   p.RGB.Opcode = RC_OPCODE_MAD;
   p.RGB.WriteMask = RC_MASK_XYZ;
   p.RGB.DestIndex = 0;
   p.Alpha.Opcode = RC_OPCODE_DP4;
   p.Alpha.WriteMask = RC_MASK_W;
   p.Alpha.DestIndex = 0;
   append_pair(c, tail, &p);
}

static void
fill_double_issue(struct radeon_compiler *c, struct rc_instruction *tail)
{
   struct rc_pair_instruction p;
   memset(&p, 0, sizeof(p));
   p.RGB.Opcode = RC_OPCODE_MAD;
   p.RGB.WriteMask = RC_MASK_XYZ;
   p.RGB.OutputWriteMask = RC_MASK_XYZ;
   p.WriteALUResult = RC_ALURESULT_X;
   append_pair(c, tail, &p);
}

static void
fill_presub_hazard_missing_nop(struct radeon_compiler *c, struct rc_instruction *tail)
{
   struct rc_pair_instruction writer;
   memset(&writer, 0, sizeof(writer));
   writer.RGB.Opcode = RC_OPCODE_MAD;
   writer.RGB.WriteMask = RC_MASK_XYZ;
   writer.RGB.DestIndex = 4;
   writer.Nop = 0;
   struct rc_instruction *w = append_pair(c, tail, &writer);

   struct rc_pair_instruction reader;
   memset(&reader, 0, sizeof(reader));
   reader.RGB.Opcode = RC_OPCODE_MAD;
   reader.RGB.WriteMask = RC_MASK_XYZ;
   reader.RGB.DestIndex = 5;
   reader.RGB.Src[RC_PAIR_PRESUB_SRC].Used = 1;
   reader.RGB.Src[RC_PAIR_PRESUB_SRC].Index = RC_PRESUB_ADD;
   reader.RGB.Src[0].Used = 1;
   reader.RGB.Src[0].File = RC_FILE_TEMPORARY;
   reader.RGB.Src[0].Index = 4; /* reads the writer's destination */
   reader.RGB.Src[1].Used = 1;
   reader.RGB.Src[1].File = RC_FILE_TEMPORARY;
   reader.RGB.Src[1].Index = 6;
   append_pair(c, w, &reader);
}

static void
fill_presub_hazard_nop_present(struct radeon_compiler *c, struct rc_instruction *tail)
{
   struct rc_pair_instruction writer;
   memset(&writer, 0, sizeof(writer));
   writer.RGB.Opcode = RC_OPCODE_MAD;
   writer.RGB.WriteMask = RC_MASK_XYZ;
   writer.RGB.DestIndex = 4;
   writer.Nop = 1; /* the required bubble */
   struct rc_instruction *w = append_pair(c, tail, &writer);

   struct rc_pair_instruction reader;
   memset(&reader, 0, sizeof(reader));
   reader.RGB.Opcode = RC_OPCODE_MAD;
   reader.RGB.WriteMask = RC_MASK_XYZ;
   reader.RGB.DestIndex = 5;
   reader.RGB.Src[RC_PAIR_PRESUB_SRC].Used = 1;
   reader.RGB.Src[RC_PAIR_PRESUB_SRC].Index = RC_PRESUB_ADD;
   reader.RGB.Src[0].Used = 1;
   reader.RGB.Src[0].File = RC_FILE_TEMPORARY;
   reader.RGB.Src[0].Index = 4;
   reader.RGB.Src[1].Used = 1;
   reader.RGB.Src[1].File = RC_FILE_TEMPORARY;
   reader.RGB.Src[1].Index = 6;
   append_pair(c, w, &reader);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();

   corpus_case("fmad", build_fmad, 0, 0);
   corpus_case("sop_repl_alpha", build_sop_repl_alpha, 1, 0);
   corpus_case("alpha_dp_dot", build_alpha_dp_dot, 0, 1);
   corpus_case("tex_modulate", build_tex_modulate, 0, 0);

   synth_case("repl_alpha_valid", fill_repl_alpha_valid, true, NULL);
   synth_case("repl_alpha_reads_nop", fill_repl_alpha_reads_nop, false,
             "REPL_ALPHA");
   synth_case("alpha_dp_valid", fill_alpha_dp_valid, true, NULL);
   synth_case("alpha_dp_orphaned", fill_alpha_dp_orphaned, false, "Alpha.DP");
   synth_case("double_issue", fill_double_issue, false, "WriteALUResult");
   synth_case("presub_hazard_missing_nop", fill_presub_hazard_missing_nop, false,
             "presubtract");
   synth_case("presub_hazard_nop_present", fill_presub_hazard_nop_present, true,
             NULL);

   glsl_type_singleton_decref();
   if (failures) {
      fprintf(stderr, "r300_classic_validate_schedule_test: %d failures\n",
              failures);
      return 1;
   }
   printf("r300_classic_validate_schedule_test: all checks passed\n");
   return 0;
}
