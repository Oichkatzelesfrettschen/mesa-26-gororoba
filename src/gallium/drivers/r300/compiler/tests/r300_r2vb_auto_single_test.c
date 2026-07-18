/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Calibration for the AUTO_SINGLE canary policy: the gate parser admits
 * only "1", the floor parser admits only a bare positive decimal uint32,
 * and the pure admission policy composes route-support shape first, then
 * both delivery cells of the plain route (READY SINGLE untyped one-input
 * plans for cv=0 clip and cv=0 window), then the vertex floor.  Synthetic
 * plans drive every decline arm; a live float producer calibrates the
 * execute row and the floor negative on real planner output.
 */

#include <stdbool.h>
#include <stdint.h>
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

static void
check_gate_parser(void)
{
   CHECK(!r300_r2vb_auto_single_gate_value(NULL), "gate: unset stays closed");
   CHECK(!r300_r2vb_auto_single_gate_value(""), "gate: empty stays closed");
   CHECK(!r300_r2vb_auto_single_gate_value("0"), "gate: zero stays closed");
   CHECK(!r300_r2vb_auto_single_gate_value("1 "),
         "gate: trailing space stays closed");
   CHECK(!r300_r2vb_auto_single_gate_value("true"),
         "gate: word value stays closed");
   CHECK(r300_r2vb_auto_single_gate_value("1"), "gate: exact 1 opens");
}

static void
check_floor_parser(void)
{
   uint32_t f = 0;
   CHECK(!r300_r2vb_auto_single_floor_value(NULL, &f),
         "floor: unset stays closed");
   CHECK(!r300_r2vb_auto_single_floor_value("", &f),
         "floor: empty stays closed");
   CHECK(!r300_r2vb_auto_single_floor_value("0", &f),
         "floor: zero stays closed");
   CHECK(!r300_r2vb_auto_single_floor_value("+1", &f),
         "floor: sign stays closed");
   CHECK(!r300_r2vb_auto_single_floor_value("-1", &f),
         "floor: negative stays closed");
   CHECK(!r300_r2vb_auto_single_floor_value(" 16384", &f),
         "floor: leading space stays closed");
   CHECK(!r300_r2vb_auto_single_floor_value("16384x", &f),
         "floor: trailing character stays closed");
   CHECK(!r300_r2vb_auto_single_floor_value("4294967296", &f),
         "floor: uint32 overflow stays closed");
   CHECK(r300_r2vb_auto_single_floor_value("1", &f) && f == 1,
         "floor: 1 parses");
   CHECK(r300_r2vb_auto_single_floor_value("16384", &f) && f == 16384,
         "floor: 16384 parses");
   CHECK(r300_r2vb_auto_single_floor_value("4294967295", &f) &&
            f == UINT32_MAX,
         "floor: uint32 max parses");
}

/* Synthetic delivery-cell plan the decline rows perturb: READY SINGLE
 * untyped one-input, the shape the policy admits. */
static struct r300_r2vb_producer_plan
single_cell(enum r300_r2vb_position_space space)
{
   struct r300_r2vb_producer_plan plan;
   memset(&plan, 0, sizeof(plan));
   plan.status = R300_R2VB_PLAN_READY;
   plan.action = R300_R2VB_PLAN_SINGLE;
   plan.key.space = space;
   plan.num_position_inputs = 1;
   return plan;
}

/* The dominant-producer draw shape from the weighted census: plain
 * TRIANGLES, 21516 vertices, non-indexed, single instance. */
static struct r300_r2vb_auto_single_draw
census_draw(void)
{
   struct r300_r2vb_auto_single_draw d;
   memset(&d, 0, sizeof(d));
   d.mode = MESA_PRIM_TRIANGLES;
   d.count = 21516;
   d.instance_count = 1;
   return d;
}

static void
check_policy(const struct r300_r2vb_producer_plan *cp,
             const struct r300_r2vb_producer_plan *wp,
             const struct r300_r2vb_auto_single_draw *d, uint32_t floor,
             enum r300_r2vb_auto_single_reason expect, const char *name)
{
   enum r300_r2vb_auto_single_reason got =
      r300_r2vb_auto_single_policy(cp, wp, d, floor);
   char label[160];
   snprintf(label, sizeof(label), "policy: %s -> %s", name,
            r300_r2vb_auto_single_reason_str(expect));
   CHECK(got == expect, label);
}

