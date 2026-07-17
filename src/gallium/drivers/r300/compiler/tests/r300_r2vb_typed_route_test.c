/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Calibration for the diagnostic typed-split route: the exact-value gate
 * parser admits only "1", and the route contract admits only the position
 * cell of a READY SPLIT plan with a typed source, flat-vertex semantics, a
 * one-vec4 carry, and an owned candidate.  Synthetic plan records drive
 * every decline arm (known-bad), and live plans calibrate the two decline
 * classes production shapes actually produce: a float over-budget SPLIT
 * declines on the absent typed source, and a fitting producer declines on
 * the plan action.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "r300_context.h"
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
fake_stack_init(void)
{
   memset(&g_screen, 0, sizeof(g_screen));
   g_screen.caps.has_tcl = false;
   r300_screen_init_nir_options(&g_screen);

   memset(&g_context, 0, sizeof(g_context));
   g_context.screen = &g_screen;
   rc_init_regalloc_state(&g_context.fs_regalloc_state, RC_FRAGMENT_PROGRAM);
   g_context.viewport.scale[0] = 320.0f;
   g_context.viewport.scale[1] = 240.0f;
   g_context.viewport.scale[2] = 0.5f;
   g_context.viewport.translate[0] = 320.0f;
   g_context.viewport.translate[1] = 240.0f;
   g_context.viewport.translate[2] = 0.5f;
}

static nir_shader *
build_float_producer(unsigned chain_length, const char *name)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, g_screen.screen.nir_options[MESA_SHADER_VERTEX],
      "%s", name);
   nir_variable *in_pos = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "in_pos");
   in_pos->data.location = VERT_ATTRIB_GENERIC0;
   in_pos->data.driver_location = 0;
   nir_variable *out_pos = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec4_type(), "gl_Position");
   out_pos->data.location = VARYING_SLOT_POS;
   out_pos->data.driver_location = 0;
   nir_def *pos = nir_load_var(&b, in_pos);
   nir_def *v = nir_channel(&b, pos, 0);
   for (unsigned i = 0; i < chain_length; i++)
      v = nir_ffma(&b, v, nir_imm_float(&b, 1.0001f + (float)(i % 7) * 0.001f),
                   nir_channel(&b, pos, 0));
   nir_store_var(&b, out_pos, nir_replicate(&b, v, 4), 0xf);
   nir_validate_shader(b.shader, "typed route VS");
   return b.shader;
}

/* Synthetic admit template every decline row perturbs: the contract reads
 * only the fields set here, so a sentinel non-NULL candidate stands in for
 * an owned shader. */
static struct r300_r2vb_producer_plan
admit_template(void)
{
   struct r300_r2vb_producer_plan plan;
   memset(&plan, 0, sizeof(plan));
   plan.status = R300_R2VB_PLAN_READY;
   plan.action = R300_R2VB_PLAN_SPLIT;
   plan.has_typed_source = true;
   plan.typed_source_class = R300_R2VB_TYPED_SOURCE_SINT;
   plan.key.input_semantics = R300_FS_INPUT_R2VB_FLAT_VERTEX;
   plan.partition.total_comps = 1;
   plan.candidate = (struct nir_shader *)&plan;
   return plan;
}

static void
check_decline(const struct r300_r2vb_producer_plan *plan, bool cv,
              const char *expect, const char *name)
{
   const char *got = r300_r2vb_typed_split_contract(plan, cv);
   char label[128];
   snprintf(label, sizeof(label), "%s declines %s", name, expect);
   CHECK(got && strcmp(got, expect) == 0, label);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   fake_stack_init();

   /* Gate parser: the exact value 1 opens; everything else stays closed. */
   CHECK(!r300_r2vb_typed_split_gate_value(NULL), "gate: unset stays closed");
   CHECK(!r300_r2vb_typed_split_gate_value(""), "gate: empty stays closed");
   CHECK(!r300_r2vb_typed_split_gate_value("0"), "gate: 0 stays closed");
   CHECK(!r300_r2vb_typed_split_gate_value("2"), "gate: 2 stays closed");
   CHECK(!r300_r2vb_typed_split_gate_value("spill1"),
         "gate: spill1 stays closed");
   CHECK(!r300_r2vb_typed_split_gate_value(" 1"),
         "gate: padded value stays closed");
   CHECK(r300_r2vb_typed_split_gate_value("1"), "gate: exact 1 opens");

   /* Contract decline arms, one perturbation each. */
   struct r300_r2vb_producer_plan plan = admit_template();
   CHECK(r300_r2vb_typed_split_contract(&plan, false) == NULL,
         "contract admits the diagnostic shape");
   check_decline(&plan, true, "computed_varying_cell", "cv=1 cell");
   check_decline(NULL, false, "plan_transient", "missing plan");

   plan = admit_template();
   plan.status = R300_R2VB_PLAN_SEMANTIC_REJECT;
   check_decline(&plan, false, "plan_not_ready", "semantic reject");

   plan = admit_template();
   plan.action = R300_R2VB_PLAN_SINGLE;
   check_decline(&plan, false, "plan_not_split", "single-pass plan");

   plan = admit_template();
   plan.has_typed_source = false;
   check_decline(&plan, false, "typed_source_absent", "float split");

   plan = admit_template();
   plan.key.input_semantics = R300_FS_INPUT_INTERPOLATED;
   check_decline(&plan, false, "input_semantics", "interpolated semantics");

   plan = admit_template();
   plan.partition.total_comps = 0;
   check_decline(&plan, false, "carry_width", "empty carry");

   plan = admit_template();
   plan.partition.total_comps = 5;
   check_decline(&plan, false, "carry_width", "wide carry");

   plan = admit_template();
   plan.candidate = NULL;
   check_decline(&plan, false, "candidate_absent", "missing candidate");

   /* Live plans: the two decline classes production shapes produce. */
   nir_shader *over = build_float_producer(90, "typed_route_float_over");
   struct r300_r2vb_producer_plan live;
   bool ran = r300_r2vb_plan_producer(&g_context, over, false,
                                      R300_R2VB_POSITION_CLIP, &live);
   CHECK(ran && live.action == R300_R2VB_PLAN_SPLIT,
         "float over-budget producer plans split");
   if (ran) {
      check_decline(&live, false, "typed_source_absent",
                    "live float split");
      r300_r2vb_plan_release(&live);
   }

   nir_shader *fits = build_float_producer(8, "typed_route_float_fits");
   ran = r300_r2vb_plan_producer(&g_context, fits, false,
                                 R300_R2VB_POSITION_CLIP, &live);
   CHECK(ran && live.action == R300_R2VB_PLAN_SINGLE,
         "fitting producer plans single");
   if (ran) {
      check_decline(&live, false, "plan_not_split", "live fitting producer");
      r300_r2vb_plan_release(&live);
   }

   ralloc_free(over);
   ralloc_free(fits);
   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   glsl_type_singleton_decref();
   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
