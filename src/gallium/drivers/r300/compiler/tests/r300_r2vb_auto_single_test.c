/*
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

#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"
#include "r300_context.h"
#include "r300_emit.h"
#include "r300_r2vb.h"
#include "r300_r2vb_plan.h"
#include "r300_reg.h"
#include "r300_screen.h"
#include "r300_shader_semantics.h"
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

static void
check_mode_compatibility(void)
{
   struct r300_r2vb_auto_single_mode_values values;
   memset(&values, 0, sizeof(values));
   CHECK(r300_r2vb_auto_single_mode_values_compatible(&values),
         "mode: an uncontaminated AUTO path is compatible");
   CHECK(!r300_r2vb_auto_single_mode_values_compatible(NULL),
         "mode: an unchecked mode record declines");

#define CHECK_MODE_CONFLICT(member, value, name)                           \
   do {                                                                    \
      memset(&values, 0, sizeof(values));                                  \
      values.member = value;                                               \
      CHECK(!r300_r2vb_auto_single_mode_values_compatible(&values), name); \
   } while (0)

   CHECK_MODE_CONFLICT(diagnostic, "", "mode: diagnostic presence conflicts");
   CHECK_MODE_CONFLICT(barrier, "", "mode: barrier override presence conflicts");
   CHECK_MODE_CONFLICT(inspect, "", "mode: no-submit inspection conflicts");
   CHECK_MODE_CONFLICT(clip_classify, "1",
                       "mode: classify-only route conflicts");
   CHECK_MODE_CONFLICT(clip_edge, "1",
                       "mode: CPU edge reconstruction conflicts");
   CHECK_MODE_CONFLICT(budget_escape, "spill1",
                       "mode: split budget escape conflicts");
   CHECK_MODE_CONFLICT(typed_split, "1", "mode: typed split conflicts");
   CHECK_MODE_CONFLICT(force_split, "1", "mode: forced split conflicts");
   CHECK_MODE_CONFLICT(varying, "1",
                       "mode: computed-varying diagnostic conflicts");
   CHECK_MODE_CONFLICT(delivery_capture, "1",
                       "mode: discarded delivery capture conflicts");

#undef CHECK_MODE_CONFLICT
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
   plan.position_source.valid = true;
   plan.position_source.app_driver_location = 0;
   plan.position_source.location_rank = 0;
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
   d.slot_layout_available = false;
   d.bo_draw_mode_compatible = false;
   d.bo_delivery_ordering_compatible = false;
   d.route_mode_compatible = true;
   d.delivery_stream_status = R300_R2VB_DELIVERY_STREAM_OK;
   d.producer_input_status = R300_R2VB_PRODUCER_INPUT_OK;
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

   /* The census-dominant 21,516-vertex draw requires the selected grid
    * representation, the BO-fetch producer, and explicit raw-submit consent.
    * Each missing contract declines before the decision token can say execute. */
   check_policy(&cp, &wp, &d, 16384, R300_R2VB_AUTO_SINGLE_SLOT_LAYOUT,
                "census draw has no admitted slot layout");
   d.slot_layout_available = true;
   d.uses_grid_layout = true;
   check_policy(&cp, &wp, &d, 16384, R300_R2VB_AUTO_SINGLE_SLOT_FETCH,
                "census grid has no BO-fetch producer");
   d.slot_fetch_enabled = true;
   check_policy(&cp, &wp, &d, 16384, R300_R2VB_AUTO_SINGLE_RAW_SUBMIT,
                "census grid has no raw-submit consent");
   d.raw_submit_accepted = true;
   check_policy(&cp, &wp, &d, 16384, R300_R2VB_AUTO_SINGLE_BO_MODE,
                "census grid conflicts with a diagnostic BO mode");
   d.bo_draw_mode_compatible = true;
   check_policy(&cp, &wp, &d, 16384, R300_R2VB_AUTO_SINGLE_ORDERING_MODE,
                "census grid conflicts with single-CS ordering");
   d.bo_delivery_ordering_compatible = true;
   check_policy(&cp, &wp, &d, 16384, R300_R2VB_AUTO_SINGLE_OK,
                "census grid carries every producer gate");

   static const struct {
      enum r300_r2vb_verdict verdict;
      enum r300_r2vb_auto_single_reason reason;
   } route_reasons[] = {
      {R2VB_ROUTE_PASSTHROUGH,
       R300_R2VB_AUTO_SINGLE_PASSTHROUGH_ROUTE},
      {R2VB_ROUTE_CANDIDATE, R300_R2VB_AUTO_SINGLE_OK},
      {R2VB_REJECT_HW_TCL, R300_R2VB_AUTO_SINGLE_HARDWARE_TCL},
      {R2VB_REJECT_INDEXED, R300_R2VB_AUTO_SINGLE_INDEXED},
      {R2VB_REJECT_INSTANCED, R300_R2VB_AUTO_SINGLE_INSTANCED},
      {R2VB_REJECT_COUNT, R300_R2VB_AUTO_SINGLE_COUNT_CEILING},
      {R2VB_REJECT_PRIM, R300_R2VB_AUTO_SINGLE_UNSUPPORTED_PRIMITIVE},
      {R2VB_REJECT_FRONTFACE, R300_R2VB_AUTO_SINGLE_FRONTFACE},
   };
   for (unsigned i = 0; i < ARRAY_SIZE(route_reasons); i++) {
      CHECK(r300_r2vb_auto_single_route_reason(
               route_reasons[i].verdict) == route_reasons[i].reason,
            "policy: classifier verdict has one AUTO_SINGLE reason");
   }
   struct r300_r2vb_auto_single_draw passthrough_route = d;
   passthrough_route.route_reason =
      R300_R2VB_AUTO_SINGLE_PASSTHROUGH_ROUTE;
   check_policy(&cp, &wp, &passthrough_route, 16384,
                R300_R2VB_AUTO_SINGLE_PASSTHROUGH_ROUTE,
                "passthrough path declines before producer inspection");

   struct r300_r2vb_auto_single_draw fits = census_draw();
   fits.count = 4095;
   fits.slot_layout_available = true;
   fits.slot_fetch_enabled = true;
   fits.raw_submit_accepted = true;
   fits.bo_draw_mode_compatible = true;
   fits.bo_delivery_ordering_compatible = true;
   check_policy(&cp, &wp, &fits, 1024, R300_R2VB_AUTO_SINGLE_OK,
                "one-row draw uses the fixed-size BO producer");
   check_policy(&cp, &wp, &fits, 8192,
                R300_R2VB_AUTO_SINGLE_BELOW_VERTEX_FLOOR,
                "ceiling-sized draw under a higher floor");

   struct r300_r2vb_auto_single_draw quad = census_draw();
   quad.mode = MESA_PRIM_TRIANGLE_FAN;
   quad.count = 4;
   check_policy(&cp, &wp, &quad, 16384,
                R300_R2VB_AUTO_SINGLE_UNSUPPORTED_PRIMITIVE,
                "four-vertex fan quad");

   /* POINTS admit under the fixed-size point contract; each requested
    * point semantic the re-ingest does not transport names its own
    * decline. */
   struct r300_r2vb_auto_single_draw points = fits;
   points.mode = MESA_PRIM_POINTS;
   points.count = 4093; /* points carry no whole-triangle count rule */
   check_policy(&cp, &wp, &points, 1024, R300_R2VB_AUTO_SINGLE_OK,
                "fixed-size POINTS draw");
   struct r300_r2vb_auto_single_draw psiz_writer = points;
   psiz_writer.vs_writes_point_size = true;
   check_policy(&cp, &wp, &psiz_writer, 1024,
                R300_R2VB_AUTO_SINGLE_POINT_SIZE_WRITER,
                "POINTS with a VS PSIZ writer");
   struct r300_r2vb_auto_single_draw sprite = points;
   sprite.sprite_coord_requested = true;
   check_policy(&cp, &wp, &sprite, 1024,
                R300_R2VB_AUTO_SINGLE_POINT_COORD_STATE,
                "POINTS with sprite-coord state");
   struct r300_r2vb_auto_single_draw pv_size = points;
   pv_size.point_size_per_vertex = true;
   check_policy(&cp, &wp, &pv_size, 1024,
                R300_R2VB_AUTO_SINGLE_POINT_VERTEX_SIZE,
                "POINTS with per-vertex point size");

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

   struct r300_r2vb_auto_single_draw query = fits;
   query.query_active = true;
   check_policy(&cp, &wp, &query, 1024,
                R300_R2VB_AUTO_SINGLE_QUERY_ACTIVE,
                "active application query");

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

   static const struct {
      enum r300_r2vb_producer_input_status status;
      enum r300_r2vb_auto_single_reason reason;
      const char *name;
   } input_declines[] = {
      {R300_R2VB_PRODUCER_INPUT_UNCHECKED,
       R300_R2VB_AUTO_SINGLE_INPUT_UNCHECKED,
       "input admission was never evaluated"},
      {R300_R2VB_PRODUCER_INPUT_CONSTANTS,
       R300_R2VB_AUTO_SINGLE_INPUT_CONSTANTS, "missing transform constants"},
      {R300_R2VB_PRODUCER_INPUT_CONSTANT_SOURCE,
       R300_R2VB_AUTO_SINGLE_INPUT_CONSTANT_SOURCE,
       "constant source is outside mirrored UBO0"},
      {R300_R2VB_PRODUCER_INPUT_POSITION_SOURCE,
       R300_R2VB_AUTO_SINGLE_INPUT_POSITION_SOURCE,
       "unresolved position source"},
      {R300_R2VB_PRODUCER_INPUT_INSTANCE_RATE,
       R300_R2VB_AUTO_SINGLE_INPUT_INSTANCE_RATE,
       "per-instance position source"},
      {R300_R2VB_PRODUCER_INPUT_BUFFER_BINDING,
       R300_R2VB_AUTO_SINGLE_INPUT_BUFFER_BINDING,
       "missing vertex buffer binding"},
      {R300_R2VB_PRODUCER_INPUT_SOURCE_CLASS,
       R300_R2VB_AUTO_SINGLE_INPUT_SOURCE_CLASS,
       "ambiguous model source authority"},
      {R300_R2VB_PRODUCER_INPUT_CPU_ACCESS,
       R300_R2VB_AUTO_SINGLE_INPUT_CPU_ACCESS,
       "model source lacks CPU access"},
      {R300_R2VB_PRODUCER_INPUT_UPLOADER,
       R300_R2VB_AUTO_SINGLE_INPUT_UPLOADER,
       "CPU-shadow source has no uploader"},
      {R300_R2VB_PRODUCER_INPUT_FORMAT,
       R300_R2VB_AUTO_SINGLE_INPUT_FORMAT, "unsupported model format"},
      {R300_R2VB_PRODUCER_INPUT_STRIDE,
       R300_R2VB_AUTO_SINGLE_INPUT_STRIDE, "invalid model stride"},
      {R300_R2VB_PRODUCER_INPUT_SPAN,
       R300_R2VB_AUTO_SINGLE_INPUT_SPAN, "model span exceeds the resource"},
   };
   for (unsigned i = 0; i < ARRAY_SIZE(input_declines); i++) {
      struct r300_r2vb_auto_single_draw bad_input = fits;
      bad_input.producer_input_status = input_declines[i].status;
      check_policy(&cp, &wp, &bad_input, 1024, input_declines[i].reason,
                   input_declines[i].name);
   }

   struct r300_r2vb_auto_single_draw mode_conflict = fits;
   mode_conflict.route_mode_compatible = false;
   check_policy(&cp, &wp, &mode_conflict, 1024,
                R300_R2VB_AUTO_SINGLE_MODE_CONFLICT,
                "diagnostic or split mode conflicts with the canary");

   static const struct {
      enum r300_r2vb_delivery_stream_status status;
      enum r300_r2vb_auto_single_reason reason;
      const char *name;
   } output_declines[] = {
      {R300_R2VB_DELIVERY_STREAM_UNCHECKED,
       R300_R2VB_AUTO_SINGLE_OUTPUT_STREAMS, "unchecked outputs"},
      {R300_R2VB_DELIVERY_STREAM_LAYOUT,
       R300_R2VB_AUTO_SINGLE_OUTPUT_STREAMS, "unmapped output layout"},
      {R300_R2VB_DELIVERY_STREAM_INSTANCE_RATE,
       R300_R2VB_AUTO_SINGLE_INPUT_INSTANCE_RATE,
       "per-instance passthrough output"},
      {R300_R2VB_DELIVERY_STREAM_FORMAT,
       R300_R2VB_AUTO_SINGLE_INPUT_FORMAT,
       "unconverted passthrough format"},
      {R300_R2VB_DELIVERY_STREAM_STRIDE,
       R300_R2VB_AUTO_SINGLE_INPUT_STRIDE,
       "passthrough stride cannot cover its record"},
      {R300_R2VB_DELIVERY_STREAM_BUFFER_BINDING,
       R300_R2VB_AUTO_SINGLE_OUTPUT_STREAMS,
       "passthrough buffer is unbound"},
      {R300_R2VB_DELIVERY_STREAM_SOURCE_CLASS,
       R300_R2VB_AUTO_SINGLE_OUTPUT_STREAMS,
       "passthrough source has no extent authority"},
      {R300_R2VB_DELIVERY_STREAM_SPAN,
       R300_R2VB_AUTO_SINGLE_INPUT_SPAN,
       "passthrough span exceeds its resource"},
      {R300_R2VB_DELIVERY_STREAM_CAPACITY,
       R300_R2VB_AUTO_SINGLE_OUTPUT_STREAMS,
       "temporary delivery slots exceed capacity"},
   };
   for (unsigned i = 0; i < ARRAY_SIZE(output_declines); i++) {
      struct r300_r2vb_auto_single_draw output_conflict = fits;
      output_conflict.delivery_stream_status = output_declines[i].status;
      check_policy(&cp, &wp, &output_conflict, 1024,
                   output_declines[i].reason, output_declines[i].name);
   }

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