static void
check_policy_matrix(void)
{
   struct r300_r2vb_producer_plan cp = single_cell(R300_R2VB_POSITION_CLIP);
   struct r300_r2vb_producer_plan wp = single_cell(R300_R2VB_POSITION_WINDOW);
   struct r300_r2vb_auto_single_draw d = census_draw();

   /* The census-dominant 21,516-vertex draw sits past the producer's
    * single-row slot ceiling: the policy declines it truthfully instead of
    * letting the producer fall back after an "execute" token (RS482
    * observation, glmark2 build scene). */
   check_policy(&cp, &wp, &d, 16384, R300_R2VB_AUTO_SINGLE_COUNT_CEILING,
                "census draw exceeds the producer slot ceiling");

   struct r300_r2vb_auto_single_draw fits = census_draw();
   fits.count = 4095;
   check_policy(&cp, &wp, &fits, 1024, R300_R2VB_AUTO_SINGLE_OK,
                "ceiling-sized draw above a low floor");
   check_policy(&cp, &wp, &fits, 8192,
                R300_R2VB_AUTO_SINGLE_BELOW_VERTEX_FLOOR,
                "ceiling-sized draw under a higher floor");

   struct r300_r2vb_auto_single_draw quad = census_draw();
   quad.mode = MESA_PRIM_TRIANGLE_FAN;
   quad.count = 4;
   check_policy(&cp, &wp, &quad, 16384,
                R300_R2VB_AUTO_SINGLE_UNSUPPORTED_PRIMITIVE,
                "four-vertex fan quad");

   struct r300_r2vb_auto_single_draw points = census_draw();
   points.mode = MESA_PRIM_POINTS;
   points.count = 65535;
   check_policy(&cp, &wp, &points, 16384,
                R300_R2VB_AUTO_SINGLE_UNSUPPORTED_PRIMITIVE,
                "large POINTS draw");

   struct r300_r2vb_auto_single_draw ragged = census_draw();
   ragged.count = 21517;
   check_policy(&cp, &wp, &ragged, 16384,
                R300_R2VB_AUTO_SINGLE_UNSUPPORTED_PRIMITIVE,
                "partial-triangle count");

   struct r300_r2vb_auto_single_draw big = census_draw();
   big.count = 66000;
   check_policy(&cp, &wp, &big, 16384, R300_R2VB_AUTO_SINGLE_COUNT_CEILING,
                "count past the 16-bit re-ingest ceiling");

   struct r300_r2vb_auto_single_draw indexed = fits;
   indexed.index_size = 2;
   check_policy(&cp, &wp, &indexed, 1024, R300_R2VB_AUTO_SINGLE_INDEXED,
                "indexed draw");

   struct r300_r2vb_auto_single_draw instanced = fits;
   instanced.instance_count = 2;
   check_policy(&cp, &wp, &instanced, 1024,
                R300_R2VB_AUTO_SINGLE_INSTANCED, "instanced draw");

   struct r300_r2vb_auto_single_draw face = fits;
   face.fs_reads_face = true;
   check_policy(&cp, &wp, &face, 1024, R300_R2VB_AUTO_SINGLE_FRONTFACE,
                "gl_FrontFacing consumer");

   struct r300_r2vb_auto_single_draw planes = fits;
   planes.clip_planes_enabled = true;
   check_policy(&cp, &wp, &planes, 1024, R300_R2VB_AUTO_SINGLE_CLIP_PLANES,
                "user clip planes");

   struct r300_r2vb_auto_single_draw ext = fits;
   ext.fs_reads_external_constants = true;
   check_policy(&cp, &wp, &ext, 1024,
                R300_R2VB_AUTO_SINGLE_FS_EXTERNAL_CONSTANTS,
                "FS external constants");

   check_policy(NULL, &wp, &fits, 1024, R300_R2VB_AUTO_SINGLE_PLAN_NOT_READY,
                "missing clip plan");

   struct r300_r2vb_producer_plan split = cp;
   split.action = R300_R2VB_PLAN_SPLIT;
   check_policy(&split, &wp, &fits, 1024,
                R300_R2VB_AUTO_SINGLE_PLAN_NOT_SINGLE, "SPLIT clip plan");

   struct r300_r2vb_producer_plan typed = cp;
   typed.has_typed_source = true;
   typed.typed_source_class = R300_R2VB_TYPED_SOURCE_SINT;
   check_policy(&typed, &wp, &fits, 1024, R300_R2VB_AUTO_SINGLE_TYPED_SOURCE,
                "typed clip plan");

   struct r300_r2vb_producer_plan wide = cp;
   wide.num_position_inputs = 2;
   check_policy(&wide, &wp, &fits, 1024, R300_R2VB_AUTO_SINGLE_INPUT_SHAPE,
                "two-input clip plan");

   /* Every window-cell failure reads as the delivery-cell decline: the clip
    * cell classified, but the accept action cannot deliver. */
   struct r300_r2vb_producer_plan wrej = wp;
   wrej.action = R300_R2VB_PLAN_REJECT;
   wrej.status = R300_R2VB_PLAN_SEMANTIC_REJECT;
   check_policy(&cp, &wrej, &fits, 1024, R300_R2VB_AUTO_SINGLE_DELIVERY_CELL,
                "rejected window cell");
   check_policy(&cp, NULL, &fits, 1024, R300_R2VB_AUTO_SINGLE_DELIVERY_CELL,
                "missing window plan");
}

