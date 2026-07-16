/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Calibration for the standing-route R2VB telemetry: known-good and
 * known-bad plans drive r300_r2vb_telemetry_note directly and the counters,
 * retention gate, and deduplication must respond exactly.  The retention
 * gate is fail-closed -- with R300_R2VB_TELEMETRY_RETAIN unset, an
 * over-budget plan writes nothing -- and the gate reads per event, so one
 * process exercises the closed and open states in order.  A structural
 * reject (control flow) carries no budget signal and never retains even
 * with the gate open.
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nir.h"
#include "nir_builder.h"

#include "r300_context.h"
#include "r300_r2vb_plan.h"
#include "r300_r2vb_telemetry.h"
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

static struct r300_screen g_screen;
static struct r300_context g_context;
static struct r300_vertex_shader g_vs;

static void
fake_stack_init(void)
{
   memset(&g_screen, 0, sizeof(g_screen));
   g_screen.caps.has_tcl = false;
   g_screen.caps.is_r400 = false;
   g_screen.caps.is_r500 = false;
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

/* Telemetry retention serializes the bound application VS, so the fake
 * context binds each specimen through the same pointer production uses. */
static void
bind_vs(nir_shader *nir)
{
   memset(&g_vs, 0, sizeof(g_vs));
   g_vs.state.type = PIPE_SHADER_IR_NIR;
   g_vs.state.ir.nir = nir;
   g_context.vs_state.state = &g_vs;
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
   v.pos = nir_load_var(&v.b, in_pos);
   return v;
}

static nir_shader *
end_vs(struct vs_build *v, nir_def *result)
{
   nir_store_var(&v->b, v->out_pos, result, 0xf);
   nir_validate_shader(v->b.shader, "r2vb telemetry VS");
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

/* A branch on a non-constant input survives every folding pass, so the plan
 * rejects with CONTROL_FLOW: the structural class retention must skip. */
static nir_shader *
build_control_flow(void)
{
   struct vs_build v = begin_vs("telemetry_control_flow");
   nir_def *x = nir_channel(&v.b, v.pos, 0);
   nir_def *cond = nir_flt_imm(&v.b, x, 0.5);
   nir_if *nif = nir_push_if(&v.b, cond);
   nir_def *then_v = nir_fmul_imm(&v.b, x, 2.0);
   nir_push_else(&v.b, nif);
   nir_def *else_v = nir_fadd_imm(&v.b, x, 1.0);
   nir_pop_if(&v.b, nif);
   nir_def *c = nir_if_phi(&v.b, then_v, else_v);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

static unsigned
count_dir_files(const char *dir)
{
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   unsigned n = 0;
   struct dirent *e;
   while ((e = readdir(d)))
      if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
         n++;
   closedir(d);
   return n;
}

/* Plan one specimen, assert the expected action (calibrating the specimen
 * itself against the planner), record it in telemetry, release it. */
static void
plan_and_note(const char *label, nir_shader *vs,
              enum r300_r2vb_plan_action expected_action)
{
   char name[96];
   bind_vs(vs);
   struct r300_r2vb_producer_plan plan;
   bool ran = r300_r2vb_plan_producer(&g_context, vs, false,
                                      R300_R2VB_POSITION_CLIP, &plan);
   snprintf(name, sizeof(name), "%s plans %s", label,
            r300_r2vb_plan_action_str(expected_action));
   CHECK(ran && plan.action == expected_action, name);
   if (ran) {
      r300_r2vb_telemetry_note(&g_context, &plan);
      r300_r2vb_plan_release(&plan);
   }
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   fake_stack_init();

   char retain_dir[] = "r2vb-telemetry-retain-XXXXXX";
   CHECK(mkdtemp(retain_dir) != NULL, "retain directory created");

   nir_shader *fits = build_producer(8, "telemetry_fits");
   nir_shader *over = build_producer(90, "telemetry_over");
   nir_shader *cflow = build_control_flow();

   const struct r300_r2vb_telemetry_counters *c =
      r300_r2vb_telemetry_get();
   struct r300_r2vb_telemetry_counters base = *c;

   /* Retention closed: the gate is unset, so even the over-budget plan
    * writes nothing. */
   unsetenv("R300_R2VB_TELEMETRY_RETAIN");
   plan_and_note("fits", fits, R300_R2VB_PLAN_SINGLE);
   plan_and_note("over/closed", over, R300_R2VB_PLAN_SPLIT);
   CHECK(c->by_action[R300_R2VB_PLAN_SINGLE] ==
            base.by_action[R300_R2VB_PLAN_SINGLE] + 1,
         "single counter counts");
   CHECK(c->by_action[R300_R2VB_PLAN_SPLIT] ==
            base.by_action[R300_R2VB_PLAN_SPLIT] + 1,
         "split counter counts");
   CHECK(c->retained == base.retained && count_dir_files(retain_dir) == 0,
         "closed gate retains nothing");

   /* Retention open: the split plan writes one content-hash file, and a
    * repeat of the same shape deduplicates on the existing file. */
   setenv("R300_R2VB_TELEMETRY_RETAIN", retain_dir, 1);
   plan_and_note("over/open", over, R300_R2VB_PLAN_SPLIT);
   CHECK(c->retained == base.retained + 1 &&
            count_dir_files(retain_dir) == 1,
         "open gate retains the over-budget producer once");
   plan_and_note("over/dedup", over, R300_R2VB_PLAN_SPLIT);
   CHECK(c->retained == base.retained + 1 &&
            count_dir_files(retain_dir) == 1,
         "recurring shape deduplicates on the content hash");

   /* Structural reject: control flow carries no budget signal. */
   plan_and_note("control-flow", cflow, R300_R2VB_PLAN_REJECT);
   CHECK(c->by_reason[R300_R2VB_PLAN_CONTROL_FLOW] ==
            base.by_reason[R300_R2VB_PLAN_CONTROL_FLOW] + 1,
         "control-flow reason counts");
   CHECK(count_dir_files(retain_dir) == 1,
         "structural reject retains nothing");
   CHECK(c->retain_failures == base.retain_failures,
         "no retention failures");

   /* Summary is print-gated; ungated it must be silent and safe. */
   r300_r2vb_telemetry_print_summary();

   ralloc_free(fits);
   ralloc_free(over);
   ralloc_free(cflow);
   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   glsl_type_singleton_decref();
   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
