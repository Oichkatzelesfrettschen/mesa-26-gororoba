/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "util/ralloc.h"

#include "nir_to_rc.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "radeon_code.h"
#include "radeon_compiler.h"
#include "radeon_regalloc.h"

/* Oracle (c), reference-path determinism: a determinism check on the
 * reference path itself, not a cross-implementation comparison.  Compiling the
 * unmodified legacy path (nir_to_rc -> r3xx_compile_fragment_program) twice
 * from two independently built NIR shaders of the same shape must produce
 * byte-identical R300 hardware code -- same ALU/TEX instruction words, same
 * config/code_offset/code_addr layout, and the same uploaded constant file
 * (rc_constants_copy's code.constants, which the ALU words index into and a
 * diff scoped to the instruction words alone would miss).  This proves the
 * harness itself before any later byte-diff work trusts it for anything: if
 * the reference compiler were sensitive to allocation addresses,
 * hash-iteration order, or uninitialized memory, a future byte-diff against
 * it would be noise, not signal. */

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

/* Compile through the unmodified legacy front end and the real backend pass
 * chain, mirroring r300_fs.c's wiring (compiler.code set before nir_to_rc,
 * r3xx_compile_fragment_program run to completion).  Consumes s. */
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
   nir_builder b = fs_builder("selfdiff_fmad");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c1 = nir_imm_vec4(&b, 0.5f, 0.25f, 2.0f, 1.0f);
   nir_def *c2 = nir_imm_vec4(&b, 0.125f, -0.5f, 0.0f, 0.75f);
   nir_store_var(&b, out, nir_build_alu3(&b, nir_op_fmad, v, c1, c2), 0xf);
   return b.shader;
}

/* EX2/LG2/RCP/RSQ are the R300 (non-r500) Alpha unit's full transcendental
 * set (r300_fragprog_emit.c's translate_alpha_opcode has no case for
 * SIN/COS/POW -- those are R500-only silicon, r500_fragprog_emit.c's
 * translate_alpha_op).  Staying inside this set keeps the corpus on the
 * RS480/RS482-class target this mission validates. */
static nir_shader *
build_transcendentals(void)
{
   nir_builder b = fs_builder("selfdiff_transc");
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

static nir_shader *
build_tex_modulate(void)
{
   nir_builder b = fs_builder("selfdiff_tex");
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

/* rc_constant's Type/UseMask bitfields share storage with each other, so
 * they are compared by name rather than folded into a struct-wide memcmp;
 * the union payload (u) is a real value union rc_constants_add_immediate_vec4
 * and friends always populate in full, so comparing it as bytes is a
 * legitimate whole-value compare, not a bitfield-hazard one. */
static bool
constants_equal(const struct rc_constant_list *a, const struct rc_constant_list *b)
{
   if (a->Count != b->Count)
      return false;
   for (unsigned i = 0; i < a->Count; i++) {
      const struct rc_constant *ca = &a->Constants[i];
      const struct rc_constant *cb = &b->Constants[i];
      if (ca->Type != cb->Type || ca->UseMask != cb->UseMask)
         return false;
      if (memcmp(&ca->u, &cb->u, sizeof(ca->u)) != 0)
         return false;
   }
   return true;
}

static void
selfdiff_case(const char *name, nir_shader *(*build)(void))
{
   struct r300_fragment_program_compiler fc_a, fc_b;
   struct rc_regalloc_state rs_a, rs_b;
   struct r300_fragment_shader_code code_a, code_b;
   memset(&code_a, 0, sizeof(code_a));
   memset(&code_b, 0, sizeof(code_b));

   char what[256];
   nir_shader *sa = build();
   nir_shader *sb = build();

   snprintf(what, sizeof(what), "%s: first compile succeeds", name);
   const bool ok_a = compile_legacy_full(sa, &fc_a, &rs_a, &code_a);
   CHECK(ok_a, what);
   if (!ok_a && fc_a.Base.ErrorMsg)
      fprintf(stderr, "  first compile said: %s\n", fc_a.Base.ErrorMsg);

   snprintf(what, sizeof(what), "%s: second compile succeeds", name);
   const bool ok_b = compile_legacy_full(sb, &fc_b, &rs_b, &code_b);
   CHECK(ok_b, what);
   if (!ok_b && fc_b.Base.ErrorMsg)
      fprintf(stderr, "  second compile said: %s\n", fc_b.Base.ErrorMsg);

   if (ok_a && ok_b) {
      snprintf(what, sizeof(what), "%s: nonempty ALU code generated", name);
      CHECK(code_a.code.code.r300.alu.length > 0, what);

      snprintf(what, sizeof(what), "%s: two compiles are byte-identical",
              name);
      CHECK(memcmp(&code_a.code.code.r300, &code_b.code.code.r300,
                   sizeof(code_a.code.code.r300)) == 0,
            what);

      /* The instruction/config block above is only half of what the two
       * compiles must agree on: rc_constants_copy (r3xx_fragprog.c) also
       * populates code.constants with the constant file the ALU words
       * index into, and a byte-diff that skips it would miss a divergence
       * in constant ordering or values while the instruction words still
       * matched. */
      snprintf(what, sizeof(what),
              "%s: two compiles upload identical constants", name);
      CHECK(constants_equal(&code_a.code.constants, &code_b.code.constants), what);
   }

   if (ok_a) {
      rc_destroy(&fc_a.Base);
      rc_destroy_regalloc_state(&rs_a);
   }
   if (ok_b) {
      rc_destroy(&fc_b.Base);
      rc_destroy_regalloc_state(&rs_b);
   }
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();

   selfdiff_case("fmad", build_fmad);
   selfdiff_case("transcendentals", build_transcendentals);
   selfdiff_case("tex_modulate", build_tex_modulate);

   glsl_type_singleton_decref();
   if (failures) {
      fprintf(stderr, "r300_classic_byte_exact_selfdiff_test: %d failures\n",
              failures);
      return 1;
   }
   printf("r300_classic_byte_exact_selfdiff_test: all checks passed\n");
   return 0;
}
