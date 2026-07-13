/*
 * SPDX-License-Identifier: MIT
 *
 * Raw-output oracle for the direct-NIR draw vertex executor (draw_vs_nir.c).
 *
 * The surfaceless-EGL pixel corpus proves the interpreter matches the
 * nir_to_tgsi bridge through rasterization, but 8-bit RGBA readback hides
 * sub-LSB float differences, and GLSL ES 1.00 / 1.20 cannot express
 * gl_VertexID / gl_InstanceID at all.  This harness drives the draw module
 * directly on a headless softpipe screen: it builds a vertex shader with
 * nir_builder, converts one copy explicitly to the TGSI reference path, and
 * sends the other through draw_create_vs_exec's default direct-NIR route with
 * identical input and draw state.  It compares the raw AOS output[slot][4]
 * float arrays.
 *
 * The system-value cases (VertexID/InstanceID/BaseVertex/BaseInstance, indexed
 * and non-indexed) are the coverage the GL corpus cannot reach; they assert
 * bitwise equality between the two paths AND against a hand-computed expected
 * value.  The exactness cases (signed-zero/denormal passthrough, add, mul,
 * select) assert bit-for-bit equality that pixel quantization would mask.
 *
 * The control-flow, integer, bitwise-select, source-modifier, and vector
 * construct/extract cases extend the corpus past the arithmetic-only ALU and
 * system-value coverage above: draw_vs_nir.c's interp_cf_list only walks
 * nir_if / nir_loop with break and continue jumps, and interp_alu delegates
 * every nir_op to nir_eval_const_opcode uniformly, so a signed/unsigned or
 * control-flow divergence between the interpreter and the bridge would not
 * show up in the float-only cases above.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "pipe/p_shader_tokens.h"

#include "frontend/sw_winsys.h"
#include "softpipe/sp_public.h"
#include "sw/null/null_sw_winsys.h"

#include "compiler/shader_enums.h"
#include "compiler/glsl_types.h"
#include "nir.h"
#include "nir_builder.h"
#include "nir/nir_to_tgsi.h"

#include "util/u_memory.h"
#include "util/ralloc.h"

#include "draw/draw_context.h"
#include "draw/draw_private.h"
#include "draw/draw_vs.h"

static unsigned g_fail;

#define CHECK(cond, name)                                                      \
   do {                                                                        \
      if (cond) {                                                              \
         printf("  ok   - %s\n", (name));                                      \
      } else {                                                                 \
         printf("  FAIL - %s\n", (name));                                      \
         g_fail++;                                                             \
      }                                                                        \
   } while (0)

/* ---- shader builders (each execution path owns a fresh nir_shader) ---- */

static nir_variable *
add_input(nir_builder *b, unsigned slot)
{
   nir_variable *v = nir_variable_create(b->shader, nir_var_shader_in,
                                         glsl_vec4_type(), "in");
   v->data.location = VERT_ATTRIB_GENERIC0 + slot;
   v->data.driver_location = slot;
   if (b->shader->num_inputs <= slot)
      b->shader->num_inputs = slot + 1;
   return v;
}

static nir_variable *
add_output(nir_builder *b, unsigned slot, gl_varying_slot loc)
{
   nir_variable *v = nir_variable_create(b->shader, nir_var_shader_out,
                                         glsl_vec4_type(), "out");
   v->data.location = loc;
   v->data.driver_location = slot;
   if (b->shader->num_outputs <= slot)
      b->shader->num_outputs = slot + 1;
   return v;
}

typedef nir_shader *(*build_fn)(const nir_shader_compiler_options *opts);

static nir_shader *
build_passthrough(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "pt");
   nir_variable *in = add_input(&b, 0);
   nir_variable *out = add_output(&b, 0, VARYING_SLOT_POS);
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);
   return b.shader;
}

static nir_shader *
build_add(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "add");
   nir_def *a = nir_load_var(&b, add_input(&b, 0));
   nir_def *c = nir_load_var(&b, add_input(&b, 1));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), nir_fadd(&b, a, c), 0xf);
   return b.shader;
}

static nir_shader *
build_mul(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "mul");
   nir_def *a = nir_load_var(&b, add_input(&b, 0));
   nir_def *c = nir_load_var(&b, add_input(&b, 1));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), nir_fmul(&b, a, c), 0xf);
   return b.shader;
}

static nir_shader *
build_select(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "sel");
   nir_def *a = nir_load_var(&b, add_input(&b, 0));
   nir_def *c = nir_load_var(&b, add_input(&b, 1));
   nir_def *cond = nir_flt(&b, nir_channel(&b, c, 0), nir_channel(&b, a, 0));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS),
                 nir_bcsel(&b, cond, a, c), 0xf);
   return b.shader;
}

