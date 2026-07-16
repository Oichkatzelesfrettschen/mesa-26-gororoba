/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Route-chain oracle for the R2VB producer plan, unit tier: hand-built
 * application vertex-shader NIR driven through the actual production chain --
 * shape validation, restage, plan, cut walk, and the emitted-slot admission
 * compile -- with a fake screen and context supplying only what that chain
 * reads (screen caps and NIR options, the fragment regalloc state, a debug
 * pointer, a zeroed viewport).  The typed split's earlier unreachability was a
 * representation-boundary failure: downstream producer tests were green while
 * the application-VS route rejected every typed shader, so this oracle pins
 * the plan's verdict at the exact entry the route consumes, not at the cut
 * machinery below it.
 *
 * Required rows: a fitting float producer plans SINGLE; an over-budget float
 * chain plans SPLIT with a float carry; over-budget bool/sint/uint typed
 * producers plan SPLIT with {f,b}/{f,i}/{f,u} carries; out-of-window signed
 * and unsigned carries and a mixed-signedness carry reject with their range
 * class as the primary reason; an under-budget typed producer rejects
 * TYPED_SINGLE_PASS_UNPROVEN; control flow rejects CONTROL_FLOW; a shader
 * without a uniform interface rejects IO_SHAPE; a wide frontier whose every
 * cut crosses more than one vec4 rejects with CARRY_WIDTH observed; and
 * planning is deterministic across repeated runs and across clip and window
 * spaces.
 */

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "r300_context.h"
#include "r300_r2vb_plan.h"
#include "r300_screen.h"
#include "r300_vs.h"
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

/* The r300_screen.c COMMON_NIR_OPTIONS fields the producer chains depend on:
 * has_fmad keeps a multiply-add fused so the restaged chain reaches the
 * emitter at full length and the slot count matches production. */
static const nir_shader_compiler_options vs_options = {
   .float_mul_add32 =
      nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
   .lower_flrp32 = true,
};

static struct r300_screen g_screen;
static struct r300_context g_context;

/* The plan chain reads r300->screen (caps, fragment NIR options),
 * r300->fs_regalloc_state, a debug pointer, and -- window space -- the bound
 * viewport; everything else stays zeroed. */
static struct r300_context *
fake_r300_context(void)
{
   memset(&g_screen, 0, sizeof(g_screen));
   g_screen.caps.is_r500 = false;
   g_screen.caps.is_r400 = false;
   g_screen.screen.nir_options[MESA_SHADER_FRAGMENT] = &vs_options;

   memset(&g_context, 0, sizeof(g_context));
   g_context.screen = &g_screen;
   /* Nonzero viewport: the window producer bakes scale/translate as
    * immediates, and zero scale would fold the whole position chain away. */
   g_context.viewport.scale[0] = 320.0f;
   g_context.viewport.scale[1] = 240.0f;
   g_context.viewport.scale[2] = 0.5f;
   g_context.viewport.translate[0] = 320.0f;
   g_context.viewport.translate[1] = 240.0f;
   g_context.viewport.translate[2] = 0.5f;
   rc_init_regalloc_state(&g_context.fs_regalloc_state, RC_FRAGMENT_PROGRAM);
   return &g_context;
}

/* Application-VS skeleton: in_pos at GENERIC0, gl_Position out, and a
 * declared UBO interface (the shape validation requires a uniform interface;
 * the arithmetic below stays input-fed so the emitted length is the chain's). */
struct vs_build {
   nir_builder b;
   nir_def *pos;
   nir_variable *out_pos;
};

static struct vs_build
begin_vs(const char *name, bool with_ubo)
{
   struct vs_build v;
   v.b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &vs_options, "%s",
                                        name);
   nir_variable *in_pos = nir_variable_create(
      v.b.shader, nir_var_shader_in, glsl_vec4_type(), "in_pos");
   in_pos->data.location = VERT_ATTRIB_GENERIC0;
   in_pos->data.driver_location = 0;
   v.out_pos = nir_variable_create(v.b.shader, nir_var_shader_out,
                                   glsl_vec4_type(), "gl_Position");
   v.out_pos->data.location = VARYING_SLOT_POS;
   v.out_pos->data.driver_location = 0;
   if (with_ubo) {
      /* The same one-field std430 interface shape the in-driver producers
       * declare; r300_optimize_nir sizes the block from interface_type. */
      const struct glsl_type *ubo_type =
         glsl_array_type(glsl_vec4_type(), 4, 16);
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
         "__r300_r2vb_plan_oracle_ubo");
      v.b.shader->info.num_ubos = 1;
   }
   v.pos = nir_load_var(&v.b, in_pos);
   return v;
}

