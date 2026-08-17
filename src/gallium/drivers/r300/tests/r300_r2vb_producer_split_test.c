/*
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
 * one-component carry admits, a program whose every admissible cut crosses
 * more than four components is declined, bounded signed and unsigned integers
 * select matching exact transports, and an unbounded uint32 carry is declined.
 * The oracle is the production
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

enum integer_carry_case {
   INTEGER_CARRY_SIGNED_EXACT,
   INTEGER_CARRY_SIGNED_POSITIVE_OUTSIDE,
   INTEGER_CARRY_SIGNED_NEGATIVE_OUTSIDE,
   INTEGER_CARRY_UNSIGNED_EXACT,
   INTEGER_CARRY_UNSIGNED_OUTSIDE,
   INTEGER_CARRY_UNSIGNED_UNBOUNDED,
   INTEGER_CARRY_SIGNED_TO_UNSIGNED,
   INTEGER_CARRY_UNSIGNED_TO_SIGNED,
};

/* Keep one integer value live across the balance region of a long FP32 chain.
 * The exact variants stop at the R300 FP24 ALU's 2^17 exact-integer boundary.
 * The outside variants extend one integer beyond either signed edge or the
 * unsigned upper edge; the unbounded variant retains the full f2u32 range. */
static nir_shader *
build_integer_carry_fs(enum integer_carry_case carry_case)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                  &fs_options,
                                                  "r2vb_split_integer_carry");
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

   nir_def *input = nir_load_var(&b, in0);
   nir_def *source = nir_channel(&b, input, 2);
   nir_def *integer;
   bool signed_producer = carry_case == INTEGER_CARRY_SIGNED_EXACT ||
                          carry_case == INTEGER_CARRY_SIGNED_POSITIVE_OUTSIDE ||
                          carry_case == INTEGER_CARRY_SIGNED_NEGATIVE_OUTSIDE ||
                          carry_case == INTEGER_CARRY_SIGNED_TO_UNSIGNED;
   if (signed_producer) {
      integer = nir_f2i32(&b, source);
      int32_t lower = carry_case == INTEGER_CARRY_SIGNED_NEGATIVE_OUTSIDE
                         ? -131073
                         : -131072;
      int32_t upper = carry_case == INTEGER_CARRY_SIGNED_POSITIVE_OUTSIDE
                         ? 131073
                         : 131072;
      integer = nir_imax(&b, integer, nir_imm_int(&b, lower));
      integer = nir_imin(&b, integer, nir_imm_int(&b, upper));
   } else {
      integer = nir_f2u32(&b, source);
      if (carry_case != INTEGER_CARRY_UNSIGNED_UNBOUNDED) {
         int32_t upper = carry_case == INTEGER_CARRY_UNSIGNED_OUTSIDE
                            ? 131073
                            : 131072;
         integer = nir_umin(&b, integer, nir_imm_int(&b, upper));
      }
   }

   nir_def *factor = nir_channel(&b, input, 1);
   nir_def *value = nir_channel(&b, input, 0);
   for (unsigned step = 0; step < 90; step++)
      value = nir_fmad(&b, value, factor, nir_imm_float(&b, 0.5f));

   bool signed_consumer =
      (signed_producer && carry_case != INTEGER_CARRY_SIGNED_TO_UNSIGNED) ||
      carry_case == INTEGER_CARRY_UNSIGNED_TO_SIGNED;
   nir_def *integer_float = signed_consumer ? nir_i2f32(&b, integer)
                                            : nir_u2f32(&b, integer);
   value = nir_fadd(&b, value, integer_float);
   nir_store_var(&b, out, nir_vec4(&b, value, value, value, value), 0xf);
   return b.shader;
}

static bool
shader_has_alu_op(nir_shader *shader, nir_op op)
{
   nir_foreach_function_impl (impl, shader) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type == nir_instr_type_alu &&
                nir_instr_as_alu(instr)->op == op)
               return true;
         }
      }
   }
   return false;
}

static bool
def_depends_on_input(nir_def *def, gl_varying_slot location, unsigned depth)
{
   if (depth > 16)
      return false;

   nir_instr *instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instr);
      if (intrinsic->intrinsic != nir_intrinsic_load_deref)
         return false;
      nir_deref_instr *deref = nir_src_as_deref(intrinsic->src[0]);
      nir_variable *variable = nir_deref_instr_get_variable(deref);
      return variable && variable->data.mode == nir_var_shader_in &&
             variable->data.location == location;
   }

   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   for (unsigned source = 0; source < nir_op_infos[alu->op].num_inputs;
        source++)
      if (def_depends_on_input(alu->src[source].src.ssa, location, depth + 1))
         return true;
   return false;
}