/* out = vec4(VertexID, InstanceID, BaseVertex, BaseInstance) as floats. */
static nir_shader *
build_sysval(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "sv");
   nir_def *v = nir_vec4(&b,
                         nir_i2f32(&b, nir_load_vertex_id(&b)),
                         nir_i2f32(&b, nir_load_instance_id(&b)),
                         nir_i2f32(&b, nir_load_base_vertex(&b)),
                         nir_i2f32(&b, nir_load_base_instance(&b)));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), v, 0xf);
   return b.shader;
}

/* out = vec4(VertexID, VertexIDZeroBase, BaseVertex, DrawID) as floats. */
static nir_shader *
build_sysval_indexed(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "svi");
   nir_def *v = nir_vec4(&b,
                         nir_i2f32(&b, nir_load_vertex_id(&b)),
                         nir_i2f32(&b, nir_load_vertex_id_zero_base(&b)),
                         nir_i2f32(&b, nir_load_base_vertex(&b)),
                         nir_i2f32(&b, nir_load_draw_id(&b)));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), v, 0xf);
   return b.shader;
}

/* out.x = sum of i in [0,4) with i >= 2, computed by a loop-carried int
 * accumulator with an if guarding the add: nir_if nested inside nir_loop,
 * with no break/continue, so interp_cf_list must recurse into the if's
 * then_list from inside the loop body and interp_phi must resolve the
 * loop-header phis nir_lower_vars_to_ssa inserts for accum/i. */
static nir_shader *
build_nested_if_in_loop(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "nil");
   nir_variable *out = add_output(&b, 0, VARYING_SLOT_POS);

   nir_variable *accum = nir_local_variable_create(b.impl, glsl_int_type(), "accum");
   nir_variable *i = nir_local_variable_create(b.impl, glsl_int_type(), "i");
   nir_store_var(&b, accum, nir_imm_int(&b, 0), 0x1);
   nir_store_var(&b, i, nir_imm_int(&b, 0), 0x1);

   nir_loop *loop = nir_push_loop(&b);
   {
      nir_def *iv = nir_load_var(&b, i);
      nir_break_if(&b, nir_ige_imm(&b, iv, 4));

      nir_if *nif = nir_push_if(&b, nir_ige_imm(&b, iv, 2));
      {
         nir_def *a = nir_load_var(&b, accum);
         nir_store_var(&b, accum, nir_iadd(&b, a, iv), 0x1);
      }
      nir_pop_if(&b, nif);

      nir_store_var(&b, i, nir_iadd_imm(&b, iv, 1), 0x1);
   }
   nir_pop_loop(&b, loop);

   nir_def *result = nir_i2f32(&b, nir_load_var(&b, accum));
   nir_def *v = nir_vec4(&b, result, nir_imm_float(&b, 0.0f),
                         nir_imm_float(&b, 0.0f), nir_imm_float(&b, 0.0f));
   nir_store_var(&b, out, v, 0xf);

   NIR_PASS(_, b.shader, nir_lower_vars_to_ssa);
   return b.shader;
}

/* out.x = sum of even i in [0,6), via a real for-loop shape: nir_jump_continue
 * on odd i jumping into an explicit continue construct (nir_loop_continue_list,
 * the increment nir_builder requires a continue jump to target -- see
 * nir_handle_add_jump in nir_control_flow.c), plus nir_jump_break stopping the
 * loop at i == 6.  glsl_to_nir lowers every GLSL `for` loop to this exact
 * body/continue-construct split, so this is the shape draw_vs_nir.c must
 * interpret for any vertex shader with a for-loop, not a corner case. */
static nir_shader *
build_loop_break_continue(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "lbc");
   nir_variable *out = add_output(&b, 0, VARYING_SLOT_POS);

   nir_variable *accum = nir_local_variable_create(b.impl, glsl_int_type(), "accum");
   nir_variable *i = nir_local_variable_create(b.impl, glsl_int_type(), "i");
   nir_store_var(&b, accum, nir_imm_int(&b, 0), 0x1);
   nir_store_var(&b, i, nir_imm_int(&b, 0), 0x1);

   nir_loop *loop = nir_push_loop(&b);
   nir_loop_add_continue_construct(loop);
   {
      nir_def *iv = nir_load_var(&b, i);
      nir_break_if(&b, nir_ige_imm(&b, iv, 6));

      nir_def *is_odd = nir_ine_imm(&b, nir_iand_imm(&b, iv, 1), 0);
      nir_if *nif = nir_push_if(&b, is_odd);
      {
         nir_jump(&b, nir_jump_continue);
      }
      nir_pop_if(&b, nif);

      nir_def *a = nir_load_var(&b, accum);
      nir_store_var(&b, accum, nir_iadd(&b, a, iv), 0x1);
   }
   nir_push_continue(&b, loop);
   {
      nir_def *iv = nir_load_var(&b, i);
      nir_store_var(&b, i, nir_iadd_imm(&b, iv, 1), 0x1);
   }
   nir_pop_loop(&b, loop);

   nir_def *result = nir_i2f32(&b, nir_load_var(&b, accum));
   nir_def *v = nir_vec4(&b, result, nir_imm_float(&b, 0.0f),
                         nir_imm_float(&b, 0.0f), nir_imm_float(&b, 0.0f));
   nir_store_var(&b, out, v, 0xf);

   /* Dual-factory corpus: pre-lower so the nir_to_tgsi bridge and the
    * interpreter see the same canonical shape.  Factory-side continue
    * lowering is covered by test_factory_continue_lowering below, which
    * feeds an unlowered clone to draw_create_vs_nir alone. */
   NIR_PASS(_, b.shader, nir_lower_vars_to_ssa);
   NIR_PASS(_, b.shader, nir_lower_continue_constructs);
   return b.shader;
}