static void
check_producer_input_preflight(void)
{
   enum { INPUT_COUNT = 5,
          INPUT_STRIDE = 12 };
   struct r300_r2vb_producer_plan plan =
      single_cell(R300_R2VB_POSITION_CLIP);
   uint8_t bytes[INPUT_COUNT * INPUT_STRIDE];
   struct r300_resource resource;
   memset(&resource, 0, sizeof(resource));
   resource.b.width0 = sizeof(bytes);
   resource.malloced_buffer = bytes;
   struct pipe_vertex_buffer vb = {
      .buffer.resource = &resource.b,
   };
   struct pipe_vertex_element ve = {
      .src_stride = INPUT_STRIDE,
      .src_format = PIPE_FORMAT_R32G32B32_FLOAT,
   };

   CHECK(r300_r2vb_producer_input_preflight(
            &plan, &ve, 1, &vb, 1, 0, INPUT_COUNT) ==
            R300_R2VB_PRODUCER_INPUT_OK,
         "input preflight: CPU-shadow position span is admitted");

   struct r300_r2vb_producer_plan bad_plan = plan;
   bad_plan.position_source.valid = false;
   CHECK(r300_r2vb_producer_input_preflight(
            &bad_plan, &ve, 1, &vb, 1, 0, INPUT_COUNT) ==
            R300_R2VB_PRODUCER_INPUT_POSITION_SOURCE,
         "input preflight: unresolved position source declines");

   struct pipe_vertex_element bad_ve = ve;
   bad_ve.instance_divisor = 1;
   CHECK(r300_r2vb_producer_input_preflight(
            &plan, &bad_ve, 1, &vb, 1, 0, INPUT_COUNT) ==
            R300_R2VB_PRODUCER_INPUT_INSTANCE_RATE,
         "input preflight: per-instance position source declines");

   bad_ve = ve;
   bad_ve.vertex_buffer_index = 1;
   CHECK(r300_r2vb_producer_input_preflight(
            &plan, &bad_ve, 1, &vb, 1, 0, INPUT_COUNT) ==
            R300_R2VB_PRODUCER_INPUT_BUFFER_BINDING,
         "input preflight: unbound vertex-buffer index declines");

   struct pipe_vertex_buffer user_vb = {
      .is_user_buffer = true,
      .buffer.user = bytes,
   };
   CHECK(r300_r2vb_producer_input_preflight(
            &plan, &ve, 1, &user_vb, 1, 0, INPUT_COUNT) ==
            R300_R2VB_PRODUCER_INPUT_SOURCE_CLASS,
         "input preflight: user pointer has no resource extent authority");

   resource.buf = (struct pb_buffer_lean *)(uintptr_t)1;
   CHECK(r300_r2vb_producer_input_preflight(
            &plan, &ve, 1, &vb, 1, 0, INPUT_COUNT) ==
            R300_R2VB_PRODUCER_INPUT_SOURCE_CLASS,
         "input preflight: dual CPU and winsys backing declines");
   resource.malloced_buffer = NULL;
   CHECK(r300_r2vb_producer_input_preflight(
            &plan, &ve, 1, &vb, 1, 0, INPUT_COUNT) ==
            R300_R2VB_PRODUCER_INPUT_CPU_ACCESS,
         "input preflight: winsys-only source cannot build the CPU model");
   resource.buf = NULL;
   resource.malloced_buffer = bytes;

   bad_ve = ve;
   bad_ve.src_format = PIPE_FORMAT_R32G32_FLOAT;
   CHECK(r300_r2vb_producer_input_preflight(
            &plan, &bad_ve, 1, &vb, 1, 0, INPUT_COUNT) ==
            R300_R2VB_PRODUCER_INPUT_FORMAT,
         "input preflight: format outside the BO producer contract declines");

   bad_ve = ve;
   bad_ve.src_stride = 10;
   CHECK(r300_r2vb_producer_input_preflight(
            &plan, &bad_ve, 1, &vb, 1, 0, INPUT_COUNT) ==
            R300_R2VB_PRODUCER_INPUT_STRIDE,
         "input preflight: non-dword stride declines");

   resource.b.width0 = sizeof(bytes) - 1;
   CHECK(r300_r2vb_producer_input_preflight(
            &plan, &ve, 1, &vb, 1, 0, INPUT_COUNT) ==
            R300_R2VB_PRODUCER_INPUT_SPAN,
         "input preflight: final fetched byte must fit the resource");
}

/* The finite source-domain matrix of the automatic route: the producer
 * model fetch admits exactly the two packed FP32 position formats, and
 * every typed, normalized, scaled, packed, or narrow format declines at
 * input_format before any transport.  The delivery side admits exactly
 * FP32x4.  Together with the plan-oracle typed-op classification (sint,
 * uint, and bool NIR ops mark has_typed_source and the policy declines
 * at typed_source / plan_not_ready), this unit test records the bounded
 * format-admission contract.  Runtime transport, w-fill, signedness,
 * re-ingest identity, and silicon observations belong to separate evidence. */
static void
check_source_domain_matrix(void)
{
   enum { INPUT_COUNT = 5, MAX_STRIDE = 16 };
   struct r300_r2vb_producer_plan plan =
      single_cell(R300_R2VB_POSITION_CLIP);
   uint8_t bytes[INPUT_COUNT * MAX_STRIDE];
   struct r300_resource resource;
   memset(&resource, 0, sizeof(resource));
   resource.b.width0 = sizeof(bytes);
   resource.malloced_buffer = bytes;
   struct pipe_vertex_buffer vb = {
      .buffer.resource = &resource.b,
   };

   static const struct {
      enum pipe_format format;
      unsigned stride;
      enum r300_r2vb_producer_input_status expect;
      const char *name;
   } inputs[] = {
      { PIPE_FORMAT_R32G32B32_FLOAT, 12,
        R300_R2VB_PRODUCER_INPUT_OK, "FLOAT_3 packed" },
      { PIPE_FORMAT_R32G32B32A32_FLOAT, 16,
        R300_R2VB_PRODUCER_INPUT_OK, "FLOAT_4 packed" },
      { PIPE_FORMAT_R32_FLOAT, 4,
        R300_R2VB_PRODUCER_INPUT_FORMAT, "FLOAT_1" },
      { PIPE_FORMAT_R32G32_FLOAT, 8,
        R300_R2VB_PRODUCER_INPUT_FORMAT, "FLOAT_2" },
      { PIPE_FORMAT_R16G16B16A16_FLOAT, 8,
        R300_R2VB_PRODUCER_INPUT_FORMAT, "FP16x4" },
      { PIPE_FORMAT_R32G32B32A32_SINT, 16,
        R300_R2VB_PRODUCER_INPUT_FORMAT, "SINT32x4" },
      { PIPE_FORMAT_R32G32B32A32_UINT, 16,
        R300_R2VB_PRODUCER_INPUT_FORMAT, "UINT32x4" },
      { PIPE_FORMAT_R16G16B16_SSCALED, 8,
        R300_R2VB_PRODUCER_INPUT_FORMAT, "GL_SHORT scaled" },
      { PIPE_FORMAT_R16G16B16A16_SNORM, 8,
        R300_R2VB_PRODUCER_INPUT_FORMAT, "SNORM16x4" },
      { PIPE_FORMAT_R8G8B8A8_UNORM, 4,
        R300_R2VB_PRODUCER_INPUT_FORMAT, "UNORM8x4" },
      { PIPE_FORMAT_R8G8B8A8_USCALED, 4,
        R300_R2VB_PRODUCER_INPUT_FORMAT, "GL_UBYTE scaled" },
      { PIPE_FORMAT_R10G10B10A2_UNORM, 4,
        R300_R2VB_PRODUCER_INPUT_FORMAT, "packed 10_10_10_2" },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(inputs); i++) {
      struct pipe_vertex_element ve = {
         .src_stride = inputs[i].stride,
         .src_format = inputs[i].format,
      };
      char label[128];
      snprintf(label, sizeof(label), "input matrix: %s -> %s",
               inputs[i].name,
               inputs[i].expect == R300_R2VB_PRODUCER_INPUT_OK
                  ? "admitted" : "input_format");
      CHECK(r300_r2vb_producer_input_preflight(
               &plan, &ve, 1, &vb, 1, 0, INPUT_COUNT) == inputs[i].expect,
            label);
   }

   static const struct {
      enum pipe_format format;
      enum r300_r2vb_delivery_stream_status expect;
      const char *name;
   } outputs[] = {
      { PIPE_FORMAT_R32G32B32A32_FLOAT,
        R300_R2VB_DELIVERY_STREAM_OK, "FLOAT_4" },
      { PIPE_FORMAT_R32G32B32_FLOAT,
        R300_R2VB_DELIVERY_STREAM_FORMAT, "FLOAT_3" },
      { PIPE_FORMAT_R32G32B32A32_SINT,
        R300_R2VB_DELIVERY_STREAM_FORMAT, "SINT32x4" },
      { PIPE_FORMAT_R8G8B8A8_UNORM,
        R300_R2VB_DELIVERY_STREAM_FORMAT, "UNORM8x4" },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(outputs); i++) {
      struct pipe_vertex_element element = {
         .src_stride = 16,
         .src_format = outputs[i].format,
      };
      char label[128];
      snprintf(label, sizeof(label), "delivery matrix: %s -> %s",
               outputs[i].name,
               outputs[i].expect == R300_R2VB_DELIVERY_STREAM_OK
                  ? "admitted" : "format");
      CHECK(r300_r2vb_delivery_element_preflight(&element) ==
               outputs[i].expect,
            label);
   }
}

static void
check_delivery_element_preflight(void)
{
   struct pipe_vertex_element element = {
      .src_stride = 16,
      .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
   };
   CHECK(r300_r2vb_delivery_element_preflight(&element) ==
            R300_R2VB_DELIVERY_STREAM_OK,
         "delivery element: exact FP32x4 vertex stream is admitted");

   struct pipe_vertex_element bad = element;
   bad.instance_divisor = 1;
   CHECK(r300_r2vb_delivery_element_preflight(&bad) ==
            R300_R2VB_DELIVERY_STREAM_INSTANCE_RATE,
         "delivery element: per-instance stream declines");

   bad = element;
   bad.src_format = PIPE_FORMAT_R32G32_FLOAT;
   CHECK(r300_r2vb_delivery_element_preflight(&bad) ==
            R300_R2VB_DELIVERY_STREAM_FORMAT,
         "delivery element: compact float stream needs conversion");

   bad = element;
   bad.src_format = PIPE_FORMAT_R32G32B32A32_SINT;
   CHECK(r300_r2vb_delivery_element_preflight(&bad) ==
            R300_R2VB_DELIVERY_STREAM_FORMAT,
         "delivery element: same-size integer stream needs conversion");

   bad = element;
   bad.src_stride = 0;
   CHECK(r300_r2vb_delivery_element_preflight(&bad) ==
            R300_R2VB_DELIVERY_STREAM_STRIDE,
         "delivery element: zero stride declines");

   bad = element;
   bad.src_offset = 16;
   CHECK(r300_r2vb_delivery_element_preflight(&bad) ==
            R300_R2VB_DELIVERY_STREAM_STRIDE,
         "delivery element: record outside the stride declines");
}

