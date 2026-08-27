/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Route-chain oracle for the R2VB producer plan, unit tier: hand-built
 * application vertex-shader NIR driven through the actual production chain --
 * shape validation, restage, plan, cut walk, and the emitted-slot admission
 * compile -- with a fake screen and context supplying only what that chain
 * reads (screen caps and NIR options, the fragment regalloc state, a debug
 * pointer, a zeroed viewport).  The typed split crosses a representation
 * boundary: downstream producer tests can pass while the application-VS route
 * rejects every typed shader, so this oracle pins the plan's verdict at the
 * exact entry the route consumes, not at the cut machinery below it.
 *
 * Required rows: a fitting float producer plans SINGLE; an over-budget float
 * chain plans SPLIT with a float carry; over-budget bool/sint/uint typed
 * producers plan SPLIT with {f,b}/{f,i}/{f,u} carries; out-of-window signed
 * and unsigned carries and a mixed-signedness carry reject with their range
 * class as the primary reason; an under-budget typed producer rejects
 * TYPED_SINGLE_PASS_UNPROVEN; control flow rejects CONTROL_FLOW; a uniform-free
 * producer plans SINGLE; a wide frontier whose every
 * cut crosses more than one vec4 rejects with CARRY_WIDTH observed; one
 * injected position-input clone failure leaves no cached plan before the next
 * request retries successfully; a classifier-side transient failure bypasses
 * the typed rejection memo; a shadow recount failure preserves the known-good
 * memo; and planning is deterministic across repeated runs and across clip and
 * window spaces.
 */

#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
   .lower_fdiv = true,
   .lower_flrp32 = true,
};

static struct r300_screen g_screen;
static struct r300_context g_context;

extern bool r300_r2vb_admits_producer_for_test(
   struct r300_context *r300, bool allow_computed_varying,
   enum r300_r2vb_position_space space);

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

/* Application-VS skeleton: in_pos at GENERIC0, gl_Position out, and an
 * optional UBO interface.  The arithmetic stays input-fed so the emitted
 * length is the chain's. */
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

/* The unsupported system value feeds a varying only.  The cv=0 position
 * producer drops that output before its structural scan; the cv=1 producer
 * keeps the value and retains the intrinsic rejection. */
static nir_shader *
build_varying_only_unsupported_intrinsic(void)
{
   struct vs_build v = begin_vs("plan_varying_only_intrinsic", true);
   nir_variable *out_color = nir_variable_create(
      v.b.shader, nir_var_shader_out, glsl_vec4_type(), "vColor");
   out_color->data.location = VARYING_SLOT_VAR0;
   out_color->data.driver_location = 1;
   nir_def *vid = nir_u2f32(&v.b, nir_load_vertex_id(&v.b));
   nir_store_var(&v.b, out_color, nir_replicate(&v.b, vid, 4), 0xf);
   return end_vs(&v, v.pos);
}

/* Lowered store_output form of the same scope split: cv=0 removes the
 * varying-only system-value work, while cv=1 keeps it as a known-bad
 * intrinsic.  The live restager must remove the lowered store before DCE so
 * its producer matches the cv=0 admission candidate. */
static nir_shader *
build_lowered_store_output(bool unsupported)
{
   struct vs_build v = begin_vs(unsupported ? "plan_store_output_bad"
                                            : "plan_store_output_good",
                                true);
   nir_def *value;
   if (unsupported) {
      nir_def *vertex_id = nir_u2f32(&v.b, nir_load_vertex_id(&v.b));
      value = nir_replicate(&v.b, vertex_id, 4);
   } else {
      value = v.pos;
   }
   nir_store_output(&v.b, value, nir_imm_int(&v.b, 0),
                    .io_semantics = {
                       .location = VARYING_SLOT_VAR0,
                       .num_slots = 1,
                    });
   return end_vs(&v, v.pos);
}

static nir_shader *
build_lowered_store_output_contract(nir_intrinsic_instr **store_out,
                                    unsigned offset, unsigned num_slots,
                                    unsigned write_mask)
{
   struct vs_build v = begin_vs("plan_store_output_contract", false);
   nir_def *components[] = {
      nir_channel(&v.b, v.pos, 0),
      nir_channel(&v.b, v.pos, 1),
   };
   nir_def *value = nir_vec(&v.b, components, ARRAY_SIZE(components));
   nir_intrinsic_instr *store = nir_store_output(
      &v.b, value, nir_imm_int(&v.b, offset),
      .io_semantics = {
         .location = VARYING_SLOT_VAR0,
         .num_slots = num_slots,
      });
   nir_intrinsic_set_write_mask(store, write_mask);
   nir_intrinsic_set_component(store, 2);
   if (store_out)
      *store_out = store;
   return end_vs(&v, v.pos);
}

static bool
shader_has_intrinsic(nir_shader *nir, nir_intrinsic_op op)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic &&
             nir_instr_as_intrinsic(instr)->intrinsic == op)
            return true;
      }
   }
   return false;
}

