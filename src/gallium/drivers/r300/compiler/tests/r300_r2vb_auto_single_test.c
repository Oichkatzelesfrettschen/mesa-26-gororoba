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
#include "r300_reg.h"
#include "r300_r2vb_plan.h"
#include "r300_screen.h"
#include "radeon_regalloc.h"
#include "util/u_upload_mgr.h"
#include "util/u_inlines.h"

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

/* One accepted layout: check the slot-to-pixel and slot-to-byte mappings
 * agree on the pitch-tight invariant across every slot (exhaustive; the
 * accepted counts stay below 2^16). */
static void
check_layout_accept(uint32_t count, bool grid, uint32_t width,
                    uint32_t height, const char *name)
{
   struct r300_r2vb_slot_layout l;
   bool ok = r300_r2vb_slot_layout_init(count, grid, &l);
   char label[160];
   snprintf(label, sizeof(label), "layout: %s -> %ux%u", name, width, height);
   CHECK(ok && l.width == width && l.height == height &&
            l.pitch_pixels == l.width && l.storage_slots >= count &&
            l.storage_bytes == l.storage_slots * 16u,
         label);
   if (!ok)
      return;
   bool map_ok = true;
   for (uint32_t s = 0; s < count; s++) {
      uint32_t x = s % l.width, y = s / l.width;
      if (x >= l.width || y >= l.height ||
          (uint64_t)y * l.pitch_pixels + x != s)
         map_ok = false;
   }
   snprintf(label, sizeof(label), "layout: %s pitch-tight mapping", name);
   CHECK(map_ok, label);
}

static void
check_layout_reject(uint32_t count, bool grid, const char *name)
{
   struct r300_r2vb_slot_layout l;
   char label[160];
   snprintf(label, sizeof(label), "layout: %s rejects", name);
   CHECK(!r300_r2vb_slot_layout_init(count, grid, &l), label);
}

static void
check_slot_layout(void)
{
   CHECK(!r300_r2vb_slot_grid_gate_value(NULL) &&
            !r300_r2vb_slot_grid_gate_value("") &&
            !r300_r2vb_slot_grid_gate_value("0") &&
            r300_r2vb_slot_grid_gate_value("1"),
         "layout: grid gate exact-value parser");
   check_layout_reject(0, true, "count zero");
   check_layout_accept(1, false, 1, 1, "single slot");
   check_layout_accept(2048, true, 2048, 1, "one grid row");
   check_layout_accept(2049, false, 2049, 1, "one-row past grid width");
   check_layout_accept(4095, false, 4095, 1, "legacy-compatible");
   check_layout_accept(4096, false, 4096, 1, "legacy boundary");
   check_layout_reject(4097, false, "first grid count with gate off");
   check_layout_accept(4097, true, 2048, 3, "first grid count");
   check_layout_accept(8192, true, 2048, 4, "exact four rows");
   check_layout_accept(21516, true, 2048, 11, "census-dominant draw");
   check_layout_accept(65535, true, 2048, 32, "maximum accepted count");
   check_layout_reject(65536, true, "re-ingest index ceiling");
   check_layout_reject(UINT32_MAX, true, "uint32 max");
}