/* Fitting float producer of the retained-corpus shape: one position input,
 * a short multiply-add chain, gl_Position out. */
static nir_shader *
build_fitting_producer(void)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, g_screen.screen.nir_options[MESA_SHADER_VERTEX],
      "auto_single_fitting");
   nir_variable *in_pos = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "in_pos");
   in_pos->data.location = VERT_ATTRIB_GENERIC0;
   in_pos->data.driver_location = 0;
   nir_variable *out_pos = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec4_type(), "gl_Position");
   out_pos->data.location = VARYING_SLOT_POS;
   out_pos->data.driver_location = 0;
   nir_def *pos = nir_load_var(&b, in_pos);
   nir_def *scaled =
      nir_fadd(&b, nir_fmul(&b, pos, nir_imm_vec4(&b, 1.25f, 0.75f, 0.5f, 1.0f)),
               nir_imm_vec4(&b, 0.0625f, 0.03125f, 0.0f, 0.0f));
   nir_store_var(&b, out_pos, scaled, 0xf);
   nir_validate_shader(b.shader, "auto single fitting VS");
   return b.shader;
}

static void
check_live_plans(void)
{
   nir_shader *vs = build_fitting_producer();
   struct r300_r2vb_producer_plan cp, wp;
   bool cok = r300_r2vb_plan_producer(&g_context, vs, false,
                                      R300_R2VB_POSITION_CLIP, &cp);
   bool wok = r300_r2vb_plan_producer(&g_context, vs, false,
                                      R300_R2VB_POSITION_WINDOW, &wp);
   CHECK(cok && cp.status == R300_R2VB_PLAN_READY &&
            cp.action == R300_R2VB_PLAN_SINGLE,
         "live: clip plan is READY SINGLE");
   CHECK(wok && wp.status == R300_R2VB_PLAN_READY &&
            wp.action == R300_R2VB_PLAN_SINGLE,
         "live: window plan is READY SINGLE");
   if (cok && wok) {
      struct r300_r2vb_auto_single_draw d = census_draw();
      d.count = 4095;
      check_policy(&cp, &wp, &d, 1024, R300_R2VB_AUTO_SINGLE_OK,
                   "live plans execute a ceiling-sized draw");
      d.count = 300;
      check_policy(&cp, &wp, &d, 1024,
                   R300_R2VB_AUTO_SINGLE_BELOW_VERTEX_FLOOR,
                   "live plans hold the floor on a small draw");
   }
   if (cok)
      r300_r2vb_plan_release(&cp);
   if (wok)
      r300_r2vb_plan_release(&wp);
   ralloc_free(vs);
}

int
main(void)
{
   fake_stack_init();

   printf("auto-single gate parser:\n");
   check_gate_parser();
   printf("auto-single floor parser:\n");
   check_floor_parser();
   printf("auto-single policy matrix:\n");
   check_policy_matrix();
   printf("auto-single live plans:\n");
   check_live_plans();

   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