/* Same shape as build_loop_break_continue but leaves the continue construct
 * intact so draw_create_vs_nir must fold it before nir_opt_dce.  Sum of even
 * i in [0,6) is 0+2+4 = 6. */
static nir_shader *
build_continue_construct_raw(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts,
                                                   "cont_raw");
   nir_variable *out = add_output(&b, 0, VARYING_SLOT_POS);
   nir_variable *accum =
      nir_local_variable_create(b.impl, glsl_int_type(), "accum");
   nir_variable *i = nir_local_variable_create(b.impl, glsl_int_type(), "i");
   nir_store_var(&b, accum, nir_imm_int(&b, 0), 0x1);
   nir_store_var(&b, i, nir_imm_int(&b, 0), 0x1);

   nir_loop *loop = nir_push_loop(&b);
   nir_loop_add_continue_construct(loop);
   {
      nir_def *iv = nir_load_var(&b, i);
      nir_break_if(&b, nir_ige_imm(&b, iv, 6));

      nir_def *is_odd = nir_ine_imm(&b, nir_iand_imm(&b, iv, 1), 0);
      nir_if *nif = nir_push_if(&b, is_odd);
      {
         nir_jump(&b, nir_jump_continue);
      }
      nir_pop_if(&b, nif);

      nir_def *a = nir_load_var(&b, accum);
      nir_store_var(&b, accum, nir_iadd(&b, a, iv), 0x1);
   }
   nir_push_continue(&b, loop);
   {
      nir_def *iv = nir_load_var(&b, i);
      nir_store_var(&b, i, nir_iadd_imm(&b, iv, 1), 0x1);
   }
   nir_pop_loop(&b, loop);

   nir_def *result = nir_i2f32(&b, nir_load_var(&b, accum));
   nir_def *v = nir_vec4(&b, result, nir_imm_float(&b, 0.0f),
                         nir_imm_float(&b, 0.0f), nir_imm_float(&b, 0.0f));
   nir_store_var(&b, out, v, 0xf);
   NIR_PASS(_, b.shader, nir_lower_vars_to_ssa);
   return b.shader;
}

/* Factory-only: unlowered continue construct must survive draw_create_vs_nir
 * (nir_lower_continue_constructs before nir_opt_dce) and produce 0+2+4=6. */
static void
test_factory_continue_lowering(struct draw_context *draw)
{
   printf("case: factory_continue_lowering\n");
   const nir_shader_compiler_options *opts =
      draw->pipe->screen->nir_options[MESA_SHADER_VERTEX];
   struct pipe_shader_state sb = { .type = PIPE_SHADER_IR_NIR };
   sb.ir.nir = build_continue_construct_raw(opts);
   struct draw_vertex_shader *interp = draw_create_vs_nir(draw, &sb);
   if (!interp) {
      CHECK(false,
            "factory_continue_lowering: draw_create_vs_nir accepts raw continue");
      return;
   }
   interp->prepare(interp, draw);
   float out[4] = {0};
   struct draw_buffer_info constants[PIPE_MAX_CONSTANT_BUFFERS] = {0};
   interp->run_linear(draw, interp, NULL, (float (*)[4])out, constants, 1, 0,
                      sizeof(out), NULL);
   float exp[4] = {6.0f, 0.0f, 0.0f, 0.0f};
   CHECK(memcmp(out, exp, sizeof(exp)) == 0,
         "factory_continue_lowering: even sum 0..5 is 6");
   if (memcmp(out, exp, sizeof(exp)) != 0)
      printf("    got %.9g want 6\n", out[0]);
   interp->delete(draw, interp);
}

/* The executable I/O bases define the Draw AOS rows even when producer
 * metadata has not published num_inputs. */
static nir_shader *
build_unpublished_input_span(const nir_shader_compiler_options *opts)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts,
                                     "unpublished input span");
   nir_def *input0 = nir_load_var(&b, add_input(&b, 0));
   nir_def *input1 = nir_load_var(&b, add_input(&b, 1));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS),
                 nir_fadd(&b, input0, input1), 0xf);
   b.shader->num_inputs = 0;
   return b.shader;
}