static void
check_producer_streams(void)
{
   struct r300_r2vb_producer_streams s;
   CHECK(!r300_r2vb_slot_fetch_gate_value(NULL) &&
            !r300_r2vb_slot_fetch_gate_value("0") &&
            r300_r2vb_slot_fetch_gate_value("1"),
         "streams: fetch gate exact-value parser");
   CHECK(r300_r2vb_producer_streams_init(64, 16, 16,
                                         PIPE_FORMAT_R32G32B32A32_FLOAT, 3,
                                         &s) &&
            s.num == 2 && s.fetch_dwords == 8 &&
            s.stream[0].offset_bytes == 0 && s.stream[0].stride_dwords == 4 &&
            s.stream[1].offset_bytes == 64 + 16 + 3 * 16 &&
            s.stream[1].stride_dwords == 4,
         "streams: two FP32x4 streams, VAP tuple 8, start-offset law");
   CHECK(r300_r2vb_producer_streams_init(0, 0, 32,
                                         PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                         &s) &&
            s.stream[1].stride_dwords == 8 && s.fetch_dwords == 8,
         "streams: wide interleaved stride fetches four dwords");
   /* The observed dominant workload: packed FLOAT_3 at stride 12, PSC
    * synthesizes W, physical tuple 4 + 3 = 7 dwords. */
   CHECK(r300_r2vb_producer_streams_init(0, 0, 12,
                                         PIPE_FORMAT_R32G32B32_FLOAT, 0,
                                         &s) &&
            s.fetch_dwords == 7 && s.stream[1].size_dwords == 3 &&
            s.stream[1].stride_dwords == 3 &&
            s.stream[1].logical_components == 4,
         "streams: packed FLOAT_3 fetches 7 dwords with logical vec4");
   CHECK(r300_r2vb_producer_streams_init(0, 0, 24,
                                         PIPE_FORMAT_R32G32B32_FLOAT, 0,
                                         &s) &&
            s.fetch_dwords == 7 && s.stream[1].stride_dwords == 6,
         "streams: interleaved FLOAT_3 keeps the 7-dword tuple");
   CHECK(!r300_r2vb_producer_streams_init(0, 0, 8,
                                          PIPE_FORMAT_R32G32B32_FLOAT, 0,
                                          &s),
         "streams: FLOAT_3 sub-record stride rejects");
   CHECK(!r300_r2vb_producer_streams_init(0, 0, 8,
                                          PIPE_FORMAT_R32G32_FLOAT, 0, &s),
         "streams: FLOAT_2 outside the observed families");
   CHECK(!r300_r2vb_producer_streams_init(0, 0, 0,
                                          PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                          &s),
         "streams: zero stride rejects");
   CHECK(!r300_r2vb_producer_streams_init(0, 0, 18,
                                          PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                          &s),
         "streams: non-dword stride rejects");
   CHECK(!r300_r2vb_producer_streams_init(UINT32_MAX, UINT32_MAX, 16,
                                          PIPE_FORMAT_R32G32B32A32_FLOAT,
                                          65535, &s),
         "streams: offset overflow rejects");
}

static void
check_producer_fetch(void)
{
   struct r300_r2vb_producer_streams s;
   struct r300_r2vb_producer_fetch f;
   CHECK(r300_r2vb_producer_streams_init(0, 0, 16,
                                         PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                         &s),
         "fetch: base streams build");
   /* count 4096, tight stride: both BOs sized to the exact last byte. */
   CHECK(r300_r2vb_producer_fetch_init(&s, 4096, 4096 * 16, 4096 * 16, &f) &&
            f.vap_vtx_size == 8 && f.vf_min == 0 && f.vf_max == 4095 &&
            f.slot_required_bytes == 4096 * 16 &&
            f.model_required_bytes == 4096 * 16,
         "fetch: exact last-byte fit, VAP tuple and index bounds");
   CHECK(!r300_r2vb_producer_fetch_init(&s, 4096, 4096 * 16 - 1, 4096 * 16,
                                        &f),
         "fetch: one-byte-short slot BO rejects");
   CHECK(!r300_r2vb_producer_fetch_init(&s, 4096, 4096 * 16, 4096 * 16 - 1,
                                        &f),
         "fetch: one-byte-short model BO rejects");
   CHECK(!r300_r2vb_producer_fetch_init(&s, 0, 16, 16, &f),
         "fetch: zero count rejects");
   CHECK(!r300_r2vb_producer_fetch_init(&s, 65536, UINT64_MAX, UINT64_MAX,
                                        &f),
         "fetch: index-ceiling count rejects");
   /* Wide interleave: extent scales with the true stride. */
   struct r300_r2vb_producer_streams wide;
   CHECK(r300_r2vb_producer_streams_init(0, 0, 64,
                                         PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                         &wide) &&
            r300_r2vb_producer_fetch_init(&wide, 100, 100 * 16,
                                          99 * 64 + 16, &f) &&
            f.model_required_bytes == 99 * 64 + 16,
         "fetch: interleaved stride extent law");
   CHECK(!r300_r2vb_producer_fetch_init(&wide, 100, 100 * 16, 99 * 64 + 15,
                                        &f),
         "fetch: interleaved one-byte-short rejects");
   /* streams_init rejects a sub-record stride at construction; the fetch
    * builder holds the same guard independently against a hand-built
    * descriptor, so neither layer trusts the other. */
   struct r300_r2vb_producer_streams narrow = s;
   narrow.stream[1].stride_dwords = 2;
   CHECK(!r300_r2vb_producer_fetch_init(&narrow, 4, 64, 64, &f),
         "fetch: overlapping sub-record stride rejects");
   /* Forged-descriptor matrix: the emission object re-proves the tuple
    * against records the normal builder would never produce. */
   struct r300_r2vb_producer_streams forged;
   forged = s; forged.stream[0].offset_bytes = 16;
   CHECK(!r300_r2vb_producer_fetch_init(&forged, 4, 1 << 20, 1 << 20, &f),
         "fetch: nonzero slot offset rejects");
   forged = s; forged.stream[0].stride_dwords = 3;
   CHECK(!r300_r2vb_producer_fetch_init(&forged, 4, 1 << 20, 1 << 20, &f),
         "fetch: slot stride under the slot record rejects");
   forged = s; forged.stream[0].logical_components = 3;
   CHECK(!r300_r2vb_producer_fetch_init(&forged, 4, 1 << 20, 1 << 20, &f),
         "fetch: slot logical width rejects");
   forged = s; forged.stream[1].logical_components = 3;
   CHECK(!r300_r2vb_producer_fetch_init(&forged, 4, 1 << 20, 1 << 20, &f),
         "fetch: model logical width rejects");
   forged = s; forged.stream[1].size_dwords = 2;
   CHECK(!r300_r2vb_producer_fetch_init(&forged, 4, 1 << 20, 1 << 20, &f),
         "fetch: unsupported model record width rejects");
   forged = s; forged.fetch_dwords = 6;
   CHECK(!r300_r2vb_producer_fetch_init(&forged, 4, 1 << 20, 1 << 20, &f),
         "fetch: understated fetch total rejects");
   struct r300_r2vb_producer_streams f3;
   CHECK(r300_r2vb_producer_streams_init(0, 0, 12,
                                         PIPE_FORMAT_R32G32B32_FLOAT, 0,
                                         &f3),
         "fetch: float3 streams build for the total check");
   f3.fetch_dwords = 8;
   CHECK(!r300_r2vb_producer_fetch_init(&f3, 4, 1 << 20, 1 << 20, &f),
         "fetch: overstated float3 total rejects");
   /* The packet stride field is 8 bits of dwords. */
   struct r300_r2vb_producer_streams huge;
   CHECK(r300_r2vb_producer_streams_init(0, 0, 1024,
                                         PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                         &huge) &&
            !r300_r2vb_producer_fetch_init(&huge, 2, 32, 4096, &f),
         "fetch: packet stride-field ceiling rejects");
}

