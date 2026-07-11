/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * R2VB carry-BO producer split decision logic.
 *
 * The fragment-ALU vertex producer runs the bound VS as a position-pass
 * fragment program; a producer whose derived FS exceeds the 64-slot emit
 * ceiling splits at a single-block SSA cut into two FP32-exact halves.  Pass A
 * packs the cut-crossing values into one vec4 carry (FRAG_RESULT_DATA0); pass B
 * reads that carry back through a flat input and finishes the position program.
 * The split is adopted only when a cut exists whose carry fits one vec4 (four
 * components) and both halves compile under the ceiling.
 *
 * This unit pins that decision on synthetic single-block programs of
 * position-pass shape (a FRAG_RESULT_DATA0 vec4 output, VAR0 flat inputs),
 * without a live winsys: a long dependent multiply-add chain over budget with a
 * one-component carry admits, and a program whose every admissible cut crosses
 * more than four components is declined.  The oracle is the production
 * r300_optimize_nir + nir_to_rc + r3xx_compile_fragment_program against an
 * is_r500=false screen, the same authority r300_fs_measure_nir_admission uses.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "nir_to_rc.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_nir_ssa_cut.h"
#include "r300_screen.h"
#include "r300_shader_semantics.h"
#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_regalloc.h"

#define R300_FS_MAX_ALU 64

static unsigned g_failures;

#define CHECK(cond, name)                 \
   do {                                   \
      if (cond) {                         \
         printf("  ok   - %s\n", (name)); \
      } else {                            \
         printf("  FAIL - %s\n", (name)); \
         g_failures++;                    \
      }                                   \
   } while (0)

/* The r300_screen.c COMMON_NIR_OPTIONS fields the multiply-add chains depend
 * on: has_fmad keeps a multiply-add from splitting into mul + add, so the chain
 * reaches the emitter as fmad and the slot count matches production. */
static const nir_shader_compiler_options fs_options = {
   .float_mul_add32 =
      nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
   .lower_flrp32 = true,
};

static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   memset(s, 0, sizeof(*s));
   s->caps.has_tcl = true;
   s->caps.is_r500 = false;
   s->caps.is_r400 = false;
   return (struct pipe_screen *)s;
}

/* A single-block position-pass producer FS: one VAR0 flat input, one
 * FRAG_RESULT_DATA0 vec4 output, and a dependent scalar multiply-add chain of
 * `length` steps.  A varying-fed chain cannot constant-fold, so it reaches the
 * emitter at full length; length > 64 puts it over the R300 ALU ceiling with a
 * one-component carry (the single running scalar) at any interior cut. */
static nir_shader *
build_chain_fs(unsigned length)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                  &fs_options, "r2vb_split_chain");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_attr0");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_FLAT;

   nir_variable *out =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "out_pos");
   out->data.location = FRAG_RESULT_DATA0;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   nir_def *k = nir_channel(&b, v, 1);
   nir_def *x = nir_channel(&b, v, 0);
   for (unsigned i = 0; i < length; i++)
      x = nir_fmad(&b, x, k, nir_imm_float(&b, 0.5f));
   nir_store_var(&b, out, nir_vec4(&b, x, x, x, x), 0xf);
   return b.shader;
}

/* A single-block position-pass producer FS whose every interior cut crosses
 * more than four components: `nchains` independent dependent chains advanced
 * round-robin, so at the balance point every chain has a live running value
 * crossing the cut.  Their sum is the position output; nchains > 4 forces the
 * carry over one vec4 at every admissible cut. */
static nir_shader *
build_parallel_chains_fs(unsigned nchains, unsigned rounds)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                  &fs_options, "r2vb_split_parallel");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_attr0");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_FLAT;

   nir_variable *out =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "out_pos");
   out->data.location = FRAG_RESULT_DATA0;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   nir_def *k = nir_channel(&b, v, 1);
   nir_def *seed = nir_channel(&b, v, 0);

   nir_def *acc[16];
   for (unsigned j = 0; j < nchains; j++)
      acc[j] = nir_fadd_imm(&b, seed, (double)j + 1.0);
   for (unsigned r = 0; r < rounds; r++)
      for (unsigned j = 0; j < nchains; j++)
         acc[j] = nir_fmad(&b, acc[j], k, nir_imm_float(&b, (float)(j + 1)));

   nir_def *s = acc[0];
   for (unsigned j = 1; j < nchains; j++)
      s = nir_fadd(&b, s, acc[j]);
   nir_store_var(&b, out, nir_vec4(&b, s, s, s, s), 0xf);
   return b.shader;
}

/* Assign the fragment inputs nir_to_rc recorded to sequential hardware
 * registers, the order r300_fs.c's allocate_hardware_inputs uses. */