static nir_shader *
end_vs(struct vs_build *v, nir_def *result)
{
   nir_store_var(&v->b, v->out_pos, result, 0xf);
   nir_validate_shader(v->b.shader, "r2vb plan oracle VS");
   return v->b.shader;
}

/* A dependent scalar fused multiply-add chain of `length` steps over base;
 * varying-fed, so it reaches the emitter at full length.  length > 64 puts
 * the restaged position pass over the R300 ALU ceiling with a one-component
 * float carry at any interior cut. */
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
build_float_fits(void)
{
   struct vs_build v = begin_vs("plan_float_fits", true);
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 8);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

static nir_shader *
build_float_over_budget(void)
{
   struct vs_build v = begin_vs("plan_float_over", true);
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 90);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

/* Over-budget chain whose result is selected through a boolean computed
 * before the chain: the bool1 def stays live across every interior cut, so
 * the admitted carry is {float running scalar, bool1 condition}. */
static nir_shader *
build_bool_carry_over_budget(void)
{
   struct vs_build v = begin_vs("plan_bool_carry", true);
   nir_def *cond = nir_flt(&v.b, nir_channel(&v.b, v.pos, 1),
                           nir_imm_float(&v.b, 0.5f));
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 90);
   nir_def *sel = nir_bcsel(&v.b, cond, c, nir_fneg(&v.b, c));
   return end_vs(&v, nir_replicate(&v.b, sel, 4));
}

/* Over-budget chain plus a typed integer computed before it and consumed
 * after it.  The clamp proves the carried component inside (or outside) the
 * R300 FP24 exact-integer window (+-131072). */
static nir_shader *
build_int_carry(bool is_unsigned, int32_t clamp_magnitude, unsigned length)
{
   struct vs_build v = begin_vs(is_unsigned ? "plan_uint_carry"
                                            : "plan_sint_carry", true);
   nir_def *y = nir_channel(&v.b, v.pos, 1);
   nir_def *typed, *back;
   if (is_unsigned) {
      nir_def *u = nir_f2u32(&v.b, y);
      typed = nir_umin(&v.b, u, nir_imm_int(&v.b, clamp_magnitude));
      back = NULL; /* consumer built after the chain */
   } else {
      nir_def *s = nir_f2i32(&v.b, y);
      typed = nir_imin(&v.b, nir_imax(&v.b, s,
                                      nir_imm_int(&v.b, -clamp_magnitude)),
                       nir_imm_int(&v.b, clamp_magnitude));
      back = NULL;
   }
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), length);
   back = is_unsigned ? nir_u2f32(&v.b, typed) : nir_i2f32(&v.b, typed);
   nir_def *sum = nir_fadd(&v.b, c, back);
   return end_vs(&v, nir_replicate(&v.b, sum, 4));
}

/* The carried integer feeds one signed and one unsigned consumer after the
 * chain, so the transport contract is unresolvable. */
static nir_shader *
build_mixed_signedness_carry(void)
{
   struct vs_build v = begin_vs("plan_mixed_carry", true);
   nir_def *s = nir_f2i32(&v.b, nir_channel(&v.b, v.pos, 1));
   nir_def *typed = nir_imin(&v.b, nir_imax(&v.b, s, nir_imm_int(&v.b, -100)),
                             nir_imm_int(&v.b, 100));
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 90);
   nir_def *sum = nir_fadd(&v.b, c,
                           nir_fadd(&v.b, nir_i2f32(&v.b, typed),
                                    nir_u2f32(&v.b, typed)));
   return end_vs(&v, nir_replicate(&v.b, sum, 4));
}

static nir_shader *
build_typed_under_budget(void)
{
   struct vs_build v = begin_vs("plan_typed_fits", true);
   nir_def *s = nir_f2i32(&v.b, nir_channel(&v.b, v.pos, 1));
   nir_def *typed = nir_imin(&v.b, nir_imax(&v.b, s, nir_imm_int(&v.b, -100)),
                             nir_imm_int(&v.b, 100));
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 8);
   nir_def *sum = nir_fadd(&v.b, c, nir_i2f32(&v.b, typed));
   return end_vs(&v, nir_replicate(&v.b, sum, 4));
}

