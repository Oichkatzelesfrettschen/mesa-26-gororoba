/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Calibration for the diagnostic typed-split route: the exact-value gate
 * parser admits only "1", and the route contract admits only the position
 * cell of a READY SPLIT plan whose key matches the requested cell, with a
 * typed source, a typed transport in the selected carry, flat-vertex
 * semantics, a one-vec4 carry, the planned model-attribute arity, and an
 * owned candidate.  Synthetic plan records drive every decline arm
 * (known-bad), live plans calibrate the two decline classes production
 * shapes actually produce, the token formatter is pinned field by field
 * (the f/i/u/b transport letters are the silicon engagement oracle), and
 * the memo-writer effective-admission matrix pins how the typed and spill1
 * gates compose.
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
 * an owned shader.  The template's cell is (cv=0, clip) with one model
 * attribute and a single SINT carry component. */
static struct r300_r2vb_producer_plan
admit_template(void)
{
   struct r300_r2vb_producer_plan plan;
   memset(&plan, 0, sizeof(plan));
   plan.status = R300_R2VB_PLAN_READY;
   plan.action = R300_R2VB_PLAN_SPLIT;
   plan.has_typed_source = true;
   plan.typed_source_class = R300_R2VB_TYPED_SOURCE_SINT;
   plan.key.allow_computed_varying = false;
   plan.key.space = R300_R2VB_POSITION_CLIP;
   plan.key.input_semantics = R300_FS_INPUT_R2VB_FLAT_VERTEX;
   plan.num_position_inputs = 1;
   plan.partition.total_comps = 1;
   plan.partition.num_bases = 1;
   plan.partition.r2vb_transport[0] = R300_MP_R2VB_SINT;
   plan.candidate = (struct nir_shader *)&plan;
   plan.legacy_split_admitted = true;
   return plan;
}

static const char *
contract_clip(const struct r300_r2vb_producer_plan *plan, bool cv,
              unsigned num_in)
{
   return r300_r2vb_typed_split_contract(plan, cv, R300_R2VB_POSITION_CLIP,
                                         num_in);
}

static void
check_decline(const struct r300_r2vb_producer_plan *plan, bool cv,
              unsigned num_in, const char *expect, const char *name)
{
   const char *got = contract_clip(plan, cv, num_in);
   char label[128];
   snprintf(label, sizeof(label), "%s declines %s", name, expect);
   CHECK(got && strcmp(got, expect) == 0, label);
}

