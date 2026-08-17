/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Admission-versus-CSO identity for the R2VB producer plan: the program the
 * throwaway admission measurement compiles must be the program the actual
 * fragment-shader CSO compiles.  The two paths share the translate body but
 * diverge around it -- CSO creation (r300_create_fs_state_internal) reruns
 * r300_optimize_nir on the handed-over NIR and precompiles through the
 * variant machinery (r300_pick_fragment_shader) with a derived key, while
 * the measurement (r300_fs_measure_nir_admission) translates directly --
 * so a semantics or pass divergence between them would let the plan admit a
 * program the producer CSO then compiles differently.  Both paths run here
 * under the same R300_FS_INPUT_R2VB_FLAT_VERTEX contract on the plan's own
 * canonical NIR, and identity is proven at the emitted-program level, not
 * the resource-vector level: two different programs can share {alu.length,
 * pixsize, constants.Count}, so agreement is required on (1) the plan's
 * recorded cost, (2) the complete r300_fragment_program_code -- every ALU
 * instruction word, the tex program, config, code_offset/addr, pixsize --
 * byte-compared as a struct, which is sound because the struct is a flat
 * pure function of the input program with no pointer or address fields, and
 * (3) the constant list value-wise (type, use mask, and the active union
 * arm per constant; the emission appends constants in traversal order, so
 * index-by-index comparison is exact).  Byte-equal emission also proves the
 * CSO path's second r300_optimize_nir run is idempotent on the plan's
 * canonical NIR.  Every leg runs: the fitting producer's baseline, and both
 * rebuilt halves (r300_mp_build_carry_pass_a / r300_mp_build_pos_pass_b on
 * the retained partition) of the over-budget producer's split.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "r300_context.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_nir_ssa_cut.h"
#include "r300_r2vb_plan.h"
#include "r300_screen.h"
#include "radeon_regalloc.h"

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

static struct r300_screen g_screen;
static struct r300_context g_context;

static void
fake_stack_init(bool is_r500)
{
   memset(&g_screen, 0, sizeof(g_screen));
   g_screen.caps.has_tcl = false;
   g_screen.caps.is_r400 = false;
   g_screen.caps.is_r500 = is_r500;
   r300_screen_init_nir_options(&g_screen);

   memset(&g_context, 0, sizeof(g_context));
   g_context.screen = &g_screen;
   g_context.context.screen = &g_screen.screen;
   /* The CSO leg runs the real create/pick/delete path, so the fake context
    * carries the real state vtable. */
   r300_init_state_functions(&g_context);
   rc_init_regalloc_state(&g_context.fs_regalloc_state, RC_FRAGMENT_PROGRAM);
   g_context.viewport.scale[0] = 320.0f;
   g_context.viewport.scale[1] = 240.0f;
   g_context.viewport.scale[2] = 0.5f;
   g_context.viewport.translate[0] = 320.0f;
   g_context.viewport.translate[1] = 240.0f;
   g_context.viewport.translate[2] = 0.5f;
}

struct vs_build {
   nir_builder b;
   nir_def *pos;
   nir_variable *out_pos;
};

static struct vs_build
begin_vs(const char *name)
{
   struct vs_build v;
   v.b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, g_screen.screen.nir_options[MESA_SHADER_VERTEX],
      "%s", name);
   nir_variable *in_pos = nir_variable_create(
      v.b.shader, nir_var_shader_in, glsl_vec4_type(), "in_pos");
   in_pos->data.location = VERT_ATTRIB_GENERIC0;
   in_pos->data.driver_location = 0;
   v.out_pos = nir_variable_create(v.b.shader, nir_var_shader_out,
                                   glsl_vec4_type(), "gl_Position");
   v.out_pos->data.location = VARYING_SLOT_POS;
   v.out_pos->data.driver_location = 0;

   /* The one-field std430 interface shape the in-driver producers declare;
    * r300_optimize_nir sizes the uniform block from interface_type. */
   const struct glsl_type *ubo_type = glsl_array_type(glsl_vec4_type(), 4, 16);
   nir_variable *ubo = nir_variable_create(v.b.shader, nir_var_mem_ubo,
                                           ubo_type, "matrix");
   ubo->data.binding = 0;
   ubo->data.driver_location = 0;
   ubo->data.explicit_binding = 1;
   struct glsl_struct_field ubo_field = {
      .type = ubo_type,
      .name = "data",
      .location = -1,
   };
   ubo->interface_type = glsl_interface_type(
      &ubo_field, 1, GLSL_INTERFACE_PACKING_STD430, false,
      "__r300_cso_identity_ubo");
   v.b.shader->info.num_ubos = 1;

   v.pos = nir_load_var(&v.b, in_pos);
   return v;
}

