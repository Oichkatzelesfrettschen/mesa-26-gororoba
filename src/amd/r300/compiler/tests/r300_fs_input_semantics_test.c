/*
 * SPDX-License-Identifier: MIT
 *
 * Fragment input-semantics helper oracle.
 *
 * r300_nir_apply_fs_input_semantics selects the float-to-int conversion
 * contract a fragment shader compiles under.  R300_FS_INPUT_INTERPOLATED nudges
 * every f2i32/f2u32 source toward its own sign by a factor of 1 + 2^-15 (the
 * FP24 plane-interpolation delivery correction); R300_FS_INPUT_R2VB_FLAT_VERTEX
 * omits the nudge, converting like the Draw vertex path.  This test isolates
 * the helper ahead of either front end on two axes:
 *
 *   1. Shape: under INTERPOLATED the helper inserts exactly one fmul by the
 *      constant 0x3f800100 (1.000030517578125) on each f2i/f2u source; under
 *      FLAT_VERTEX it inserts none.  A shader-authored ftrunc/ffloor is a
 *      control: neither mode rewrites it, because the correction targets the
 *      compiler's own float-to-int lowering, not explicit truncation.  A
 *      float-only shader is a control: neither mode touches it.
 *
 *   2. Numeric boundary: after the nudge and nir_lower_int_to_float (which
 *      lowers f2i32 to ftrunc and f2u32 to ffloor), the folded integer result
 *      of a value one ULP below a positive integer differs by one between the
 *      modes, and matches at and above the integer.  This proves the enum has
 *      an observable, intentional effect; it does not claim the flat result is
 *      FP24-equivalent to gallivm, which is a separate source-domain property.
 *
 * The shape shaders feed a dynamic varying so the conversion cannot fold away
 * before the helper runs; the boundary shaders feed a literal so the whole
 * chain folds to a readable constant.
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "nir.h"
#include "nir_builder.h"

#include "r300_nir.h"

static int failures;

#define CHECK(cond, what)                                                     \
   do {                                                                       \
      if (!(cond)) {                                                          \
         fprintf(stderr, "FAIL: %s\n", (what));                              \
         failures++;                                                          \
      }                                                                       \
   } while (0)

/* The exact single-precision bit pattern of 1 + 2^-15, the away-from-zero
 * multiplier r300_nir_lower_f2i_epsilon materializes. */
#define R300_F2I_EPS_MULTIPLIER_BITS 0x3f800100u

enum conv_kind {
   CONV_F2I, /* signed: lowers to ftrunc (round toward zero) */
   CONV_F2U, /* unsigned: lowers to ffloor (round toward -inf) */
};

/* A fragment shader whose single scalar output is i2f(f2i(x)) / u2f(f2u(x)).
 * A dynamic varying x keeps the conversion from folding; a literal x lets the
 * whole chain fold once the modes and lowering have run. */
static nir_shader *
build_conv(enum conv_kind kind, bool dynamic, float literal)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_fs_input_semantics");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_float_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *x;
   if (dynamic) {
      nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_float_type(), "in0");
      in->data.location = VARYING_SLOT_VAR0;
      in->data.driver_location = 0;
      in->data.interpolation = INTERP_MODE_SMOOTH;
      x = nir_load_var(&b, in);
   } else {
      x = nir_imm_float(&b, literal);
   }

   nir_def *as_int = kind == CONV_F2U ? nir_f2u32(&b, x) : nir_f2i32(&b, x);
   nir_def *back = kind == CONV_F2U ? nir_u2f32(&b, as_int)
                                    : nir_i2f32(&b, as_int);
   nir_store_var(&b, out, back, 0x1);
   return b.shader;
}

/* A fragment shader whose output is a shader-authored ftrunc/ffloor of a
 * dynamic varying: the helper must leave it untouched under both modes. */
static nir_shader *
build_explicit_round(nir_op op)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_fs_input_semantics_round");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_float_type(), "in0");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_float_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *x = nir_load_var(&b, in);
   nir_def *r = nir_build_alu1(&b, op, x);
   nir_store_var(&b, out, r, 0x1);
   return b.shader;
}

/* A float-only fragment shader (fmad of a dynamic varying): neither mode has
 * an f2i/f2u source to touch, so the helper is a no-op either way. */
static nir_shader *
build_float_only(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_fs_input_semantics_float");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_float_type(), "in0");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_float_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *x = nir_load_var(&b, in);
   nir_def *r = nir_fadd_imm(&b, nir_fmul_imm(&b, x, 3.0f), 0.5f);
   nir_store_var(&b, out, r, 0x1);
   return b.shader;
}