static void
check_token_contains(const struct r300_r2vb_producer_plan *plan,
                     const char *decline, const char *needle,
                     const char *name)
{
   char line[512];
   r300_r2vb_typed_split_note_format(plan, R300_R2VB_POSITION_CLIP, decline,
                                     line, sizeof(line));
   char label[160];
   snprintf(label, sizeof(label), "token %s carries %s", name, needle);
   CHECK(strstr(line, needle) != NULL, label);
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
   CHECK(contract_clip(&plan, false, 1) == NULL,
         "contract admits the diagnostic shape");
   check_decline(&plan, true, 1, "computed_varying_cell", "cv=1 cell");
   check_decline(NULL, false, 1, "plan_transient", "missing plan");

   plan = admit_template();
   plan.status = R300_R2VB_PLAN_SEMANTIC_REJECT;
   check_decline(&plan, false, 1, "plan_not_ready", "semantic reject");

   plan = admit_template();
   plan.action = R300_R2VB_PLAN_SINGLE;
   check_decline(&plan, false, 1, "plan_not_split", "single-pass plan");

   /* The contract defends its own authority: a plan cell keyed for another
    * cell declines instead of executing a mismatched program. */
   plan = admit_template();
   plan.key.space = R300_R2VB_POSITION_WINDOW;
   check_decline(&plan, false, 1, "plan_key_mismatch", "wrong-space key");

   plan = admit_template();
   plan.key.allow_computed_varying = true;
   check_decline(&plan, false, 1, "plan_key_mismatch", "cv-keyed plan");

   plan = admit_template();
   plan.has_typed_source = false;
   check_decline(&plan, false, 1, "typed_source_absent", "float split");

   /* Typed source with a float-only selected carry: the typed computation
    * converts before the cut, so executing the cell proves nothing about
    * typed transport and the contract declines it. */
   plan = admit_template();
   plan.partition.r2vb_transport[0] = R300_MP_R2VB_FLOAT;
   check_decline(&plan, false, 1, "typed_carry_absent",
                 "float-only carry");

   plan = admit_template();
   plan.key.input_semantics = R300_FS_INPUT_INTERPOLATED;
   check_decline(&plan, false, 1, "input_semantics",
                 "interpolated semantics");

   plan = admit_template();
   plan.partition.total_comps = 0;
   check_decline(&plan, false, 1, "carry_width", "empty carry");

   plan = admit_template();
   plan.partition.total_comps = 5;
   check_decline(&plan, false, 1, "carry_width", "wide carry");

   /* Pass B rebuilds at the planned arity; a diverging caller count
    * declines rather than executing a program the plan did not measure. */
   plan = admit_template();
   check_decline(&plan, false, 2, "input_count_mismatch",
                 "caller arity drift");

   plan = admit_template();
   plan.candidate = NULL;
   check_decline(&plan, false, 1, "candidate_absent", "missing candidate");

   /* Token formatter: the carry letters come from r2vb_transport, so signed,
    * unsigned, and boolean transport stay distinguishable in the token
    * alone (the T3 unsigned boundary cell must not print as 'i'). */
   plan = admit_template();
   plan.partition.num_bases = 4;
   plan.partition.total_comps = 4;
   plan.partition.r2vb_transport[0] = R300_MP_R2VB_SINT;
   plan.partition.r2vb_transport[1] = R300_MP_R2VB_UINT;
   plan.partition.r2vb_transport[2] = R300_MP_R2VB_BOOL1;
   plan.partition.r2vb_transport[3] = R300_MP_R2VB_FLOAT;
   check_token_contains(&plan, NULL, "carry_types=iubf", "transport letters");
   plan.partition.r2vb_transport[2] = R300_MP_R2VB_BOOL32;
   check_token_contains(&plan, NULL, "carry_types=iubf",
                        "bool32 transport letter");
   check_token_contains(&plan, NULL, "plan_status=ready plan_action=split",
                        "status/action fields");
   check_token_contains(&plan, NULL, "space=clip", "space field");
   check_token_contains(&plan, NULL, "typed_source=sint",
                        "typed-source field");
   check_token_contains(&plan, NULL, "decision=execute decline_reason=none",
                        "execute decision");
   check_token_contains(&plan, "carry_width",
                        "decision=decline decline_reason=carry_width",
                        "decline decision");
   check_token_contains(NULL, "plan_transient", "plan_status=transient",
                        "transient plan status");

   /* Memo-writer effective-admission matrix: the typed writer records
    * exactly what its contract admits (spill1 never overrides a typed
    * decline), and the legacy writer keys a SPLIT on spill1 plus the first
    * transport-valid cut (the typed gate never widens the legacy mapping). */
   plan = admit_template();
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_TYPED_DIAGNOSTIC, false, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_SPLIT,
         "matrix: typed writer, contract admit, spill1 off -> split");
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_LEGACY_FLOAT, true, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_SPLIT,
         "matrix: legacy writer, split plan, spill1 on -> split");
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_LEGACY_FLOAT, false, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_REJECT,
         "matrix: legacy writer, split plan, spill1 off -> reject");

   plan.legacy_split_admitted = false;
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_LEGACY_FLOAT, true, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_REJECT,
         "matrix: later typed cut leaves legacy writer at reject");
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_TYPED_DIAGNOSTIC, false, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_SPLIT,
         "matrix: later typed cut stays available to typed writer");

   /* The pinned both-gates row: a typed-arm cell the contract declines
    * (typed source absent after restage) stays a reject with spill1 armed,
    * so the shadow prediction matches the typed writer's memo and the
    * boolean-source float-carry shape produces no false divergence. */
   plan = admit_template();
   plan.has_typed_source = false;
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_TYPED_DIAGNOSTIC, true, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_REJECT,
         "matrix: typed writer, contract decline, spill1 on -> reject");

   plan = admit_template();
   plan.action = R300_R2VB_PLAN_SINGLE;
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_LEGACY_FLOAT, false, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_FITS,
         "matrix: legacy writer, single plan -> fits");
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_TYPED_DIAGNOSTIC, true, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_REJECT,
         "matrix: typed writer, single plan -> reject");
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_FORCED_FLOAT_SPLIT, true, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_SPLIT,
         "matrix: forced writer, single plan, spill1 on -> split");
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_FORCED_FLOAT_SPLIT, false, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_REJECT,
         "matrix: forced writer, single plan, spill1 off -> reject");

   plan.action = R300_R2VB_PLAN_SPLIT;
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_FORCED_FLOAT_SPLIT, true, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_REJECT,
         "matrix: forced writer, mismatched split plan -> reject");

   plan = admit_template();
   plan.action = R300_R2VB_PLAN_REJECT;
   CHECK(r300_r2vb_plan_effective_admission(
            &plan, R300_R2VB_MEMO_WRITER_LEGACY_FLOAT, true, false,
            R300_R2VB_POSITION_CLIP, 1) == R300_R2VB_ADMIT_REJECT,
         "matrix: legacy writer, reject plan -> reject");

   /* Live plans: the two decline classes production shapes produce. */
   nir_shader *over = build_float_producer(90, "typed_route_float_over");
   struct r300_r2vb_producer_plan live;
   bool ran = r300_r2vb_plan_producer(&g_context, over, false,
                                      R300_R2VB_POSITION_CLIP, &live);
   CHECK(ran && live.action == R300_R2VB_PLAN_SPLIT,
         "float over-budget producer plans split");
   if (ran) {
      check_decline(&live, false, live.num_position_inputs,
                    "typed_source_absent", "live float split");
      r300_r2vb_plan_release(&live);
   }

   nir_shader *fits = build_float_producer(8, "typed_route_float_fits");
   ran = r300_r2vb_plan_producer(&g_context, fits, false,
                                 R300_R2VB_POSITION_CLIP, &live);
   CHECK(ran && live.action == R300_R2VB_PLAN_SINGLE,
         "fitting producer plans single");
   if (ran) {
      check_decline(&live, false, live.num_position_inputs, "plan_not_split",
                    "live fitting producer");
      r300_r2vb_plan_release(&live);
   }

   ralloc_free(over);
   ralloc_free(fits);
   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   glsl_type_singleton_decref();
   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