/* The typed value converts back to float before the heavy chain, so only a
 * float crosses any cut while the whole-program scan still marks the typed
 * source: the carry type and the source class are independent facts. */
static nir_shader *
build_typed_float_before_cut(void)
{
   struct vs_build v = begin_vs("plan_typed_float_cut", true);
   nir_def *s = nir_f2i32(&v.b, nir_channel(&v.b, v.pos, 1));
   nir_def *typed = nir_imin(&v.b, nir_imax(&v.b, s, nir_imm_int(&v.b, -100)),
                             nir_imm_int(&v.b, 100));
   nir_def *back = nir_i2f32(&v.b, typed);
   nir_def *c = fmad_chain(&v.b, nir_fadd(&v.b, nir_channel(&v.b, v.pos, 0),
                                          back),
                           90);
   nir_def *sum = nir_fadd(&v.b, c, back);
   return end_vs(&v, nir_replicate(&v.b, sum, 4));
}

/* A vertex-stage system value outside the plain-I/O intrinsic set. */
static nir_shader *
build_unsupported_intrinsic(void)
{
   struct vs_build v = begin_vs("plan_unsupported_intrinsic", true);
   nir_def *vid = nir_u2f32(&v.b, nir_load_vertex_id(&v.b));
   nir_def *sum = nir_fadd(&v.b, nir_channel(&v.b, v.pos, 0), vid);
   return end_vs(&v, nir_replicate(&v.b, sum, 4));
}

/* A single dependent chain long enough that no balanced cut leaves both
 * halves (plus their pack/unpack overhead) under the 64-slot ceiling: the
 * ranked candidates fail alternately on the carry half and the position
 * half, so the walk retains two distinct failure classes.  This is the
 * shape of the silicon-validated over-budget decline witness. */
static nir_shader *
build_unsplittable_long_chain(void)
{
   struct vs_build v = begin_vs("plan_unsplittable_chain", true);
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 140);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

static nir_shader *
build_control_flow(void)
{
   struct vs_build v = begin_vs("plan_control_flow", true);
   nir_def *cond = nir_flt(&v.b, nir_channel(&v.b, v.pos, 0),
                           nir_imm_float(&v.b, 0.0f));
   nir_if *nif = nir_push_if(&v.b, cond);
   nir_def *then_v = nir_imm_float(&v.b, 1.0f);
   nir_push_else(&v.b, nif);
   nir_def *else_v = nir_imm_float(&v.b, 2.0f);
   nir_pop_if(&v.b, nif);
   nir_def *phi = nir_if_phi(&v.b, then_v, else_v);
   return end_vs(&v, nir_replicate(&v.b, phi, 4));
}

static nir_shader *
build_no_uniform_interface(void)
{
   struct vs_build v = begin_vs("plan_no_uniform", false);
   return end_vs(&v, v.pos);
}

/* The RS482 spill1 reference producer shape (r2vb_varying.vert): a computed
 * flat varying from the first input plus a passthrough varying of a second
 * input.  The position-pass admission the route consults at cv=0 measures
 * the restaged position producer alone, so this shader memos FITS there;
 * the varying delivery is the route's separate concern (passthrough
 * re-ingest, or the cv=1 varying producer). */
static nir_shader *
build_computed_and_passthrough_varyings(void)
{
   struct vs_build v = begin_vs("plan_computed_varying", true);
   nir_variable *in_attr = nir_variable_create(
      v.b.shader, nir_var_shader_in, glsl_vec4_type(), "in_attr");
   in_attr->data.location = VERT_ATTRIB_GENERIC1;
   in_attr->data.driver_location = 1;
   nir_variable *out_color = nir_variable_create(
      v.b.shader, nir_var_shader_out, glsl_vec4_type(), "vColor");
   out_color->data.location = VARYING_SLOT_VAR0;
   out_color->data.driver_location = 1;
   nir_variable *out_attr = nir_variable_create(
      v.b.shader, nir_var_shader_out, glsl_vec4_type(), "vAttr");
   out_attr->data.location = VARYING_SLOT_VAR1;
   out_attr->data.driver_location = 2;

   nir_store_var(&v.b, out_color,
                 nir_fmul_imm(&v.b, v.pos, 2.0), 0xf);
   nir_store_var(&v.b, out_attr, nir_load_var(&v.b, in_attr), 0xf);
   return end_vs(&v, v.pos);
}

