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
 * subset.
 *
 * The MOV/ADD/MUL/MAD shapes above never produce a half-full pair (each
 * always dual-issues across both lanes from one original instruction), so
 * the multiset-invariance oracle still holds for them unchanged.
 * r300_classic_schedule's cross-instruction RGB+Alpha merge and
 * critical-path reorder tie-break -- which do change the pair multiset and
 * do produce a non-identity order -- are exercised below (VEC-collect and
 * the compaction tests) and value-checked in
 * r300_classic_pair_value_test.c's pair-form CPU evaluator, the oracle a
 * multiset-invariance proof no longer stands in for once merging is live. */

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

/* ---------- pair-stream oracles ---------- */

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

/* rc_pair_sub_instruction and rc_pair_instruction are bitfield structs: a
 * struct-wide memcmp folds every field's storage together and a stray
 * compiler-inserted padding bit (real for a mixed-width bitfield layout)
 * would read as a mismatch that has no named field behind it, or mask an
 * actual field divergence sharing the same byte.  Naming each field keeps
 * the comparison tied to what the pair form actually encodes. */
static bool
sub_instruction_equal(const struct rc_pair_sub_instruction *a,
                      const struct rc_pair_sub_instruction *b)
{
   if (a->Opcode != b->Opcode || a->DestIndex != b->DestIndex ||
       a->WriteMask != b->WriteMask || a->Target != b->Target ||
       a->OutputWriteMask != b->OutputWriteMask ||
       a->DepthWriteMask != b->DepthWriteMask || a->Saturate != b->Saturate ||
       a->Omod != b->Omod)
      return false;
   for (unsigned i = 0; i < 4; i++) {
      if (a->Src[i].Used != b->Src[i].Used || a->Src[i].File != b->Src[i].File ||
          a->Src[i].Index != b->Src[i].Index)
         return false;
   }
   for (unsigned i = 0; i < 3; i++) {
      if (a->Arg[i].Source != b->Arg[i].Source || a->Arg[i].Swizzle != b->Arg[i].Swizzle ||
          a->Arg[i].Abs != b->Arg[i].Abs || a->Arg[i].Negate != b->Arg[i].Negate)
         return false;
   }
   return true;
}