/* Count fmul instructions one of whose sources is the epsilon multiplier
 * constant.  This is the fingerprint the interpolated correction leaves and
 * the flat contract must not. */
static unsigned
count_epsilon_multipliers(nir_shader *s)
{
   unsigned count = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->op != nir_op_fmul)
               continue;
            for (unsigned i = 0; i < 2; i++) {
               if (!nir_src_is_const(alu->src[i].src))
                  continue;
               const unsigned comp = alu->src[i].swizzle[0];
               const uint32_t bits =
                  nir_const_value_as_uint(nir_src_as_const_value(alu->src[i].src)[comp], 32);
               if (bits == R300_F2I_EPS_MULTIPLIER_BITS)
                  count++;
            }
         }
      }
   }
   return count;
}

/* Count ftrunc/ffloor instructions: a control that the helper does not add,
 * remove, or alter shader-authored truncation. */
static unsigned
count_op(nir_shader *s, nir_op op)
{
   unsigned count = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            if (nir_instr_as_alu(instr)->op == op)
               count++;
         }
      }
   }
   return count;
}

static void
opt_to_fixpoint(nir_shader *s)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_dce);
   } while (progress);
}

/* Fold a constant-fed conversion under one mode and read the scalar the
 * fragment output store carries.  Runs the same helper -> int-to-float ->
 * fold sequence order the production front ends and the Draw path use. */
static bool
fold_conv(enum conv_kind kind, float literal,
          enum r300_fs_input_semantics mode, float *out_value)
{
   nir_shader *s = build_conv(kind, false, literal);
   NIR_PASS(_, s, r300_nir_apply_fs_input_semantics, mode);
   NIR_PASS(_, s, nir_lower_int_to_float);
   opt_to_fixpoint(s);

   bool found = false;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
               continue;
            if (!nir_src_is_const(intr->src[1])) {
               ralloc_free(s);
               return false;
            }
            *out_value = nir_src_comp_as_float(intr->src[1], 0);
            found = true;
         }
      }
   }
   ralloc_free(s);
   return found;
}

/* Group 1: the interpolated correction inserts exactly one epsilon multiply
 * per f2i/f2u source; the flat contract inserts none; both compile. */
static void
case_multiplier_shape(void)
{
   nir_shader *interp = build_conv(CONV_F2I, true, 0.0f);
   const bool ic = r300_nir_apply_fs_input_semantics(interp,
                                                     R300_FS_INPUT_INTERPOLATED);
   CHECK(ic, "interpolated f2i returns changed");
   CHECK(count_epsilon_multipliers(interp) == 1,
         "interpolated f2i inserts exactly one epsilon multiply");
   ralloc_free(interp);

   nir_shader *flat = build_conv(CONV_F2I, true, 0.0f);
   const bool fc = r300_nir_apply_fs_input_semantics(flat,
                                                     R300_FS_INPUT_R2VB_FLAT_VERTEX);
   CHECK(!fc, "flat f2i returns unchanged");
   CHECK(count_epsilon_multipliers(flat) == 0,
         "flat f2i inserts no epsilon multiply");
   ralloc_free(flat);

   nir_shader *interp_u = build_conv(CONV_F2U, true, 0.0f);
   r300_nir_apply_fs_input_semantics(interp_u, R300_FS_INPUT_INTERPOLATED);
   CHECK(count_epsilon_multipliers(interp_u) == 1,
         "interpolated f2u inserts exactly one epsilon multiply");
   ralloc_free(interp_u);

   nir_shader *flat_u = build_conv(CONV_F2U, true, 0.0f);
   r300_nir_apply_fs_input_semantics(flat_u, R300_FS_INPUT_R2VB_FLAT_VERTEX);
   CHECK(count_epsilon_multipliers(flat_u) == 0,
         "flat f2u inserts no epsilon multiply");
   ralloc_free(flat_u);
}

/* Controls: a shader-authored ftrunc/ffloor and a float-only shader carry no
 * f2i/f2u source, so neither mode adds an epsilon multiply or perturbs the
 * authored truncation. */