static nir_intrinsic_instr *
shader_first_intrinsic(nir_shader *nir, nir_intrinsic_op op)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic &&
             nir_instr_as_intrinsic(instr)->intrinsic == op)
            return nir_instr_as_intrinsic(instr);
      }
   }
   return NULL;
}

static nir_alu_instr *
shader_def_alu(nir_shader *nir, nir_def *def)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *alu = nir_instr_as_alu(instr);
         if (&alu->def == def)
            return alu;
      }
   }
   return NULL;
}

/* A single dependent chain long enough that no balanced cut leaves both
 * halves (plus their pack/unpack overhead) under the 64-slot ceiling: the
 * ranked candidates fail alternately on the carry half and the position
 * half, so the walk retains two distinct failure classes.  This is a
 * host-calibrated admission witness, not a silicon result; reproduce it with
 * `meson test -C build r300-r2vb-plan-oracle`.  The register/source
 * authority for the separate eight-stream boundary is
 * `src/amd/r300/common/r300_reg.h` and `r300_context.h`; the RS482
 * register-table notes in `docs/hardware/rs482-hybrid-vertex-tcl-design.md`
 * and this nine-input calibration row are retained evidence, not primary
 * hardware authority.  A nine-input pass-B capture that executes without
 * truncation or decline falsifies that boundary. */
static nir_shader *
build_unsplittable_long_chain(void)
{
   struct vs_build v = begin_vs("plan_unsplittable_chain", true);
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 140);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

/* Keep every model input live in gl_Position so the planner counts the
 * declared position-input domain rather than a varying-only declaration. */
static nir_shader *
build_input_ceiling(unsigned num_inputs)
{
   struct vs_build v = begin_vs("plan_input_ceiling", true);
   nir_def *sum = nir_channel(&v.b, v.pos, 0);
   for (unsigned i = 1; i < num_inputs; i++) {
      nir_variable *input = nir_variable_create(
         v.b.shader, nir_var_shader_in, glsl_vec4_type(), "model_input");
      input->data.location = VERT_ATTRIB_GENERIC0 + i;
      input->data.driver_location = i;
      nir_def *value = nir_load_var(&v.b, input);
      sum = nir_fadd(&v.b, sum, nir_channel(&v.b, value, i % 4));
   }
   nir_def *c = fmad_chain(&v.b, sum, 90);
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
   nir_variable *out_attr = nir_variable_create(
      v.b.shader, nir_var_shader_out, glsl_vec4_type(), "vAttr");
   out_attr->data.location = VARYING_SLOT_VAR1;
   out_attr->data.driver_location = 2;
   /* The declaration order is intentionally different from the location
    * order; the cv=1 source contract uses locations and ranks, not list order. */
   nir_variable *out_color = nir_variable_create(
      v.b.shader, nir_var_shader_out, glsl_vec4_type(), "vColor");
   out_color->data.location = VARYING_SLOT_VAR0;
   out_color->data.driver_location = 1;

   nir_store_var(&v.b, out_color,
                 nir_fmul_imm(&v.b, v.pos, 2.0), 0xf);
   nir_store_var(&v.b, out_attr, nir_load_var(&v.b, in_attr), 0xf);
   return end_vs(&v, v.pos);
}

/* Float-only position plus a typed computation that feeds only a varying:
 * the typed ops vanish from the restaged position candidate, so the cv=0
 * cell plans SINGLE with no typed source.  The typed-under-budget row is
 * this row's attribution partner: typed ops feeding position keep the
 * TYPED_SINGLE_PASS_UNPROVEN decline. */
static nir_shader *
build_typed_varying_float_position(void)
{
   struct vs_build v = begin_vs("plan_typed_varying_only", true);
   nir_variable *in_attr = nir_variable_create(
      v.b.shader, nir_var_shader_in, glsl_vec4_type(), "in_attr");
   in_attr->data.location = VERT_ATTRIB_GENERIC1;
   in_attr->data.driver_location = 1;
   nir_variable *out_color = nir_variable_create(
      v.b.shader, nir_var_shader_out, glsl_vec4_type(), "vColor");
   out_color->data.location = VARYING_SLOT_VAR0;
   out_color->data.driver_location = 1;

   nir_def *s = nir_f2i32(&v.b, nir_channel(&v.b, nir_load_var(&v.b, in_attr), 0));
   nir_def *typed = nir_imin(&v.b, nir_imax(&v.b, s, nir_imm_int(&v.b, -16)),
                             nir_imm_int(&v.b, 16));
   nir_store_var(&v.b, out_color,
                 nir_replicate(&v.b, nir_i2f32(&v.b, typed), 4), 0xf);

   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 8);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