static void
test_factory_input_span(struct draw_context *draw)
{
   printf("case: factory_input_span\n");
   const nir_shader_compiler_options *opts =
      draw->pipe->screen->nir_options[MESA_SHADER_VERTEX];
   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR };
   state.ir.nir = build_unpublished_input_span(opts);

   CHECK(draw_vs_nir_supported(&state),
         "factory_input_span: lowered constant input rows are admitted");
   struct draw_vertex_shader *interp = draw_create_vs_nir(draw, &state);
   if (!interp) {
      CHECK(false, "factory_input_span: factory accepts executable input rows");
      return;
   }

   CHECK(interp->info.num_inputs == 2,
         "factory_input_span: intrinsic bases publish two AOS rows");
   interp->prepare(interp, draw);
   const float input[2][4] = {
      {1.0f, 2.0f, 3.0f, 4.0f},
      {5.0f, 6.0f, 7.0f, 8.0f},
   };
   const float expected[4] = {6.0f, 8.0f, 10.0f, 12.0f};
   float output[4] = {0};
   struct draw_buffer_info constants[PIPE_MAX_CONSTANT_BUFFERS] = {0};
   interp->run_linear(draw, interp, input, (float (*)[4])output, constants, 1,
                      sizeof(input), sizeof(output), NULL);
   CHECK(memcmp(output, expected, sizeof(expected)) == 0,
         "factory_input_span: both input rows execute");
   interp->delete(draw, interp);
}

/* out = vec4(iadd, ishl, ushr, imin(a,b)) where a, b come from f2i32 on the
 * loaded inputs, results converted back with i2f32: the f2i32/i2f32 round trip
 * plus ishl/ushr shift-amount handling is the r300 SW-TCL integer path
 * (gl_VertexID-derived indexing), not exercised by the float-only cases. */
static nir_shader *
build_integer_arith(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "iarith");
   nir_def *in0 = nir_load_var(&b, add_input(&b, 0));
   nir_def *a = nir_f2i32(&b, nir_channel(&b, in0, 0));
   nir_def *bb = nir_f2i32(&b, nir_channel(&b, in0, 1));

   nir_def *v = nir_vec4(&b,
                         nir_i2f32(&b, nir_iadd(&b, a, bb)),
                         nir_i2f32(&b, nir_ishl(&b, a, nir_imm_int(&b, 1))),
                         nir_i2f32(&b, nir_ushr(&b, a, nir_imm_int(&b, 2))),
                         nir_i2f32(&b, nir_imin(&b, a, bb)));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), v, 0xf);
   return b.shader;
}

/* out = vec4(iand, ior, ixor, ineg) on f2i32(in0.xy): the bitwise lane, kept
 * apart from build_integer_arith so a failure names the exact op family. */
static nir_shader *
build_integer_bitwise(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "ibit");
   nir_def *in0 = nir_load_var(&b, add_input(&b, 0));
   nir_def *a = nir_f2i32(&b, nir_channel(&b, in0, 0));
   nir_def *bb = nir_f2i32(&b, nir_channel(&b, in0, 1));

   nir_def *v = nir_vec4(&b,
                         nir_i2f32(&b, nir_iand(&b, a, bb)),
                         nir_i2f32(&b, nir_ior(&b, a, bb)),
                         nir_i2f32(&b, nir_ixor(&b, a, bb)),
                         nir_i2f32(&b, nir_ineg(&b, a)));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), v, 0xf);
   return b.shader;
}

/* out = vec4(imin, imax, umin, umax) of the constants (-1, 5): -1 as a u32 bit
 * pattern is 0xffffffff, the largest possible unsigned value, so imin/imax and
 * umin/umax disagree on every component.  A per-opcode evaluator that reused
 * the signed comparison for the unsigned op (or vice versa) would pass every
 * other integer case here and only diverge on this one. */
static nir_shader *
build_integer_minmax_signed_unsigned(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "mmsu");
   nir_def *x = nir_imm_int(&b, -1);
   nir_def *y = nir_imm_int(&b, 5);

   nir_def *v = nir_vec4(&b,
                         nir_i2f32(&b, nir_imin(&b, x, y)),
                         nir_i2f32(&b, nir_imax(&b, x, y)),
                         nir_u2f32(&b, nir_umin(&b, x, y)),
                         nir_u2f32(&b, nir_umax(&b, x, y)));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), v, 0xf);
   return b.shader;
}

/* out.x = a 4-way mux of in0.x/in0.y/in1.x/in1.y selected by two chained
 * bcsel conditions built from in1.z, in1.w: nir_bcsel feeding nir_bcsel, as
 * opposed to build_select's single bcsel. */
