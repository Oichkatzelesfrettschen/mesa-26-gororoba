/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "util/ralloc.h"

#include "classic/r300_classic_emit.h"
#include "classic/r300_classic_regalloc.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "radeon_code.h"
#include "radeon_compiler.h"
#include "radeon_regalloc.h"

/* Phase-4 exit criterion: the classic-path rc_program is structurally valid
 * per RC's own validators.  The test runs the whole classic ladder -- NIR
 * through the production optimizer, selection, SSA register allocation,
 * emission -- and then the real backend pass chain
 * (r3xx_compile_fragment_program: native rewrite, pair translate, pair
 * regalloc, rc_validate_final_shader, r300BuildFragmentProgramHwCode).  A
 * structurally invalid program fails those passes with Base.Error set; a
 * valid one produces nonempty R300 hardware code. */

static int failures;

#define CHECK(cond, what)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL: %s\n", what);                                \
         failures++;                                                         \
      }                                                                      \
   } while (0)

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

static nir_builder
fs_builder(const char *name)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
   };
   return nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "%s",
                                         name);
}

static nir_shader *
build_fmad_shader(void)
{
   nir_builder b = fs_builder("classic_emit_fmad");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "in_color");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in);
   nir_def *c1 = nir_imm_vec4(&b, 0.5f, 0.5f, 0.5f, 0.5f);
   nir_def *c2 = nir_imm_vec4(&b, 0.25f, 0.25f, 0.25f, 0.25f);
   nir_def *mad = nir_build_alu3(&b, nir_op_fmad, v, c1, c2);
   nir_def *m = nir_fmax(&b, mad, nir_imm_vec4(&b, 0, 0, 0, 0));
   nir_store_var(&b, out, m, 0xf);
   return b.shader;
}

static void
case_full_ladder_compiles_to_hw_code(void)
{
   void *ctx = ralloc_context(NULL);
   nir_shader *s = build_fmad_shader();

   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(s, r300_screen(ps));

   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result sel;
   CHECK(r300_classic_select(ctx, s, t, NULL, 0, R300_FS_INPUT_INTERPOLATED, NULL, &sel), "selection ran");
   CHECK(sel.program != NULL, "shader selected");
   if (!sel.program) {
      if (sel.reject_reason)
         fprintf(stderr, "  rejected: %s\n", sel.reject_reason);
      ralloc_free(ctx);
      return;
   }

   struct r300_classic_regalloc_result ra;
   CHECK(r300_classic_regalloc(ctx, sel.program, &ra), "allocation ran");
   CHECK(ra.temp_of_ssa != NULL, "allocation fits");

   struct rc_regalloc_state rs;
   rc_init_regalloc_state(&rs, RC_FRAGMENT_PROGRAM);
   struct r300_fragment_program_compiler fc;
   memset(&fc, 0, sizeof(fc));
   rc_init(&fc.Base, &rs);
   fc.Base.type = RC_FRAGMENT_PROGRAM;
   fc.Base.has_half_swizzles = true;
   fc.Base.has_presub = true;
   fc.Base.has_omod = true;
   fc.Base.max_temp_regs = t->max_temp_regs;
   fc.Base.max_constants = t->max_const_regs;
   fc.Base.max_alu_insts = t->max_alu_insts;
   fc.Base.max_tex_insts = t->max_tex_insts;
   struct rX00_fragment_program_code code;
   memset(&code, 0, sizeof(code));
   fc.code = &code;
   fc.AllocateHwInputs = allocate_identity_inputs;

   CHECK(r300_classic_emit(sel.program, &sel.immediates, &sel.states, &fc),
         "emission succeeded");

   r3xx_compile_fragment_program(&fc);
   CHECK(!fc.Base.Error, "backend pass chain accepts the classic program");
   if (fc.Base.Error && fc.Base.ErrorMsg)
      fprintf(stderr, "  backend said: %s\n", fc.Base.ErrorMsg);
   CHECK(code.code.r300.alu.length > 0, "hardware ALU code generated");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
   ralloc_free(ctx);
}

static nir_shader *
build_discard_shader(void)
{
   nir_builder b = fs_builder("classic_emit_discard");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "in_color");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in);
   nir_def *cond = nir_flt(&b, nir_channel(&b, v, 3),
                           nir_imm_float(&b, 0.5f));
   nir_terminate_if(&b, cond);
   nir_store_var(&b, out, v, 0xf);
   return b.shader;
}

/* A discard shader must carry KIL through the whole ladder into hardware
 * code -- the backend pass chain, not just the front end, accepts it. */
static void
case_discard_ladder_compiles_to_hw_code(void)
{
   void *ctx = ralloc_context(NULL);
   nir_shader *s = build_discard_shader();

   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(s, r300_screen(ps));

   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result sel;
   CHECK(r300_classic_select(ctx, s, t, NULL, 0, R300_FS_INPUT_INTERPOLATED, NULL, &sel), "selection ran");
   CHECK(sel.program != NULL, "discard shader selected");
   if (!sel.program) {
      if (sel.reject_reason)
         fprintf(stderr, "  rejected: %s\n", sel.reject_reason);
      ralloc_free(ctx);
      return;
   }

   struct r300_classic_regalloc_result ra;
   CHECK(r300_classic_regalloc(ctx, sel.program, &ra), "allocation ran");
   CHECK(ra.temp_of_ssa != NULL, "allocation fits");

   struct rc_regalloc_state rs;
   rc_init_regalloc_state(&rs, RC_FRAGMENT_PROGRAM);
   struct r300_fragment_program_compiler fc;
   memset(&fc, 0, sizeof(fc));
   rc_init(&fc.Base, &rs);
   fc.Base.type = RC_FRAGMENT_PROGRAM;
   fc.Base.has_half_swizzles = true;
   fc.Base.has_presub = true;
   fc.Base.has_omod = true;
   fc.Base.max_temp_regs = t->max_temp_regs;
   fc.Base.max_constants = t->max_const_regs;
   fc.Base.max_alu_insts = t->max_alu_insts;
   fc.Base.max_tex_insts = t->max_tex_insts;
   struct rX00_fragment_program_code code;
   memset(&code, 0, sizeof(code));
   fc.code = &code;
   fc.AllocateHwInputs = allocate_identity_inputs;

   CHECK(r300_classic_emit(sel.program, &sel.immediates, &sel.states, &fc),
         "emission succeeded");

   bool has_kil = false;
   for (struct rc_instruction *inst = fc.Base.Program.Instructions.Next;
        inst != &fc.Base.Program.Instructions; inst = inst->Next)
      if (inst->U.I.Opcode == RC_OPCODE_KIL ||
          inst->U.I.Opcode == RC_OPCODE_KILP)
         has_kil = true;
   CHECK(has_kil, "emitted program carries a discard");

   r3xx_compile_fragment_program(&fc);
   CHECK(!fc.Base.Error, "backend pass chain accepts the discard program");
   if (fc.Base.Error && fc.Base.ErrorMsg)
      fprintf(stderr, "  backend said: %s\n", fc.Base.ErrorMsg);
   CHECK(code.code.r300.alu.length > 0, "hardware ALU code generated");

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
   ralloc_free(ctx);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   case_full_ladder_compiles_to_hw_code();
   case_discard_ladder_compiles_to_hw_code();
   glsl_type_singleton_decref();
   if (failures) {
      fprintf(stderr, "r300_classic_emit_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_classic_emit_test: all checks passed\n");
   return 0;
}