/* A computed varying plus a second input feeding the position computation:
 * the production arity rule (r300_vs_nir_is_fragment_aluable) rejects this
 * at cv=1, because with a computed varying present every non-first input
 * must appear solely as a passthrough varying source. */
static nir_shader *
build_computed_varying_second_input_computes(void)
{
   struct vs_build v = begin_vs("plan_cv_second_input", true);
   nir_variable *in_attr = nir_variable_create(
      v.b.shader, nir_var_shader_in, glsl_vec4_type(), "in_attr");
   in_attr->data.location = VERT_ATTRIB_GENERIC1;
   in_attr->data.driver_location = 1;
   nir_variable *out_color = nir_variable_create(
      v.b.shader, nir_var_shader_out, glsl_vec4_type(), "vColor");
   out_color->data.location = VARYING_SLOT_VAR0;
   out_color->data.driver_location = 1;

   nir_store_var(&v.b, out_color,
                 nir_fmul_imm(&v.b, v.pos, 2.0), 0xf);
   return end_vs(&v, nir_fadd(&v.b, v.pos, nir_load_var(&v.b, in_attr)));
}

/* Eight interleaved over-budget chains summed at the end: stepping every
 * chain at each round keeps all eight running scalars live at every interior
 * cut, so no crossing set fits one vec4, while the length keeps even the
 * vectorized form over the 64-slot ceiling.  One multiplier per chain:
 * distinct across chains so CSE cannot merge the pairs sharing a base
 * channel, shared within a chain so the constant file stays inside the
 * 32-vec4 limit. */
static nir_shader *
build_wide_frontier(void)
{
   struct vs_build v = begin_vs("plan_wide_frontier", true);
   nir_def *chain[8], *base[8], *mul[8];
   for (unsigned k = 0; k < 8; k++) {
      base[k] = nir_channel(&v.b, v.pos, k % 4);
      mul[k] = nir_imm_float(&v.b, 1.0002f + (float)k * 0.0011f);
      chain[k] = base[k];
   }
   for (unsigned i = 0; i < 40; i++)
      for (unsigned k = 0; k < 8; k++)
         chain[k] = nir_ffma(&v.b, chain[k], mul[k], base[k]);
   nir_def *sum = chain[0];
   for (unsigned k = 1; k < 8; k++)
      sum = nir_fadd(&v.b, sum, chain[k]);
   return end_vs(&v, nir_replicate(&v.b, sum, 4));
}

/* Compact carry signature: one letter per transported base (f/i/u/b), the
 * same encoding the split trace prints. */
static void
carry_sig(const struct r300_r2vb_producer_plan *plan, char *buf, size_t len)
{
   unsigned n = 0;
   for (unsigned i = 0; i < plan->partition.num_bases && n + 1 < len; i++) {
      switch (plan->partition.r2vb_transport[i]) {
      case R300_MP_R2VB_SINT:   buf[n++] = 'i'; break;
      case R300_MP_R2VB_UINT:   buf[n++] = 'u'; break;
      case R300_MP_R2VB_BOOL1:
      case R300_MP_R2VB_BOOL32: buf[n++] = 'b'; break;
      default:                  buf[n++] = 'f'; break;
      }
   }
   buf[n] = '\0';
}

/* Sort the carry signature so the row contract is order-independent: the cut
 * ranks bases by discovery order, which is an implementation detail. */
static void
sort_sig(char *sig)
{
   for (char *a = sig; *a; a++)
      for (char *b = a + 1; *b; b++)
         if (*b < *a) {
            char t = *a;
            *a = *b;
            *b = t;
         }
}

static bool
plan_row(struct r300_context *r300, nir_shader *vs,
         enum r300_r2vb_position_space space,
         struct r300_r2vb_producer_plan *plan)
{
   bool ok = r300_r2vb_plan_producer(r300, vs, false, space, plan);
   printf("    plan action=%s primary=%s mask=0x%" PRIx64 " typed=%d class=%d "
          "carry_bases=%u pos_alu=%u\n",
          r300_r2vb_plan_action_str(plan->action),
          r300_r2vb_plan_reason_str(plan->primary_reason),
          plan->observed_reason_mask, plan->has_typed_source,
          plan->typed_source_class, plan->partition.num_bases, plan->baseline.alu);
   return ok;
}