static nir_shader *
build_bcsel_chain(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "bcselc");
   nir_def *in0 = nir_load_var(&b, add_input(&b, 0));
   nir_def *in1 = nir_load_var(&b, add_input(&b, 1));

   nir_def *cond0 = nir_flt(&b, nir_imm_float(&b, 0.0f), nir_channel(&b, in1, 2));
   nir_def *cond1 = nir_flt(&b, nir_imm_float(&b, 0.0f), nir_channel(&b, in1, 3));

   nir_def *lo = nir_bcsel(&b, cond1, nir_channel(&b, in0, 1), nir_channel(&b, in1, 1));
   nir_def *hi = nir_bcsel(&b, cond1, nir_channel(&b, in0, 0), nir_channel(&b, in1, 0));
   nir_def *result = nir_bcsel(&b, cond0, hi, lo);

   nir_def *v = nir_vec4(&b, result, nir_imm_float(&b, 0.0f),
                         nir_imm_float(&b, 0.0f), nir_imm_float(&b, 0.0f));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), v, 0xf);
   return b.shader;
}

/* out = fneg(fabs(in0)) + in1: source modifiers folded into the ALU op
 * (rather than a separate mov), which is how nir_opt_algebraic and TGSI
 * source-modifier lowering both emit them. */
static nir_shader *
build_fabs_fneg(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "absneg");
   nir_def *a = nir_load_var(&b, add_input(&b, 0));
   nir_def *c = nir_load_var(&b, add_input(&b, 1));
   nir_def *negabs = nir_fneg(&b, nir_fabs(&b, a));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), nir_fadd(&b, negabs, c), 0xf);
   return b.shader;
}

/* out = vec4(in1.w, in0.z, in1.y, in0.x): every component pulled from a
 * different source and a different non-identity swizzle index, so
 * interp_alu's per-component swizzle gather (mirroring
 * nir_try_constant_fold_alu) is exercised on all four lanes at once instead
 * of the identity/broadcast patterns the earlier cases use. */
static nir_shader *
build_vector_construct_extract(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "vce");
   nir_def *in0 = nir_load_var(&b, add_input(&b, 0));
   nir_def *in1 = nir_load_var(&b, add_input(&b, 1));

   nir_def *v = nir_vec4(&b,
                         nir_channel(&b, in1, 3),
                         nir_channel(&b, in0, 2),
                         nir_channel(&b, in1, 1),
                         nir_channel(&b, in0, 0));
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), v, 0xf);
   return b.shader;
}

/* A termination intrinsic that interp_intrinsic does not implement has no
 * vertex-executor case, so draw_vs_nir_supported rejects it before the
 * interpreter reaches UNREACHABLE. */
static nir_shader *
build_unsupported_intrinsic(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "unsup");
   nir_variable *out = add_output(&b, 0, VARYING_SLOT_POS);
   nir_discard(&b);
   nir_store_var(&b, out, nir_imm_vec4(&b, 0.0f, 0.0f, 0.0f, 0.0f), 0xf);
   return b.shader;
}

/* A tex instruction (nir_instr_type_tex) has no case in interp_block's
 * instruction-type switch at all, so it must be caught independently of the
 * intrinsic allowlist above. */
static nir_shader *
build_unsupported_tex(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "unsuptex");
   nir_variable *out = add_output(&b, 0, VARYING_SLOT_POS);
   nir_def *coord = nir_imm_vec2(&b, 0.0f, 0.0f);
   nir_def *color = nir_tex(&b, coord, .dim = GLSL_SAMPLER_DIM_2D,
                            .dest_type = nir_type_float32);
   nir_store_var(&b, out, color, 0xf);
   return b.shader;
}

static nir_shader *
build_dynamic_input_row(const nir_shader_compiler_options *opts)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts,
                                     "dynamic input row");
   nir_def *offset = nir_load_vertex_id_zero_base(&b);
   nir_io_semantics semantics = {
      .location = VERT_ATTRIB_GENERIC0,
      .num_slots = 2,
   };
   nir_def *value = nir_load_input(&b, 4, 32, offset,
                                   .base = 0,
                                   .component = 0,
                                   .io_semantics = semantics);
   nir_store_var(&b, add_output(&b, 0, VARYING_SLOT_POS), value, 0xf);
   b.shader->num_inputs = 2;
   return b.shader;
}

static void
check_unsupported(struct pipe_context *pipe, const char *name, build_fn build)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_VERTEX];
   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR };
   state.ir.nir = build(opts);

   char msg[128];
   snprintf(msg, sizeof(msg), "%s: draw_vs_nir_supported rejects it", name);
   CHECK(!draw_vs_nir_supported(&state), msg);

   ralloc_free(state.ir.nir);
}

/* ---- the dual-factory driver ---- */

