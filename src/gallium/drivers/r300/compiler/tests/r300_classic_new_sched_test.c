/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
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
#include "r300_fragprog_swizzle.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "radeon_code.h"
#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_program_constants.h"
#include "radeon_program_pair.h"
#include "radeon_regalloc.h"

/* The new-scheduler increment exercises r300_classic_schedule over the
 * post-rc_pair_translate RC_INSTRUCTION_PAIR stream for the straight-line
 * MOV/MAD/ADD/MUL subset.  Two oracles gate each corpus shader:
 *
 *   (b) schedule legality -- r300_classic_validate_schedule accepts the new
 *       schedule (the same instrument that gates the legacy scheduler).
 *
 *   equivalence -- the new schedule is a pure reordering of the translated
 *       pairs: the multiset of pair instructions is unchanged (only list
 *       order and the presubtract NOP bubble may differ) and the order is a
 *       valid def-before-use topological order.  For a single-assignment
 *       stream that is a proof the scheduled program computes every value the
 *       translated program did; chained with the existing front-end parity
 *       oracle (nir_to_rc == classic normal IR) and the shared, unchanged
 *       rc_pair_translate, it establishes semantic equivalence to nir_to_rc
 *       through the scheduler without a second value evaluator.
 *
 * A control drives the same corpus through the legacy rc_pair_schedule and
 * asserts legality there too, proving the harness before trusting it, and a
 * texture shader confirms the pass defers to rc_pair_schedule outside its
 * subset. */

static int failures;

#define CHECK(cond, what)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL: %s\n", what);                                \
         failures++;                                                         \
      }                                                                      \
   } while (0)

/* ---------- classic compile to normal RC IR ---------- */

static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   memset(s, 0, sizeof(*s));
   s->caps.has_tcl = true;
   return (struct pipe_screen *)s;
}

static void
fs_compiler_init(struct r300_fragment_program_compiler *fc,
                 struct rc_regalloc_state *rs)
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