static void
case_float_single(struct r300_context *r300)
{
   printf("float producer under budget plans SINGLE\n");
   nir_shader *vs = build_float_fits();
   struct r300_r2vb_producer_plan plan;
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
         "planner runs");
   CHECK(plan.action == R300_R2VB_PLAN_SINGLE, "action single");
   CHECK(plan.status == R300_R2VB_PLAN_READY, "status ready");
   CHECK(plan.primary_reason == R300_R2VB_PLAN_OK, "reason ok");
   CHECK(!plan.has_typed_source, "float-only source");
   CHECK(plan.baseline.alu > 0 && plan.baseline.alu <= 64, "measured cost in budget");
   CHECK(plan.candidate != NULL, "canonical candidate retained");
   CHECK(plan.key.input_semantics == R300_FS_INPUT_R2VB_FLAT_VERTEX,
         "flat producer semantics");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

/* A producer with no uniform-class variable is admissible: the production
 * route delivers passthrough and transform-only producers, and the RS482
 * shadow-parity corpus caught the plan diverging from the memo (io_shape
 * reject vs memo FITS) on exactly these shaders when the shape scan demanded
 * a uniform interface. */
static void
case_uniform_free_single(struct r300_context *r300)
{
   printf("uniform-free producer under budget plans SINGLE\n");
   for (unsigned sp = 0; sp < 2; sp++) {
      struct vs_build v = begin_vs("plan_uniform_free_fits", false);
      nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 8);
      nir_shader *vs = end_vs(&v, nir_replicate(&v.b, c, 4));
      struct r300_r2vb_producer_plan plan;
      enum r300_r2vb_position_space space =
         sp ? R300_R2VB_POSITION_WINDOW : R300_R2VB_POSITION_CLIP;
      CHECK(plan_row(r300, vs, space, &plan), "planner runs");
      CHECK(plan.action == R300_R2VB_PLAN_SINGLE,
            sp ? "action single (window)" : "action single (clip)");
      CHECK(plan.status == R300_R2VB_PLAN_READY, "status ready");
      CHECK(plan.baseline.alu > 0 && plan.baseline.alu <= 64,
            "measured cost in budget");
      r300_r2vb_plan_release(&plan);
      ralloc_free(vs);
   }
}

/* The cv=0 plan cell predicts the position-pass admission memo, which the
 * clip route consults for its position leg while varyings ride the
 * passthrough re-ingest or the cv=1 varying producer.  A computed varying
 * therefore stays outside the cv=0 cell instead of rejecting it: the RS482
 * shadow-parity corpus caught the plan diverging (reject/io_shape vs memo
 * FITS) on exactly the spill1 reference producer in both spaces. */
static void
case_computed_varying_position_cell(struct r300_context *r300)
{
   printf("computed-varying producer plans SINGLE on the cv=0 position cell\n");
   for (unsigned sp = 0; sp < 2; sp++) {
      nir_shader *vs = build_computed_and_passthrough_varyings();
      struct r300_r2vb_producer_plan plan;
      enum r300_r2vb_position_space space =
         sp ? R300_R2VB_POSITION_WINDOW : R300_R2VB_POSITION_CLIP;
      CHECK(plan_row(r300, vs, space, &plan), "planner runs");
      CHECK(plan.action == R300_R2VB_PLAN_SINGLE,
            sp ? "action single (window)" : "action single (clip)");
      CHECK(plan.status == R300_R2VB_PLAN_READY, "status ready");
      CHECK(plan.baseline.alu > 0 && plan.baseline.alu <= 64,
            "position pass measured in budget");
      r300_r2vb_plan_release(&plan);
      ralloc_free(vs);
   }
}

/* cv=1 keeps the production arity rule: a computed varying pins every
 * non-first input to passthrough-varying use only, so a second input feeding
 * the position computation rejects with IO_SHAPE, matching
 * r300_vs_nir_is_fragment_aluable. */