struct raw_case {
   const char *name;
   build_fn build;
   unsigned num_in, num_out, count;
   const float *input;            /* count * num_in * 4, or NULL */
   const unsigned *fetch_elts;    /* count, or NULL */
   unsigned instance_id, start_instance, start_index, elt_size;
   int elt_bias;
   const float *expected;         /* count * num_out * 4, or NULL */
};

static void
run_case(struct draw_context *draw, const struct raw_case *tc)
{
   printf("case: %s\n", tc->name);

   draw->instance_id = tc->instance_id;
   draw->start_instance = tc->start_instance;
   draw->start_index = tc->start_index;
   draw->pt.user.eltSize = tc->elt_size;
   draw->pt.user.eltBias = tc->elt_bias;

   const nir_shader_compiler_options *opts =
      draw->pipe->screen->nir_options[MESA_SHADER_VERTEX];

   nir_shader *bridge_nir = tc->build(opts);
   const void *bridge_tokens = nir_to_tgsi(bridge_nir, draw->pipe->screen);
   if (!bridge_tokens) {
      CHECK(false, "TGSI reference compilation returned tokens");
      ralloc_free(bridge_nir);
      return;
   }
   struct pipe_shader_state bridge_state = {
      .type = PIPE_SHADER_IR_TGSI,
      .tokens = bridge_tokens,
   };
   struct pipe_shader_state interp_state = { .type = PIPE_SHADER_IR_NIR };
   interp_state.ir.nir = tc->build(opts);

   struct draw_vertex_shader *bridge =
      draw_create_vs_exec(draw, &bridge_state);
   struct draw_vertex_shader *interp =
      draw_create_vs_exec(draw, &interp_state);
   FREE((void *)bridge_tokens);
   if (!bridge || !interp) {
      CHECK(false, "both factories returned a shader");
      if (bridge)
         bridge->delete(draw, bridge);
      if (interp)
         interp->delete(draw, interp);
      return;
   }
   CHECK(interp->state.type == PIPE_SHADER_IR_NIR,
         "factory selects the direct-NIR executor");
   bridge->prepare(bridge, draw);
   interp->prepare(interp, draw);

   const unsigned out_floats = tc->count * tc->num_out * 4;
   float *ob = calloc(out_floats, sizeof(float));
   float *oi = calloc(out_floats, sizeof(float));
   struct draw_buffer_info constants[PIPE_MAX_CONSTANT_BUFFERS] = {0};

   const unsigned in_stride = tc->num_in * 4 * sizeof(float);
   const unsigned out_stride = tc->num_out * 4 * sizeof(float);

   bridge->run_linear(draw, bridge, (const float (*)[4])tc->input, (float (*)[4])ob,
                      constants, tc->count, in_stride, out_stride, tc->fetch_elts);
   interp->run_linear(draw, interp, (const float (*)[4])tc->input, (float (*)[4])oi,
                      constants, tc->count, in_stride, out_stride, tc->fetch_elts);

   /* Bitwise: the interpreter must reproduce the bridge exactly for these ops
    * (mov/add/mul/select/system-values/int/control-flow all round identically
    * on the host). */
   bool exact = memcmp(ob, oi, out_floats * sizeof(float)) == 0;
   char msg[128];
   snprintf(msg, sizeof(msg), "%s: interpreter bit-identical to bridge", tc->name);
   CHECK(exact, msg);
   if (!exact) {
      for (unsigned i = 0; i < out_floats; i++) {
         if (memcmp(&ob[i], &oi[i], sizeof(float)) != 0) {
            uint32_t bb, bi;
            memcpy(&bb, &ob[i], 4);
            memcpy(&bi, &oi[i], 4);
            printf("    elem %u: bridge %.9g (0x%08x) interp %.9g (0x%08x)\n",
                   i, ob[i], bb, oi[i], bi);
         }
      }
   }

   if (tc->expected) {
      bool ok = memcmp(oi, tc->expected, out_floats * sizeof(float)) == 0;
      snprintf(msg, sizeof(msg), "%s: interpreter matches expected values", tc->name);
      CHECK(ok, msg);
      if (!ok)
         for (unsigned i = 0; i < out_floats; i++)
            if (oi[i] != tc->expected[i])
               printf("    elem %u: got %.9g want %.9g\n", i, oi[i], tc->expected[i]);
   }

   free(ob);
   free(oi);
   bridge->delete(draw, bridge);
   interp->delete(draw, interp);
}

