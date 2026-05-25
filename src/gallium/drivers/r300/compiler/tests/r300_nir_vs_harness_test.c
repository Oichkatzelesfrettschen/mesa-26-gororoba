/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * NIR-input admission harness for r300_nir_to_rc_direct.
 *
 * The rc_program corpus harness tests RC passes; it cannot reach
 * r300_nir_to_rc_direct, which consumes a nir_shader.  This harness builds a
 * tiny vertex shader with nir_builder, runs the production lowering
 * (r300_nir_lower_for_rc), and calls r300_nir_to_rc_direct, then asserts on the
 * emitted rc_program and the compiler error state.  It needs no pipe_screen
 * beyond a stack struct r300_screen whose caps the lowering reads (is_r500,
 * has_tcl, is_r400); r300_nir_to_rc_direct ignores its screen argument.
 *
 * Two boundaries are pinned.  First, two vertex inputs at distinct
 * driver_locations must map to distinct RC input slots: a regression here is
 * the input-collapse class where every input aliases IN[0].  Second, an
 * unsupported VS system value (VertexIndex / InstanceIndex) must fail
 * compilation deterministically -- rc_error sets compiler->Error so pipeline
 * creation fails -- rather than aborting the process, because R300VK ingests
 * arbitrary user SPIR-V.  This is admission correctness, not system-value
 * support.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "nir_to_rc.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_nir_to_rc_direct.h"
#include "r300_screen.h"
#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_program_constants.h"
#include "radeon_regalloc.h"

static unsigned g_failures;

#define CHECK(cond, name)                                                      \
   do {                                                                        \
      if (cond) {                                                              \
         printf("  ok   - %s\n", (name));                                      \
      } else {                                                                 \
         printf("  FAIL - %s\n", (name));                                      \
         g_failures++;                                                         \
      }                                                                        \
   } while (0)

/* A stack screen whose caps the lowering reads.  has_tcl = true selects the
 * HW-TCL route that actually reaches r300_nir_to_rc_direct; is_r500/is_r400
 * false makes it an R300-class part. */
static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   memset(s, 0, sizeof(*s));
   s->caps.has_tcl = true;
   s->caps.is_r500 = false;
   s->caps.is_r400 = false;
   return (struct pipe_screen *)s;
}

enum vs_sysval {
   VS_SYSVAL_NONE,
   VS_SYSVAL_VERTEX_ID,
   VS_SYSVAL_INSTANCE_ID,
};

/* Build a vertex-shader nir_shader.  The body is gl_Position = in0 + in1 (two
 * distinct generic attributes); a non-NONE sysval adds an unsupported VS
 * system-value read so the emitter must reject it. */
static nir_shader *
build_vs(enum vs_sysval sysval)
{
   static const nir_shader_compiler_options options;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                  "r300_nir_vs_harness");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in0");
   in0->data.location = VERT_ATTRIB_GENERIC0;
   in0->data.driver_location = 0;

   nir_variable *in1 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in1");
   in1->data.location = VERT_ATTRIB_GENERIC1;
   in1->data.driver_location = 1;

   nir_variable *pos =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_Position");
   pos->data.location = VARYING_SLOT_POS;
   pos->data.driver_location = 0;

   nir_def *value = nir_fadd(&b, nir_load_var(&b, in0), nir_load_var(&b, in1));

   nir_def *sv = NULL;
   if (sysval == VS_SYSVAL_VERTEX_ID)
      sv = nir_load_vertex_id(&b);
   else if (sysval == VS_SYSVAL_INSTANCE_ID)
      sv = nir_load_instance_id(&b);
   if (sv) {
      nir_def *f = nir_u2f32(&b, sv);
      value = nir_fadd(&b, value, nir_vec4(&b, f, f, f, f));
   }

   nir_store_var(&b, pos, value, 0xf);
   return b.shader;
}

/* Build a VS that reads a system value as a load_deref of a nir_var_system_value
 * variable -- the form spirv_to_nir emits (vtn maps gl_VertexIndex /
 * gl_InstanceIndex to SYSTEM_VALUE_VERTEX_ID / SYSTEM_VALUE_INSTANCE_ID), before
 * any nir_lower_system_values runs.  build_vs emits the post-lowering intrinsic
 * form; this pins the pre-lowering deref form the real R300VK SPIR-V path
 * produces, which reaches nir_to_rc as "Unknown intrinsic: load_deref" when
 * detection misses it. */