static void
case_computed_varying_arity_reject(struct r300_context *r300)
{
   printf("computed varying with a computing second input rejects at cv=1\n");
   nir_shader *vs = build_computed_varying_second_input_computes();
   struct r300_r2vb_producer_plan plan;
   bool ran = r300_r2vb_plan_producer(r300, vs, true,
                                      R300_R2VB_POSITION_CLIP, &plan);
   printf("    plan action=%s primary=%s mask=0x%" PRIx64 "\n",
          r300_r2vb_plan_action_str(plan.action),
          r300_r2vb_plan_reason_str(plan.primary_reason),
          plan.observed_reason_mask);
   CHECK(ran, "planner runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT, "action reject");
   CHECK(plan.primary_reason == R300_R2VB_PLAN_IO_SHAPE, "reason io_shape");
   if (ran)
      r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

static void
case_float_split(struct r300_context *r300)
{
   printf("over-budget float chain plans SPLIT with a float carry\n");
   for (unsigned sp = 0; sp < 2; sp++) {
      nir_shader *vs = build_float_over_budget();
      struct r300_r2vb_producer_plan plan;
      enum r300_r2vb_position_space space =
         sp ? R300_R2VB_POSITION_WINDOW : R300_R2VB_POSITION_CLIP;
      CHECK(plan_row(r300, vs, space, &plan), "planner runs");
      CHECK(plan.action == R300_R2VB_PLAN_SPLIT,
            sp ? "action split (window)" : "action split (clip)");
      if (plan.action == R300_R2VB_PLAN_SPLIT) {
         char sig[R300_MP_MAX_CARRY_COMPS + 1];
         carry_sig(&plan, sig, sizeof(sig));
         sort_sig(sig);
         CHECK(strchr(sig, 'f') != NULL && strchr(sig, 'i') == NULL &&
                  strchr(sig, 'b') == NULL,
               "float-only carry");
         CHECK(plan.pass_a_cost.alu > 0 && plan.pass_a_cost.alu <= 64 &&
                  plan.pass_b_cost.alu > 0 && plan.pass_b_cost.alu <= 64,
               "both halves measured in budget");
      }
      r300_r2vb_plan_release(&plan);
      ralloc_free(vs);
   }
}

static void
case_typed_split_rows(struct r300_context *r300)
{
   printf("typed over-budget producers plan SPLIT with typed carries\n");

   nir_shader *vs = build_bool_carry_over_budget();
   struct r300_r2vb_producer_plan plan;
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan), "bool row runs");
   CHECK(plan.action == R300_R2VB_PLAN_SPLIT, "bool carry splits");
   CHECK(plan.has_typed_source &&
            plan.typed_source_class == R300_R2VB_TYPED_SOURCE_BOOL,
         "bool typed-source class");
   if (plan.action == R300_R2VB_PLAN_SPLIT) {
      char sig[R300_MP_MAX_CARRY_COMPS + 1];
      carry_sig(&plan, sig, sizeof(sig));
      sort_sig(sig);
      CHECK(strcmp(sig, "bf") == 0, "carry {f,b}");
   }
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);

   vs = build_int_carry(false, 1000, 90);
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan), "sint row runs");
   CHECK(plan.action == R300_R2VB_PLAN_SPLIT, "bounded sint carry splits");
   CHECK(plan.has_typed_source &&
            plan.typed_source_class == R300_R2VB_TYPED_SOURCE_SINT,
         "sint typed-source class");
   if (plan.action == R300_R2VB_PLAN_SPLIT) {
      char sig[R300_MP_MAX_CARRY_COMPS + 1];
      carry_sig(&plan, sig, sizeof(sig));
      sort_sig(sig);
      CHECK(strcmp(sig, "fi") == 0, "carry {f,i}");
   }
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);

   vs = build_int_carry(true, 1000, 90);
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan), "uint row runs");
   CHECK(plan.action == R300_R2VB_PLAN_SPLIT, "bounded uint carry splits");
   if (plan.action == R300_R2VB_PLAN_SPLIT) {
      char sig[R300_MP_MAX_CARRY_COMPS + 1];
      carry_sig(&plan, sig, sizeof(sig));
      sort_sig(sig);
      CHECK(strcmp(sig, "fu") == 0, "carry {f,u}");
   }
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

static void
case_typed_reject_rows(struct r300_context *r300)
{
   printf("typed range and signedness rejects carry their class\n");

   /* 300000 > 131072: outside the FP24 exact-integer window. */
   nir_shader *vs = build_int_carry(false, 300000, 90);
   struct r300_r2vb_producer_plan plan;
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
         "signed range row runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT &&
            plan.primary_reason == R300_R2VB_PLAN_SIGNED_RANGE,
         "signed out-of-window rejects SIGNED_RANGE");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);

   vs = build_int_carry(true, 300000, 90);
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
         "unsigned range row runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT &&
            plan.primary_reason == R300_R2VB_PLAN_UNSIGNED_RANGE,
         "unsigned out-of-window rejects UNSIGNED_RANGE");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);

   vs = build_mixed_signedness_carry();
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
         "mixed row runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT &&
            plan.primary_reason == R300_R2VB_PLAN_MIXED_SIGNEDNESS,
         "mixed consumers reject MIXED_SIGNEDNESS");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);

   vs = build_typed_under_budget();
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
         "under-budget typed row runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT &&
            plan.primary_reason == R300_R2VB_PLAN_TYPED_SINGLE_PASS_UNPROVEN,
         "fitting typed producer rejects TYPED_SINGLE_PASS_UNPROVEN");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