static void
check_model_source(void)
{
   CHECK(r300_r2vb_model_source_classify(true, true, true) ==
            R300_R2VB_MODEL_UNSUPPORTED,
         "model: user buffer declines regardless of backing");
   CHECK(r300_r2vb_model_source_classify(false, true, false) ==
            R300_R2VB_MODEL_REAL_BO,
         "model: winsys BO fetches in place");
   CHECK(r300_r2vb_model_source_classify(false, true, true) ==
            R300_R2VB_MODEL_UNSUPPORTED,
         "model: dual backing declines until authority is proven");
   CHECK(R300_R2VB_MODEL_UNSUPPORTED == 0,
         "model: a zeroed record is fail-closed by construction");
   CHECK(r300_r2vb_model_source_classify(false, false, true) ==
            R300_R2VB_MODEL_CPU_SHADOW_UPLOAD,
         "model: CPU shadow uploads");
   CHECK(r300_r2vb_model_source_classify(false, false, false) ==
            R300_R2VB_MODEL_UNSUPPORTED,
         "model: unbacked resource declines");
}

/* Faithful uploader backing: a malloc-backed pipe_screen/pipe_context
 * pair drives the REAL u_upload_mgr suballocation, offset, and owned-
 * reference machinery -- only the storage allocator is substituted, so
 * the transaction under test is the production code path. */
struct fake_buffer {
   struct pipe_resource b;
   uint8_t *data;
};

static bool g_fail_resource_create;
static bool g_fail_buffer_map;

static struct pipe_resource *
fake_resource_create(struct pipe_screen *screen,
                     const struct pipe_resource *templ)
{
   if (g_fail_resource_create)
      return NULL;
   struct fake_buffer *fb = calloc(1, sizeof(*fb));
   fb->b = *templ;
   pipe_reference_init(&fb->b.reference, 1);
   fb->b.screen = screen;
   fb->data = calloc(1, templ->width0);
   return &fb->b;
}