static void
gate_allocate_inputs(struct r300_fragment_program_compiler *c,
                     void (*allocate)(void *data, unsigned input, unsigned hwreg),
                     void *mydata)
{
   const struct r300_shader_semantics *inputs = c->UserData;
   int reg = 0;
   for (int i = 0; i < ATTR_COLOR_COUNT; i++)
      if (inputs->color[i] != ATTR_UNUSED)
         allocate(mydata, inputs->color[i], reg++);
   if (inputs->face != ATTR_UNUSED)
      allocate(mydata, inputs->face, reg++);
   for (int i = 0; i < ATTR_GENERIC_COUNT; i++)
      if (inputs->generic[i] != ATTR_UNUSED)
         allocate(mydata, inputs->generic[i], reg++);
   if (inputs->fog != ATTR_UNUSED)
      allocate(mydata, inputs->fog, reg++);
   if (inputs->wpos != ATTR_UNUSED)
      allocate(mydata, inputs->wpos, reg++);
}

/* Compile a fragment program through the production oracle and report the
 * fit verdict.  The R300 ALU budget is enforced by the RC backend's schedule,
 * not by nir_to_rc, so the full r3xx_compile_fragment_program is the authority;
 * c.Base.Error after it is the fit verdict. */
static bool
oracle_fits(nir_shader *nir)
{
   struct r300_screen screen = {0};
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state ext = {0};
   struct r300_fragment_shader_code fs_code = {0};
   union r300_shader_code code = { .f = &fs_code };
   r300_shader_semantics_reset(&fs_code.inputs);

   struct rc_regalloc_state rs;
   struct r300_fragment_program_compiler c;
   rc_init_regalloc_state(&rs, RC_FRAGMENT_PROGRAM);
   memset(&c, 0, sizeof(c));
   rc_init(&c.Base, &rs);
   c.Base.type = RC_FRAGMENT_PROGRAM;
   c.Base.is_r400 = false;
   c.Base.is_r500 = false;
   c.Base.has_half_swizzles = true;
   c.Base.has_presub = true;
   c.Base.has_omod = true;
   c.Base.max_temp_regs = 32;
   c.Base.max_constants = 32;
   c.Base.max_alu_insts = R300_FS_MAX_ALU;
   c.Base.max_tex_insts = 32;
   c.code = &fs_code.code;
   c.AllocateHwInputs = gate_allocate_inputs;
   c.UserData = &fs_code.inputs;

   r300_optimize_nir(nir, r300_screen(ps));
   nir_to_rc(nir, ps, ext, code, &c.Base);
   bool ok = false;
   if (!c.Base.Error) {
      c.Base.remove_unused_constants = true;
      r3xx_compile_fragment_program(&c);
      ok = !c.Base.Error;
   }
   rc_destroy(&c.Base);
   rc_destroy_regalloc_state(&rs);
   return ok;
}

/* An over-budget single-component-carry chain: a cut fits one vec4, both halves
 * compile under the ceiling, and the unsplit program does not. */
static void
case_over_budget_chain_splits(void)
{
   struct r300_screen screen = {0};
   struct pipe_screen *ps = fake_r300_screen(&screen);

   /* The unsplit program is over budget: the split has something to recover. */
   CHECK(!oracle_fits(build_chain_fs(90)),
         "90-step multiply-add chain exceeds the 64-slot ceiling unsplit");

   nir_shader *pos = build_chain_fs(90);
   r300_optimize_nir(pos, r300_screen(ps));

   struct r300_mp_partition part;
   bool have_cut = r300_mp_find_vec4_cut(pos, &part);
   CHECK(have_cut, "find_vec4_cut returns a single-vec4 cut for the chain");
   if (!have_cut) {
      ralloc_free(pos);
      return;
   }
   CHECK(part.total_comps <= 4, "chain carry fits one FP32 vec4");

   nir_shader *pass_a = r300_mp_build_carry_pass_a(pos, &part);
   nir_shader *pass_b = r300_mp_build_pos_pass_b(pos, &part, 1);
   ralloc_free(pos);
   CHECK(pass_a != NULL, "carry pass A builds");
   CHECK(pass_b != NULL, "position pass B builds");
   if (!pass_a || !pass_b) {
      if (pass_a)
         ralloc_free(pass_a);
      if (pass_b)
         ralloc_free(pass_b);
      return;
   }

   /* oracle_fits runs nir_to_rc, which takes ownership of the shader; do not
    * free pass_a/pass_b afterward. */
   CHECK(oracle_fits(pass_a), "carry pass A compiles under the 64-slot ceiling");
   CHECK(oracle_fits(pass_b),
         "position pass B compiles under the 64-slot ceiling");
}

/* A program whose every admissible cut crosses more than four components is
 * declined -- the split cannot pack the carry into one FP32 vec4. */
static void
case_wide_carry_declined(void)
{
   struct r300_screen screen = {0};
   struct pipe_screen *ps = fake_r300_screen(&screen);

   nir_shader *pos = build_parallel_chains_fs(5, 18);
   r300_optimize_nir(pos, r300_screen(ps));

   struct r300_mp_partition part;
   bool have_cut = r300_mp_find_vec4_cut(pos, &part);
   CHECK(!have_cut,
         "find_vec4_cut declines when every cut carries more than one vec4");
   ralloc_free(pos);
}

int
main(void)
{
   printf("r300 R2VB carry-BO producer split\n");
   case_over_budget_chain_splits();
   case_wide_carry_declined();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