static nir_shader *
end_vs(struct vs_build *v, nir_def *result)
{
   nir_store_var(&v->b, v->out_pos, result, 0xf);
   nir_validate_shader(v->b.shader, "r2vb admission-cso identity VS");
   return v->b.shader;
}

static nir_def *
fmad_chain(nir_builder *b, nir_def *base, unsigned length)
{
   nir_def *v = base;
   for (unsigned i = 0; i < length; i++)
      v = nir_ffma(b, v, nir_imm_float(b, 1.0001f + (float)(i % 7) * 0.001f),
                   base);
   return v;
}

static nir_shader *
build_producer(unsigned chain_length, const char *name)
{
   struct vs_build v = begin_vs(name);
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), chain_length);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

/* Over-budget producer with a bounded signed integer computed before the
 * chain and consumed after it, the shape the typed diagnostic route
 * executes: the clamp proves the carried component inside the R300 FP24
 * exact-integer window, so the plan admits SINT transport and the typed
 * half rebuild is the program under identity test. */
static nir_shader *
build_sint_carry_producer(const char *name)
{
   struct vs_build v = begin_vs(name);
   nir_def *y = nir_channel(&v.b, v.pos, 1);
   nir_def *s = nir_f2i32(&v.b, y);
   nir_def *typed = nir_imin(&v.b, nir_imax(&v.b, s,
                                            nir_imm_int(&v.b, -1000)),
                             nir_imm_int(&v.b, 1000));
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 90);
   nir_def *back = nir_i2f32(&v.b, typed);
   nir_def *sum = nir_fadd(&v.b, c, back);
   return end_vs(&v, nir_replicate(&v.b, sum, 4));
}

static bool
partition_has_typed_transport(const struct r300_mp_partition *p)
{
   for (unsigned i = 0; i < p->num_bases; i++) {
      switch (p->r2vb_transport[i]) {
      case R300_MP_R2VB_SINT:
      case R300_MP_R2VB_UINT:
      case R300_MP_R2VB_BOOL1:
      case R300_MP_R2VB_BOOL32:
         return true;
      default:
         break;
      }
   }
   return false;
}

static bool
costs_equal(const struct r300_fs_admission_cost *a,
            const struct r300_fs_admission_cost *b)
{
   return a->alu == b->alu && a->temps == b->temps && a->consts == b->consts;
}

static nir_shader *
build_r500_admission_fs(unsigned chain_length, const char *name)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, g_screen.screen.nir_options[MESA_SHADER_FRAGMENT],
      "%s", name);
   nir_variable *in = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "in_color");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_FLAT;
   nir_variable *out = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *base = nir_channel(&b, nir_load_var(&b, in), 0);
   nir_def *value = fmad_chain(&b, base, chain_length);
   nir_store_var(&b, out, nir_replicate(&b, value, 4), 0xf);
   nir_validate_shader(b.shader, "r500 admission cost");
   return b.shader;
}

static void
run_r500_admission_cost(void)
{
   nir_shader *fits = build_r500_admission_fs(4, "r500_admission_fits");
   struct r300_fs_admission_cost cost = {17, 19, 23};
   unsigned alu = 29;
   enum r300_fs_admission verdict = r300_fs_measure_nir_admission(
      &g_context, fits, &alu, R300_FS_INPUT_R2VB_FLAT_VERTEX, &cost);
   CHECK(verdict == R300_FS_ADMIT_FITS,
         "R500 known-good shader fits the admission budget");
   CHECK(alu == cost.alu && cost.alu > 0,
         "R500 admission reports emitted instruction slots in both outputs");
   CHECK(cost.temps > 0 && cost.consts > 0,
         "R500 admission reports temporary and constant high-water marks");
   ralloc_free(fits);

   /* A null shader is a known-bad admission input. */
   struct r300_fs_admission_cost rejected_cost = {17, 19, 23};
   unsigned rejected_alu = 29;
   verdict = r300_fs_measure_nir_admission(
      &g_context, NULL, &rejected_alu, R300_FS_INPUT_R2VB_FLAT_VERTEX,
      &rejected_cost);
   CHECK(verdict == R300_FS_ADMIT_REJECT && rejected_alu == 0 &&
            rejected_cost.alu == 0 && rejected_cost.temps == 0 &&
            rejected_cost.consts == 0,
         "R500 rejected input preserves the zeroed cost invariant");
}