static nir_shader *
build_vs_sysval_deref(gl_system_value sysval)
{
   static const nir_shader_compiler_options options;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                  "r300_nir_vs_harness_deref");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in0");
   in0->data.location = VERT_ATTRIB_GENERIC0;
   in0->data.driver_location = 0;

   nir_variable *pos =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_Position");
   pos->data.location = VARYING_SLOT_POS;
   pos->data.driver_location = 0;

   nir_variable *sv =
      nir_variable_create(b.shader, nir_var_system_value, glsl_int_type(),
                          sysval == SYSTEM_VALUE_INSTANCE_ID ? "gl_InstanceID"
                                                             : "gl_VertexID");
   sv->data.location = sysval;

   nir_def *f = nir_u2f32(&b, nir_load_var(&b, sv));
   nir_def *value =
      nir_fadd(&b, nir_load_var(&b, in0), nir_vec4(&b, f, f, f, f));
   nir_store_var(&b, pos, value, 0xf);
   return b.shader;
}

/* Count load_deref reads of a nir_var_system_value variable still present. */
static unsigned
count_system_value_derefs(nir_shader *s)
{
   unsigned count = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_deref)
               continue;
            nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
            if (deref && nir_deref_mode_is(deref, nir_var_system_value))
               count++;
         }
      }
   }
   return count;
}

/* Run the production lowering + the direct emitter on a built shader. */
static void
run_vs(struct r300_vertex_program_compiler *c, struct rc_regalloc_state *rs,
       nir_shader *nir)
{
   struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state ext = {0};

   rc_init_regalloc_state(rs, RC_VERTEX_PROGRAM);
   memset(c, 0, sizeof(*c));
   rc_init(&c->Base, rs);
   c->Base.type = RC_VERTEX_PROGRAM;
   c->Base.is_r400 = false;
   c->Base.is_r500 = false;
   c->Base.has_half_swizzles = false;
   c->Base.has_presub = false;
   c->Base.has_omod = false;
   c->Base.max_temp_regs = 32;
   c->Base.max_constants = 256;
   c->Base.max_alu_insts = 256;

   r300_nir_lower_for_rc(nir, ps, ext);
   r300_nir_to_rc_direct(&c->Base, nir, ps, ext);
}

static void
teardown_vs(struct r300_vertex_program_compiler *c, struct rc_regalloc_state *rs)
{
   rc_destroy(&c->Base);
   rc_destroy_regalloc_state(rs);
}

/* Collect the distinct RC_FILE_INPUT source indices the program reads. */
static unsigned
distinct_input_slots(struct radeon_compiler *c, unsigned *seen, unsigned cap)
{
   unsigned count = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      const struct rc_opcode_info *info = rc_get_opcode_info(inst->U.I.Opcode);
      for (unsigned s = 0; s < info->NumSrcRegs; s++) {
         if (inst->U.I.SrcReg[s].File != RC_FILE_INPUT)
            continue;
         unsigned idx = inst->U.I.SrcReg[s].Index;
         bool already = false;
         for (unsigned k = 0; k < count; k++)
            if (seen[k] == idx)
               already = true;
         if (!already && count < cap)
            seen[count++] = idx;
      }
   }
   return count;
}

static void
case_two_attributes_do_not_alias(void)
{
   struct r300_vertex_program_compiler c;
   struct rc_regalloc_state rs;
   run_vs(&c, &rs, build_vs(VS_SYSVAL_NONE));

   CHECK(!c.Base.Error, "dual-attribute VS compiles without error");

   unsigned seen[8];
   unsigned n = distinct_input_slots(&c.Base, seen, 8);
   bool has0 = false, has1 = false;
   for (unsigned k = 0; k < n; k++) {
      has0 |= seen[k] == 0;
      has1 |= seen[k] == 1;
   }
   CHECK(has0 && has1,
         "two driver_locations map to two distinct RC input slots (no alias)");

   teardown_vs(&c, &rs);
}