static void
case_controls_untouched(void)
{
   struct {
      const char *name;
      nir_op op;
   } rounds[] = {{"ftrunc", nir_op_ftrunc}, {"ffloor", nir_op_ffloor}};

   for (unsigned r = 0; r < ARRAY_SIZE(rounds); r++) {
      for (unsigned m = 0; m < 2; m++) {
         const enum r300_fs_input_semantics mode =
            m == 0 ? R300_FS_INPUT_INTERPOLATED : R300_FS_INPUT_R2VB_FLAT_VERTEX;
         nir_shader *s = build_explicit_round(rounds[r].op);
         r300_nir_apply_fs_input_semantics(s, mode);
         char what[128];
         snprintf(what, sizeof(what),
                  "authored %s untouched (no epsilon multiply), mode %u",
                  rounds[r].name, m);
         CHECK(count_epsilon_multipliers(s) == 0, what);
         snprintf(what, sizeof(what), "authored %s survives, mode %u",
                  rounds[r].name, m);
         CHECK(count_op(s, rounds[r].op) == 1, what);
         ralloc_free(s);
      }
   }

   for (unsigned m = 0; m < 2; m++) {
      const enum r300_fs_input_semantics mode =
         m == 0 ? R300_FS_INPUT_INTERPOLATED : R300_FS_INPUT_R2VB_FLAT_VERTEX;
      nir_shader *s = build_float_only();
      const bool changed = r300_nir_apply_fs_input_semantics(s, mode);
      char what[128];
      snprintf(what, sizeof(what), "float-only shader is a no-op, mode %u", m);
      CHECK(!changed, what);
      CHECK(count_epsilon_multipliers(s) == 0, what);
      ralloc_free(s);
   }
}

/* Group 2: the modes produce an observably different integer at a boundary one
 * ULP below a positive integer and agree at and above it.  f2i truncates
 * toward zero; f2u floors.  The table is the load-bearing distinction. */
static void
case_boundary_behavior(void)
{
   struct boundary_case {
      enum conv_kind kind;
      const char *name;
      float input;
      float interp_expect; /* trunc/floor(input * (1 + 2^-15)) */
      float flat_expect;   /* trunc/floor(input)              */
   };

   const float below2 = nextafterf(2.0f, -INFINITY);
   const float above2 = nextafterf(2.0f, INFINITY);
   const float below_neg2 = nextafterf(-2.0f, -INFINITY);
   const float above_neg2 = nextafterf(-2.0f, INFINITY);
   const float below1 = nextafterf(1.0f, -INFINITY);

   const struct boundary_case cases[] = {
      /* Signed f2i (trunc): the nudge lifts a just-under-2 value across the
       * boundary; at and above 2 both modes agree. */
      {CONV_F2I, "f2i below +2", below2, 2.0f, 1.0f},
      {CONV_F2I, "f2i at +2", 2.0f, 2.0f, 2.0f},
      {CONV_F2I, "f2i above +2", above2, 2.0f, 2.0f},
      /* Signed f2i on the negative side: trunc rounds toward zero, the nudge
       * pushes away from zero (more negative), so a value just above -2
       * (magnitude just under 2) crosses the boundary the way just-below-+2
       * does -- flat keeps -1, the nudge reaches -2.  At and below -2 both
       * modes already sit at -2. */
      {CONV_F2I, "f2i above -2", above_neg2, -2.0f, -1.0f},
      {CONV_F2I, "f2i at -2", -2.0f, -2.0f, -2.0f},
      {CONV_F2I, "f2i below -2", below_neg2, -2.0f, -2.0f},
      /* Unsigned f2u (floor) around +1 and +2. */
      {CONV_F2U, "f2u below +1", below1, 1.0f, 0.0f},
      {CONV_F2U, "f2u at +1", 1.0f, 1.0f, 1.0f},
      {CONV_F2U, "f2u below +2", below2, 2.0f, 1.0f},
      {CONV_F2U, "f2u at +2", 2.0f, 2.0f, 2.0f},
   };

   for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
      const struct boundary_case *c = &cases[i];
      float iv = 0.0f, fv = 0.0f;
      char what[160];

      snprintf(what, sizeof(what), "%s: interpolated folds", c->name);
      CHECK(fold_conv(c->kind, c->input, R300_FS_INPUT_INTERPOLATED, &iv),
            what);
      snprintf(what, sizeof(what), "%s: flat folds", c->name);
      CHECK(fold_conv(c->kind, c->input, R300_FS_INPUT_R2VB_FLAT_VERTEX, &fv),
            what);

      snprintf(what, sizeof(what), "%s: interpolated == %g (got %g)", c->name,
               c->interp_expect, iv);
      CHECK(iv == c->interp_expect, what);
      snprintf(what, sizeof(what), "%s: flat == %g (got %g)", c->name,
               c->flat_expect, fv);
      CHECK(fv == c->flat_expect, what);
   }
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   printf("r300 fragment input-semantics helper oracle\n");
   case_multiplier_shape();
   case_controls_untouched();
   case_boundary_behavior();
   glsl_type_singleton_decref();

   if (failures) {
      fprintf(stderr, "r300_fs_input_semantics_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_fs_input_semantics_test: all checks passed\n");
   return 0;
}