static void
case_structural_rejects(struct r300_context *r300)
{
   printf("structural rejects name their shape class\n");

   nir_shader *vs = build_control_flow();
   struct r300_r2vb_producer_plan plan;
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
         "control-flow row runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT &&
            plan.primary_reason == R300_R2VB_PLAN_CONTROL_FLOW,
         "if/else rejects CONTROL_FLOW");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);

   /* Uniform-free producers are admissible (the RS482 shadow-parity corpus
    * delivers them byte-identically); the shape gate keys on gl_Position,
    * not on a uniform interface. */
   vs = build_no_uniform_interface();
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
         "no-uniform row runs");
   CHECK(plan.action == R300_R2VB_PLAN_SINGLE &&
            plan.primary_reason == R300_R2VB_PLAN_OK,
         "uniform-free producer plans SINGLE");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

static void
case_wide_frontier(struct r300_context *r300)
{
   printf("wide frontier rejects with the crossing width observed\n");
   nir_shader *vs = build_wide_frontier();
   struct r300_r2vb_producer_plan plan;
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
         "wide row runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT, "wide frontier rejects");
   CHECK(plan.observed_reason_mask &
            ((1u << R300_R2VB_PLAN_CARRY_WIDTH) |
             (1u << R300_R2VB_PLAN_NO_EXACT_CUT)),
         "carry width or no-cut observed");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

static void
case_typed_float_before_cut(struct r300_context *r300)
{
   printf("typed source folded to float before the cut splits with a float carry\n");
   nir_shader *vs = build_typed_float_before_cut();
   struct r300_r2vb_producer_plan plan;
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan), "row runs");
   CHECK(plan.action == R300_R2VB_PLAN_SPLIT, "splits");
   CHECK(plan.has_typed_source &&
            plan.typed_source_class == R300_R2VB_TYPED_SOURCE_SINT,
         "typed source recorded independently of the carry");
   if (plan.action == R300_R2VB_PLAN_SPLIT) {
      bool all_float = true;
      for (unsigned i = 0; i < plan.partition.num_bases; i++)
         if (plan.partition.r2vb_transport[i] != R300_MP_R2VB_FLOAT)
            all_float = false;
      CHECK(all_float, "carry is float-only");
   }
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

static void
case_unsupported_intrinsic(struct r300_context *r300)
{
   printf("system-value intrinsic rejects INTRINSIC\n");
   nir_shader *vs = build_unsupported_intrinsic();
   struct r300_r2vb_producer_plan plan;
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan), "row runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT &&
            plan.primary_reason == R300_R2VB_PLAN_INTRINSIC &&
            plan.status == R300_R2VB_PLAN_SEMANTIC_REJECT,
         "load_vertex_id rejects INTRINSIC");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

static void
case_multi_candidate_failures(struct r300_context *r300)
{
   printf("candidate walk retains every failure class\n");
   nir_shader *vs = build_unsplittable_long_chain();
   struct r300_r2vb_producer_plan plan;
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan), "row runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT, "rejects");
   unsigned classes = 0;
   for (unsigned r = 1; r < R300_R2VB_PLAN_REASON_COUNT; r++)
      if (plan.observed_reason_mask & (1ull << r))
         classes++;
   CHECK(classes >= 2, "mask retains at least two distinct classes");
   CHECK((plan.observed_reason_mask & (1ull << R300_R2VB_PLAN_PASS_A)) &&
            (plan.observed_reason_mask & (1ull << R300_R2VB_PLAN_PASS_B)),
         "both half-compile classes observed across the walk");
   CHECK(plan.primary_reason == R300_R2VB_PLAN_PASS_A,
         "precedence reports the earlier class, not the walk order");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

/* Cache lifetime through the real getter: the fake context binds a fake VS
 * so r300_r2vb_producer_plan_get exercises key compare, replacement, and
 * release exactly as the driver does. */