/* Value-wise constant comparison: type, use mask, and the active union arm.
 * The inactive union bytes stay out of the comparison because the list
 * storage is not zero-initialized past the active arm. */
static bool
constants_identical(const struct rc_constant *a, const struct rc_constant *b,
                    unsigned count)
{
   for (unsigned i = 0; i < count; i++) {
      if (a[i].Type != b[i].Type || a[i].UseMask != b[i].UseMask)
         return false;
      switch (a[i].Type) {
      case RC_CONSTANT_EXTERNAL:
         if (a[i].u.External != b[i].u.External)
            return false;
         break;
      case RC_CONSTANT_IMMEDIATE:
         /* memcmp gives the bit-exact float comparison identity needs. */
         if (memcmp(a[i].u.Immediate, b[i].u.Immediate,
                    sizeof(a[i].u.Immediate)))
            return false;
         break;
      case RC_CONSTANT_STATE:
         if (memcmp(a[i].u.State, b[i].u.State, sizeof(a[i].u.State)))
            return false;
         break;
      }
   }
   return true;
}

/* One identity leg: the given program, measured and CSO-compiled under the
 * FLAT contract, must reproduce the plan-recorded cost on both paths and
 * emit the identical normalized program. */
static void
check_identity(const char *label, const nir_shader *program,
               const struct r300_fs_admission_cost *expected)
{
   char name[128];

   /* Measurement leg, with the emitted-program snapshot. */
   nir_shader *measured = nir_shader_clone(NULL, program);
   struct r300_fs_admission_cost cost;
   struct r300_fs_admission_program prog;
   enum r300_fs_admission verdict = r300_fs_measure_nir_admission_program(
      &g_context, measured, NULL, R300_FS_INPUT_R2VB_FLAT_VERTEX, &cost,
      &prog);
   snprintf(name, sizeof(name), "%s measurement fits at %u/%u/%u", label,
            expected->alu, expected->temps, expected->consts);
   CHECK(verdict == R300_FS_ADMIT_FITS && costs_equal(&cost, expected), name);

   /* CSO leg: create_fs_state_internal takes ownership of the clone and
    * precompiles through the variant machinery. */
   struct pipe_shader_state st = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = nir_shader_clone(NULL, program),
   };
   struct r300_fragment_shader *fs = r300_create_fs_state_internal(
      &g_context.context, &st, R300_FS_INPUT_R2VB_FLAT_VERTEX);
   snprintf(name, sizeof(name), "%s CSO compiles", label);
   CHECK(fs && fs->shader && !fs->shader->error && !fs->shader->dummy, name);
   if (fs && fs->shader && !fs->shader->error && !fs->shader->dummy &&
       verdict == R300_FS_ADMIT_FITS) {
      struct r300_fs_admission_cost cso_cost = {
         .alu = fs->shader->code.code.r300.alu.length,
         .temps = fs->shader->code.code.r300.pixsize,
         .consts = fs->shader->code.constants.Count,
      };
      snprintf(name, sizeof(name), "%s CSO cost matches the measurement",
               label);
      CHECK(costs_equal(&cso_cost, expected), name);

      /* Program identity: the whole emitted code block byte-compares (the
       * struct is pointer-free and both legs CALLOC the backing store, so
       * the zeroed instruction tails compare equal too), and the constant
       * lists match value-wise. */
      snprintf(name, sizeof(name), "%s emitted program words identical",
               label);
      CHECK(memcmp(&prog.code, &fs->shader->code.code.r300,
                   sizeof(prog.code)) == 0, name);
      snprintf(name, sizeof(name), "%s constant lists identical", label);
      CHECK(prog.num_constants == fs->shader->code.constants.Count &&
            constants_identical(prog.constants,
                                fs->shader->code.constants.Constants,
                                prog.num_constants), name);
   }
   if (fs)
      g_context.context.delete_fs_state(&g_context.context, fs);
   r300_fs_admission_program_release(&prog);
   ralloc_free(measured);
}