static void
fake_resource_destroy(struct pipe_screen *screen, struct pipe_resource *res)
{
   struct fake_buffer *fb = (struct fake_buffer *)res;
   free(fb->data);
   free(fb);
}

static struct pipe_transfer g_fake_transfer;

static void *
fake_buffer_map(struct pipe_context *ctx, struct pipe_resource *res,
                unsigned level, unsigned usage, const struct pipe_box *box,
                struct pipe_transfer **transfer)
{
   if (g_fail_buffer_map)
      return NULL;
   g_fake_transfer.resource = res;
   g_fake_transfer.box = *box;
   *transfer = &g_fake_transfer;
   return ((struct fake_buffer *)res)->data + box->x;
}

static void
fake_buffer_unmap(struct pipe_context *ctx, struct pipe_transfer *transfer)
{
}

static void
fake_transfer_flush_region(struct pipe_context *ctx,
                           struct pipe_transfer *transfer,
                           const struct pipe_box *box)
{
}

static void
check_upload_integration(void)
{
   g_context.context.screen = &g_screen.screen;
   g_screen.screen.resource_create = fake_resource_create;
   g_screen.screen.resource_destroy = fake_resource_destroy;
   g_context.context.buffer_map = fake_buffer_map;
   g_context.context.buffer_unmap = fake_buffer_unmap;
   g_context.context.transfer_flush_region = fake_transfer_flush_region;
   g_context.context.resource_release = u_default_resource_release;
   g_context.uploader = u_upload_create(&g_context.context, 128 * 1024,
                                        PIPE_BIND_CUSTOM, PIPE_USAGE_STREAM,
                                        0);
   CHECK(g_context.uploader != NULL, "upload: faithful uploader created");

   /* Packed FLOAT_3 CPU shadow: prefix sentinel, records from a nonzero
    * start, suffix sentinel; the copied span must be exactly the selected
    * records. */
   enum { STRIDE = 12, START = 3, COUNT = 5, PRE = 36 };
   struct r300_resource shadow;
   memset(&shadow, 0, sizeof(shadow));
   shadow.b.width0 = PRE + (START + COUNT) * STRIDE + 24;
   pipe_reference_init(&shadow.b.reference, 1);
   uint8_t *bytes = malloc(shadow.b.width0);
   for (unsigned i = 0; i < shadow.b.width0; i++)
      bytes[i] = (uint8_t)(0xA0 ^ i);
   shadow.malloced_buffer = bytes;

   struct pipe_vertex_buffer vb = { .buffer_offset = PRE,
                                    .buffer.resource = &shadow.b };
   struct pipe_vertex_element ve = { .src_offset = 0, .src_stride = STRIDE,
                                     .src_format =
                                        PIPE_FORMAT_R32G32B32_FLOAT };
   struct r300_r2vb_producer_streams s;
   CHECK(r300_r2vb_producer_streams_init(PRE, 0, STRIDE,
                                         PIPE_FORMAT_R32G32B32_FLOAT, START,
                                         &s),
         "upload: packed stream contract builds");
   struct r300_r2vb_model_fetch mf;
   r300_r2vb_model_fetch_init(&mf);
   bool ok = r300_r2vb_materialize_model_fetch_for_test(
      &g_context, &vb, &ve, START, COUNT, &s.stream[1], &mf);
   CHECK(ok && mf.kind == R300_R2VB_MODEL_CPU_SHADOW_UPLOAD &&
            mf.resource && mf.uploaded_bytes == COUNT * STRIDE &&
            mf.gpu_offset % 4 == 0,
         "upload: shadow uploads the exact packed span");
   if (ok) {
      const uint8_t *up = ((struct fake_buffer *)mf.resource)->data;
      CHECK(memcmp(up + mf.gpu_offset, bytes + PRE + START * STRIDE,
                   COUNT * STRIDE) == 0,
            "upload: copied bytes equal the selected source records");
      /* Rebind: the emission fetch validates against the ACTUAL uploaded
       * resource and the uploader's offset, not the application offset. */
      struct r300_r2vb_producer_fetch f;
      CHECK(r300_r2vb_producer_streams_rebind(&s, &mf, 64 * 16, COUNT, &f) &&
               f.streams.stream[1].offset_bytes == mf.gpu_offset &&
               f.vap_vtx_size == 7,
            "upload: rebound stream carries the uploader offset");
      /* Span authority: the uploader is a suballocator, so a count drift
       * past the materialized transaction must reject even though the
       * backing BO has capacity. */
      CHECK(!r300_r2vb_producer_streams_rebind(&s, &mf, 64 * 16, COUNT + 1,
                                               &f),
            "upload: rebind past the materialized count rejects");
      CHECK(!r300_r2vb_producer_streams_rebind(&s, &mf, 64 * 16, COUNT - 1,
                                               &f),
            "upload: rebind under the materialized count rejects");
      struct r300_r2vb_producer_streams forged_stride = s;
      forged_stride.stream[1].stride_dwords = 6;
      CHECK(!r300_r2vb_producer_streams_rebind(&forged_stride, &mf, 64 * 16,
                                               COUNT, &f),
            "upload: forged rebound stride rejects");
      struct r300_r2vb_producer_streams forged_rec = s;
      forged_rec.stream[1].size_dwords = 4;
      forged_rec.fetch_dwords = 8;
      CHECK(!r300_r2vb_producer_streams_rebind(&forged_rec, &mf, 64 * 16,
                                               COUNT, &f),
            "upload: forged rebound record width rejects");
   }
   r300_r2vb_model_fetch_fini(&mf);
   CHECK(mf.kind == R300_R2VB_MODEL_UNSUPPORTED && !mf.resource,
         "upload: fini returns the fail-closed empty record");

   /* Interleaved FLOAT_3 at stride 24: the span preserves inter-record
    * padding because the descriptor keeps stepping by the true stride. */
   struct r300_r2vb_producer_streams si;
   CHECK(r300_r2vb_producer_streams_init(PRE, 0, 24,
                                         PIPE_FORMAT_R32G32B32_FLOAT, 0,
                                         &si),
         "upload: interleaved stream contract builds");
   ve.src_stride = 24;
   struct r300_r2vb_model_fetch mi;
   r300_r2vb_model_fetch_init(&mi);
   ok = r300_r2vb_materialize_model_fetch_for_test(&g_context, &vb, &ve, 0,
                                                   4, &si.stream[1], &mi);
   CHECK(ok && mi.uploaded_bytes == 3 * 24 + 12,
         "upload: interleaved span is (count-1)*stride + record");
   if (ok) {
      const uint8_t *up = ((struct fake_buffer *)mi.resource)->data;
      CHECK(memcmp(up + mi.gpu_offset, bytes + PRE, 3 * 24 + 12) == 0,
            "upload: interleaved padding survives the copy");
   }
   r300_r2vb_model_fetch_fini(&mi);

   /* Direct BO: reference in place at the application offset, refcount
    * restored by fini. */
   struct r300_resource realbo;
   memset(&realbo, 0, sizeof(realbo));
   realbo.b.width0 = 4096;
   pipe_reference_init(&realbo.b.reference, 1);
   realbo.buf = (struct pb_buffer_lean *)&realbo; /* non-NULL marker */
   struct pipe_vertex_buffer vbr = { .buffer_offset = 16,
                                     .buffer.resource = &realbo.b };
   ve.src_stride = STRIDE;
   struct r300_r2vb_producer_streams sr;
   CHECK(r300_r2vb_producer_streams_init(16, 0, STRIDE,
                                         PIPE_FORMAT_R32G32B32_FLOAT, 2,
                                         &sr),
         "upload: direct-BO stream contract builds");
   struct r300_r2vb_model_fetch mr;
   r300_r2vb_model_fetch_init(&mr);
   ok = r300_r2vb_materialize_model_fetch_for_test(&g_context, &vbr, &ve, 2,
                                                   8, &sr.stream[1], &mr);
   CHECK(ok && mr.kind == R300_R2VB_MODEL_REAL_BO &&
            mr.resource == &realbo.b && mr.gpu_offset == 16 + 2 * STRIDE &&
            mr.uploaded_bytes == 0 && realbo.b.reference.count == 2,
         "upload: direct BO references in place at the adjusted offset");
   r300_r2vb_model_fetch_fini(&mr);
   CHECK(realbo.b.reference.count == 1,
         "upload: fini restores the direct-BO reference count");

   /* Offset-coherence negative: a stream whose offset disagrees with the
    * recomputed source offset declines before any upload. */
   struct r300_r2vb_producer_streams sm = s;
   sm.stream[1].offset_bytes += 4;
   struct r300_r2vb_model_fetch mm;
   r300_r2vb_model_fetch_init(&mm);
   CHECK(!r300_r2vb_materialize_model_fetch_for_test(
            &g_context, &vb, &ve, START, COUNT, &sm.stream[1], &mm) &&
            mm.kind == R300_R2VB_MODEL_UNSUPPORTED,
         "upload: offset-coherence mismatch declines fail-closed");

   /* One-byte-short source: the last record's final byte falls outside
    * width0. */
   shadow.b.width0 = PRE + (START + COUNT) * STRIDE - 1;
   struct r300_r2vb_model_fetch ms;
   r300_r2vb_model_fetch_init(&ms);
   CHECK(!r300_r2vb_materialize_model_fetch_for_test(
            &g_context, &vb, &ve, START, COUNT, &s.stream[1], &ms) &&
            ms.kind == R300_R2VB_MODEL_UNSUPPORTED,
         "upload: one-byte-short source declines fail-closed");

   /* Target-scale growth: the dominant draw's 258,192-byte span exceeds
    * the uploader's 128 KiB default, so this row proves the growth
    * allocation and the byte copy at production size, after a small
    * priming upload forces a nonzero suballocation offset for the
    * follow-on transaction. */
   {
      enum { TCOUNT = 21516, TSTRIDE = 12 };
      struct r300_resource big;
      memset(&big, 0, sizeof(big));
      big.b.width0 = TCOUNT * TSTRIDE;
      pipe_reference_init(&big.b.reference, 1);
      uint8_t *bigsrc = malloc(big.b.width0);
      for (unsigned i = 0; i < big.b.width0; i++)
         bigsrc[i] = (uint8_t)(i * 2654435761u >> 24);
      big.malloced_buffer = bigsrc;
      struct pipe_vertex_buffer vbt = { .buffer_offset = 0,
                                        .buffer.resource = &big.b };
      struct pipe_vertex_element vet = { .src_offset = 0,
                                         .src_stride = TSTRIDE,
                                         .src_format =
                                            PIPE_FORMAT_R32G32B32_FLOAT };
      struct r300_r2vb_producer_streams st;
      CHECK(r300_r2vb_producer_streams_init(0, 0, TSTRIDE,
                                            PIPE_FORMAT_R32G32B32_FLOAT, 0,
                                            &st),
            "upload: target-scale stream contract builds");
      struct r300_r2vb_model_fetch mt;
      r300_r2vb_model_fetch_init(&mt);
      bool tok = r300_r2vb_materialize_model_fetch_for_test(
         &g_context, &vbt, &vet, 0, TCOUNT, &st.stream[1], &mt);
      /* Growth row: the 258,192-byte request exceeds the production
       * uploader's 128 KiB default, so the uploader must rotate to a
       * larger backing BO; the offset MAY be zero here -- the nonzero-
       * offset behavior is proven separately below on a pre-sized
       * uploader. */
      CHECK(tok && mt.uploaded_bytes == TCOUNT * TSTRIDE &&
               mt.span_bytes == TCOUNT * TSTRIDE &&
               (uint64_t)mt.gpu_offset + mt.span_bytes <=
                  mt.resource->width0,
            "upload: 258192-byte span grows past the uploader default");
      if (tok) {
         const uint8_t *up = ((struct fake_buffer *)mt.resource)->data;
         CHECK(memcmp(up + mt.gpu_offset, bigsrc, TCOUNT * TSTRIDE) == 0,
               "upload: target-scale bytes copied exactly");
         struct r300_r2vb_producer_fetch ft;
         CHECK(r300_r2vb_producer_streams_rebind(&st, &mt,
                                                 (uint64_t)TCOUNT * 16,
                                                 TCOUNT, &ft) &&
                  ft.vap_vtx_size == 7,
               "upload: target-scale rebound validates inside the span");
      }
      r300_r2vb_model_fetch_fini(&mt);

      /* Nonzero-suballocation row: a 512 KiB uploader holds both the
       * priming allocation and the target span in one backing BO, so the
       * target's descriptor offset must be nonzero and aligned, and the
       * guard bytes on both sides of the span must stay untouched. */
      struct u_upload_mgr *saved = g_context.uploader;
      g_context.uploader = u_upload_create(&g_context.context, 512 * 1024,
                                           PIPE_BIND_CUSTOM,
                                           PIPE_USAGE_STREAM, 0);
      unsigned prime_off = 0;
      struct pipe_resource *prime = NULL;
      uint8_t prime_src[64] = { 0x5A };
      u_upload_data_ref(g_context.uploader, 0, sizeof(prime_src), 4,
                        prime_src, &prime_off, &prime);
      struct r300_r2vb_model_fetch mo;
      r300_r2vb_model_fetch_init(&mo);
      bool ook = r300_r2vb_materialize_model_fetch_for_test(
         &g_context, &vbt, &vet, 0, TCOUNT, &st.stream[1], &mo);
      CHECK(ook && mo.gpu_offset > 0 && mo.gpu_offset % 4 == 0 &&
               mo.resource == prime,
            "upload: primed uploader yields a nonzero aligned offset");
      if (ook) {
         const uint8_t *up = ((struct fake_buffer *)mo.resource)->data;
         CHECK(memcmp(up + mo.gpu_offset, bigsrc, TCOUNT * TSTRIDE) == 0,
               "upload: nonzero-offset bytes copied exactly");
         CHECK(up[prime_off] == 0x5A,
               "upload: guard bytes before the span stay untouched");
         struct r300_r2vb_producer_fetch fo;
         CHECK(r300_r2vb_producer_streams_rebind(&st, &mo,
                                                 (uint64_t)TCOUNT * 16,
                                                 TCOUNT, &fo) &&
                  fo.streams.stream[1].offset_bytes == mo.gpu_offset,
               "upload: rebound descriptor carries the nonzero offset");
      }
      r300_r2vb_model_fetch_fini(&mo);
      pipe_resource_reference(&prime, NULL);
      u_upload_destroy(g_context.uploader);
      g_context.uploader = saved;

      /* Failure injection: each fallible operation fails once, the
       * record stays fail-closed empty, and a later upload succeeds. */
      struct r300_r2vb_model_fetch mfail;
      g_fail_resource_create = true;
      r300_r2vb_model_fetch_init(&mfail);
      CHECK(!r300_r2vb_materialize_model_fetch_for_test(
               &g_context, &vbt, &vet, 0, TCOUNT, &st.stream[1], &mfail) &&
               mfail.kind == R300_R2VB_MODEL_UNSUPPORTED && !mfail.resource,
            "upload: allocation failure leaves the record fail-closed");
      g_fail_resource_create = false;
      g_fail_buffer_map = true;
      r300_r2vb_model_fetch_init(&mfail);
      CHECK(!r300_r2vb_materialize_model_fetch_for_test(
               &g_context, &vbt, &vet, 0, TCOUNT, &st.stream[1], &mfail) &&
               mfail.kind == R300_R2VB_MODEL_UNSUPPORTED && !mfail.resource,
            "upload: map failure leaves the record fail-closed");
      g_fail_buffer_map = false;
      r300_r2vb_model_fetch_init(&mfail);
      CHECK(r300_r2vb_materialize_model_fetch_for_test(
               &g_context, &vbt, &vet, 0, TCOUNT, &st.stream[1], &mfail),
            "upload: the uploader recovers after injected failures");
      r300_r2vb_model_fetch_fini(&mfail);

      /* Repeated lifetime: a thousand materialize/rebind/fini cycles
       * exercise uploader rotation, old-buffer release, and reference
       * balancing; leaks surface under the ASan gate. */
      bool loop_ok = true;
      for (unsigned it = 0; it < 1000 && loop_ok; it++) {
         struct r300_r2vb_model_fetch ml;
         r300_r2vb_model_fetch_init(&ml);
         struct r300_r2vb_producer_fetch fl;
         loop_ok = r300_r2vb_materialize_model_fetch_for_test(
                      &g_context, &vbt, &vet, 0, TCOUNT, &st.stream[1],
                      &ml) &&
                   r300_r2vb_producer_streams_rebind(
                      &st, &ml, (uint64_t)TCOUNT * 16, TCOUNT, &fl);
         r300_r2vb_model_fetch_fini(&ml);
      }
      CHECK(loop_ok, "upload: 1000 materialize/rebind/fini cycles hold");
      free(bigsrc);
   }
   free(bytes);
   u_upload_destroy(g_context.uploader);
   g_context.uploader = NULL;
}