static void
case_cache_lifetime(struct r300_context *r300)
{
   printf("plan cache: retention, viewport keying, release\n");
   static struct r300_vertex_shader fake_vs;
   memset(&fake_vs, 0, sizeof(fake_vs));
   nir_shader *vs = build_float_fits();
   fake_vs.state.type = PIPE_SHADER_IR_NIR;
   fake_vs.state.ir.nir = vs;
   r300->vs_state.state = &fake_vs;

   const struct r300_r2vb_producer_plan *clip1 =
      r300_r2vb_producer_plan_get(r300, false, R300_R2VB_POSITION_CLIP);
   const struct r300_r2vb_producer_plan *clip2 =
      r300_r2vb_producer_plan_get(r300, false, R300_R2VB_POSITION_CLIP);
   CHECK(clip1 && clip1 == clip2, "same key returns the retained plan");
   CHECK(clip1 && clip1->status == R300_R2VB_PLAN_READY &&
            clip1->action == R300_R2VB_PLAN_SINGLE,
         "cached plan is READY/SINGLE");

   const struct r300_r2vb_producer_plan *win1 =
      r300_r2vb_producer_plan_get(r300, false, R300_R2VB_POSITION_WINDOW);
   CHECK(win1 && win1 != clip1, "clip and window hold separate plans");

   /* The replacement plan may reuse the freed allocation, so the witness is
    * the stored key, never pointer identity. */
   uint32_t bits_before = win1->key.viewport_scale[0];
   r300->viewport.scale[0] = 640.0f;
   uint32_t bits_after;
   memcpy(&bits_after, &r300->viewport.scale[0], sizeof(bits_after));
   const struct r300_r2vb_producer_plan *win2 =
      r300_r2vb_producer_plan_get(r300, false, R300_R2VB_POSITION_WINDOW);
   const struct r300_r2vb_producer_plan *clip3 =
      r300_r2vb_producer_plan_get(r300, false, R300_R2VB_POSITION_CLIP);
   CHECK(win2 && win2->key.viewport_scale[0] == bits_after &&
            win2->key.viewport_scale[0] != bits_before,
         "viewport bit change re-plans window space");
   CHECK(clip3 == clip1, "clip plan survives the viewport change");
   r300->viewport.scale[0] = 320.0f;

   r300_r2vb_plan_cache_release(&fake_vs);
   bool all_null = true;
   for (unsigned cv = 0; cv < 2; cv++)
      for (unsigned sp = 0; sp < 2; sp++)
         if (fake_vs.r2vb_plan[cv][sp])
            all_null = false;
   CHECK(all_null, "release clears every slot");
   CHECK(r300_r2vb_plan_shadow_divergences() == 0,
         "no shadow divergence recorded");

   r300->vs_state.state = NULL;
   ralloc_free(vs);
}

static void
case_deterministic(struct r300_context *r300)
{
   printf("repeated planning is deterministic\n");
   nir_shader *vs1 = build_int_carry(false, 1000, 90);
   nir_shader *vs2 = build_int_carry(false, 1000, 90);
   struct r300_r2vb_producer_plan p1, p2;
   CHECK(plan_row(r300, vs1, R300_R2VB_POSITION_CLIP, &p1) &&
            plan_row(r300, vs2, R300_R2VB_POSITION_CLIP, &p2),
         "both runs plan");
   CHECK(p1.action == p2.action && p1.primary_reason == p2.primary_reason &&
            p1.observed_reason_mask == p2.observed_reason_mask &&
            p1.partition.cut_index == p2.partition.cut_index &&
            p1.partition.num_bases == p2.partition.num_bases &&
            p1.pass_a_cost.alu == p2.pass_a_cost.alu &&
            p1.pass_b_cost.alu == p2.pass_b_cost.alu,
         "identical verdict, mask, cut, and costs");
   r300_r2vb_plan_release(&p1);
   r300_r2vb_plan_release(&p2);
   ralloc_free(vs1);
   ralloc_free(vs2);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   struct r300_context *r300 = fake_r300_context();

   case_float_single(r300);
   case_uniform_free_single(r300);
   case_computed_varying_position_cell(r300);
   case_computed_varying_arity_reject(r300);
   case_float_split(r300);
   case_typed_split_rows(r300);
   case_typed_reject_rows(r300);
   case_structural_rejects(r300);
   case_typed_float_before_cut(r300);
   case_unsupported_intrinsic(r300);
   case_multi_candidate_failures(r300);
   case_wide_frontier(r300);
   case_cache_lifetime(r300);
   case_deterministic(r300);

   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   glsl_type_singleton_decref();
   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