static bool
shader_has_input_conversion(nir_shader *shader, nir_op op,
                            gl_varying_slot location)
{
   nir_foreach_function_impl (impl, shader) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->op == op &&
                def_depends_on_input(alu->src[0].src.ssa, location, 0))
               return true;
         }
      }
   }
   return false;
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

   r300_optimize_nir(nir, &r300_screen(ps)->caps);
   nir_to_rc(nir, &r300_screen(ps)->caps, ext, code, &c.Base);
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
   r300_optimize_nir(pos, &r300_screen(ps)->caps);

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

   /* Pass-B carry-input rank contract: the producer draw feeds embedded
    * attribute a to VAR0+a, model attributes first, carry last -- so a
    * num_in=1 pass B must read the carry as a flat input at VAR0+1 while the
    * model input stays at VAR0. */
   bool carry_in_ok = false, model_in_ok = false;
   nir_foreach_variable_with_modes(var, pass_b, nir_var_shader_in) {
      if (var->data.location == VARYING_SLOT_VAR0 + 1 &&
          var->data.interpolation == INTERP_MODE_FLAT)
         carry_in_ok = true;
      if (var->data.location == VARYING_SLOT_VAR0)
         model_in_ok = true;
   }
   CHECK(carry_in_ok, "pass B reads the carry as a flat input at VAR0 + num_in");
   CHECK(model_in_ok, "pass B keeps the model input at VAR0");
   CHECK(pass_b->info.inputs_read & BITFIELD64_BIT(VARYING_SLOT_VAR0 + 1),
         "pass B inputs_read covers the carry slot");

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
   r300_optimize_nir(pos, &r300_screen(ps)->caps);

   struct r300_mp_partition part;
   bool have_cut = r300_mp_find_vec4_cut(pos, &part);
   CHECK(!have_cut,
         "find_vec4_cut declines when every cut carries more than one vec4");
   ralloc_free(pos);
}