static void
check_output_stream_preflight(void)
{
   nir_builder builder = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, g_screen.screen.nir_options[MESA_SHADER_VERTEX],
      "auto_single_output_preflight");
   nir_variable *input_position = nir_variable_create(
      builder.shader, nir_var_shader_in, glsl_vec4_type(), "in_position");
   input_position->data.location = VERT_ATTRIB_POS;
   input_position->data.driver_location = 0;
   nir_variable *input_attribute = nir_variable_create(
      builder.shader, nir_var_shader_in, glsl_vec4_type(), "in_attribute");
   input_attribute->data.location = VERT_ATTRIB_GENERIC0;
   input_attribute->data.driver_location = 1;
   nir_variable *output_position = nir_variable_create(
      builder.shader, nir_var_shader_out, glsl_vec4_type(), "gl_Position");
   output_position->data.location = VARYING_SLOT_POS;
   nir_variable *output_attribute = nir_variable_create(
      builder.shader, nir_var_shader_out, glsl_vec4_type(), "out_attribute");
   output_attribute->data.location = VARYING_SLOT_VAR0;
   nir_def *position = nir_load_var(&builder, input_position);
   nir_store_var(&builder, output_position,
                 nir_fadd(&builder, position, position), 0xf);
   nir_store_var(&builder, output_attribute,
                 nir_load_var(&builder, input_attribute), 0xf);
   nir_validate_shader(builder.shader, "AUTO_SINGLE output preflight");

   struct r300_vertex_element_state elements;
   memset(&elements, 0, sizeof(elements));
   elements.count = 2;
   elements.velem[0] = (struct pipe_vertex_element){
      .src_stride = 16,
      .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
      .vertex_buffer_index = 0,
   };
   elements.velem[1] = (struct pipe_vertex_element){
      .src_stride = 16,
      .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
      .vertex_buffer_index = 1,
   };
   uint8_t shadow_bytes[48] = {0};
   struct r300_resource position_resource;
   struct r300_resource attribute_resource;
   memset(&position_resource, 0, sizeof(position_resource));
   memset(&attribute_resource, 0, sizeof(attribute_resource));
   position_resource.b.width0 = sizeof(shadow_bytes);
   position_resource.buf = (struct pb_buffer_lean *)(uintptr_t)1;
   attribute_resource.b.width0 = sizeof(shadow_bytes);
   attribute_resource.buf = (struct pb_buffer_lean *)(uintptr_t)2;

   struct r300_vertex_element_state *saved_elements = g_context.velems;
   struct vertex_info saved_vertex_info = g_context.vertex_info;
   unsigned saved_buffer_count = g_context.nr_vertex_buffers;
   struct pipe_vertex_buffer saved_buffers[2] = {
      g_context.vertex_buffer[0],
      g_context.vertex_buffer[1],
   };
   g_context.velems = &elements;
   g_context.vertex_info.num_attribs = 2;
   g_context.nr_vertex_buffers = 2;
   g_context.vertex_buffer[0] = (struct pipe_vertex_buffer){
      .buffer.resource = &position_resource.b,
   };
   g_context.vertex_buffer[1] = (struct pipe_vertex_buffer){
      .buffer.resource = &attribute_resource.b,
   };
   const struct pipe_draw_start_count_bias draw = {
      .start = 1,
      .count = 2,
   };

   CHECK(r300_r2vb_auto_single_output_streams_preflight(
            &g_context, builder.shader, -1, &draw) ==
            R300_R2VB_DELIVERY_STREAM_OK,
         "output preflight: one real-BO authority is admitted");

   attribute_resource.malloced_buffer = shadow_bytes;
   CHECK(r300_r2vb_auto_single_output_streams_preflight(
            &g_context, builder.shader, -1, &draw) ==
            R300_R2VB_DELIVERY_STREAM_SOURCE_CLASS,
         "output preflight: dual backing authority declines");

   attribute_resource.buf = NULL;
   CHECK(r300_r2vb_auto_single_output_streams_preflight(
            &g_context, builder.shader, -1, &draw) ==
            R300_R2VB_DELIVERY_STREAM_OK,
         "output preflight: one CPU-shadow authority is admitted");

   attribute_resource.malloced_buffer = NULL;
   CHECK(r300_r2vb_auto_single_output_streams_preflight(
            &g_context, builder.shader, -1, &draw) ==
            R300_R2VB_DELIVERY_STREAM_SOURCE_CLASS,
         "output preflight: missing backing authority declines");

   attribute_resource.malloced_buffer = shadow_bytes;
   attribute_resource.b.width0 = sizeof(shadow_bytes) - 1;
   CHECK(r300_r2vb_auto_single_output_streams_preflight(
            &g_context, builder.shader, -1, &draw) ==
            R300_R2VB_DELIVERY_STREAM_SPAN,
         "output preflight: one-byte-short passthrough span declines");

   g_context.velems = saved_elements;
   g_context.vertex_info = saved_vertex_info;
   g_context.nr_vertex_buffers = saved_buffer_count;
   g_context.vertex_buffer[0] = saved_buffers[0];
   g_context.vertex_buffer[1] = saved_buffers[1];
   ralloc_free(builder.shader);
}

static void
check_passthrough_upload_extent(void)
{
   struct pipe_vertex_element elements[2] = {
      {
         .src_offset = 0,
         .src_stride = 32,
         .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
         .vertex_buffer_index = 2,
      },
      {
         .src_offset = 16,
         .src_stride = 32,
         .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
         .vertex_buffer_index = 2,
      },
   };
   uint32_t end = 0;
   CHECK(r300_r2vb_vertex_buffer_upload_end(
            elements, ARRAY_SIZE(elements), 2, 8, 2, 3, 168, &end) &&
            end == 168,
         "passthrough upload: high interleaved offset fixes the exact end");
   CHECK(!r300_r2vb_vertex_buffer_upload_end(
            elements, ARRAY_SIZE(elements), 2, 8, 2, 3, 167, &end),
         "passthrough upload: one-byte-short resource declines");

   struct pipe_vertex_element extreme = {
      .src_offset = UINT16_MAX,
      .src_stride = UINT16_MAX,
      .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
      .vertex_buffer_index = 0,
   };
   CHECK(!r300_r2vb_vertex_buffer_upload_end(
            &extreme, 1, 0, UINT32_MAX, UINT32_MAX, UINT32_MAX,
            UINT32_MAX, &end),
         "passthrough upload: maximum offsets cannot wrap the extent");

   extreme.src_offset = 0;
   CHECK(!r300_r2vb_vertex_buffer_upload_end(
            &extreme, 1, 0, 0, UINT32_MAX, 2, UINT32_MAX, &end),
         "passthrough upload: start plus count overflow declines");

   elements[0].instance_divisor = 1;
   CHECK(!r300_r2vb_vertex_buffer_upload_end(
            elements, ARRAY_SIZE(elements), 2, 8, 2, 3, 168, &end),
         "passthrough upload: per-instance element declines");
   elements[0].instance_divisor = 0;
   CHECK(!r300_r2vb_vertex_buffer_upload_end(
            elements, ARRAY_SIZE(elements), 2, 8, 2, 3, 168, NULL),
         "passthrough upload: missing result storage declines");
}

enum constant_source_fixture {
   CONSTANT_SOURCE_FIXTURE_NONE = 0,
   CONSTANT_SOURCE_FIXTURE_UBO0,
   CONSTANT_SOURCE_FIXTURE_UBO0_END,
   CONSTANT_SOURCE_FIXTURE_UBO0_SECOND_MATRIX,
   CONSTANT_SOURCE_FIXTURE_UBO0_OVERRUN,
   CONSTANT_SOURCE_FIXTURE_UBO1,
   CONSTANT_SOURCE_FIXTURE_DYNAMIC_BLOCK,
   CONSTANT_SOURCE_FIXTURE_DYNAMIC_OFFSET,
   CONSTANT_SOURCE_FIXTURE_PUSH,
   CONSTANT_SOURCE_FIXTURE_CONSTANT,
   CONSTANT_SOURCE_FIXTURE_UBO_UNIFORM_BLOCK_INTEL,
   CONSTANT_SOURCE_FIXTURE_GLOBAL_CONSTANT_UNIFORM_BLOCK_INTEL,
   CONSTANT_SOURCE_FIXTURE_PUSH_DATA_INTEL,
   CONSTANT_SOURCE_FIXTURE_PUSH_CONSTANT_ZINK,
};

static nir_shader *
build_constant_source_fixture(enum constant_source_fixture fixture)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, g_screen.screen.nir_options[MESA_SHADER_VERTEX],
      "r2vb_constant_source_fixture");
   b.shader->info.num_ubos = 2;

   nir_def *dynamic = nir_load_vertex_id(&b);
   switch (fixture) {
   case CONSTANT_SOURCE_FIXTURE_NONE:
      break;
   case CONSTANT_SOURCE_FIXTURE_UBO0:
      nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                   .align_mul = 16, .range = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_UBO0_END:
      nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 48),
                   .align_mul = 16, .range_base = 48, .range = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_UBO0_SECOND_MATRIX:
      nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 64),
                   .align_mul = 16, .range_base = 64, .range = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_UBO0_OVERRUN:
      nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 128),
                   .align_mul = 16, .range_base = 128, .range = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_UBO1:
      nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 1), nir_imm_int(&b, 0),
                   .align_mul = 16, .range = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_DYNAMIC_BLOCK:
      nir_load_ubo(&b, 4, 32, dynamic, nir_imm_int(&b, 0),
                   .align_mul = 16, .range = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_DYNAMIC_OFFSET:
      nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 0), dynamic,
                   .align_mul = 4, .range = 64);
      break;
   case CONSTANT_SOURCE_FIXTURE_PUSH:
      nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0),
                             .align_mul = 16, .range = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_CONSTANT:
      nir_load_constant(&b, 4, 32, nir_imm_int(&b, 0),
                        .align_mul = 16, .range = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_UBO_UNIFORM_BLOCK_INTEL:
      nir_load_ubo_uniform_block_intel(
         &b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
         .align_mul = 16, .range = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_GLOBAL_CONSTANT_UNIFORM_BLOCK_INTEL:
      nir_load_global_constant_uniform_block_intel(
         &b, 4, 32, nir_imm_int64(&b, 0), .align_mul = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_PUSH_DATA_INTEL:
      nir_load_push_data_intel(&b, 4, 32, nir_imm_int(&b, 0),
                               .range = 16);
      break;
   case CONSTANT_SOURCE_FIXTURE_PUSH_CONSTANT_ZINK:
      nir_load_push_constant_zink(&b, 4, 32, nir_imm_int(&b, 0));
      break;
   }

   nir_validate_shader(b.shader, "R2VB constant-source fixture");
   return b.shader;
}

static void
check_constant_source_contract(void)
{
   struct {
      enum constant_source_fixture fixture;
      enum r300_r2vb_constant_source_contract expected_contract;
      uint32_t expected_bytes;
      const char *name;
   } cases[] = {
      {CONSTANT_SOURCE_FIXTURE_NONE, R300_R2VB_CONSTANT_SOURCE_NONE, 0,
       "constant source: shader without external loads needs no mirror"},
      {CONSTANT_SOURCE_FIXTURE_UBO0,
       R300_R2VB_CONSTANT_SOURCE_UBO0_PREFIX64, 16,
       "constant source: first UBO0 vector maps to the mirrored prefix"},
      {CONSTANT_SOURCE_FIXTURE_UBO0_END,
       R300_R2VB_CONSTANT_SOURCE_UBO0_PREFIX64, 64,
       "constant source: final UBO0 prefix vector ends at byte 64"},
      {CONSTANT_SOURCE_FIXTURE_UBO0_SECOND_MATRIX,
       R300_R2VB_CONSTANT_SOURCE_UBO0_PREFIX64, 80,
       "constant source: the second-matrix window at byte 64 scans"},
      {CONSTANT_SOURCE_FIXTURE_UBO0_OVERRUN,
       R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED, 0,
       "constant source: UBO0 byte 128 starts outside the window"},
      {CONSTANT_SOURCE_FIXTURE_UBO1,
       R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED, 0,
       "constant source: UBO1 remains outside the production contract"},
      {CONSTANT_SOURCE_FIXTURE_DYNAMIC_BLOCK,
       R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED, 0,
       "constant source: dynamic UBO selection declines"},
      {CONSTANT_SOURCE_FIXTURE_DYNAMIC_OFFSET,
       R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED, 0,
       "constant source: dynamic UBO offset declines"},
      {CONSTANT_SOURCE_FIXTURE_PUSH,
       R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED, 0,
       "constant source: push constants decline"},
      {CONSTANT_SOURCE_FIXTURE_CONSTANT,
       R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED, 0,
       "constant source: constant-memory loads decline"},
      {CONSTANT_SOURCE_FIXTURE_UBO_UNIFORM_BLOCK_INTEL,
       R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED, 0,
       "constant source: Intel uniform-block UBO loads decline"},
      {CONSTANT_SOURCE_FIXTURE_GLOBAL_CONSTANT_UNIFORM_BLOCK_INTEL,
       R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED, 0,
       "constant source: Intel uniform-block global loads decline"},
      {CONSTANT_SOURCE_FIXTURE_PUSH_DATA_INTEL,
       R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED, 0,
       "constant source: Intel push-data loads decline"},
      {CONSTANT_SOURCE_FIXTURE_PUSH_CONSTANT_ZINK,
       R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED, 0,
       "constant source: Zink push-constant loads decline"},
   };

   for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
      nir_shader *shader = build_constant_source_fixture(cases[i].fixture);
      uint32_t required_bytes = UINT32_MAX;
      enum r300_r2vb_constant_source_contract contract =
         r300_r2vb_constant_source_scan(shader, &required_bytes);
      CHECK(contract == cases[i].expected_contract &&
               required_bytes == cases[i].expected_bytes,
            cases[i].name);
      ralloc_free(shader);
   }
}