static nir_shader *
build_typed_varying_opcode(nir_op op, bool is_unsigned, const char *name)
{
   struct vs_build v = begin_vs(name, true);
   nir_variable *in_attr = nir_variable_create(
      v.b.shader, nir_var_shader_in, glsl_vec4_type(), "in_attr");
   in_attr->data.location = VERT_ATTRIB_GENERIC1;
   in_attr->data.driver_location = 1;
   nir_variable *out_color = nir_variable_create(
      v.b.shader, nir_var_shader_out, glsl_vec4_type(), "vColor");
   out_color->data.location = VARYING_SLOT_VAR0;
   out_color->data.driver_location = 1;

   nir_def *input = nir_load_var(&v.b, in_attr);
   nir_def *x = nir_channel(&v.b, input, 0);
   nir_def *y = nir_channel(&v.b, input, 1);
   nir_def *lhs = is_unsigned ? nir_f2u32(&v.b, x) : nir_f2i32(&v.b, x);
   nir_def *rhs = is_unsigned ? nir_f2u32(&v.b, y) : nir_f2i32(&v.b, y);
   nir_def *typed = nir_build_alu2(&v.b, op, lhs, rhs);
   nir_def *output = is_unsigned ? nir_u2f32(&v.b, typed)
                                  : nir_i2f32(&v.b, typed);
   nir_store_var(&v.b, out_color, nir_replicate(&v.b, output, 4), 0xf);

   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 8);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
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

/* Both children inherit uninitialized process-cached gate values because this
 * case runs before the production admission helper.  The positive child
 * proves the under-budget force-split decision reaches the shadow check; the
 * mismatch child perturbs the cached plan so the same decision exercises the
 * release counter or the assertion-build stop. */