static void
case_typed_integer_carries_require_exact_ranges(void)
{
   struct r300_screen screen = {0};
   struct pipe_screen *ps = fake_r300_screen(&screen);

   nir_shader *signed_pos = build_integer_carry_fs(INTEGER_CARRY_SIGNED_EXACT);
   r300_optimize_nir(signed_pos, &r300_screen(ps)->caps);
   struct r300_mp_partition signed_part;
   bool signed_cut = r300_mp_find_vec4_cut(signed_pos, &signed_part);
   CHECK(signed_cut, "range-proven signed carry admits a vec4 cut");
   if (signed_cut) {
      bool signed_transport = false;
      for (unsigned base = 0; base < signed_part.num_bases; base++)
         signed_transport |=
            signed_part.r2vb_transport[base] == R300_MP_R2VB_SINT;
      CHECK(signed_transport, "signed carry selects exact signed transport");
      nir_shader *pass_a = r300_mp_build_carry_pass_a(signed_pos, &signed_part);
      nir_shader *pass_b = r300_mp_build_pos_pass_b(signed_pos, &signed_part, 1);
      CHECK(pass_a && shader_has_alu_op(pass_a, nir_op_i2f32),
            "signed pass A contains i2f32 transport");
      CHECK(pass_b && shader_has_input_conversion(
                         pass_b, nir_op_ftrunc, VARYING_SLOT_VAR0 + 1),
            "signed pass B reconstructs the flat carry with ftrunc");
      CHECK(pass_b && !shader_has_input_conversion(
                         pass_b, nir_op_f2i32, VARYING_SLOT_VAR0 + 1),
            "signed flat carry bypasses f2i epsilon lowering");
      CHECK(pass_a && oracle_fits(pass_a),
            "signed carry pass A compiles under the 64-slot ceiling");
      CHECK(pass_b && oracle_fits(pass_b),
            "signed carry pass B compiles under the 64-slot ceiling");
   }
   ralloc_free(signed_pos);

   nir_shader *signed_positive_outside =
      build_integer_carry_fs(INTEGER_CARRY_SIGNED_POSITIVE_OUTSIDE);
   r300_optimize_nir(signed_positive_outside, &r300_screen(ps)->caps);
   struct r300_mp_partition signed_positive_part;
   CHECK(!r300_mp_find_vec4_cut(signed_positive_outside,
                                &signed_positive_part),
         "signed carry above positive 2^17 boundary declines");
   ralloc_free(signed_positive_outside);

   nir_shader *signed_negative_outside =
      build_integer_carry_fs(INTEGER_CARRY_SIGNED_NEGATIVE_OUTSIDE);
   r300_optimize_nir(signed_negative_outside, &r300_screen(ps)->caps);
   struct r300_mp_partition signed_negative_part;
   CHECK(!r300_mp_find_vec4_cut(signed_negative_outside,
                                &signed_negative_part),
         "signed carry below negative 2^17 boundary declines");
   ralloc_free(signed_negative_outside);

   nir_shader *unsigned_pos =
      build_integer_carry_fs(INTEGER_CARRY_UNSIGNED_EXACT);
   r300_optimize_nir(unsigned_pos, &r300_screen(ps)->caps);
   struct r300_mp_partition unsigned_part;
   bool unsigned_cut = r300_mp_find_vec4_cut(unsigned_pos, &unsigned_part);
   CHECK(unsigned_cut, "range-proven unsigned carry admits a vec4 cut");
   if (unsigned_cut) {
      bool unsigned_transport = false;
      for (unsigned base = 0; base < unsigned_part.num_bases; base++)
         unsigned_transport |=
            unsigned_part.r2vb_transport[base] == R300_MP_R2VB_UINT;
      CHECK(unsigned_transport, "unsigned carry selects exact unsigned transport");
      nir_shader *pass_a =
         r300_mp_build_carry_pass_a(unsigned_pos, &unsigned_part);
      nir_shader *pass_b =
         r300_mp_build_pos_pass_b(unsigned_pos, &unsigned_part, 1);
      CHECK(pass_a && shader_has_alu_op(pass_a, nir_op_u2f32),
            "unsigned pass A contains u2f32 transport");
      CHECK(pass_b && shader_has_input_conversion(
                         pass_b, nir_op_ffloor, VARYING_SLOT_VAR0 + 1),
            "unsigned pass B reconstructs the flat carry with ffloor");
      CHECK(pass_b && !shader_has_input_conversion(
                         pass_b, nir_op_f2u32, VARYING_SLOT_VAR0 + 1),
            "unsigned flat carry bypasses f2u epsilon lowering");
      CHECK(pass_a && oracle_fits(pass_a),
            "unsigned carry pass A compiles under the 64-slot ceiling");
      CHECK(pass_b && oracle_fits(pass_b),
            "unsigned carry pass B compiles under the 64-slot ceiling");
   }
   ralloc_free(unsigned_pos);

   nir_shader *unsigned_outside =
      build_integer_carry_fs(INTEGER_CARRY_UNSIGNED_OUTSIDE);
   r300_optimize_nir(unsigned_outside, &r300_screen(ps)->caps);
   struct r300_mp_partition unsigned_outside_part;
   CHECK(!r300_mp_find_vec4_cut(unsigned_outside, &unsigned_outside_part),
         "unsigned carry above 2^17 boundary declines");
   ralloc_free(unsigned_outside);

   nir_shader *unbounded_pos =
      build_integer_carry_fs(INTEGER_CARRY_UNSIGNED_UNBOUNDED);
   r300_optimize_nir(unbounded_pos, &r300_screen(ps)->caps);
   struct r300_mp_partition unbounded_part;
   CHECK(!r300_mp_find_vec4_cut(unbounded_pos, &unbounded_part),
         "unproven uint32 carry declines the FP32 transport");
   ralloc_free(unbounded_pos);

   nir_shader *signed_to_unsigned =
      build_integer_carry_fs(INTEGER_CARRY_SIGNED_TO_UNSIGNED);
   r300_optimize_nir(signed_to_unsigned, &r300_screen(ps)->caps);
   struct r300_mp_partition signed_to_unsigned_part;
   CHECK(!r300_mp_find_vec4_cut(signed_to_unsigned,
                                &signed_to_unsigned_part),
         "signed producer with unsigned post-cut consumer declines");
   ralloc_free(signed_to_unsigned);

   nir_shader *unsigned_to_signed =
      build_integer_carry_fs(INTEGER_CARRY_UNSIGNED_TO_SIGNED);
   r300_optimize_nir(unsigned_to_signed, &r300_screen(ps)->caps);
   struct r300_mp_partition unsigned_to_signed_part;
   CHECK(!r300_mp_find_vec4_cut(unsigned_to_signed,
                                &unsigned_to_signed_part),
         "unsigned producer with signed post-cut consumer declines");
   ralloc_free(unsigned_to_signed);
}

int
main(void)
{
   printf("r300 R2VB carry-BO producer split\n");
   case_over_budget_chain_splits();
   case_wide_carry_declined();
   case_typed_integer_carries_require_exact_ranges();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