static bool
pair_instruction_equal(const struct rc_pair_instruction *a, const struct rc_pair_instruction *b)
{
   return sub_instruction_equal(&a->RGB, &b->RGB) && sub_instruction_equal(&a->Alpha, &b->Alpha) &&
          a->WriteALUResult == b->WriteALUResult &&
          a->ALUResultCompare == b->ALUResultCompare && a->Nop == b->Nop &&
          a->SemWait == b->SemWait;
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
         if (pair_instruction_equal(&after[a], &before[b])) {
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

/* A shape r300_classic_schedule does not reorder+merge identically to the
 * schedule_and_check corpus -- either because it defers to rc_pair_schedule
 * entirely (a TEX block, outside the pass's model) or because it merges
 * pairs itself (a VEC-collect's per-channel writes, which changes the pair
 * count so schedule_and_check's multiset-invariance check no longer
 * applies) -- must still leave a legality-passing stream either way. */
static void
legal_after_schedule_check(const char *name, nir_shader *(*build)(void))
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
   snprintf(what, sizeof(what), "%s: schedule passes legality oracle", name);
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
 * expands the collect into per-channel MOVs writing disjoint channels of one
 * temporary index.  Single assignment is tracked per (index, channel), so
 * this index having more than one definer is legal as long as no two
 * definers claim the same channel; three of the four channels translate to
 * RGB-only pairs and the fourth to an Alpha-only pair, so
 * r300_classic_schedule's own merge search folds one RGB-only/Alpha-only
 * pair together here too. */
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

/* ---------- safety tests: defer instead of hard-fail ----------
 *
 * These three shapes cannot arise from any real front end (a genuine RAW
 * cycle, an already-over-budget translated stream, and so on are not
 * something straight-line dataflow code compiles to), so they are built
 * directly in RC_INSTRUCTION_PAIR form -- skipping rc_pair_translate --
 * the same way r300_classic_validate_schedule_test.c's synth_case corpus
 * isolates one hazard at a time. */

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

/* A mutual RAW cycle -- pair0 reads what pair1 writes and pair1 reads what
 * pair0 writes -- has no topological order and is exactly the "no ready
 * node left" shape r300_classic_schedule must defer on rather than
 * rc_error hard-failing.  Proves the defer by running the identical
 * hand-built stream through r300_classic_schedule and through
 * rc_pair_schedule directly on two separate compiler instances and
 * requiring the same outcome: no error raised, no pair silently dropped.
 * This is a scheduling-pass-level proof; a stream with no valid order is
 * not a meaningful input to the unrelated, unmodified regalloc/emit tail,
 * so it is not driven through those stages. */
static void
test_cycle_defers_without_hard_fail(void)
{
   struct rc_pair_instruction p0, p1;

   memset(&p0, 0, sizeof(p0));
   p0.RGB.Opcode = RC_OPCODE_MOV;
   p0.RGB.WriteMask = RC_MASK_XYZ;
   p0.RGB.DestIndex = 0;
   p0.RGB.Src[0].Used = 1;
   p0.RGB.Src[0].File = RC_FILE_TEMPORARY;
   p0.RGB.Src[0].Index = 1; /* reads pair1's destination */

   memset(&p1, 0, sizeof(p1));
   p1.RGB.Opcode = RC_OPCODE_MOV;
   p1.RGB.WriteMask = RC_MASK_XYZ;
   p1.RGB.DestIndex = 1;
   p1.RGB.Src[0].Used = 1;
   p1.RGB.Src[0].File = RC_FILE_TEMPORARY;
   p1.RGB.Src[0].Index = 0; /* reads pair0's destination: the cycle */

   struct r300_fragment_program_compiler fc_new;
   struct rc_regalloc_state rs_new;
   fs_compiler_init(&fc_new, &rs_new);
   struct rc_instruction *tail_new =
      append_pair(&fc_new.Base, &fc_new.Base.Program.Instructions, &p0);
   append_pair(&fc_new.Base, tail_new, &p1);

   struct r300_fragment_program_compiler fc_legacy;
   struct rc_regalloc_state rs_legacy;
   fs_compiler_init(&fc_legacy, &rs_legacy);
   struct rc_instruction *tail_legacy =
      append_pair(&fc_legacy.Base, &fc_legacy.Base.Program.Instructions, &p0);
   append_pair(&fc_legacy.Base, tail_legacy, &p1);

   int opt = 0;
   r300_classic_schedule(&fc_new.Base, &opt);
   rc_pair_schedule(&fc_legacy.Base, &opt);

   CHECK(!fc_new.Base.Error,
        "cycle: r300_classic_schedule defers instead of rc_error hard-failing");
   CHECK(!fc_legacy.Base.Error, "cycle: rc_pair_schedule control raises no error");
   CHECK(count_pairs(&fc_new.Base) == 2,
        "cycle: deferred schedule still carries both pairs");
   CHECK(count_pairs(&fc_new.Base) == count_pairs(&fc_legacy.Base),
        "cycle: deferred schedule matches rc_pair_schedule's own outcome");

   rc_destroy(&fc_new.Base);
   rc_destroy_regalloc_state(&rs_new);
   rc_destroy(&fc_legacy.Base);
   rc_destroy_regalloc_state(&rs_legacy);
}

/* Three pairs with no temporary dependency between the two output writers:
 * D defines temp10 with no reads, B reads temp10 and exports it to
 * COLOR[0], and C exports an unrelated value to the SAME COLOR[0] with no
 * reads at all, in program order B, C, D.  Without an output/depth
 * write-after-write edge the greedy scheduler would place C (ready from
 * the start) ahead of B (blocked on D), inverting the two exports'
 * relative order.  Walking the final RC_OPCODE tags (MOV=B, ADD=C,
 * MUL=D) proves B still precedes C. */
static void
test_output_waw_preserves_order(void)
{
   struct rc_pair_instruction b, c, d;

   memset(&d, 0, sizeof(d));
   d.RGB.Opcode = RC_OPCODE_MUL;
   d.RGB.WriteMask = RC_MASK_XYZ;
   d.RGB.DestIndex = 10;

   memset(&b, 0, sizeof(b));
   b.RGB.Opcode = RC_OPCODE_MOV;
   b.RGB.Src[0].Used = 1;
   b.RGB.Src[0].File = RC_FILE_TEMPORARY;
   b.RGB.Src[0].Index = 10;
   b.RGB.Target = 0;
   b.RGB.OutputWriteMask = RC_MASK_XYZ;

   memset(&c, 0, sizeof(c));
   c.RGB.Opcode = RC_OPCODE_ADD;
   c.RGB.Target = 0;
   c.RGB.OutputWriteMask = RC_MASK_XYZ;

   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);
   struct rc_instruction *tail = append_pair(&fc.Base, &fc.Base.Program.Instructions, &b);
   tail = append_pair(&fc.Base, tail, &c);
   append_pair(&fc.Base, tail, &d);

   int opt = 0;
   r300_classic_schedule(&fc.Base, &opt);

   CHECK(!fc.Base.Error, "output_waw: schedule raises no error");

   int pos_mov = -1, pos_add = -1, pos_mul = -1, pos = 0;
   for (struct rc_instruction *i = fc.Base.Program.Instructions.Next;
        i != &fc.Base.Program.Instructions; i = i->Next, pos++) {
      if (i->Type != RC_INSTRUCTION_PAIR)
         continue;
      if (i->U.P.RGB.Opcode == RC_OPCODE_MOV)
         pos_mov = pos;
      else if (i->U.P.RGB.Opcode == RC_OPCODE_ADD)
         pos_add = pos;
      else if (i->U.P.RGB.Opcode == RC_OPCODE_MUL)
         pos_mul = pos;
   }
   CHECK(pos_mov >= 0 && pos_add >= 0 && pos_mul >= 0,
        "output_waw: all three pairs survive scheduling");
   CHECK(pos_mov < pos_add,
        "output_waw: the earlier COLOR[0] export (MOV) still precedes the later one (ADD)");
   CHECK(pos_mul < pos_mov,
        "output_waw: MOV's temp10 producer (MUL) still precedes its reader");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* Two independent half-full pairs -- an RGB-only MOV and an Alpha-only MOV --
 * that r300_classic_schedule's own merge search (rc_pair_try_merge, the same
 * source-slot machinery rc_pair_schedule's merge_instructions() uses) now
 * folds into one full pair itself, so a translated stream that starts over
 * budget can still fit without ever calling the legacy scheduler. */
static void
test_compaction_merges_itself(void)
{
   struct rc_pair_instruction rgb_only, alpha_only;

   memset(&rgb_only, 0, sizeof(rgb_only));
   rgb_only.RGB.Opcode = RC_OPCODE_MOV;
   rgb_only.RGB.WriteMask = RC_MASK_XYZ;
   rgb_only.RGB.DestIndex = 0;
   rgb_only.Alpha.Opcode = RC_OPCODE_NOP;

   memset(&alpha_only, 0, sizeof(alpha_only));
   alpha_only.RGB.Opcode = RC_OPCODE_NOP;
   alpha_only.Alpha.Opcode = RC_OPCODE_MOV;
   alpha_only.Alpha.WriteMask = RC_MASK_W;
   alpha_only.Alpha.DestIndex = 1;

   struct r300_fragment_program_compiler fc;
   struct rc_regalloc_state rs;
   fs_compiler_init(&fc, &rs);
   fc.Base.max_alu_insts = 1;
   struct rc_instruction *tail =
      append_pair(&fc.Base, &fc.Base.Program.Instructions, &rgb_only);
   append_pair(&fc.Base, tail, &alpha_only);

   CHECK(count_pairs(&fc.Base) == 2,
        "compaction: the translated stream starts at two half-full pairs");

   int opt = 0;
   r300_classic_schedule(&fc.Base, &opt);

   CHECK(!fc.Base.Error, "compaction: schedule raises no error");
   CHECK(count_pairs(&fc.Base) == 1,
        "compaction: r300_classic_schedule's own merge fits the one-slot budget without deferring");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
}

/* Three independent RGB-only pairs and no Alpha-only partner: the merge
 * search can pair at most one of them away (there is nothing to pair the
 * other two with), so the merged count (2) still exceeds a one-slot budget
 * and the pass must defer to rc_pair_schedule -- proven the same way
 * test_cycle_defers_without_hard_fail proves a defer, by running the
 * identical hand-built stream through r300_classic_schedule and through
 * rc_pair_schedule directly on two separately-seeded compiler instances and
 * requiring the same final pair count. */
static void
test_compaction_still_defers_when_merge_insufficient(void)
{
   struct rc_pair_instruction rgb_a, rgb_b, alpha_c;

   memset(&rgb_a, 0, sizeof(rgb_a));
   rgb_a.RGB.Opcode = RC_OPCODE_MOV;
   rgb_a.RGB.WriteMask = RC_MASK_XYZ;
   rgb_a.RGB.DestIndex = 0;
   rgb_a.Alpha.Opcode = RC_OPCODE_NOP;

   memset(&rgb_b, 0, sizeof(rgb_b));
   rgb_b.RGB.Opcode = RC_OPCODE_MOV;
   rgb_b.RGB.WriteMask = RC_MASK_XYZ;
   rgb_b.RGB.DestIndex = 1;
   rgb_b.Alpha.Opcode = RC_OPCODE_NOP;

   memset(&alpha_c, 0, sizeof(alpha_c));
   alpha_c.RGB.Opcode = RC_OPCODE_NOP;
   alpha_c.Alpha.Opcode = RC_OPCODE_MOV;
   alpha_c.Alpha.WriteMask = RC_MASK_W;
   alpha_c.Alpha.DestIndex = 2;

   struct r300_fragment_program_compiler fc_new;
   struct rc_regalloc_state rs_new;
   fs_compiler_init(&fc_new, &rs_new);
   fc_new.Base.max_alu_insts = 1;
   struct rc_instruction *tail_new =
      append_pair(&fc_new.Base, &fc_new.Base.Program.Instructions, &rgb_a);
   tail_new = append_pair(&fc_new.Base, tail_new, &rgb_b);
   append_pair(&fc_new.Base, tail_new, &alpha_c);

   struct r300_fragment_program_compiler fc_legacy;
   struct rc_regalloc_state rs_legacy;
   fs_compiler_init(&fc_legacy, &rs_legacy);
   fc_legacy.Base.max_alu_insts = 1;
   struct rc_instruction *tail_legacy =
      append_pair(&fc_legacy.Base, &fc_legacy.Base.Program.Instructions, &rgb_a);
   tail_legacy = append_pair(&fc_legacy.Base, tail_legacy, &rgb_b);
   append_pair(&fc_legacy.Base, tail_legacy, &alpha_c);

   int opt = 0;
   r300_classic_schedule(&fc_new.Base, &opt);
   rc_pair_schedule(&fc_legacy.Base, &opt);

   CHECK(!fc_new.Base.Error,
        "compaction_insufficient: r300_classic_schedule defers instead of handing over "
        "a still-over-budget schedule");
   CHECK(!fc_legacy.Base.Error, "compaction_insufficient: rc_pair_schedule control raises no error");
   CHECK(count_pairs(&fc_new.Base) == count_pairs(&fc_legacy.Base),
        "compaction_insufficient: deferred schedule matches rc_pair_schedule's own outcome");

   rc_destroy(&fc_new.Base);
   rc_destroy_regalloc_state(&rs_new);
   rc_destroy(&fc_legacy.Base);
   rc_destroy_regalloc_state(&rs_legacy);
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

   /* A VEC-collect ALU program is handled directly now (per-channel single
    * assignment plus the pair's own merge search); a texture program still
    * defers to the legacy scheduler entirely (a TEX block is outside the
    * pass's model).  Both must still leave a legality-passing stream. */
   legal_after_schedule_check("vec_collect", build_vec_collect);
   legal_after_schedule_check("tex", build_tex);

   /* Safety: every shape the scheduler cannot handle itself defers to
    * rc_pair_schedule rather than emitting something illegal or hard-failing
    * the compile. */
   test_cycle_defers_without_hard_fail();
   test_output_waw_preserves_order();
   test_compaction_merges_itself();
   test_compaction_still_defers_when_merge_insufficient();

   glsl_type_singleton_decref();
   if (failures) {
      fprintf(stderr, "r300_classic_new_sched_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_classic_new_sched_test: all checks passed\n");
   return 0;
}