static void
check_vertex_array_restore(void)
{
   g_context.vertex_arrays_dirty = false;
   r300_r2vb_restore_vertex_array_state(&g_context);
   CHECK(g_context.vertex_arrays_dirty,
         "re-ingest restore: application vertex arrays are re-emitted");
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
      d.slot_layout_available = true;
      d.slot_fetch_enabled = true;
      d.raw_submit_accepted = true;
      d.bo_draw_mode_compatible = true;
      d.bo_delivery_ordering_compatible = true;
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
            l.pitch_pixels == (height == 1 ? align(width, 2) : width) &&
            l.storage_slots >= count &&
            l.storage_bytes == l.storage_slots * 16u,
         label);
   if (!ok)
      return;
   bool map_ok = true;
   for (uint32_t s = 0; s < count; s++) {
      uint32_t x = UINT32_MAX, y = UINT32_MAX;
      uint64_t byte_offset = UINT64_MAX;
      if (!r300_r2vb_slot_layout_address(&l, s, &x, &y, &byte_offset) ||
          x >= l.width || y >= l.height || byte_offset != (uint64_t)s * 16u)
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
   CHECK(!r300_r2vb_bo_draw_delivery_mode_value(NULL) &&
            !r300_r2vb_bo_draw_delivery_mode_value("") &&
            !r300_r2vb_bo_draw_delivery_mode_value("producer_submit3") &&
            !r300_r2vb_bo_draw_delivery_mode_value("PRODUCER_DELIVER") &&
            r300_r2vb_bo_draw_delivery_mode_value("producer_deliver"),
         "layout: BO delivery selector exact-value parser");
   CHECK(!r300_r2vb_immediate_producer_shape_ok(0, 1) &&
            !r300_r2vb_immediate_producer_shape_ok(1, 0) &&
            r300_r2vb_immediate_producer_shape_ok(1024, 1) &&
            !r300_r2vb_immediate_producer_shape_ok(1025, 1) &&
            r300_r2vb_immediate_producer_shape_ok(682, 2) &&
            !r300_r2vb_immediate_producer_shape_ok(683, 2) &&
            r300_r2vb_immediate_producer_shape_ok(227, 8) &&
            !r300_r2vb_immediate_producer_shape_ok(228, 8) &&
            !r300_r2vb_immediate_producer_shape_ok(1, 9) &&
            !r300_r2vb_immediate_producer_shape_ok(2047, 1) &&
            !r300_r2vb_immediate_producer_shape_ok(2048, 1),
         "layout: count-scaled immediate payload stays within one IB");
   uint32_t rebased_offset = UINT32_MAX;
   CHECK(r300_r2vb_rebased_buffer_offset(8, 12, 2, &rebased_offset) &&
            rebased_offset == 32 &&
            r300_r2vb_rebased_buffer_offset(UINT32_MAX, 1, 0,
                                            &rebased_offset) &&
            rebased_offset == UINT32_MAX &&
            !r300_r2vb_rebased_buffer_offset(UINT32_MAX, 1, 1,
                                             &rebased_offset) &&
            !r300_r2vb_rebased_buffer_offset(0, 16, 1, NULL),
         "layout: passthrough buffer rebasing is checked and exact");
   struct r300_r2vb_slot_layout selected;
   CHECK(r300_r2vb_slot_layout_select(4097, NULL, true, &selected) &&
            selected.policy == R300_R2VB_LAYOUT_GRID_2048,
         "layout: absent selector uses the count-derived grid policy");
   CHECK(r300_r2vb_slot_layout_select(4096, "legacy_row", true, &selected) &&
            selected.policy == R300_R2VB_LAYOUT_LEGACY_ROW &&
            selected.width == 4096 && selected.height == 1,
         "layout: explicit legacy-row selector");
   CHECK(r300_r2vb_slot_layout_select(4096, "grid_2048", true, &selected) &&
            selected.policy == R300_R2VB_LAYOUT_GRID_2048 &&
            selected.width == 2048 && selected.height == 2,
         "layout: explicit grid selector");
   CHECK(!r300_r2vb_slot_layout_select(4096, "grid_2048", false, &selected),
         "layout: explicit grid selector requires the grid gate");
   CHECK(!r300_r2vb_slot_layout_select(4096, "", true, &selected) &&
            !r300_r2vb_slot_layout_select(4096, "grid", true, &selected),
         "layout: empty and unrecognized selectors fail closed");
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

   /* Explicit policy: the two representations of one count are separately
    * addressable, so the 4096 layout-boundary comparison (one row versus
    * 2048x2) and the one-row 2559/2560/2561 color-render-axis boundary
    * cells name their shape instead of deriving it from count. */
   struct r300_r2vb_slot_layout row, grid;
   CHECK(r300_r2vb_slot_layout_init_policy(4096, R300_R2VB_LAYOUT_LEGACY_ROW,
                                           &row) &&
            row.width == 4096 && row.height == 1,
         "layout: policy row 4096 -> 4096x1");
   CHECK(r300_r2vb_slot_layout_init_policy(4096, R300_R2VB_LAYOUT_GRID_2048,
                                           &grid) &&
            grid.width == 2048 && grid.height == 2 &&
            grid.storage_slots == 4096 &&
            grid.storage_bytes == row.storage_bytes,
         "layout: policy grid 4096 -> 2048x2, same storage, no tail");
   CHECK(r300_r2vb_slot_layout_init_policy(4097, R300_R2VB_LAYOUT_GRID_2048,
                                           &grid) &&
            grid.width == 2048 && grid.height == 3 &&
            grid.storage_slots - grid.count == 2047,
         "layout: policy grid 4097 -> 2048x3, 2047-slot sentinel tail");
   {
      uint32_t x = UINT32_MAX, y = UINT32_MAX;
      uint64_t byte_offset = UINT64_MAX;
      struct r300_r2vb_slot_layout malformed = grid;
      malformed.pitch_pixels++;
      CHECK(!r300_r2vb_slot_layout_address(&malformed, 4096, &x, &y,
                                           &byte_offset) &&
               x == UINT32_MAX && y == UINT32_MAX &&
               byte_offset == UINT64_MAX,
            "layout: malformed grid pitch fails without output mutation");
      malformed = grid;
      malformed.storage_bytes -= 16;
      CHECK(!r300_r2vb_slot_layout_address(&malformed, 4096, &x, &y,
                                           &byte_offset),
            "layout: malformed storage extent fails closed");
      CHECK(!r300_r2vb_slot_layout_address(&grid, grid.count, &x, &y,
                                           &byte_offset) &&
               !r300_r2vb_slot_layout_address(NULL, 0, &x, &y,
                                              &byte_offset) &&
               !r300_r2vb_slot_layout_address(&grid, 0, NULL, &y,
                                              &byte_offset),
            "layout: out-of-domain slot and null arguments fail closed");
   }
   CHECK(!r300_r2vb_slot_layout_init_policy(4097,
                                            R300_R2VB_LAYOUT_LEGACY_ROW,
                                            &row),
         "layout: policy row rejects 4097");
   CHECK(!r300_r2vb_slot_layout_init_policy(
            1, R300_R2VB_LAYOUT_LEGACY_ROW, NULL) &&
            !r300_r2vb_slot_layout_init(1, false, NULL) &&
            !r300_r2vb_slot_layout_select(1, NULL, false, NULL),
         "layout: null output rejects every constructor");
   for (uint32_t w = 2559; w <= 2561; w++) {
      char label[64];
      snprintf(label, sizeof(label), "layout: policy row %u one-row", w);
      CHECK(r300_r2vb_slot_layout_init_policy(w, R300_R2VB_LAYOUT_LEGACY_ROW,
                                              &row) &&
               row.width == w && row.height == 1 &&
               row.pitch_pixels == align(w, 2),
            label);
   }
   CHECK(r300_r2vb_slot_layout_init_policy(2559,
                                           R300_R2VB_LAYOUT_LEGACY_ROW,
                                           &row) &&
            row.storage_slots - row.count == 1,
         "layout: odd one-row width retains one physical tail slot");
   CHECK(r300_r2vb_slot_layout_init_policy(8192, R300_R2VB_LAYOUT_GRID_2048,
                                           &grid) &&
            grid.width == 2048 && grid.height == 4,
         "layout: policy grid 8192 -> 2048x4");
   CHECK(r300_r2vb_slot_layout_init_policy(21516, R300_R2VB_LAYOUT_GRID_2048,
                                           &grid) &&
            grid.width == 2048 && grid.height == 11,
         "layout: policy grid 21516 -> 2048x11");
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
   CHECK(!r300_r2vb_producer_streams_init(0, 0, 1024,
                                          PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                          &s),
         "streams: stride above packet field rejects");
   CHECK(r300_r2vb_producer_streams_init(0, 0, 1020,
                                         PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                         &s) &&
            s.stream[1].stride_dwords == R300_R2VB_VBPNTR_STRIDE_DWORDS_MAX,
         "streams: packet stride field maximum is representable");
   CHECK(!r300_r2vb_producer_streams_init(UINT32_MAX - 14, 0, 16,
                                          PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                          &s),
         "streams: fetch crossing 32-bit offset rejects");
   CHECK(r300_r2vb_producer_streams_init(UINT32_MAX - 15, 0, 16,
                                         PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                         &s),
         "streams: fetch ending at 32-bit boundary fits");
   CHECK(!r300_r2vb_producer_streams_init(0, 0, 16,
                                          PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                          NULL),
         "streams: null output rejects");
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
   forged = s;
   forged.stream[1].stride_dwords =
      R300_R2VB_VBPNTR_STRIDE_DWORDS_MAX + 1;
   CHECK(!r300_r2vb_producer_fetch_init(&forged, 4, 1 << 20, 1 << 20, &f),
         "fetch: model stride-field ceiling rejects");
   forged = s; forged.stream[0].offset_bytes = 16;
   CHECK(!r300_r2vb_producer_fetch_init(&forged, 4, 1 << 20, 1 << 20, &f),
         "fetch: nonzero slot offset rejects");
   forged = s; forged.stream[0].stride_dwords = 8;
   CHECK(!r300_r2vb_producer_fetch_init(&forged, 4, 1 << 20, 1 << 20, &f),
         "fetch: forged slot stride 8 rejects");
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
   /* The packet stride field is 8 bits of dwords; streams_init rejects the
    * unrepresentable value before fetch construction. */
   struct r300_r2vb_producer_streams huge;
   CHECK(!r300_r2vb_producer_streams_init(0, 0, 1024,
                                          PIPE_FORMAT_R32G32B32A32_FLOAT, 0,
                                          &huge),
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
   /* Full driver-resource shape: the winsys-facing transaction casts a
    * pipe_resource to r300_resource and reads buf and domain, so the
    * fake allocates the complete struct with a non-NULL dummy buf. */
   struct r300_resource r;
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
   fb->r.b = *templ;
   pipe_reference_init(&fb->r.b.reference, 1);
   fb->r.b.screen = screen;
   fb->data = calloc(1, templ->width0);
   fb->r.buf = (struct pb_buffer_lean *)fb->data;
   fb->r.domain = RADEON_DOMAIN_GTT;
   return &fb->r.b;
}

static void
fake_resource_destroy(struct pipe_screen *screen, struct pipe_resource *res)
{
   struct fake_buffer *fb = (struct fake_buffer *)res;
   free(fb->data);
   free(fb);
}

/* Fake winsys for the CS-staging and emission rows: a real command
 * buffer, scripted cs_validate failures, and order-of-first-add
 * relocation indices.  A validate failure clears the seen set, matching
 * the real winsys contract that a validation flush drops the buffers
 * added since the prior validation. */
static struct {
   unsigned adds;
   unsigned validates;
   unsigned fail_validates; /* decrement-to-zero failure script */
   struct pb_buffer_lean *seen[8];
   unsigned num_seen;
} g_fws;

static unsigned
fws_add_buffer(struct radeon_cmdbuf *rcs, struct pb_buffer_lean *buf,
               unsigned usage, enum radeon_bo_domain domain)
{
   g_fws.adds++;
   for (unsigned i = 0; i < g_fws.num_seen; i++)
      if (g_fws.seen[i] == buf)
         return i;
   g_fws.seen[g_fws.num_seen] = buf;
   return g_fws.num_seen++;
}

static int
fws_lookup_buffer(struct radeon_cmdbuf *rcs, struct pb_buffer_lean *buf)
{
   for (unsigned i = 0; i < g_fws.num_seen; i++)
      if (g_fws.seen[i] == buf)
         return (int)i;
   return -1;
}

static bool
fws_validate(struct radeon_cmdbuf *rcs)
{
   g_fws.validates++;
   if (g_fws.fail_validates) {
      g_fws.fail_validates--;
      g_fws.num_seen = 0;
      memset(g_fws.seen, 0, sizeof(g_fws.seen));
      return false;
   }
   return true;
}

static bool
fws_check_space(struct radeon_cmdbuf *rcs, unsigned dw)
{
   return true;
}

static struct radeon_winsys g_fake_winsys;

static void
check_swtcl_vertex_size_restore(void)
{
   uint32_t command_stream[32] = {
      CP_PACKET0(R300_VAP_VTX_SIZE, 0),
      8,
   };
   struct radeon_winsys *saved_winsys = g_context.rws;
   uint32_t *saved_buffer = g_context.cs.current.buf;
   unsigned saved_max_dwords = g_context.cs.current.max_dw;
   unsigned saved_current_dword = g_context.cs.current.cdw;
   struct pb_buffer_lean *saved_vbo = g_context.vbo;
   size_t saved_vbo_offset = g_context.draw_vbo_offset;
   unsigned saved_vertex_size = g_context.vertex_info.size;

   memset(&g_fws, 0, sizeof(g_fws));
   g_fake_winsys.cs_lookup_buffer = fws_lookup_buffer;
   struct pb_buffer_lean *application_vbo =
      (struct pb_buffer_lean *)(uintptr_t)1;
   g_fws.seen[0] = application_vbo;
   g_fws.num_seen = 1;
   g_context.rws = &g_fake_winsys;
   g_context.cs.current.buf = command_stream;
   g_context.cs.current.max_dw = ARRAY_SIZE(command_stream);
   g_context.cs.current.cdw = 2;
   g_context.vbo = application_vbo;
   g_context.draw_vbo_offset = 64;
   g_context.vertex_info.size = 12;

   const unsigned start = g_context.cs.current.cdw;
   r300_emit_vertex_arrays_swtcl(&g_context, false);
   CHECK(g_context.cs.current.cdw - start ==
            R300_EMIT_VERTEX_ARRAYS_SWTCL_DWORDS,
         "SWTCL arrays: emission and reservation share one dword count");
   CHECK(command_stream[0] == CP_PACKET0(R300_VAP_VTX_SIZE, 0) &&
            command_stream[1] == 8,
         "SWTCL arrays: the preceding producer vertex size remains historical");
   CHECK(command_stream[start] == CP_PACKET0(R300_VAP_VTX_SIZE, 0) &&
            command_stream[start + 1] == 12,
         "SWTCL arrays: application VAP_VTX_SIZE precedes its vertex fetch");
   CHECK(command_stream[start + 2] ==
               CP_PACKET3(R300_PACKET3_3D_LOAD_VBPNTR, 3) &&
            command_stream[start + 4] == (12 | (12 << 8)),
         "SWTCL arrays: LOAD_VBPNTR carries the same application tuple");

   g_context.rws = saved_winsys;
   g_context.cs.current.buf = saved_buffer;
   g_context.cs.current.max_dw = saved_max_dwords;
   g_context.cs.current.cdw = saved_current_dword;
   g_context.vbo = saved_vbo;
   g_context.draw_vbo_offset = saved_vbo_offset;
   g_context.vertex_info.size = saved_vertex_size;
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
   struct u_upload_mgr *saved_uploader = g_context.uploader;
   struct r300_r2vb_model_fetch no_uploader;
   r300_r2vb_model_fetch_init(&no_uploader);
   g_context.uploader = NULL;
   CHECK(!r300_r2vb_materialize_model_fetch_for_test(
            &g_context, &vb, &ve, START, COUNT, &s.stream[1],
            &no_uploader) &&
            no_uploader.kind == R300_R2VB_MODEL_UNSUPPORTED &&
            !no_uploader.resource,
         "upload: missing uploader declines CPU-shadow materialization");
   g_context.uploader = saved_uploader;
   r300_r2vb_model_fetch_fini(&no_uploader);
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
      uint8_t prime_src[64];
      uint8_t tail_src[64];
      for (unsigned i = 0; i < sizeof(prime_src); i++) {
         prime_src[i] = (uint8_t)(0x5A + i);
         tail_src[i] = (uint8_t)(0xA5 - i);
      }
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
         CHECK(mo.gpu_offset >= prime_off + sizeof(prime_src) &&
                  memcmp(up + prime_off, prime_src, sizeof(prime_src)) == 0,
               "upload: the full priming guard span stays untouched");
         unsigned tail_off = 0;
         struct pipe_resource *tail = NULL;
         u_upload_data_ref(g_context.uploader, 0, sizeof(tail_src), 4,
                           tail_src, &tail_off, &tail);
         CHECK(tail == mo.resource && tail_off >= mo.gpu_offset + mo.span_bytes,
               "upload: post-span sentinel stays after the target range");
         if (tail == mo.resource &&
             tail_off >= mo.gpu_offset + mo.span_bytes) {
            CHECK(memcmp(up + tail_off, tail_src, sizeof(tail_src)) == 0,
                  "upload: the full post-span sentinel stays untouched");
         }
         pipe_resource_reference(&tail, NULL);
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
check_position_source_rank(void)
{
   /* Table-driven identity oracle: the source scan records semantic rank and
    * driver location separately, while the delivery mapper resolves the
    * physical velem from driver location.  Each case builds a VS with inputs
    * at the named locations, feeds gl_Position from one of them, and checks
    * both identities independent of declaration order. */
   static const struct {
      unsigned num_inputs;
      unsigned locations[3];
      unsigned location_fractions[3];
      unsigned driver_locations[3];
      unsigned position_index; /* which declared input feeds position */
      bool declare_reversed;
      unsigned want_rank;
      unsigned want_driver_location;
      unsigned want_velem;
      const char *name;
   } cases[] = {
      { 1, { 0 }, { 0 }, { 0 }, 0, false, 0, 0, 0,
        "single input at location 0" },
      { 2, { 0, 3 }, { 0, 0 }, { 0, 1 }, 1, false, 1, 1, 1,
        "position at location 3 of {0,3}" },
      { 3, { 1, 4, 7 }, { 0, 0, 0 }, { 0, 1, 2 }, 1, false, 1, 1, 1,
        "position at location 4 of {1,4,7}" },
      { 2, { 0, 3 }, { 0, 0 }, { 0, 1 }, 1, true, 1, 1, 1,
        "reversed declaration order keeps location rank" },
      { 3, { 2, 5, 9 }, { 0, 0, 0 }, { 0, 1, 2 }, 2, false, 2, 2, 2,
        "position at the highest sparse location" },
      { 2, { 0, 0 }, { 0, 1 }, { 0, 0 }, 1, false, 1, 0, 0,
        "component-packed fractions share one physical velem" },
      { 2, { 0, 0 }, { 0, 0 }, { 0, 1 }, 0, true, 1, 0, 0,
        "declaration order breaks equal location fractions" },
   };
   for (unsigned c = 0; c < ARRAY_SIZE(cases); c++) {
      nir_builder b = nir_builder_init_simple_shader(
         MESA_SHADER_VERTEX,
         g_screen.screen.nir_options[MESA_SHADER_VERTEX], "rank_case");
      nir_variable *vars[3] = { 0 };
      for (unsigned di = 0; di < cases[c].num_inputs; di++) {
         unsigned i = cases[c].declare_reversed
                         ? cases[c].num_inputs - 1 - di
                         : di;
         vars[i] = nir_variable_create(b.shader, nir_var_shader_in,
                                       glsl_vec4_type(), "in");
         vars[i]->data.location = VERT_ATTRIB_GENERIC0 + cases[c].locations[i];
         vars[i]->data.location_frac = cases[c].location_fractions[i];
         vars[i]->data.driver_location = cases[c].driver_locations[i];
      }
      nir_variable *out_pos = nir_variable_create(
         b.shader, nir_var_shader_out, glsl_vec4_type(), "gl_Position");
      out_pos->data.location = VARYING_SLOT_POS;
      nir_store_var(&b, out_pos,
                    nir_load_var(&b, vars[cases[c].position_index]), 0xf);
      /* Every non-position input still feeds a varying so the original
       * shader keeps its full input list. */
      for (unsigned i = 0; i < cases[c].num_inputs; i++) {
         if (i == cases[c].position_index)
            continue;
         nir_variable *ov = nir_variable_create(
            b.shader, nir_var_shader_out, glsl_vec4_type(), "var");
         ov->data.location = VARYING_SLOT_VAR0 + i;
         nir_store_var(&b, ov, nir_load_var(&b, vars[i]), 0xf);
      }
      nir_validate_shader(b.shader, "rank case");
      struct r300_r2vb_position_source src;
      CHECK(r300_r2vb_position_source_scan(b.shader, &src) && src.valid,
            cases[c].name);
      CHECK(src.location_rank == cases[c].want_rank &&
               src.app_driver_location == cases[c].want_driver_location,
            "rank case: scan records the expected rank and location");
      CHECK(cases[c].want_velem ==
               (unsigned)r300_r2vb_input_velem_index_for_test(
                  b.shader, vars[cases[c].position_index]),
            "rank case: mapper resolves the physical velem");
      ralloc_free(b.shader);
   }
   /* Two inputs feeding position leave no single survivor. */
   {
      nir_builder b = nir_builder_init_simple_shader(
         MESA_SHADER_VERTEX,
         g_screen.screen.nir_options[MESA_SHADER_VERTEX], "rank_two");
      nir_variable *a = nir_variable_create(b.shader, nir_var_shader_in,
                                            glsl_vec4_type(), "a");
      a->data.location = VERT_ATTRIB_GENERIC0;
      nir_variable *bb = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "b");
      bb->data.location = VERT_ATTRIB_GENERIC0 + 1;
      bb->data.driver_location = 1;
      nir_variable *out_pos = nir_variable_create(
         b.shader, nir_var_shader_out, glsl_vec4_type(), "gl_Position");
      out_pos->data.location = VARYING_SLOT_POS;
      nir_store_var(&b, out_pos,
                    nir_fadd(&b, nir_load_var(&b, a), nir_load_var(&b, bb)),
                    0xf);
      nir_validate_shader(b.shader, "rank two");
      struct r300_r2vb_position_source src;
      CHECK(!r300_r2vb_position_source_scan(b.shader, &src) && !src.valid,
            "rank case: two position-feeding inputs leave the record invalid");
      ralloc_free(b.shader);
   }
}

static void
check_logical_binding(void)
{
   /* The three-namespace binding: measured application source identity,
    * VAP destination vectors, and the FS hardware input register the RS
    * block routes TC0 to, each derived rather than assumed. */
   struct r300_shader_semantics fs;
   r300_shader_semantics_reset(&fs);
   fs.generic[0] = 0;
   fs.num_generic = 1;
   fs.num_total = 1;
   unsigned hwreg = 99;
   CHECK(r300_r2vb_producer_fs_input_hwreg(&fs, &hwreg) && hwreg == 0,
         "binding: single-generic FS derives hardware input register 0");
   {
      struct r300_shader_semantics bad = fs;
      bad.color[0] = 1;
      bad.num_total = 2;
      CHECK(!r300_r2vb_producer_fs_input_hwreg(&bad, &hwreg),
            "binding: a color input breaks the producer FS contract");
      bad = fs;
      bad.wpos = 1;
      bad.num_total = 2;
      CHECK(!r300_r2vb_producer_fs_input_hwreg(&bad, &hwreg),
            "binding: a WPOS input breaks the producer FS contract");
      bad = fs;
      bad.generic[1] = 1;
      bad.num_generic = 2;
      bad.num_total = 2;
      CHECK(!r300_r2vb_producer_fs_input_hwreg(&bad, &hwreg),
            "binding: a second generic breaks the producer FS contract");
   }

   /* Derived RS block for the expected logical tuple: POS + one
    * 4-component TC0 routed to FS input register 0. */
   struct r300_rs_block rs;
   memset(&rs, 0, sizeof(rs));
   rs.vap_vtx_state_cntl = 0x5555;
   rs.vap_vsm_vtx_assm = R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0;
   rs.vap_out_vtx_fmt[0] = R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT;
   rs.vap_out_vtx_fmt[1] = 4;
   /* The exact whole words from the live immediate-producer decode:
    * ip 0x00d10000, count 0x00040004 (IT 4 + HIRES), inst 0x00000008. */
   rs.ip[0] = R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
              R300_RS_SEL_T(R300_RS_SEL_C1) | R300_RS_SEL_R(R300_RS_SEL_C2) |
              R300_RS_SEL_Q(R300_RS_SEL_C3);
   rs.count = R300_IT_COUNT(4) | R300_HIRES_EN;
   rs.inst[0] = R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
                R300_RS_INST_TEX_ADDR(0);

   struct r300_r2vb_position_source src = {
      .app_driver_location = 0, .location_rank = 0, .valid = true
   };
   struct r300_r2vb_producer_logical_binding bind;
   CHECK(r300_r2vb_producer_logical_binding_init(&src, &fs, &rs, 0, 6, &bind),
         "binding: all four authorities agree and the binding builds");
   CHECK(bind.fs_hw_input_reg == 0 && bind.velem_index == 0 &&
            bind.slot_dst_vec_loc == 0 && bind.model_dst_vec_loc == 6,
         "binding: record carries the derived namespaces");
   {
      struct r300_r2vb_position_source badsrc = src;
      badsrc.valid = false;
      CHECK(!r300_r2vb_producer_logical_binding_init(&badsrc, &fs, &rs, 0, 6,
                                                     &bind),
            "binding: an unmeasured source identity declines");
      badsrc = src;
      badsrc.app_driver_location = 0;
      badsrc.location_rank = 1;
      CHECK(!r300_r2vb_producer_logical_binding_init(&badsrc, &fs, &rs, 0, 6,
                                                     &bind),
            "binding: a source rank disagreeing with its driver location declines");
      badsrc.app_driver_location = 1;
      CHECK(r300_r2vb_producer_logical_binding_init(&badsrc, &fs, &rs, 0, 6,
                                                    &bind) &&
               bind.app_driver_location == 1 && bind.location_rank == 1 &&
               bind.velem_index == 1,
            "binding: a rank-one source identity reaches the logical binding");
      struct r300_rs_block badrs = rs;
      badrs.vap_vsm_vtx_assm = R300_INPUT_CNTL_POS;
      CHECK(!r300_r2vb_producer_logical_binding_init(&src, &fs, &badrs, 0, 6,
                                                     &bind),
            "binding: an RS assembly missing TC0 declines");
      badrs = rs;
      badrs.inst[0] = R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
                      R300_RS_INST_TEX_ADDR(1);
      CHECK(!r300_r2vb_producer_logical_binding_init(&src, &fs, &badrs, 0, 6,
                                                     &bind),
            "binding: RS writing another FS register declines");
   }

   /* Contract checker over a real transaction shape: FLOAT_4 slot +
    * FLOAT_3 model through the actual interface builder. */
   CHECK(r300_r2vb_producer_logical_binding_init(&src, &fs, &rs, 0, 6, &bind),
         "binding: rebuild for the checker rows");
   struct r300_r2vb_producer_streams st = { 0 };
   struct r300_r2vb_producer_fetch ft = { 0 };
   struct r300_r2vb_producer_interface it = { 0 };
   CHECK(r300_r2vb_producer_streams_init(0, 0, 12,
                                         PIPE_FORMAT_R32G32B32_FLOAT, 0,
                                         &st) &&
            r300_r2vb_producer_fetch_init(&st, 4, 64, 48, &ft) &&
            r300_r2vb_producer_interface_init(&ft, 0, 6, &it),
         "binding: checker transaction builds");
   CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &rs) == 0,
         "binding: the valid transaction reports zero violations");
   {
      struct r300_r2vb_producer_interface bad = it;
      bad.vap_vtx_size++;
      CHECK(r300_r2vb_producer_binding_check(&ft, &bad, &bind, &rs) &
               R300_R2VB_BINDING_FETCH_SIZE,
            "binding: a fetch-size drift reports FETCH_SIZE");
      bad = it;
      /* Flip the model element's W select from FP_ONE to W. */
      bad.prog_stream_cntl_ext[0] ^=
         (uint32_t)((R300_SWIZZLE_SELECT_FP_ONE ^ R300_SWIZZLE_SELECT_W)
                    << R300_SWIZZLE_SELECT_W_SHIFT) << 16;
      CHECK(r300_r2vb_producer_binding_check(&ft, &bad, &bind, &rs) &
               R300_R2VB_BINDING_SWIZZLE,
            "binding: an XYZW model swizzle on FLOAT_3 reports SWIZZLE");
      bad = it;
      bad.prog_stream_cntl[0] &= ~((uint32_t)R300_LAST_VEC << 16);
      CHECK(r300_r2vb_producer_binding_check(&ft, &bad, &bind, &rs) &
               R300_R2VB_BINDING_LAST_VEC,
            "binding: a missing model LAST_VEC reports LAST_VEC");
      bad = it;
      bad.prog_stream_cntl[3] = 1;
      CHECK(r300_r2vb_producer_binding_check(&ft, &bad, &bind, &rs) &
               R300_R2VB_BINDING_TAIL_STATE,
            "binding: a stale tail register reports TAIL_STATE");
      struct r300_rs_block badrs = rs;
      badrs.vap_out_vtx_fmt[1] = 0;
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_OUTPUT_FMT,
            "binding: a missing TEX0 output tuple reports OUTPUT_FMT");
      badrs = rs;
      badrs.ip[0] = R300_RS_SEL_S(R300_RS_SEL_C0) |
                    R300_RS_SEL_T(R300_RS_SEL_K0) |
                    R300_RS_SEL_R(R300_RS_SEL_C2) |
                    R300_RS_SEL_Q(R300_RS_SEL_C3);
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_RS_COMPONENTS,
            "binding: a constant-fed TC0 component reports RS_COMPONENTS");
      badrs = rs;
      badrs.vap_vtx_state_cntl = 0;
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_VAP_ASSEMBLY,
            "binding: a drifted VTX_STATE_CNTL reports VAP_ASSEMBLY");
      badrs = rs;
      badrs.inst[0] = R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
                      R300_RS_INST_TEX_ADDR(2);
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_FS_REGISTER,
            "binding: RS writing the wrong FS register reports FS_REGISTER");
      /* RS-exactness rows pinned to the calibration decode. */
      badrs = rs;
      badrs.count = R300_IT_COUNT(5) | R300_HIRES_EN;
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_RS_COMPONENTS,
            "binding: an inflated interpolator count reports RS_COMPONENTS");
      badrs = rs;
      badrs.inst_count = 1;
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_RS_COMPONENTS,
            "binding: a second RS instruction count reports RS_COMPONENTS");
      badrs = rs;
      badrs.ip[0] |= R300_RS_TEX_PTR(4);
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_RS_COMPONENTS,
            "binding: a nonzero RS texture pointer reports RS_COMPONENTS");
      badrs = rs;
      badrs.inst[0] |= R300_RS_INST_TEX_ID(1);
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_FS_REGISTER,
            "binding: a wrong interpolator TEX_ID reports FS_REGISTER");
      badrs = rs;
      badrs.ip[1] = R300_RS_SEL_S(R300_RS_SEL_C1);
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_TAIL_STATE,
            "binding: a stale extra RS entry reports TAIL_STATE");
      CHECK(!r300_r2vb_producer_logical_binding_init(&src, &fs, &badrs, 0, 6,
                                                     &bind),
            "binding: the constructor declines a stale extra RS entry");
      badrs = rs;
      badrs.gb_enable = R300_GB_POINT_STUFF_ENABLE |
                        (R300_GB_TEX_ST << R300_GB_TEX0_SOURCE_SHIFT);
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_GB_STATE,
            "binding: GB texture stuffing reports GB_STATE");
      CHECK(!r300_r2vb_producer_logical_binding_init(&src, &fs, &badrs, 0, 6,
                                                     &bind),
            "binding: the constructor declines GB texture stuffing");
      badrs = rs;
      uint32_t valid_prog_stream_cntl = it.prog_stream_cntl[0];
      it.prog_stream_cntl[0] |= (1u << R300_SKIP_DWORDS_SHIFT);
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_SWIZZLE,
            "binding: a stale slot skip count reports SWIZZLE");
      it.prog_stream_cntl[0] = valid_prog_stream_cntl |
                               (1u << (16 + R300_SKIP_DWORDS_SHIFT));
      CHECK(r300_r2vb_producer_binding_check(&ft, &it, &bind, &badrs) &
               R300_R2VB_BINDING_SWIZZLE,
            "binding: a stale model skip count reports SWIZZLE");
      it.prog_stream_cntl[0] = valid_prog_stream_cntl;
      CHECK(r300_r2vb_producer_logical_binding_init(&src, &fs, &rs, 0, 6,
                                                    &bind),
            "binding: the pristine block rebuilds after the negatives");
   }

   /* Runtime constructor: destinations decode from the derived stream
    * state -- the calibrated word 0x26030003 -- never from caller
    * literals. */
   {
      struct r300_r2vb_producer_plan plan;
      memset(&plan, 0, sizeof(plan));
      plan.status = R300_R2VB_PLAN_READY;
      plan.action = R300_R2VB_PLAN_SINGLE;
      plan.key.space = R300_R2VB_POSITION_WINDOW;
      plan.position_source.app_driver_location = 0;
      plan.position_source.location_rank = 0;
      plan.position_source.valid = true;
      struct r300_vertex_stream_state psc;
      memset(&psc, 0, sizeof(psc));
      psc.count = 1;
      psc.vap_prog_stream_cntl[0] = 0x26030003;
      psc.vap_prog_stream_cntl_ext[0] = 0xf688f688;
      struct r300_r2vb_producer_logical_binding rb;
      CHECK(r300_r2vb_producer_logical_binding_from_state(
               &plan, NULL, &fs, &rs, &psc, &rb),
            "from_state: the calibrated derived word builds the binding");
      CHECK(rb.slot_dst_vec_loc == R300_R2VB_CAL_SLOT_DST_VEC_LOC &&
               rb.model_dst_vec_loc == R300_R2VB_CAL_MODEL_DST_VEC_LOC &&
               rb.fs_hw_input_reg == 0 && rb.app_driver_location == 0 &&
               rb.location_rank == 0 && rb.velem_index == 0,
            "from_state: the position source identity stays at element zero");
      struct r300_r2vb_position_source model_source = {
         .app_driver_location = 1, .location_rank = 1, .valid = true
      };
      CHECK(r300_r2vb_producer_logical_binding_from_state(
               &plan, &model_source, &fs, &rs, &psc, &rb) &&
               rb.app_driver_location == 1 && rb.location_rank == 1 &&
               rb.velem_index == 1,
            "from_state: the selected model source identity reaches the binding");
      struct r300_vertex_stream_state bad = psc;
      bad.count = 2;
      CHECK(!r300_r2vb_producer_logical_binding_from_state(
               &plan, NULL, &fs, &rs, &bad, &rb),
            "from_state: a second register pair declines");
      bad = psc;
      bad.vap_prog_stream_cntl[0] &= ~((uint32_t)R300_LAST_VEC << 16);
      CHECK(!r300_r2vb_producer_logical_binding_from_state(
               &plan, NULL, &fs, &rs, &bad, &rb),
            "from_state: a model element without LAST_VEC declines");
      bad = psc;
      /* Slot vector drifted from 0 to 1. */
      bad.vap_prog_stream_cntl[0] = 0x26030103;
      CHECK(!r300_r2vb_producer_logical_binding_from_state(
               &plan, NULL, &fs, &rs, &bad, &rb),
            "from_state: a drifted slot destination vector declines");
      bad = psc;
      /* Model vector drifted from 6 to 5. */
      bad.vap_prog_stream_cntl[0] = 0x25030003;
      CHECK(!r300_r2vb_producer_logical_binding_from_state(
               &plan, NULL, &fs, &rs, &bad, &rb),
            "from_state: a drifted model destination vector declines");
      bad = psc;
      bad.vap_prog_stream_cntl[3] = 1;
      CHECK(!r300_r2vb_producer_logical_binding_from_state(
               &plan, NULL, &fs, &rs, &bad, &rb),
            "from_state: a stale stream tail register declines");
      struct r300_r2vb_producer_plan noplan = plan;
      noplan.position_source.valid = false;
      CHECK(!r300_r2vb_producer_logical_binding_from_state(
               &noplan, NULL, &fs, &rs, &psc, &rb),
            "from_state: an unmeasured plan source declines");

      /* The transaction receives the live PSC that the producer interface
       * reconstructs for the FLOAT_3 model stream.  The calibrated decode
       * fixture above remains a separate extraction test. */
      psc.vap_prog_stream_cntl[0] = 0x26020003;
      psc.vap_prog_stream_cntl_ext[0] = 0xfa88f688;

      /* The all-fallible transaction over the same authorities: a
       * CPU-shadow model source, a slot BO sized for the layout, and
       * the derived-state binding, through the real validate phase. */
      printf("producer bo-draw transaction:\n");
      /* The faithful uploader stack; check_upload_integration wires the
       * same hooks, and this section may run first. */
      g_context.context.screen = &g_screen.screen;
      g_screen.screen.resource_create = fake_resource_create;
      g_screen.screen.resource_destroy = fake_resource_destroy;
      g_context.context.buffer_map = fake_buffer_map;
      g_context.context.buffer_unmap = fake_buffer_unmap;
      g_context.context.transfer_flush_region = fake_transfer_flush_region;
      g_context.context.resource_release = u_default_resource_release;
      if (!g_context.uploader)
         g_context.uploader = u_upload_create(&g_context.context, 128 * 1024,
                                              PIPE_BIND_CUSTOM,
                                              PIPE_USAGE_STREAM, 0);
      plan.num_position_inputs = 1;
      enum { TSTRIDE = 12, TCOUNT = 5 };
      struct r300_resource shadow;
      memset(&shadow, 0, sizeof(shadow));
      shadow.b.width0 = TCOUNT * TSTRIDE;
      pipe_reference_init(&shadow.b.reference, 1);
      uint8_t *bytes = calloc(1, shadow.b.width0);
      shadow.malloced_buffer = bytes;
      struct pipe_vertex_buffer vb = { .buffer_offset = 0,
                                       .buffer.resource = &shadow.b };
      struct pipe_vertex_element ve = { .src_offset = 0,
                                        .src_stride = TSTRIDE,
                                        .src_format =
                                           PIPE_FORMAT_R32G32B32_FLOAT };
      struct r300_r2vb_slot_layout txn_layout;
      CHECK(r300_r2vb_slot_layout_init_policy(
               TCOUNT, R300_R2VB_LAYOUT_LEGACY_ROW, &txn_layout),
            "txn: the output layout is valid");
      struct fake_buffer *slotfb = (struct fake_buffer *)fake_resource_create(
         &g_screen.screen, &(struct pipe_resource){ .width0 = 4096 * 16 });
      /* Output authority fixture: a CPU-shadow color target bound as the
       * framebuffer's first color buffer, one row of FP32x4 texels. */
      struct r300_resource outshadow;
      memset(&outshadow, 0, sizeof(outshadow));
      outshadow.b.width0 = (uint32_t)txn_layout.storage_bytes;
      outshadow.b.height0 = 1;
      outshadow.b.format = PIPE_FORMAT_R32G32B32A32_FLOAT;
      pipe_reference_init(&outshadow.b.reference, 1);
      uint8_t *outbytes = calloc(1, (size_t)txn_layout.storage_bytes);
      outshadow.malloced_buffer = outbytes;
      outshadow.buf = (struct pb_buffer_lean *)outbytes;
      outshadow.domain = RADEON_DOMAIN_GTT;
      struct pipe_framebuffer_state tfb;
      memset(&tfb, 0, sizeof(tfb));
      tfb.width = txn_layout.width;
      tfb.height = txn_layout.height;
      tfb.nr_cbufs = 1;
      tfb.cbufs[0].texture = &outshadow.b;
      g_context.fb_state.state = &tfb;
      struct r300_r2vb_producer_bo_draw txn;
      r300_r2vb_producer_bo_draw_init(&txn);
      /* Transaction-owned gate: gate off declines as the first fallible
       * operation, before the model upload retains anything. */
      unsetenv("R300_R2VB_SLOT_FETCH");
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource &&
               txn.state == R300_R2VB_BO_DRAW_EMPTY,
            "txn: gate off declines before any upload");
      setenv("R300_R2VB_SLOT_FETCH", "1", 1);
      CHECK(r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn),
            "txn: the complete validate phase builds the transaction");
      CHECK(txn.slot_resource == &slotfb->r.b &&
               txn.output_resource == &outshadow.b && txn.model.resource &&
               txn.count == TCOUNT &&
               txn.state == R300_R2VB_BO_DRAW_VALIDATED &&
               txn.output_required_bytes == txn_layout.storage_bytes &&
               txn.output_valid_bytes == (uint64_t)TCOUNT * 16 &&
               txn.output_offset == 0 &&
               txn.output_pitch_pixels == txn.layout.pitch_pixels &&
               txn.required_cs_dwords ==
                  r300_r2vb_producer_bo_draw_cs_dwords(),
            "txn: storage referenced, output authority fixed, CS size fixed");
      CHECK(memcmp(&txn.psc_snapshot, &psc, sizeof(psc)) == 0 &&
               memcmp(&txn.rs_snapshot, &rs, sizeof(rs)) == 0,
            "txn: the derived-state words are frozen by value");
      CHECK(r300_r2vb_producer_binding_check(&txn.fetch, &txn.psc,
                                             &txn.logical, &rs) == 0,
            "txn: the built transaction passes the full contract check");
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn),
            "txn: validate into an owned transaction declines");
      r300_r2vb_producer_bo_draw_fini(&txn);
      CHECK(txn.slot_resource == NULL && txn.model.resource == NULL &&
               txn.output_resource == NULL &&
               txn.state == R300_R2VB_BO_DRAW_EMPTY,
            "txn: fini releases all three references and returns to EMPTY");

      /* Multirow transaction: the first count above the legacy-row ceiling
       * allocates three physical rows.  Validation distinguishes the valid
       * 4097-record prefix from the 2047 poisonable padding records. */
      {
         enum { GRID_COUNT = 4097 };
         struct r300_r2vb_slot_layout grid_layout;
         CHECK(r300_r2vb_slot_layout_init_policy(
                  GRID_COUNT, R300_R2VB_LAYOUT_GRID_2048, &grid_layout),
               "txn grid: the 2048-wide multirow layout is valid");
         struct r300_resource grid_shadow;
         memset(&grid_shadow, 0, sizeof(grid_shadow));
         grid_shadow.b.width0 = GRID_COUNT * TSTRIDE;
         pipe_reference_init(&grid_shadow.b.reference, 1);
         uint8_t *grid_model_bytes = calloc(1, grid_shadow.b.width0);
         grid_shadow.malloced_buffer = grid_model_bytes;
         struct pipe_vertex_buffer grid_vb = {
            .buffer_offset = 0,
            .buffer.resource = &grid_shadow.b,
         };
         struct fake_buffer *grid_slot =
            (struct fake_buffer *)fake_resource_create(
               &g_screen.screen,
               &(struct pipe_resource){
                  .width0 = (uint32_t)grid_layout.storage_bytes,
               });
         struct r300_resource grid_output;
         memset(&grid_output, 0, sizeof(grid_output));
         grid_output.b.width0 = (uint32_t)grid_layout.storage_bytes;
         grid_output.b.height0 = 1;
         grid_output.b.format = PIPE_FORMAT_R32G32B32A32_FLOAT;
         pipe_reference_init(&grid_output.b.reference, 1);
         uint8_t *grid_output_bytes =
            calloc(1, (size_t)grid_layout.storage_bytes);
         grid_output.malloced_buffer = grid_output_bytes;
         grid_output.buf = (struct pb_buffer_lean *)grid_output_bytes;
         grid_output.domain = RADEON_DOMAIN_GTT;
         tfb.width = grid_layout.width;
         tfb.height = grid_layout.height;
         tfb.cbufs[0].texture = &grid_output.b;
         r300_r2vb_producer_bo_draw_init(&txn);
         CHECK(grid_model_bytes && grid_slot && grid_output_bytes &&
                  r300_r2vb_producer_bo_draw_validate(
                     &g_context, &plan, &fs, &rs, &psc, &grid_vb, &ve, 1, 1,
                     &grid_layout, &grid_slot->r.b, &grid_output.b, 0,
                     GRID_COUNT, R300_R2VB_POSITION_WINDOW, NULL, &txn),
               "txn grid: validation accepts the complete physical extent");
         CHECK(txn.state == R300_R2VB_BO_DRAW_VALIDATED &&
                  txn.layout.width == 2048 && txn.layout.height == 3 &&
                  txn.output_valid_bytes == (uint64_t)GRID_COUNT * 16 &&
                  txn.output_required_bytes == grid_layout.storage_bytes &&
                  txn.output_required_bytes - txn.output_valid_bytes ==
                     (uint64_t)2047 * 16,
               "txn grid: valid prefix and poisonable padding stay distinct");
         r300_r2vb_producer_bo_draw_fini(&txn);
         if (grid_slot) {
            struct pipe_resource *grid_slot_resource = &grid_slot->r.b;
            pipe_resource_reference(&grid_slot_resource, NULL);
         }
         free(grid_output_bytes);
         free(grid_model_bytes);
         tfb.width = txn_layout.width;
         tfb.height = txn_layout.height;
         tfb.cbufs[0].texture = &outshadow.b;
      }

      /* Failure edges: each fallible layer declines and leaves no
       * retained storage. */
      struct r300_r2vb_producer_plan rejected_plan = plan;
      rejected_plan.status = R300_R2VB_PLAN_SEMANTIC_REJECT;
      rejected_plan.action = R300_R2VB_PLAN_REJECT;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &rejected_plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource && !txn.output_resource &&
               txn.state == R300_R2VB_BO_DRAW_EMPTY,
            "txn: a rejected plan declines before model materialization");
      struct r300_r2vb_producer_plan typed_plan = plan;
      typed_plan.has_typed_source = true;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &typed_plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource && !txn.output_resource &&
               txn.state == R300_R2VB_BO_DRAW_EMPTY,
            "txn: a typed plan declines before model materialization");
      struct r300_r2vb_producer_plan wrong_space_plan = plan;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &wrong_space_plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_CLIP, NULL, &txn) &&
               !txn.model.resource && !txn.output_resource &&
               txn.state == R300_R2VB_BO_DRAW_EMPTY,
            "txn: a caller space different from the plan key declines");
      struct r300_r2vb_producer_plan badplan = plan;
      badplan.position_source.valid = false;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &badplan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource,
            "txn: an unmeasured source declines with nothing retained");
      struct pipe_vertex_element badve = ve;
      badve.src_format = PIPE_FORMAT_R32G32_FLOAT;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &badve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn),
            "txn: an inadmissible element format declines");
      struct fake_buffer *tiny = (struct fake_buffer *)fake_resource_create(
         &g_screen.screen, &(struct pipe_resource){ .width0 = 16 });
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &tiny->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource,
            "txn: an undersized slot BO declines and releases the model");
      struct r300_vertex_stream_state badpsc = psc;
      badpsc.vap_prog_stream_cntl[0] = 0x25030003;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &badpsc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource,
            "txn: a drifted derived binding declines after materialization");
      badpsc = psc;
      badpsc.vap_prog_stream_cntl[0] |= 1u << R300_SKIP_DWORDS_SHIFT;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &badpsc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource,
            "txn: a live PSC skip count declines after reconstruction");
      badpsc = psc;
      badpsc.vap_prog_stream_cntl_ext[0] ^=
         1u << R300_SWIZZLE_SELECT_X_SHIFT;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &badpsc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource,
            "txn: a live PSC EXT mismatch declines after reconstruction");
      /* Output-authority negatives: framebuffer identity, extent, and
       * format each decline after materialization with nothing retained. */
      tfb.cbufs[0].texture = &slotfb->r.b;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource && !txn.output_resource,
            "txn: an output that is not the bound color target declines");
      tfb.cbufs[0].texture = &outshadow.b;
      tfb.width = txn_layout.width - 1;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource,
            "txn: a one-pixel-short framebuffer extent declines");
      tfb.width = txn_layout.width;
      outshadow.b.width0 = (uint32_t)txn_layout.storage_bytes - 1;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource,
            "txn: one byte short of the physical storage declines");
      outshadow.b.width0 = (uint32_t)txn_layout.storage_bytes;
      outshadow.b.format = PIPE_FORMAT_R8G8B8A8_UNORM;
      CHECK(!r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn) &&
               !txn.model.resource,
            "txn: an output format outside FP32x4 declines");
      outshadow.b.format = PIPE_FORMAT_R32G32B32A32_FLOAT;

      /* CS staging and mechanical emission through the fake winsys. */
      printf("producer bo-draw CS staging + emission:\n");
      static uint32_t csbuf[256];
      g_fake_winsys.cs_add_buffer = fws_add_buffer;
      g_fake_winsys.cs_lookup_buffer = fws_lookup_buffer;
      g_fake_winsys.cs_validate = fws_validate;
      g_fake_winsys.cs_check_space = fws_check_space;
      g_context.rws = &g_fake_winsys;
      g_context.cs.current.buf = csbuf;
      g_context.cs.current.max_dw = 256;
      g_context.cs.current.cdw = 0;
      memset(&g_fws, 0, sizeof(g_fws));
      r300_r2vb_producer_bo_draw_init(&txn);
      CHECK(r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn),
            "cs: validate builds the transaction for staging");
      CHECK(!r300_r2vb_producer_bo_draw_emit(&g_context, &txn),
            "cs: emission before READY declines");
      unsigned stage_cdw = g_context.cs.current.cdw;
      CHECK(r300_r2vb_producer_bo_draw_stage_cs(&g_context, &txn, &plan, &fs,
                                                &rs, &psc),
            "cs: staging validates the complete buffer list");
      CHECK(txn.state == R300_R2VB_BO_DRAW_READY &&
               txn.slot_reloc_index >= 0 && txn.model_reloc_index >= 0 &&
               txn.output_reloc_index >= 0 && g_fws.validates == 1 &&
               g_context.cs.current.cdw == stage_cdw,
            "cs: READY holds validated relocation indices, zero registers");
      CHECK(!r300_r2vb_producer_bo_draw_stage_cs(&g_context, &txn, &plan, &fs,
                                                 &rs, &psc),
            "cs: staging twice declines");
      unsigned emit_cdw = g_context.cs.current.cdw;
      CHECK(r300_r2vb_producer_bo_draw_emit(&g_context, &txn),
            "cs: emission completes mechanically from READY");
      CHECK(g_context.cs.current.cdw - emit_cdw ==
               r300_r2vb_producer_bo_draw_cs_dwords() &&
               txn.state == R300_R2VB_BO_DRAW_EMITTED,
            "cs: the custom range is exactly the fixed dword count");
      CHECK(csbuf[emit_cdw + 1] == psc.vap_prog_stream_cntl[0] &&
               csbuf[emit_cdw + r300_r2vb_producer_bo_draw_cs_dwords() - 1] ==
                  ((TCOUNT << R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
                   R300_VAP_VF_CNTL__PRIM_POINTS |
                   R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST),
            "cs: first stream word and draw word carry the snapshot");
      CHECK(!r300_r2vb_producer_bo_draw_emit(&g_context, &txn),
            "cs: emitting twice declines");
      r300_r2vb_producer_bo_draw_fini(&txn);

      /* A first validation failure retries the complete population; the
       * retry re-adds both the ordinary state buffers and the three
       * producer BOs, so the relocation indices come from the final CS. */
      r300_r2vb_producer_bo_draw_init(&txn);
      CHECK(r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn),
            "cs: validate for the retry row");
      memset(&g_fws, 0, sizeof(g_fws));
      g_fws.fail_validates = 1;
      CHECK(r300_r2vb_producer_bo_draw_stage_cs(&g_context, &txn, &plan, &fs,
                                                &rs, &psc) &&
               txn.state == R300_R2VB_BO_DRAW_READY && g_fws.validates == 2 &&
               txn.slot_reloc_index >= 0,
            "cs: a validation flush retries the complete population once");
      r300_r2vb_producer_bo_draw_fini(&txn);

      /* A second validation failure declines cleanly: VALIDATED state
       * retained, zero registers written. */
      r300_r2vb_producer_bo_draw_init(&txn);
      CHECK(r300_r2vb_producer_bo_draw_validate(
               &g_context, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1,
               &txn_layout, &slotfb->r.b, &outshadow.b, 0, TCOUNT,
               R300_R2VB_POSITION_WINDOW, NULL, &txn),
            "cs: validate for the double-failure row");
      memset(&g_fws, 0, sizeof(g_fws));
      g_fws.fail_validates = 2;
      stage_cdw = g_context.cs.current.cdw;
      CHECK(!r300_r2vb_producer_bo_draw_stage_cs(&g_context, &txn, &plan, &fs,
                                                 &rs, &psc) &&
               txn.state == R300_R2VB_BO_DRAW_VALIDATED &&
               g_context.cs.current.cdw == stage_cdw,
            "cs: a second validation failure declines with zero registers");
      /* Mutable contents changed under the same pointer: the by-value
       * snapshot catches the drift and staging declines. */
      memset(&g_fws, 0, sizeof(g_fws));
      rs.ip[3] ^= 0x1;
      CHECK(!r300_r2vb_producer_bo_draw_stage_cs(&g_context, &txn, &plan, &fs,
                                                 &rs, &psc),
            "cs: RS contents drift under the same pointer declines");
      rs.ip[3] ^= 0x1;
      psc.vap_prog_stream_cntl[1] ^= 0x1;
      CHECK(!r300_r2vb_producer_bo_draw_stage_cs(&g_context, &txn, &plan, &fs,
                                                 &rs, &psc),
            "cs: PSC contents drift under the same pointer declines");
      psc.vap_prog_stream_cntl[1] ^= 0x1;
      r300_r2vb_producer_bo_draw_fini(&txn);
      g_context.rws = NULL;
      g_context.cs.current.buf = NULL;
      g_context.cs.current.max_dw = 0;
      g_context.cs.current.cdw = 0;

      g_context.fb_state.state = NULL;
      unsetenv("R300_R2VB_SLOT_FETCH");
      pipe_resource_reference(&(struct pipe_resource *){ &tiny->r.b }, NULL);
      pipe_resource_reference(&(struct pipe_resource *){ &slotfb->r.b }, NULL);
      free(outbytes);
      free(bytes);
      /* The upload-integration section creates its own uploader; a
       * dangling one here would leak its buffer pool. */
      u_upload_destroy(g_context.uploader);
      g_context.uploader = NULL;
   }
}