static void
run_space(enum r300_r2vb_position_space space)
{
   const char *space_name =
      space == R300_R2VB_POSITION_WINDOW ? "window" : "clip";
   char label[96];

   /* Fitting producer: the identity holds on the plan's canonical baseline
    * candidate. */
   nir_shader *fits = build_producer(8, "cso_identity_fits");
   struct r300_r2vb_producer_plan plan;
   bool ran = r300_r2vb_plan_producer(&g_context, fits, false, space, &plan);
   snprintf(label, sizeof(label), "fits/%s plans SINGLE", space_name);
   CHECK(ran && plan.action == R300_R2VB_PLAN_SINGLE && plan.candidate, label);
   if (ran && plan.action == R300_R2VB_PLAN_SINGLE && plan.candidate) {
      snprintf(label, sizeof(label), "fits/%s baseline", space_name);
      check_identity(label, plan.candidate, &plan.baseline);
   }
   if (ran)
      r300_r2vb_plan_release(&plan);
   ralloc_free(fits);

   /* Over-budget producer: the identity holds on both halves rebuilt from
    * the retained winning partition, exactly as the split route builds
    * them. */
   nir_shader *over = build_producer(90, "cso_identity_over");
   ran = r300_r2vb_plan_producer(&g_context, over, false, space, &plan);
   snprintf(label, sizeof(label), "over/%s plans SPLIT", space_name);
   CHECK(ran && plan.action == R300_R2VB_PLAN_SPLIT && plan.candidate, label);
   if (ran && plan.action == R300_R2VB_PLAN_SPLIT && plan.candidate) {
      nir_shader *pass_a =
         r300_mp_build_carry_pass_a(plan.candidate, &plan.partition);
      nir_shader *pass_b = r300_mp_build_pos_pass_b(
         plan.candidate, &plan.partition, plan.num_position_inputs);
      snprintf(label, sizeof(label), "over/%s halves rebuild", space_name);
      CHECK(pass_a && pass_b, label);
      if (pass_a) {
         r300_optimize_nir(pass_a, &g_screen.caps);
         snprintf(label, sizeof(label), "over/%s carry pass", space_name);
         check_identity(label, pass_a, &plan.pass_a_cost);
         ralloc_free(pass_a);
      }
      if (pass_b) {
         r300_optimize_nir(pass_b, &g_screen.caps);
         snprintf(label, sizeof(label), "over/%s position pass", space_name);
         check_identity(label, pass_b, &plan.pass_b_cost);
         ralloc_free(pass_b);
      }
   }
   if (ran)
      r300_r2vb_plan_release(&plan);
   ralloc_free(over);

   /* Typed over-budget producer: the identity holds on the exact halves
    * the typed diagnostic route executes -- rebuilt from the plan's owned
    * candidate and retained partition, optimized before their admission
    * costs are compared, with a typed transport in the selected carry. */
   nir_shader *typed = build_sint_carry_producer("cso_identity_sint");
   ran = r300_r2vb_plan_producer(&g_context, typed, false, space, &plan);
   snprintf(label, sizeof(label), "sint/%s plans SPLIT with typed carry",
            space_name);
   CHECK(ran && plan.action == R300_R2VB_PLAN_SPLIT && plan.candidate &&
            plan.has_typed_source &&
            partition_has_typed_transport(&plan.partition),
         label);
   if (ran && plan.action == R300_R2VB_PLAN_SPLIT && plan.candidate) {
      nir_shader *pass_a =
         r300_mp_build_carry_pass_a(plan.candidate, &plan.partition);
      nir_shader *pass_b = r300_mp_build_pos_pass_b(
         plan.candidate, &plan.partition, plan.num_position_inputs);
      snprintf(label, sizeof(label), "sint/%s halves rebuild", space_name);
      CHECK(pass_a && pass_b, label);
      if (pass_a) {
         r300_optimize_nir(pass_a, &g_screen.caps);
         snprintf(label, sizeof(label), "sint/%s carry pass", space_name);
         check_identity(label, pass_a, &plan.pass_a_cost);
         ralloc_free(pass_a);
      }
      if (pass_b) {
         r300_optimize_nir(pass_b, &g_screen.caps);
         snprintf(label, sizeof(label), "sint/%s position pass", space_name);
         check_identity(label, pass_b, &plan.pass_b_cost);
         ralloc_free(pass_b);
      }
   }
   if (ran)
      r300_r2vb_plan_release(&plan);
   ralloc_free(typed);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   fake_stack_init(false);

   run_space(R300_R2VB_POSITION_CLIP);
   run_space(R300_R2VB_POSITION_WINDOW);

   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   fake_stack_init(true);
   run_r500_admission_cost();
   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   glsl_type_singleton_decref();
   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