/* An unsupported VS system value must set compiler->Error (so the caller fails
 * compilation deterministically) and name the intrinsic in the diagnostic.
 * Reaching this code at all proves no process abort occurred. */
static void
case_system_value_rejected(enum vs_sysval sysval, const char *label,
                           const char *needle)
{
   struct r300_vertex_program_compiler c;
   struct rc_regalloc_state rs;
   run_vs(&c, &rs, build_vs(sysval));

   printf("  (%s)\n", label);
   CHECK(c.Base.Error,
         "the unsupported VS system value sets a compiler error");
   CHECK(c.Base.ErrorMsg && strstr(c.Base.ErrorMsg, needle),
         "the diagnostic names the offending intrinsic");

   teardown_vs(&c, &rs);
}

/* When the driver reserves a synthetic-attribute slot,
 * r300_nir_lower_vs_system_values_to_inputs rewrites the system value to a read
 * of that input before r300_nir_to_rc_direct runs, so it compiles cleanly
 * (instead of the deterministic rejection) and reads the reserved RC slot. */
static void
case_system_value_lowered_to_input(void)
{
   struct r300_vertex_program_compiler c;
   struct rc_regalloc_state rs;
   nir_shader *nir = build_vs(VS_SYSVAL_VERTEX_ID);

   bool changed = r300_nir_lower_vs_system_values_to_inputs(nir, 2, -1);
   run_vs(&c, &rs, nir);

   CHECK(changed, "the lowering pass rewrote load_vertex_id");
   CHECK(!c.Base.Error,
         "a system value with a reserved slot compiles without error");

   unsigned seen[8];
   unsigned n = distinct_input_slots(&c.Base, seen, 8);
   bool has_slot2 = false;
   for (unsigned k = 0; k < n; k++)
      has_slot2 |= seen[k] == 2;
   CHECK(has_slot2, "the lowered system value reads the reserved RC input slot");

   teardown_vs(&c, &rs);
}

/* Calibration for the real R300VK SPIR-V path.  Detection and lowering must
 * handle the load_deref(nir_var_system_value) form, not just the intrinsic the
 * nir_builder cases emit.  vk_spirv_to_nir leaves the deref form and never
 * gathers system_values_read, so r300vk_pipeline.c detects the read by scanning
 * the NIR via r300_nir_vs_reads_system_values; this pins that detection and the
 * lowering on the exact form that reaches nir_to_rc unlowered when missed. */
static void
case_system_value_deref_form(void)
{
   struct r300_vertex_program_compiler c;
   struct rc_regalloc_state rs;
   nir_shader *nir = build_vs_sysval_deref(SYSTEM_VALUE_VERTEX_ID);

   bool reads_vid = false, reads_iid = false;
   r300_nir_vs_reads_system_values(nir, &reads_vid, &reads_iid);
   CHECK(reads_vid && !reads_iid,
         "detection sees gl_VertexIndex in the load_deref(system_value) form");

   bool changed = r300_nir_lower_vs_system_values_to_inputs(nir, 1, -1);
   CHECK(changed, "the lowering pass rewrote the system-value deref");
   CHECK(count_system_value_derefs(nir) == 0,
         "no system-value deref survives the lowering pass");

   run_vs(&c, &rs, nir);
   CHECK(!c.Base.Error,
         "the deref-form system value compiles without error after lowering");

   unsigned seen[8];
   unsigned n = distinct_input_slots(&c.Base, seen, 8);
   bool has_slot1 = false;
   for (unsigned k = 0; k < n; k++)
      has_slot1 |= seen[k] == 1;
   CHECK(has_slot1,
         "the lowered deref-form value reads the reserved RC input slot");

   teardown_vs(&c, &rs);
}

int
main(void)
{
   printf("r300 NIR-to-RC admission harness\n");
   case_two_attributes_do_not_alias();
   case_system_value_rejected(VS_SYSVAL_VERTEX_ID, "load_vertex_id", "vertex_id");
   case_system_value_rejected(VS_SYSVAL_INSTANCE_ID, "load_instance_id",
                              "instance_id");
   case_system_value_lowered_to_input();
   case_system_value_deref_form();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