static void
check_position_mapping_and_interface(void)
{
   /* Position-input mapping: one plan input reads location 0 = velem[0];
    * the classifier proves the element, binding, and format identity. */
   CHECK(r300_r2vb_position_input_mapping_ok(
            1, 2, 0, 1, true, PIPE_FORMAT_R32G32B32_FLOAT),
         "mapping: single input on bound float3 element admits");
   CHECK(r300_r2vb_position_input_mapping_ok(
            1, 1, 0, 1, true, PIPE_FORMAT_R32G32B32A32_FLOAT),
         "mapping: float4 element admits");
   CHECK(!r300_r2vb_position_input_mapping_ok(
            2, 2, 0, 1, true, PIPE_FORMAT_R32G32B32_FLOAT),
         "mapping: multi-input plan declines");
   CHECK(!r300_r2vb_position_input_mapping_ok(
            1, 0, 0, 1, true, PIPE_FORMAT_R32G32B32_FLOAT),
         "mapping: missing element declines");
   CHECK(!r300_r2vb_position_input_mapping_ok(
            1, 1, 1, 1, true, PIPE_FORMAT_R32G32B32_FLOAT),
         "mapping: out-of-range buffer binding declines");
   CHECK(!r300_r2vb_position_input_mapping_ok(
            1, 1, 0, 1, false, PIPE_FORMAT_R32G32B32_FLOAT),
         "mapping: unbound buffer declines");
   CHECK(!r300_r2vb_position_input_mapping_ok(
            1, 1, 0, 1, true, PIPE_FORMAT_R32G32_FLOAT),
         "mapping: format outside the admitted families declines");

   /* Interface builder: PSC words from the shared translators, both
    * elements in the first register pair, LAST_VEC on the model, unused
    * registers zeroed, VAP_VTX_SIZE carried from the fetch object. */
   struct r300_r2vb_producer_streams st;
   struct r300_r2vb_producer_fetch ft;
   struct r300_r2vb_producer_interface it;
   CHECK(r300_r2vb_producer_streams_init(0, 0, 12,
                                         PIPE_FORMAT_R32G32B32_FLOAT, 0,
                                         &st) &&
            r300_r2vb_producer_fetch_init(&st, 4, 64, 48, &ft) &&
            r300_r2vb_producer_interface_init(&ft, 0, 6, &it),
         "interface: float3 pair builds");
   CHECK(it.vap_vtx_size == 7, "interface: physical tuple carries 7");
   CHECK((it.prog_stream_cntl[0] & R300_LAST_VEC) == 0 &&
            (it.prog_stream_cntl[0] & ((uint32_t)R300_LAST_VEC << 16)) != 0,
         "interface: LAST_VEC sits on the model element only");
   CHECK(((it.prog_stream_cntl[0] >> R300_DST_VEC_LOC_SHIFT) & 0x1f) == 0 &&
            ((it.prog_stream_cntl[0] >>
              (16 + R300_DST_VEC_LOC_SHIFT)) & 0x1f) == 6,
         "interface: destination vector locations encode");
   bool tail_zero = true;
   for (unsigned i = 1; i < 8; i++)
      if (it.prog_stream_cntl[i] || it.prog_stream_cntl_ext[i])
         tail_zero = false;
   CHECK(tail_zero, "interface: stream registers 1..7 are zeroed");
   /* The FLOAT_3 swizzle sources W from the constant-one select; pin it
    * against the FLOAT_4 identity swizzle rather than a hand-coded
    * constant so the shared translator stays the single authority. */
   CHECK((it.prog_stream_cntl_ext[0] >> 16) !=
            (it.prog_stream_cntl_ext[0] & 0xffff),
         "interface: float3 swizzle differs from the float4 identity");
   CHECK(!r300_r2vb_producer_interface_init(&ft, 3, 3, &it),
         "interface: aliased destination vectors reject");
   CHECK(!r300_r2vb_producer_interface_init(&ft, 32, 1, &it),
         "interface: destination location past the 5-bit field rejects");
   struct r300_r2vb_producer_fetch forged = ft;
   forged.streams.stream[1].size_dwords = 2;
   CHECK(!r300_r2vb_producer_interface_init(&forged, 0, 6, &it),
         "interface: unsupported model record rejects");
}

int
main(void)
{
   fake_stack_init();

   printf("position mapping + producer interface:\n");
   check_position_mapping_and_interface();

   printf("model source classification:\n");
   check_model_source();
   printf("upload integration (faithful allocator, real u_upload_mgr):\n");
   check_upload_integration();
   printf("producer fetch streams:\n");
   check_producer_streams();
   printf("producer fetch extent:\n");
   check_producer_fetch();
   printf("slot-grid layout:\n");
   check_slot_layout();
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