/* Compile through the classic ladder into normal RC IR (consumes s). */
static bool
compile_classic(void *ctx, nir_shader *s,
                struct r300_fragment_program_compiler *fc,
                struct rc_regalloc_state *rs, const char **why)
{
   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(s, r300_screen(ps));

   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result sel;
   if (!r300_classic_select(ctx, s, t, NULL, 0, NULL, &sel) || !sel.program) {
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

/* ---------- pair-stream oracles ---------- */

static unsigned
count_pairs(struct radeon_compiler *c)
{
   unsigned n = 0;
   for (struct rc_instruction *i = c->Program.Instructions.Next;
        i != &c->Program.Instructions; i = i->Next)
      if (i->Type == RC_INSTRUCTION_PAIR)
         n++;
   return n;
}

/* Snapshot every pair's operation with the NOP bubble cleared, so a
 * post-schedule multiset compare ignores the one field scheduling is allowed
 * to add.  rc_pair_instruction is pointer-free, so a value copy is exact. */
static unsigned
snapshot_pairs(struct radeon_compiler *c, struct rc_pair_instruction *out,
               unsigned cap)
{
   unsigned n = 0;
   for (struct rc_instruction *i = c->Program.Instructions.Next;
        i != &c->Program.Instructions && n < cap; i = i->Next) {
      if (i->Type != RC_INSTRUCTION_PAIR)
         continue;
      out[n] = i->U.P;
      out[n].Nop = 0;
      n++;
   }
   return n;
}

/* Every post-schedule pair matches exactly one pre-schedule snapshot: the
 * scheduler reordered the pairs without adding, dropping, or mutating any. */
static bool
multiset_unchanged(const struct rc_pair_instruction *before, unsigned nbefore,
                   const struct rc_pair_instruction *after, unsigned nafter)
{
   if (nbefore != nafter)
      return false;
   bool matched[64] = {false};
   if (nbefore > 64)
      return false;
   for (unsigned a = 0; a < nafter; a++) {
      bool found = false;
      for (unsigned b = 0; b < nbefore; b++) {
         if (matched[b])
            continue;
         if (memcmp(&after[a], &before[b], sizeof(after[a])) == 0) {
            matched[b] = true;
            found = true;
            break;
         }
      }
      if (!found)
         return false;
   }
   return true;
}

/* Walk the scheduled order and require every temporary a pair reads to have
 * been defined by an earlier pair -- the def-before-use property that makes
 * the reordering value-preserving. */
static bool
topological_order_valid(struct radeon_compiler *c)
{
   bool defined[RC_REGISTER_MAX_INDEX] = {false};
   for (struct rc_instruction *i = c->Program.Instructions.Next;
        i != &c->Program.Instructions; i = i->Next) {
      if (i->Type != RC_INSTRUCTION_PAIR)
         continue;
      const struct rc_pair_instruction *p = &i->U.P;
      for (unsigned pipe = 0; pipe < 2; pipe++) {
         const struct rc_pair_sub_instruction *sub = pipe ? &p->Alpha : &p->RGB;
         for (unsigned k = 0; k < 4; k++)
            if (sub->Src[k].Used && sub->Src[k].File == RC_FILE_TEMPORARY &&
                !defined[sub->Src[k].Index])
               return false;
      }
      if (p->RGB.WriteMask)
         defined[p->RGB.DestIndex] = true;
      if (p->Alpha.WriteMask)
         defined[p->Alpha.DestIndex] = true;
   }
   return true;
}

/* Translate + new-schedule one classic-compiled program, then apply the two
 * scheduler oracles. */
static void
schedule_and_check(const char *name, nir_shader *(*build)(void))
{
   void *ctx = ralloc_context(NULL);
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   const char *why = NULL;

   char what[256];
   snprintf(what, sizeof(what), "%s: classic front end compiles", name);
   const bool ok = compile_classic(ctx, build(), &fc, &rs, &why);
   CHECK(ok, what);
   if (!ok) {
      if (why)
         fprintf(stderr, "  classic: %s\n", why);
      ralloc_free(ctx);
      return;
   }

   int opt = 0;
   rc_pair_translate(&fc.Base, NULL);

   struct rc_pair_instruction before[64];
   const unsigned nbefore = snapshot_pairs(&fc.Base, before, 64);

   r300_classic_schedule(&fc.Base, &opt);

   struct rc_pair_instruction after[64];
   const unsigned nafter = snapshot_pairs(&fc.Base, after, 64);

   const char *reject = NULL;
   snprintf(what, sizeof(what), "%s: new schedule passes legality oracle", name);
   CHECK(r300_classic_validate_schedule(&fc.Base, &reject), what);
   if (reject)
      fprintf(stderr, "  legality: %s\n", reject);

   snprintf(what, sizeof(what), "%s: new schedule is a pure reordering", name);
   CHECK(multiset_unchanged(before, nbefore, after, nafter), what);

   snprintf(what, sizeof(what), "%s: new schedule is a valid topological order",
            name);
   CHECK(topological_order_valid(&fc.Base), what);

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
   ralloc_free(ctx);
}

static void
allocate_identity_inputs(struct r300_fragment_program_compiler *c,
                         void (*allocate)(void *data, unsigned input,
                                          unsigned hwreg),
                         void *mydata)
{
   (void)c;
   for (unsigned i = 0; i < 8; i++)
      allocate(mydata, i, i);
}

/* End-to-end: with R300_CLASSIC_NEW_SCHED set, run the whole backend pass
 * chain -- r300_classic_schedule then rc_pair_remove_dead_sources,
 * rc_pair_regalloc, rc_validate_final_shader, and r300BuildFragmentProgramHwCode
 * -- and require it to accept the new-scheduled stream and emit nonempty R300
 * hardware code.  This proves the reordered, unmerged pair stream is
 * emittable through the unchanged regalloc/validation/emit tail, rather than
 * assuming it. */
static void
end_to_end_emit(const char *name, nir_shader *(*build)(void))
{
   void *ctx = ralloc_context(NULL);
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   const char *why = NULL;

   char what[256];
   const bool ok = compile_classic(ctx, build(), &fc, &rs, &why);
   snprintf(what, sizeof(what), "%s: classic front end compiles", name);
   CHECK(ok, what);
   if (!ok) {
      if (why)
         fprintf(stderr, "  classic: %s\n", why);
      ralloc_free(ctx);
      return;
   }

   struct rX00_fragment_program_code code;
   memset(&code, 0, sizeof(code));
   fc.code = &code;
   fc.AllocateHwInputs = allocate_identity_inputs;

   r3xx_compile_fragment_program(&fc);

   snprintf(what, sizeof(what),
            "%s: backend accepts the new-scheduled stream", name);
   CHECK(!fc.Base.Error, what);
   if (fc.Base.Error && fc.Base.ErrorMsg)
      fprintf(stderr, "  backend said: %s\n", fc.Base.ErrorMsg);

   snprintf(what, sizeof(what), "%s: hardware ALU code generated", name);
   CHECK(code.code.r300.alu.length > 0, what);

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
   ralloc_free(ctx);
}

/* Control: the legacy scheduler over the same program must also pass the
 * legality oracle, proving the harness and validator before trusting them. */
static void
legacy_control(const char *name, nir_shader *(*build)(void))
{
   void *ctx = ralloc_context(NULL);
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   const char *why = NULL;

   char what[256];
   const bool ok = compile_classic(ctx, build(), &fc, &rs, &why);
   if (!ok) {
      ralloc_free(ctx);
      return;
   }

   int opt = 0;
   rc_pair_translate(&fc.Base, NULL);
   rc_pair_schedule(&fc.Base, &opt);

   const char *reject = NULL;
   snprintf(what, sizeof(what), "%s: legacy schedule passes legality oracle",
            name);
   CHECK(r300_classic_validate_schedule(&fc.Base, &reject), what);
   if (reject)
      fprintf(stderr, "  legality: %s\n", reject);

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
   ralloc_free(ctx);
}

/* Outside the straight-line ALU subset the pass defers to rc_pair_schedule;
 * a texture program carries a TEX block the pass does not model.  Driving it
 * through r300_classic_schedule must still leave a legal stream. */
static void
fallback_check(const char *name, nir_shader *(*build)(void))
{
   void *ctx = ralloc_context(NULL);
   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   const char *why = NULL;

   char what[256];
   const bool ok = compile_classic(ctx, build(), &fc, &rs, &why);
   snprintf(what, sizeof(what), "%s: classic front end compiles", name);
   CHECK(ok, what);
   if (!ok) {
      if (why)
         fprintf(stderr, "  classic: %s\n", why);
      ralloc_free(ctx);
      return;
   }

   int opt = 0;
   rc_pair_translate(&fc.Base, NULL);
   r300_classic_schedule(&fc.Base, &opt);

   const char *reject = NULL;
   snprintf(what, sizeof(what), "%s: deferred schedule passes legality oracle",
            name);
   CHECK(r300_classic_validate_schedule(&fc.Base, &reject), what);
   if (reject)
      fprintf(stderr, "  legality: %s\n", reject);

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
   ralloc_free(ctx);
}

/* ---------- subset corpus ---------- */

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

/* MOV: passthrough writes all four channels, so the pair splits xyz onto the
 * RGB pipe and w onto the Alpha pipe. */
static nir_shader *
build_mov(void)
{
   nir_builder b = fs_builder("newsched_mov");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);
   return b.shader;
}

static nir_shader *
build_add(void)
{
   nir_builder b = fs_builder("newsched_add");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c = nir_imm_vec4(&b, 0.5f, 0.25f, -0.125f, 0.75f);
   nir_store_var(&b, out, nir_fadd(&b, v, c), 0xf);
   return b.shader;
}

static nir_shader *
build_mul(void)
{
   nir_builder b = fs_builder("newsched_mul");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c = nir_imm_vec4(&b, 2.0f, 0.5f, 3.0f, 0.25f);
   nir_store_var(&b, out, nir_fmul(&b, v, c), 0xf);
   return b.shader;
}

static nir_shader *
build_mad(void)
{
   nir_builder b = fs_builder("newsched_mad");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c1 = nir_imm_vec4(&b, 0.5f, 0.25f, 2.0f, 1.0f);
   nir_def *c2 = nir_imm_vec4(&b, 0.125f, -0.5f, 0.0f, 0.75f);
   nir_store_var(&b, out, nir_build_alu3(&b, nir_op_fmad, v, c1, c2), 0xf);
   return b.shader;
}

/* A dependent chain of the four subset ops, so the scheduler drains a real
 * def/use graph rather than a single instruction. */
static nir_shader *
build_chain(void)
{
   nir_builder b = fs_builder("newsched_chain");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c1 = nir_imm_vec4(&b, 0.5f, 0.5f, 0.5f, 0.5f);
   nir_def *c2 = nir_imm_vec4(&b, 0.25f, 0.25f, 0.25f, 0.25f);
   nir_def *c3 = nir_imm_vec4(&b, 1.5f, 1.5f, 1.5f, 1.5f);
   nir_def *t0 = nir_fmul(&b, v, c1);
   nir_def *t1 = nir_fadd(&b, t0, c2);
   nir_def *t2 = nir_build_alu3(&b, nir_op_fmad, t1, c3, v);
   nir_def *t3 = nir_fadd(&b, t2, v);
   nir_store_var(&b, out, t3, 0xf);
   return b.shader;
}

/* Two independent vec4 products of the same varying, summed: the scheduler's
 * ready set holds both products at once after the varying load, so the pick
 * order exercises more than one candidate while every temporary keeps a
 * single definer. */
static nir_shader *
build_parallel(void)
{
   nir_builder b = fs_builder("newsched_parallel");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c1 = nir_imm_vec4(&b, 0.5f, 0.25f, 2.0f, 1.0f);
   nir_def *c2 = nir_imm_vec4(&b, 1.5f, 0.75f, 0.125f, 0.5f);
   nir_def *p1 = nir_fmul(&b, v, c1);
   nir_def *p2 = nir_fmul(&b, v, c2);
   nir_store_var(&b, out, nir_fadd(&b, p1, p2), 0xf);
   return b.shader;
}

/* Distinct per-channel arithmetic recombined through a vec4: classic emit
 * expands the collect into per-channel MOVs writing one temporary index, so
 * that index has more than one definer.  The per-index single-assignment
 * precondition does not hold, and the scheduler defers to rc_pair_schedule --
 * the legal-but-merged path this case pins. */
static nir_shader *
build_vec_collect(void)
{
   nir_builder b = fs_builder("newsched_veccollect");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *x = nir_fmul_imm(&b, nir_channel(&b, v, 0), 2.0f);
   nir_def *y = nir_fadd_imm(&b, nir_channel(&b, v, 1), 0.25f);
   nir_def *z = nir_fmul_imm(&b, nir_channel(&b, v, 2), 0.5f);
   nir_def *w = nir_fadd_imm(&b, nir_channel(&b, v, 3), 0.75f);
   nir_store_var(&b, out, nir_vec4(&b, x, y, z, w), 0xf);
   return b.shader;
}

/* Texture modulate: a TEX block outside the scheduler's subset, driving the
 * deferral path. */
static nir_shader *
build_tex(void)
{
   nir_builder b = fs_builder("newsched_tex");
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

int
main(void)
{
   /* Route the backend pass chain through r300_classic_schedule before the
    * gate's env is first read (r3xx_compile_fragment_program caches it). */
   setenv("R300_CLASSIC_NEW_SCHED", "1", 1);

   glsl_type_singleton_init_or_ref();

   schedule_and_check("mov", build_mov);
   schedule_and_check("add", build_add);
   schedule_and_check("mul", build_mul);
   schedule_and_check("mad", build_mad);
   schedule_and_check("chain", build_chain);
   schedule_and_check("parallel", build_parallel);

   /* Prove the new-scheduled stream is emittable through the unchanged
    * regalloc/validation/machine-code tail, flag on. */
   end_to_end_emit("mov", build_mov);
   end_to_end_emit("add", build_add);
   end_to_end_emit("mul", build_mul);
   end_to_end_emit("mad", build_mad);
   end_to_end_emit("chain", build_chain);
   end_to_end_emit("parallel", build_parallel);

   legacy_control("mov", build_mov);
   legacy_control("add", build_add);
   legacy_control("mul", build_mul);
   legacy_control("mad", build_mad);
   legacy_control("chain", build_chain);
   legacy_control("parallel", build_parallel);

   /* Outside the per-index single-assignment subset the pass defers to the
    * legacy scheduler; both a VEC-collect ALU program and a texture program
    * must still leave a legality-passing stream. */
   fallback_check("vec_collect", build_vec_collect);
   fallback_check("tex", build_tex);

   glsl_type_singleton_decref();
   if (failures) {
      fprintf(stderr, "r300_classic_new_sched_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_classic_new_sched_test: all checks passed\n");
   return 0;
}