int
main(void)
{
   /* Force the C exec path so both run_linear implementations take their
    * scalar route. */
   setenv("DRAW_USE_LLVM", "0", 1);

   struct sw_winsys *winsys = null_sw_create();
   if (!winsys)
      return 77;
   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      winsys->destroy(winsys);
      return 77;
   }
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   if (!pipe) {
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 77;
   }
   struct draw_context *draw = draw_create(pipe);
   if (!draw) {
      pipe->destroy(pipe);
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 77;
   }

   static const struct pipe_rasterizer_state rast = { .clamp_vertex_color = 0 };
   draw_set_rasterizer_state(draw, &rast, NULL);

   /* Two-vertex input: signed zero, denormal, large, and small magnitudes so a
    * bit-exact passthrough proves no value is quantized or flushed. */
   static const float in2[2 * 2 * 4] = {
      /* vertex 0: in0            in1            */
      -0.0f, 1.5e-40f, 1e30f, 0.5f,   2.0f, -3.0f, 0.25f, -0.0f,
      /* vertex 1 */
       7.0f, -0.125f, 123456.0f, -9.0f,  1.0f, 4.0f, -0.5f, 8.0f,
   };
   float add_exp[2 * 4], mul_exp[2 * 4];
   for (unsigned v = 0; v < 2; v++)
      for (unsigned c = 0; c < 4; c++) {
         add_exp[v * 4 + c] = in2[v * 8 + c] + in2[v * 8 + 4 + c];
         mul_exp[v * 4 + c] = in2[v * 8 + c] * in2[v * 8 + 4 + c];
      }

   struct raw_case pt = { "passthrough_signed_zero", build_passthrough,
      1, 1, 2, in2, NULL, 0, 0, 0, 0, 0, in2 };
   run_case(draw, &pt);

   struct raw_case add = { "add_exact", build_add, 2, 1, 2, in2, NULL,
      0, 0, 0, 0, 0, add_exp };
   run_case(draw, &add);

   struct raw_case mul = { "mul_exact", build_mul, 2, 1, 2, in2, NULL,
      0, 0, 0, 0, 0, mul_exp };
   run_case(draw, &mul);

   struct raw_case sel = { "select_exact", build_select, 2, 1, 2, in2, NULL,
      0, 0, 0, 0, 0, NULL };
   run_case(draw, &sel);

   /* Non-indexed system values: basevertex = start_index (eltSize 0). */
   const unsigned SI = 5, INST = 7, SINST = 3, N = 4;
   float sv_exp[4 * 4];
   for (unsigned i = 0; i < N; i++) {
      sv_exp[i * 4 + 0] = (float)(i + SI);   /* VertexID   */
      sv_exp[i * 4 + 1] = (float)INST;       /* InstanceID */
      sv_exp[i * 4 + 2] = (float)SI;         /* BaseVertex */
      sv_exp[i * 4 + 3] = (float)SINST;      /* BaseInstance */
   }
   struct raw_case sv = { "sysval_nonindexed", build_sysval, 0, 1, N, NULL, NULL,
      INST, SINST, SI, 0, 0, sv_exp };
   run_case(draw, &sv);

   /* Indexed system values: basevertex = eltBias, VertexID = fetch_elts[i]. */
   static const unsigned elts[4] = { 10, 11, 12, 13 };
   const int BIAS = 100;
   float svi_exp[4 * 4];
   for (unsigned i = 0; i < 4; i++) {
      svi_exp[i * 4 + 0] = (float)elts[i];              /* VertexID          */
      svi_exp[i * 4 + 1] = (float)((int)elts[i] - BIAS);/* VertexIDZeroBase  */
      svi_exp[i * 4 + 2] = (float)BIAS;                 /* BaseVertex        */
      svi_exp[i * 4 + 3] = 9.0f;
   }
   draw->pt.user.drawid = 9;
   struct raw_case svi = { "sysval_indexed", build_sysval_indexed, 0, 1, 4, NULL,
      elts, 0, 0, 0, 4 /*eltSize*/, BIAS, svi_exp };
   run_case(draw, &svi);
   draw->pt.user.drawid = 0;

   /* Control flow: single vertex, no vertex input needed (the shaders compute
    * a constant from loop-carried int state). */
   static const float in1z[1 * 4] = { 0.0f, 0.0f, 0.0f, 0.0f };

   /* build_nested_if_in_loop: sum of i in [0,4) with i >= 2 -> 2 + 3 = 5. */
   float nil_exp[4] = { 5.0f, 0.0f, 0.0f, 0.0f };
   struct raw_case nil = { "nested_if_in_loop", build_nested_if_in_loop,
      1, 1, 1, in1z, NULL, 0, 0, 0, 0, 0, nil_exp };
   run_case(draw, &nil);

   /* build_loop_break_continue: sum of even i in [0,6) -> 0 + 2 + 4 = 6. */
   float lbc_exp[4] = { 6.0f, 0.0f, 0.0f, 0.0f };
   struct raw_case lbc = { "loop_break_continue", build_loop_break_continue,
      1, 1, 1, in1z, NULL, 0, 0, 0, 0, 0, lbc_exp };
   run_case(draw, &lbc);

   /* Integer arithmetic: in0 = (13, 3, x, x) -> f2i32 gives a=13, b=3. */
   static const float int_in[1 * 2 * 4] = {
      13.0f, 3.0f, 0.0f, 0.0f,   0.0f, 0.0f, 0.0f, 0.0f,
   };
   float iarith_exp[4] = {
      (float)(13 + 3),        /* iadd            */
      (float)(13 << 1),       /* ishl a, 1       */
      (float)(13u >> 2),      /* ushr a, 2       */
      (float)(13 < 3 ? 13 : 3), /* imin(a, b)    */
   };
   struct raw_case iarith = { "integer_arith", build_integer_arith,
      2, 1, 1, int_in, NULL, 0, 0, 0, 0, 0, iarith_exp };
   run_case(draw, &iarith);

   float ibit_exp[4] = {
      (float)(13 & 3),    /* iand */
      (float)(13 | 3),    /* ior  */
      (float)(13 ^ 3),    /* ixor */
      (float)(-13),       /* ineg(a) */
   };
   struct raw_case ibit = { "integer_bitwise", build_integer_bitwise,
      2, 1, 1, int_in, NULL, 0, 0, 0, 0, 0, ibit_exp };
   run_case(draw, &ibit);

   /* imin/imax/umin/umax of (-1, 5): -1 as u32 is 0xffffffff, the largest
    * possible unsigned value, so signed and unsigned min/max disagree on
    * every lane. */
   float mmsu_exp[4] = {
      -1.0f,                       /* imin(-1, 5) = -1  */
      5.0f,                        /* imax(-1, 5) = 5   */
      5.0f,                        /* umin(-1, 5) = 5   */
      (float)(uint32_t)-1,         /* umax(-1, 5) = UINT32_MAX */
   };
   struct raw_case mmsu = { "integer_minmax_signed_unsigned",
      build_integer_minmax_signed_unsigned,
      1, 1, 1, in1z, NULL, 0, 0, 0, 0, 0, mmsu_exp };
   run_case(draw, &mmsu);

   /* bcsel chain: in0 = (10, 20, x, x), in1 = (30, 40, +1, +1) -> both
    * conditions true -> hi branch of the first mux -> in0.x = 10. */
   static const float bcsel_in[1 * 2 * 4] = {
      10.0f, 20.0f, 0.0f, 0.0f,   30.0f, 40.0f, 1.0f, 1.0f,
   };
   float bcsel_exp[4] = { 10.0f, 0.0f, 0.0f, 0.0f };
   struct raw_case bcselc = { "bcsel_chain", build_bcsel_chain,
      2, 1, 1, bcsel_in, NULL, 0, 0, 0, 0, 0, bcsel_exp };
   run_case(draw, &bcselc);

   /* fabs/fneg source modifiers: in0 = (-3, x, x, x), in1 = (1, x, x, x) ->
    * -fabs(-3) + 1 = -3 + 1 = -2. */
   static const float absneg_in[1 * 2 * 4] = {
      -3.0f, 0.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f, 0.0f,
   };
   float absneg_exp[4] = { -2.0f, 0.0f, 0.0f, 0.0f };
   struct raw_case absneg = { "fabs_fneg_modifiers", build_fabs_fneg,
      2, 1, 1, absneg_in, NULL, 0, 0, 0, 0, 0, absneg_exp };
   run_case(draw, &absneg);

   /* vector construct/extract: out = vec4(in1.w, in0.z, in1.y, in0.x). */
   static const float vce_in[1 * 2 * 4] = {
      1.0f, 2.0f, 3.0f, 4.0f,     5.0f, 6.0f, 7.0f, 8.0f,
   };
   float vce_exp[4] = { 8.0f, 3.0f, 6.0f, 1.0f };
   struct raw_case vce = { "vector_construct_extract",
      build_vector_construct_extract, 2, 1, 1, vce_in, NULL, 0, 0, 0, 0, 0,
      vce_exp };
   run_case(draw, &vce);

   /* Factory must lower continue constructs itself; dual-factory cases above
    * pre-lower so the bridge path can run. */
   test_factory_continue_lowering(draw);
   test_factory_input_span(draw);

   /* Admission predicate: shapes the interpreter cannot execute must be
    * rejected before draw_create_vs_exec ever dispatches to
    * draw_create_vs_nir. */
   check_unsupported(pipe, "unsupported_intrinsic", build_unsupported_intrinsic);
   check_unsupported(pipe, "unsupported_tex", build_unsupported_tex);
   check_unsupported(pipe, "dynamic_input_row", build_dynamic_input_row);

   draw_destroy(draw);
   pipe->destroy(pipe);
   screen->destroy(screen);
   winsys->destroy(winsys);

   printf("\n%s: %u failures\n", g_fail ? "FAILURE" : "SUCCESS", g_fail);
   return g_fail ? 1 : 0;
}