static void
check_position_mapping_and_interface(void)
{
   /* Position-input mapping: one plan input reads location 0 = velem[0];
    * the classifier proves the element, binding, and format identity. */
   CHECK(r300_r2vb_position_input_mapping_ok(
            1, 0, 0, 2, 0, 1, true, PIPE_FORMAT_R32G32B32_FLOAT),
         "mapping: single input on bound float3 element admits");
   CHECK(r300_r2vb_position_input_mapping_ok(
            1, 0, 0, 1, 0, 1, true, PIPE_FORMAT_R32G32B32A32_FLOAT),
         "mapping: float4 element admits");
   CHECK(!r300_r2vb_position_input_mapping_ok(
            2, 0, 0, 2, 0, 1, true, PIPE_FORMAT_R32G32B32_FLOAT),
         "mapping: multi-input plan declines");
   CHECK(!r300_r2vb_position_input_mapping_ok(
            1, 0, 0, 0, 0, 1, true, PIPE_FORMAT_R32G32B32_FLOAT),
         "mapping: missing element declines");
   CHECK(!r300_r2vb_position_input_mapping_ok(
            1, 0, 0, 1, 1, 1, true, PIPE_FORMAT_R32G32B32_FLOAT),
         "mapping: out-of-range buffer binding declines");
   CHECK(!r300_r2vb_position_input_mapping_ok(
            1, 0, 0, 1, 0, 1, false, PIPE_FORMAT_R32G32B32_FLOAT),
         "mapping: unbound buffer declines");
   CHECK(!r300_r2vb_position_input_mapping_ok(
            1, 0, 0, 1, 0, 1, true, PIPE_FORMAT_R32G32_FLOAT),
         "mapping: format outside the admitted families declines");

   /* Interface builder: PSC words from the shared translators, both
    * elements in the first register pair, LAST_VEC on the model, unused
    * registers zeroed, VAP_VTX_SIZE carried from the fetch object. */
   struct r300_r2vb_producer_streams st = {0};
   struct r300_r2vb_producer_fetch ft = {0};
   struct r300_r2vb_producer_interface it = {0};
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
   /* Semantic swizzle oracle: decode the translator's word rather than
    * comparing encodings, so the test proves the MEANING -- FLOAT_4 is
    * the XYZW identity, FLOAT_3 routes W from the constant-one select --
    * with the full write mask on both. */
   {
      uint16_t s4 = it.prog_stream_cntl_ext[0] & 0xffff;
      uint16_t s3 = it.prog_stream_cntl_ext[0] >> 16;
      unsigned mask4 = (s4 >> R300_WRITE_ENA_SHIFT) & 0xf;
      unsigned mask3 = (s3 >> R300_WRITE_ENA_SHIFT) & 0xf;
      CHECK(((s4 >> R300_SWIZZLE_SELECT_X_SHIFT) & 7) ==
               R300_SWIZZLE_SELECT_X &&
            ((s4 >> R300_SWIZZLE_SELECT_Y_SHIFT) & 7) ==
               R300_SWIZZLE_SELECT_Y &&
            ((s4 >> R300_SWIZZLE_SELECT_Z_SHIFT) & 7) ==
               R300_SWIZZLE_SELECT_Z &&
            ((s4 >> R300_SWIZZLE_SELECT_W_SHIFT) & 7) ==
               R300_SWIZZLE_SELECT_W && mask4 == 0xf,
            "interface: float4 swizzle decodes as the XYZW identity");
      CHECK(((s3 >> R300_SWIZZLE_SELECT_X_SHIFT) & 7) ==
               R300_SWIZZLE_SELECT_X &&
            ((s3 >> R300_SWIZZLE_SELECT_Y_SHIFT) & 7) ==
               R300_SWIZZLE_SELECT_Y &&
            ((s3 >> R300_SWIZZLE_SELECT_Z_SHIFT) & 7) ==
               R300_SWIZZLE_SELECT_Z &&
            ((s3 >> R300_SWIZZLE_SELECT_W_SHIFT) & 7) ==
               R300_SWIZZLE_SELECT_FP_ONE && mask3 == 0xf,
            "interface: float3 swizzle decodes as XYZ1");
   }
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
   printf("producer logical binding:\n");
   check_logical_binding();
   printf("position source rank oracle:\n");
   check_position_source_rank();

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
   printf("auto-single mode compatibility:\n");
   check_mode_compatibility();
   printf("auto-single policy matrix:\n");
   check_policy_matrix();
   printf("auto-single producer input preflight:\n");
   check_producer_input_preflight();
   check_source_domain_matrix();

   /* R300_R2VB_STANDING opens on the exact value 1; unset, empty, zero,
    * and boolean aliases stay closed. */
   CHECK(r300_r2vb_standing_gate_value("1"),
         "standing gate: exact 1 arms");
   CHECK(!r300_r2vb_standing_gate_value(NULL),
         "standing gate: unset stays closed");
   CHECK(!r300_r2vb_standing_gate_value(""),
         "standing gate: empty stays closed");
   CHECK(!r300_r2vb_standing_gate_value("0"),
         "standing gate: zero stays closed");
   CHECK(!r300_r2vb_standing_gate_value("true"),
         "standing gate: boolean alias stays closed");
   CHECK(!r300_r2vb_standing_gate_value("11"),
         "standing gate: prefix match stays closed");
   printf("auto-single delivery element preflight:\n");
   check_delivery_element_preflight();
   printf("auto-single output stream preflight:\n");
   check_output_stream_preflight();
   printf("passthrough upload extent:\n");
   check_passthrough_upload_extent();
   printf("constant-source contract:\n");
   check_constant_source_contract();
   printf("re-ingest vertex-array restore:\n");
   check_vertex_array_restore();
   printf("SWTCL application vertex-size restoration:\n");
   check_swtcl_vertex_size_restore();
   printf("auto-single live plans:\n");
   check_live_plans();

   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