static void
case_force_split_shadow(struct r300_context *r300)
{
   printf("forced under-budget split has a calibrated shadow writer\n");
   static struct r300_vertex_shader fake_vs;
   memset(&fake_vs, 0, sizeof(fake_vs));
   nir_shader *vs = build_float_fits();
   fake_vs.state.type = PIPE_SHADER_IR_NIR;
   fake_vs.state.ir.nir = vs;
   r300->vs_state.state = &fake_vs;

   const struct r300_r2vb_producer_plan *plan =
      r300_r2vb_producer_plan_get(r300, false, R300_R2VB_POSITION_CLIP);
   struct r300_r2vb_producer_plan *cached = fake_vs.r2vb_plan[0][0];
   setenv("R300_R2VB_FORCE_SPLIT", "1", 1);
   setenv("R300_R2VB_BUDGET_ESCAPE", "spill1", 1);

   bool forced_shadow_witness = false;
   if (plan && plan->action == R300_R2VB_PLAN_SINGLE && cached) {
      pid_t child = fork();
      if (child == 0) {
         bool admitted = r300_r2vb_admits_producer_for_test(
            r300, false, R300_R2VB_POSITION_CLIP);
         _exit(admitted &&
                     fake_vs.r2vb_admission[0][0] == R300_R2VB_ADMIT_SPLIT &&
                     r300_r2vb_plan_shadow_divergences() == 0
                  ? 0
                  : 1);
      } else if (child > 0) {
         int status = 0;
         forced_shadow_witness = waitpid(child, &status, 0) == child &&
                                 WIFEXITED(status) &&
                                 WEXITSTATUS(status) == 0;
      }
   }
   CHECK(forced_shadow_witness,
         "forced split reaches the matching shadow writer");

   bool forced_mismatch_witness = false;
   if (plan && plan->action == R300_R2VB_PLAN_SINGLE && cached) {
      pid_t child = fork();
      if (child == 0) {
         cached->action = R300_R2VB_PLAN_SPLIT;
         bool admitted = r300_r2vb_admits_producer_for_test(
            r300, false, R300_R2VB_POSITION_CLIP);
#ifdef NDEBUG
         _exit(admitted && r300_r2vb_plan_shadow_divergences() > 0 ? 0 : 1);
#else
         _exit(admitted ? 0 : 1);
#endif
      } else if (child > 0) {
         int status = 0;
         forced_mismatch_witness = waitpid(child, &status, 0) == child;
#ifdef NDEBUG
         forced_mismatch_witness = forced_mismatch_witness &&
                                   WIFEXITED(status) &&
                                   WEXITSTATUS(status) == 0;
#else
         forced_mismatch_witness = forced_mismatch_witness &&
                                   WIFSIGNALED(status) &&
                                   WTERMSIG(status) == SIGABRT;
#endif
      }
   }
   CHECK(forced_mismatch_witness,
         "forced split detects a mismatched plan action");

   unsetenv("R300_R2VB_FORCE_SPLIT");
   unsetenv("R300_R2VB_BUDGET_ESCAPE");
   r300_r2vb_plan_cache_release(&fake_vs);
   r300->vs_state.state = NULL;
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

/* The cv=1 cell owns one computed and one passthrough varying.  The valid
 * row declares the outputs in a different order from their locations, while
 * the paired arity row keeps the known-bad second-position-input rejection. */
static nir_shader *build_computed_varying_second_input_computes(void);

static void
case_computed_varying_single_cell(struct r300_context *r300)
{
   printf("computed-varying producer plans SINGLE at cv=1\n");
   nir_shader *vs = build_computed_and_passthrough_varyings();
   struct r300_r2vb_producer_plan plan;
   bool ran = r300_r2vb_plan_producer(r300, vs, true,
                                      R300_R2VB_POSITION_CLIP, &plan);
   CHECK(ran, "cv=1 valid row runs");
   CHECK(plan.action == R300_R2VB_PLAN_SINGLE &&
            plan.status == R300_R2VB_PLAN_READY,
         "cv=1 computed plus passthrough plans SINGLE");
   CHECK(plan.varying_slot == VARYING_SLOT_VAR0 &&
            plan.varying_source.valid,
         "cv=1 varying source keeps the computed slot identity");
   if (ran)
      r300_r2vb_plan_release(&plan);
   ralloc_free(vs);

   vs = build_computed_varying_second_input_computes();
   ran = r300_r2vb_plan_producer(r300, vs, true,
                                 R300_R2VB_POSITION_CLIP, &plan);
   CHECK(ran && plan.action == R300_R2VB_PLAN_REJECT &&
            plan.primary_reason == R300_R2VB_PLAN_IO_SHAPE,
         "cv=1 second position input remains a known-bad reject");
   if (ran)
      r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

/* The cell's typed class comes from its own producer: a typed computation
 * feeding only a varying leaves the cv=0 position cell SINGLE and untyped.
 * This calibrates the plan contract itself -- today's production memo never
 * reaches this shape (the classify gate's whole-program float whitelist
 * rejects it before any memo write, and the clip route's direct memo write
 * runs only after classify admits), so the row guards the plan as the
 * future admission authority rather than shadow parity. */
static void
case_typed_varying_position_cell(struct r300_context *r300)
{
   printf("typed varying-only producer plans SINGLE and untyped at cv=0\n");
   for (unsigned sp = 0; sp < 2; sp++) {
      nir_shader *vs = build_typed_varying_float_position();
      struct r300_r2vb_producer_plan plan;
      enum r300_r2vb_position_space space =
         sp ? R300_R2VB_POSITION_WINDOW : R300_R2VB_POSITION_CLIP;
      CHECK(plan_row(r300, vs, space, &plan), "planner runs");
      CHECK(plan.action == R300_R2VB_PLAN_SINGLE,
            sp ? "action single (window)" : "action single (clip)");
      CHECK(!plan.has_typed_source,
            "typed source stays outside the position cell");
      r300_r2vb_plan_release(&plan);
      ralloc_free(vs);
   }
}

/* The cv=1 varying producer owns the computed output, so its optimized
 * candidate carries typed operations even when the position candidate is
 * float-only.  The plan keeps the cv=1 single-pass domain unproven rather
 * than admitting the position cell as a complete producer. */
static void
case_typed_computed_varying_reject(struct r300_context *r300)
{
   printf("typed computed varying rejects an unproven cv=1 single pass\n");
   nir_shader *vs = build_typed_varying_float_position();
   struct r300_r2vb_producer_plan plan;
   bool ran = r300_r2vb_plan_producer(r300, vs, true,
                                      R300_R2VB_POSITION_CLIP, &plan);
   printf("    plan action=%s primary=%s mask=0x%" PRIx64 "\n",
          r300_r2vb_plan_action_str(plan.action),
          r300_r2vb_plan_reason_str(plan.primary_reason),
          plan.observed_reason_mask);
   CHECK(ran, "planner runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT, "action reject");
   CHECK(plan.primary_reason == R300_R2VB_PLAN_TYPED_SINGLE_PASS_UNPROVEN,
         "reason typed single pass unproven");
   CHECK(plan.has_typed_source, "cv=1 typed source persists in plan");
   CHECK(plan.typed_source_class == R300_R2VB_TYPED_SOURCE_SINT,
         "cv=1 typed source class persists in plan");
   if (ran)
      r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

/* nir_lower_int_to_float handles each division and remainder opcode through
 * the r300 fragment backend.  Keep one live cv=1 producer per opcode so the
 * planner scan sees the source before the lowering pass rewrites it. */
static void
case_typed_varying_integer_opcode_coverage(struct r300_context *r300)
{
   static const struct {
      nir_op op;
      bool is_unsigned;
      enum r300_r2vb_typed_source_class source_class;
      const char *name;
   } cases[] = {
      { nir_op_idiv, false, R300_R2VB_TYPED_SOURCE_SINT,
        "plan_typed_varying_idiv" },
      { nir_op_udiv, true, R300_R2VB_TYPED_SOURCE_UINT,
        "plan_typed_varying_udiv" },
      { nir_op_irem, false, R300_R2VB_TYPED_SOURCE_SINT,
        "plan_typed_varying_irem" },
      { nir_op_imod, false, R300_R2VB_TYPED_SOURCE_SINT,
        "plan_typed_varying_imod" },
      { nir_op_umod, true, R300_R2VB_TYPED_SOURCE_UINT,
        "plan_typed_varying_umod" },
   };

   printf("typed computed varying classifies every integer div/rem opcode\n");
   for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
      nir_shader *vs = build_typed_varying_opcode(cases[i].op,
                                                  cases[i].is_unsigned,
                                                  cases[i].name);
      struct r300_r2vb_producer_plan plan;
      bool ran = r300_r2vb_plan_producer(r300, vs, true,
                                         R300_R2VB_POSITION_CLIP, &plan);
      CHECK(ran, "opcode planner runs");
      CHECK(plan.action == R300_R2VB_PLAN_REJECT,
            "opcode remains rejected at cv=1");
      CHECK(plan.primary_reason == R300_R2VB_PLAN_TYPED_SINGLE_PASS_UNPROVEN,
            "opcode records typed rejection");
      CHECK(plan.has_typed_source, "opcode typed source persists");
      CHECK(plan.typed_source_class == cases[i].source_class,
            "opcode typed class persists");
      if (ran)
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
      CHECK(plan.legacy_split_admitted,
            sp ? "first window cut stays in legacy memo"
               : "first clip cut stays in legacy memo");
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
   CHECK(plan.legacy_split_admitted,
         "first bool cut stays in legacy memo");
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
   CHECK(plan.legacy_split_admitted,
         "first sint cut stays in legacy memo");
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
   CHECK(plan.legacy_split_admitted,
         "first uint cut stays in legacy memo");
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

   vs = build_varying_only_unsupported_intrinsic();
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
         "varying-only intrinsic row runs");
   CHECK(plan.action == R300_R2VB_PLAN_SINGLE &&
            plan.primary_reason == R300_R2VB_PLAN_OK,
         "cv=0 drops unsupported varying-only work");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);

   vs = build_varying_only_unsupported_intrinsic();
   CHECK(r300_r2vb_plan_producer(r300, vs, true,
                                 R300_R2VB_POSITION_CLIP, &plan),
         "cv=1 varying-only intrinsic row runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT &&
            plan.primary_reason == R300_R2VB_PLAN_INTRINSIC,
         "cv=1 retains the varying intrinsic rejection");
   r300_r2vb_plan_release(&plan);

   static struct r300_vertex_shader fake_vs;
   memset(&fake_vs, 0, sizeof(fake_vs));
   fake_vs.state.type = PIPE_SHADER_IR_NIR;
   fake_vs.state.ir.nir = vs;
   r300->vs_state.state = &fake_vs;
   CHECK(r300_r2vb_admits_producer_for_test(
            r300, false, R300_R2VB_POSITION_CLIP),
         "production cv=0 gate matches the position-only plan");
   r300_r2vb_plan_cache_release(&fake_vs);
   r300->vs_state.state = NULL;
   ralloc_free(vs);
}

static void
case_lowered_store_output_scope(struct r300_context *r300)
{
   printf("lowered store_output scope matches planner and restager\n");
   nir_shader *vs = build_lowered_store_output(true);
   struct r300_r2vb_producer_plan plan;
   CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
         "lowered store_output cv=0 row runs");
   CHECK(plan.action == R300_R2VB_PLAN_SINGLE &&
            plan.status == R300_R2VB_PLAN_READY,
         "cv=0 removes unsupported varying-only store_output work");
   if (plan.status == R300_R2VB_PLAN_READY) {
      nir_shader *fs = r300_r2vb_build_restaged_fs_nir(
         r300, vs, VARYING_SLOT_POS, R300_R2VB_POSITION_CLIP);
      CHECK(fs != NULL, "live restager builds the cv=0 candidate");
      if (fs) {
         CHECK(!shader_has_intrinsic(fs, nir_intrinsic_load_vertex_id),
               "live restager DCE removes the unsupported store_output source");
         CHECK(!shader_has_intrinsic(fs, nir_intrinsic_store_output),
               "live restager removes the non-target store_output");
         ralloc_free(fs);
      }
   }
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);

   vs = build_lowered_store_output(true);
   bool ran = r300_r2vb_plan_producer(r300, vs, true,
                                      R300_R2VB_POSITION_CLIP, &plan);
   CHECK(ran, "lowered store_output cv=1 row runs");
   CHECK(plan.action == R300_R2VB_PLAN_REJECT &&
            plan.primary_reason == R300_R2VB_PLAN_INTRINSIC,
         "cv=1 retains the unsupported store_output source as a reject");
   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

static void
case_lowered_store_output_offset_component_contract(struct r300_context *r300)
{
   printf("lowered store_output offset and component contracts are preserved\n");

   nir_intrinsic_instr *store = NULL;
   nir_shader *vs = build_lowered_store_output_contract(&store, 1, 2, 0x3);
   gl_varying_slot location;
   CHECK(store && r300_r2vb_output_store_location(store, &location) &&
            location == VARYING_SLOT_VAR0 + 1,
         "constant offset within a multi-slot declaration selects the slot");

   nir_shader *fs = r300_r2vb_build_restaged_fs_nir(
      r300, vs, VARYING_SLOT_VAR0 + 1, R300_R2VB_POSITION_CLIP);
   CHECK(fs != NULL, "offset output restager builds");
   if (fs) {
      nir_intrinsic_instr *restaged =
         shader_first_intrinsic(fs, nir_intrinsic_store_output);
      CHECK(restaged != NULL, "offset output survives target restaging");
      if (restaged) {
         nir_io_semantics semantics = nir_intrinsic_io_semantics(restaged);
         CHECK(semantics.location == FRAG_RESULT_DATA0 &&
                  semantics.num_slots == 1,
               "restager maps the effective slot to one color record");
         CHECK(nir_src_is_const(restaged->src[1]) &&
                  nir_src_as_uint(restaged->src[1]) == 0,
               "restager clears the lowered output offset");
         CHECK(restaged->num_components == 4 &&
                  nir_intrinsic_write_mask(restaged) == 0xf &&
                  (!nir_intrinsic_has_component(restaged) ||
                   nir_intrinsic_component(restaged) == 0),
               "restager resets the component when padding the record");
         nir_alu_instr *padded = shader_def_alu(fs, restaged->src[0].ssa);
         bool preserves_component_lanes =
            padded && padded->op == nir_op_vec4 &&
            nir_src_is_const(padded->src[0].src) &&
            nir_src_is_const(padded->src[1].src) &&
            nir_const_value_as_float(
               *nir_src_as_const_value(padded->src[0].src), 32) == 0.0f &&
            nir_const_value_as_float(
               *nir_src_as_const_value(padded->src[1].src), 32) == 0.0f;
         CHECK(preserves_component_lanes,
               "padding keeps component-qualified values in their lanes");
      }
      ralloc_free(fs);
   }
   ralloc_free(vs);

   vs = build_lowered_store_output_contract(&store, 1, 2, 0x1);
   fs = r300_r2vb_build_restaged_fs_nir(
      r300, vs, VARYING_SLOT_VAR0 + 1, R300_R2VB_POSITION_CLIP);
   CHECK(fs != NULL, "masked output restager builds");
   if (fs) {
      nir_intrinsic_instr *restaged =
         shader_first_intrinsic(fs, nir_intrinsic_store_output);
      nir_alu_instr *padded = restaged
         ? shader_def_alu(fs, restaged->src[0].ssa)
         : NULL;
      CHECK(padded && padded->op == nir_op_vec4 &&
               nir_src_is_const(padded->src[3].src) &&
               nir_const_value_as_float(
                  *nir_src_as_const_value(padded->src[3].src), 32) == 0.0f,
            "padding preserves an inactive source lane as zero");
      ralloc_free(fs);
   }
   ralloc_free(vs);

   vs = build_lowered_store_output_contract(&store, 2, 2, 0x3);
   CHECK(!r300_r2vb_output_store_location(store, &location),
         "out-of-range lowered output offsets are rejected");
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
case_input_ceiling(struct r300_context *r300)
{
   printf("split pass-B admits seven inputs and declines eight\n");
   for (unsigned num_inputs = R300_R2VB_MAX_PRODUCER_INPUTS - 1;
        num_inputs <= R300_R2VB_MAX_PRODUCER_INPUTS; num_inputs++) {
      nir_shader *vs = build_input_ceiling(num_inputs);
      struct r300_r2vb_producer_plan plan;
      CHECK(plan_row(r300, vs, R300_R2VB_POSITION_CLIP, &plan),
            "input ceiling row runs");
      CHECK(plan.num_position_inputs == num_inputs,
            num_inputs == 7 ? "seven position inputs counted"
                             : "eight position inputs counted");
      if (num_inputs == R300_R2VB_MAX_PRODUCER_INPUTS - 1) {
         CHECK(plan.action == R300_R2VB_PLAN_SPLIT,
               "seven model inputs plus carry split");
         CHECK(plan.pass_b_cost.alu > 0 && plan.pass_b_cost.alu <= 64,
               "seven-input pass B fits the ALU budget");
         CHECK(!(plan.observed_reason_mask &
                 (1ull << R300_R2VB_PLAN_IO_SHAPE)),
               "seven-input source identity decline stays route-optional");
      } else {
         CHECK(plan.action == R300_R2VB_PLAN_REJECT &&
                  plan.primary_reason == R300_R2VB_PLAN_IO_SHAPE,
               "eight model inputs plus carry decline at the ceiling");
      }
      r300_r2vb_plan_release(&plan);
      ralloc_free(vs);
   }
}

static void
case_input_count_failure(struct r300_context *r300)
{
   printf("position-input count has an explicit failure value\n");
   CHECK(r300_r2vb_count_position_inputs(NULL) == 0,
         "missing shader count fails closed");

   nir_shader *vs = build_float_fits();
   CHECK(r300_r2vb_count_position_inputs(vs) == 1,
         "valid single-input shader counts one");

   static struct r300_vertex_shader fake_vs;
   memset(&fake_vs, 0, sizeof(fake_vs));
   fake_vs.state.type = PIPE_SHADER_IR_NIR;
   fake_vs.state.ir.nir = vs;
   r300->vs_state.state = &fake_vs;
   r300_r2vb_test_fail_position_input_clone_once();
   const struct r300_r2vb_producer_plan *failed =
      r300_r2vb_producer_plan_get(r300, false, R300_R2VB_POSITION_CLIP);
   CHECK(!failed && !fake_vs.r2vb_plan[0][0],
         "injected clone failure leaves the plan slot empty");
   const struct r300_r2vb_producer_plan *retry =
      r300_r2vb_producer_plan_get(r300, false, R300_R2VB_POSITION_CLIP);
   CHECK(retry && retry->status == R300_R2VB_PLAN_READY &&
            retry->action == R300_R2VB_PLAN_SINGLE,
         "the next request retries and caches the known-good plan");
   r300_r2vb_plan_cache_release(&fake_vs);
   r300->vs_state.state = NULL;
   ralloc_free(vs);

   vs = build_float_fits();
   memset(&fake_vs, 0, sizeof(fake_vs));
   fake_vs.state.type = PIPE_SHADER_IR_NIR;
   fake_vs.state.ir.nir = vs;
   r300->vs_state.state = &fake_vs;
   r300_r2vb_test_fail_position_source_clone_once();
   failed = r300_r2vb_producer_plan_get(
      r300, false, R300_R2VB_POSITION_CLIP);
   CHECK(!failed && !fake_vs.r2vb_plan[0][0],
         "injected source-scan failure leaves the plan slot empty");
   retry = r300_r2vb_producer_plan_get(
      r300, false, R300_R2VB_POSITION_CLIP);
   CHECK(retry && retry->status == R300_R2VB_PLAN_READY &&
            retry->action == R300_R2VB_PLAN_SINGLE,
         "the source-scan retry caches the known-good plan");
   r300_r2vb_plan_cache_release(&fake_vs);
   r300->vs_state.state = NULL;
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
 * release exactly as the driver does.  The known-good route then writes the
 * admission memo and reaches the shadow checker before the counter witness. */
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

   fake_vs.r2vb_admission[0][1] = R300_R2VB_ADMIT_REJECT;
   r300->viewport.scale[0] = 800.0f;
   bool refreshed_window = r300_r2vb_admits_producer_for_test(
      r300, false, R300_R2VB_POSITION_WINDOW);
   CHECK(refreshed_window &&
            fake_vs.r2vb_admission[0][1] == R300_R2VB_ADMIT_FITS,
         "viewport key refreshes a stale window admission memo");
   r300->viewport.scale[0] = 320.0f;

   static struct r300_fragment_shader fake_fs;
   static struct r300_fragment_shader_code fake_fs_code;
   memset(&fake_fs, 0, sizeof(fake_fs));
   memset(&fake_fs_code, 0, sizeof(fake_fs_code));
   fake_fs.shader = &fake_fs_code;
   fake_fs_code.inputs.face = ATTR_UNUSED;
   r300->fs.state = &fake_fs;

   struct pipe_draw_info info = {0};
   info.mode = MESA_PRIM_TRIANGLES;
   info.instance_count = 1;
   struct pipe_draw_start_count_bias draw = { .count = 3 };
   unsetenv("R300_R2VB_AUTO_SINGLE");
   unsetenv("R300_R2VB_AUTO_SINGLE_MIN_VERTICES");
   unsetenv("R300_R2VB_BUDGET_ESCAPE");
   unsetenv("R300_R2VB_BO_DRAW");
   unsetenv("R300_R2VB_DIVIDE");
   unsetenv("R300_R2VB_FORCE_SPLIT");
   unsetenv("R300_R2VB_PLAN_DEBUG");
   setenv("R300_R2VB_TYPED_SPLIT", "1", 1);
   unsetenv("R300_R2VB_VARYING");
   unsetenv("R300_R2VB_TELEMETRY");
   unsetenv("R300_R2VB_TELEMETRY_RETAIN");
   unsetenv("R300_R2VB_TELEMETRY_RETAIN_SCOPE");
   setenv("R300_R2VB_MVP_EXEC", "1", 1);
   setenv("R300_R2VB_RESTAGE", "1", 1);
   r300_r2vb_test_fail_position_input_clone_after_one();
   bool transient_route = r300_r2vb_route_mvp(r300, &info, &draw);
   CHECK(!transient_route &&
            fake_vs.r2vb_admission[0][0] == R300_R2VB_ADMIT_UNMEASURED,
         "classifier allocation failure bypasses the typed rejection memo");
   bool route_ok = r300_r2vb_route_mvp(r300, &info, &draw);
   CHECK(route_ok, "admission memo path accepts the known-good producer");
   CHECK(fake_vs.r2vb_admission[0][0] == R300_R2VB_ADMIT_FITS,
         "known-good route records the FITS memo");
   CHECK(r300_r2vb_plan_shadow_divergences() == 0,
         "known-good shadow check records no divergence");

   fake_vs.r2vb_admission[0][0] = R300_R2VB_ADMIT_UNMEASURED;
   r300_r2vb_test_fail_shadow_recount_once();
   bool shadow_retry = r300_r2vb_route_mvp(r300, &info, &draw);
   CHECK(shadow_retry &&
            fake_vs.r2vb_admission[0][0] == R300_R2VB_ADMIT_FITS,
         "shadow recount failure preserves the known-good memo");
   CHECK(r300_r2vb_plan_shadow_divergences() == 0,
         "transient shadow recount records no divergence");

   /* The mismatch witness runs in a child because assertion builds terminate
    * at the shadow boundary.  Release builds keep the memo authoritative and
    * return normally, so the expected outcome follows the build contract. */
   bool child_witness = false;
   struct r300_r2vb_producer_plan *cached = fake_vs.r2vb_plan[0][0];
   if (cached) {
      enum r300_r2vb_plan_action saved_action = cached->action;
      fake_vs.r2vb_admission[0][0] = R300_R2VB_ADMIT_UNMEASURED;
      cached->action = R300_R2VB_PLAN_REJECT;
      pid_t child = fork();
      if (child == 0) {
         bool admitted = r300_r2vb_route_mvp(r300, &info, &draw);
#ifdef NDEBUG
         _exit(admitted && r300_r2vb_plan_shadow_divergences() > 0 ? 0 : 1);
#else
         _exit(admitted ? 0 : 1);
#endif
      } else if (child > 0) {
         int status = 0;
         child_witness = waitpid(child, &status, 0) == child;
#ifdef NDEBUG
         child_witness = child_witness && WIFEXITED(status) &&
                         WEXITSTATUS(status) == 0;
#else
         child_witness = child_witness && WIFSIGNALED(status) &&
                         WTERMSIG(status) == SIGABRT;
#endif
      }
      cached->action = saved_action;
      fake_vs.r2vb_admission[0][0] = R300_R2VB_ADMIT_UNMEASURED;
   }
   CHECK(child_witness,
         "controlled shadow mismatch reaches its failure contract");
   unsetenv("R300_R2VB_MVP_EXEC");
   unsetenv("R300_R2VB_RESTAGE");
   unsetenv("R300_R2VB_TYPED_SPLIT");
   r300->fs.state = NULL;

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
case_lowered_store_output_computed_varying(struct r300_context *r300)
{
   printf("multi-slot store_output computed-varying scans use the effective slot\n");

   struct vs_build v = begin_vs("plan_store_output_computed", true);
   nir_def *value = nir_fadd(&v.b, nir_channel(&v.b, v.pos, 0),
                              nir_imm_float(&v.b, 1.0f));
   nir_store_output(&v.b, nir_replicate(&v.b, value, 4), nir_imm_int(&v.b, 1),
                    .io_semantics = {
                       .location = VARYING_SLOT_VAR0,
                       .num_slots = 2,
                    });
   nir_shader *vs = end_vs(&v, v.pos);

   const gl_varying_slot computed_slot = VARYING_SLOT_VAR0 + 1;
   CHECK(r300_r2vb_first_computed_varying(vs) == computed_slot,
         "first computed varying follows the lowered output offset");

   struct r300_r2vb_position_source source;
   CHECK(r300_r2vb_varying_source_scan(vs, computed_slot, &source) &&
            source.valid && source.location_rank == 0,
         "varying source scan retains the lowered output survivor");

   struct r300_r2vb_producer_plan plan;
   bool ran = r300_r2vb_plan_producer(r300, vs, true,
                                      R300_R2VB_POSITION_CLIP, &plan);
   CHECK(ran, "multi-slot computed-varying plan runs");
   CHECK(plan.action == R300_R2VB_PLAN_SINGLE &&
            plan.status == R300_R2VB_PLAN_READY &&
            plan.varying_slot == computed_slot &&
            plan.varying_source.valid,
         "plan admits the effective computed varying once");
   if (ran)
      r300_r2vb_plan_release(&plan);
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
   case_force_split_shadow(r300);
   case_uniform_free_single(r300);
   case_computed_varying_position_cell(r300);
   case_computed_varying_single_cell(r300);
   case_typed_varying_position_cell(r300);
   case_typed_computed_varying_reject(r300);
   case_typed_varying_integer_opcode_coverage(r300);
   case_computed_varying_arity_reject(r300);
   case_float_split(r300);
   case_typed_split_rows(r300);
   case_typed_reject_rows(r300);
   case_structural_rejects(r300);
   case_lowered_store_output_scope(r300);
   case_lowered_store_output_offset_component_contract(r300);
   case_lowered_store_output_computed_varying(r300);
   case_typed_float_before_cut(r300);
   case_unsupported_intrinsic(r300);
   case_multi_candidate_failures(r300);
   case_wide_frontier(r300);
   case_input_ceiling(r300);
   case_input_count_failure(r300);
   case_cache_lifetime(r300);
   case_deterministic(r300);

   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   glsl_type_singleton_decref();
   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
