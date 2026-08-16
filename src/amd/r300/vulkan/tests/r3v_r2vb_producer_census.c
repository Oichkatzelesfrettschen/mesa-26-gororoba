/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Host-only resource census over the R2VB producer corpus: every specimen
 * runs through the production plan chain (restage, optimize, backend
 * measurement, cut walk) and emits one machine-readable row carrying the
 * plan verdict, the backend cost vectors {alu, temps, consts} for the
 * baseline and both split halves, and the RC statistics line of every
 * successful compile the planner ran (vector/scalar instruction counts,
 * presubtract and omod usage, temps, consts, literals, cycles).
 *
 * The statistics ride the existing shader-db path: rc_run_compiler prints
 * rc_get_stats through the context debug callback after every pass run that
 * reaches emit, so the census registers a capture callback on the fake
 * context and the production compile code stays untouched.  A compile that
 * dies at the emit ceiling produces no statistics line; its census datum is
 * the plan's over-budget classification itself, and on a SPLIT row the
 * per-instruction statistics therefore describe the admitted halves, never
 * the original over-budget producer.
 *
 * Statistics records carry a phase label.  phase=walk is the planner's own
 * candidate-walk compile sequence, diagnostic because a walk position does
 * not identify which candidate or half produced it.  The programs the plan
 * actually selected -- the SINGLE baseline, or both halves rebuilt from the
 * retained partition -- are then recompiled explicitly under
 * phase=selected-baseline / selected-pass-a / selected-pass-b, and those
 * named records are the mining surface.  Capture is fail-closed: a dropped
 * record (capacity) or a truncated line fails the row, and the determinism
 * check covers the full statistics transcript, not only the summary row.
 *
 * Census rows feed the compaction rule selection in
 * docs/hardware/rs482-producer-alu-compaction-design.md: which shapes occur,
 * which are ALU-bound rather than temp or constant bound, and how the
 * vector/scalar pipes pack.  The verdict expectations double as calibration:
 * the fitting control plans SINGLE, the over-budget chain plans SPLIT, and
 * the typed corpus rows reproduce the route-chain oracle's verdicts, so a
 * census run that drifts from the oracle fails instead of mining wrong data.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "r3v_device.h"
#include "r3v_pipeline.h"
#include "r3v_private.h"
#include "r3v_shader_module.h"

#include "vulkan/runtime/vk_instance.h"
#include "vulkan/runtime/vk_physical_device.h"

#include "r300_context.h"
#include "r300_fs.h"
#include "compiler/r300_nir.h"
#include "r300_r2vb_plan.h"
#include "r300_screen.h"
#include "radeon_regalloc.h"

#include "typed_carry_corpus/r3v_typed_carry_spirv.h"

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
static struct vk_instance g_vk_instance;
static struct vk_physical_device g_vk_pdev;
static struct r3v_device g_r3v_device;

/* Statistics lines captured from one planner invocation.  rc_run_compiler
 * emits one SHADER_INFO line per compile that survives to emit, in compile
 * order: the baseline position producer first, then each candidate's carry
 * and position halves.  Capture is fail-closed: a record past the capacity
 * or a line the buffer clips sets a flag the row check fails on, so the
 * census never silently under-reports. */
#define CENSUS_MAX_STATS 32
static char g_stats[CENSUS_MAX_STATS][224];
static unsigned g_stats_count;
static bool g_stats_overflow;
static bool g_stats_truncated;

static void
capture_debug_message(void *data, unsigned *id, enum util_debug_type type,
                      const char *fmt, va_list args)
{
   (void)data;
   (void)id;
   if (type != UTIL_DEBUG_TYPE_SHADER_INFO)
      return;
   if (g_stats_count >= CENSUS_MAX_STATS) {
      g_stats_overflow = true;
      return;
   }
   int written = vsnprintf(g_stats[g_stats_count], sizeof(g_stats[0]), fmt,
                           args);
   if (written < 0 || (size_t)written >= sizeof(g_stats[0]))
      g_stats_truncated = true;
   g_stats_count++;
}

static void
stats_reset(void)
{
   g_stats_count = 0;
   g_stats_overflow = false;
   g_stats_truncated = false;
}

/* Per-attempt transcript of every captured statistics line with its phase
 * label; the determinism check compares the two attempts' transcripts whole,
 * so the RC statistics are covered, not only the summary row. */
static char g_transcript[2][32768];

static void
transcript_append_stats(char *transcript, size_t len, const char *phase)
{
   size_t off = strlen(transcript);
   for (unsigned i = 0; i < g_stats_count; i++) {
      if (off >= len - 1) {
         g_stats_truncated = true;
         break;
      }
      int written = snprintf(transcript + off, len - off,
                             "phase=%s seq=%u %s\n", phase, i, g_stats[i]);
      if (written < 0 || (size_t)written >= len - off) {
         g_stats_truncated = true;
         break;
      }
      off += (size_t)written;
   }
}

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
   g_context.context.debug.debug_message = capture_debug_message;
   rc_init_regalloc_state(&g_context.fs_regalloc_state, RC_FRAGMENT_PROGRAM);
   g_context.viewport.scale[0] = 320.0f;
   g_context.viewport.scale[1] = 240.0f;
   g_context.viewport.scale[2] = 0.5f;
   g_context.viewport.translate[0] = 320.0f;
   g_context.viewport.translate[1] = 240.0f;
   g_context.viewport.translate[2] = 0.5f;

   memset(&g_vk_instance, 0, sizeof(g_vk_instance));
   vk_object_base_instance_init(&g_vk_instance, &g_vk_instance.base,
                                VK_OBJECT_TYPE_INSTANCE);
   list_inithead(&g_vk_instance.debug_utils.instance_callbacks);
   list_inithead(&g_vk_instance.debug_utils.callbacks);
   g_vk_instance.app_info.api_version = VK_API_VERSION_1_0;
   memset(&g_vk_pdev, 0, sizeof(g_vk_pdev));
   vk_object_base_instance_init(&g_vk_instance, &g_vk_pdev.base,
                                VK_OBJECT_TYPE_PHYSICAL_DEVICE);
   g_vk_pdev.instance = &g_vk_instance;
   memset(&g_r3v_device, 0, sizeof(g_r3v_device));
   vk_object_base_init(&g_r3v_device.vk, &g_r3v_device.vk.base,
                       VK_OBJECT_TYPE_DEVICE);
   g_r3v_device.vk.physical = &g_vk_pdev;
   g_r3v_device.screen = (struct pipe_screen *)&g_screen;
}

static nir_shader *
prepare_module(const uint32_t *words, size_t size_bytes)
{
   struct r3v_shader_module *mod = calloc(1, sizeof(*mod) + size_bytes);
   if (!mod)
      return NULL;
   mod->base.type = VK_OBJECT_TYPE_SHADER_MODULE;
   mod->code_size = size_bytes;
   memcpy(mod->code, words, size_bytes);

   const VkPipelineShaderStageCreateInfo stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = r3v_shader_module_to_handle(mod),
      .pName = "main",
   };

   nir_shader *nir = NULL;
   VkResult result = r3v_prepare_shader_nir(&g_r3v_device, &stage_info,
                                            NULL, NULL, &nir);
   free(mod);
   return result == VK_SUCCESS ? nir : NULL;
}

/* Synthetic anchors built on the screen's production vertex options: a
 * fitting control and the recur-style dependent chain the corpus modules
 * embed, so the census spans the fits / over-budget boundary with known
 * verdicts. */
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
      "%s",
      name);
   nir_variable *in_pos = nir_variable_create(
      v.b.shader, nir_var_shader_in, glsl_vec4_type(), "in_pos");
   in_pos->data.location = VERT_ATTRIB_GENERIC0;
   in_pos->data.driver_location = 0;
   v.out_pos = nir_variable_create(v.b.shader, nir_var_shader_out,
                                   glsl_vec4_type(), "gl_Position");
   v.out_pos->data.location = VARYING_SLOT_POS;
   v.out_pos->data.driver_location = 0;

   /* The one-field std430 interface shape the in-driver producers declare;
    * r300_optimize_nir sizes the uniform block from interface_type. */
   const struct glsl_type *ubo_type = glsl_array_type(glsl_vec4_type(), 4, 16);
   nir_variable *ubo = nir_variable_create(v.b.shader, nir_var_mem_ubo,
                                           ubo_type, "matrix");
   ubo->data.binding = 0;
   ubo->data.driver_location = 0;
   ubo->data.explicit_binding = 1;
   struct glsl_struct_field ubo_field = {
      .type = ubo_type,
      .name = "data",
      .location = -1,
   };
   ubo->interface_type = glsl_interface_type(
      &ubo_field, 1, GLSL_INTERFACE_PACKING_STD430, false,
      "__r3v_census_ubo");
   v.b.shader->info.num_ubos = 1;

   v.pos = nir_load_var(&v.b, in_pos);
   return v;
}

static nir_shader *
end_vs(struct vs_build *v, nir_def *result)
{
   nir_store_var(&v->b, v->out_pos, result, 0xf);
   nir_validate_shader(v->b.shader, "r2vb producer census VS");
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
build_float_fits(void)
{
   struct vs_build v = begin_vs("census_float_fits");
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 8);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

static nir_shader *
build_float_over_budget(void)
{
   struct vs_build v = begin_vs("census_float_over");
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), 90);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

struct census_row {
   const char *name;
   /* Exactly one of the two sources is set. */
   const uint32_t *spirv;
   size_t spirv_size;
   nir_shader *(*build)(void);
   enum r300_r2vb_plan_action action;
   enum r300_r2vb_plan_reason primary_reason;
   /* Prepared-producer contract: the typed scan reads the restaged position
    * candidate, so these fields describe the producer the row measures. */
   bool prepared_has_typed;
   enum r300_r2vb_typed_source_class typed_source_class;
   /* SPLIT carry contract: the required transport and the complete allowed
    * transport alphabet for the selected partition. */
   char required_carry;
   const char *allowed_carry;
};

static const struct census_row rows[] = {
   { "census_float_fits", NULL, 0, build_float_fits,
     R300_R2VB_PLAN_SINGLE, R300_R2VB_PLAN_OK,
     false, R300_R2VB_TYPED_SOURCE_NONE, 0, "" },
   { "census_float_over", NULL, 0, build_float_over_budget,
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     false, R300_R2VB_TYPED_SOURCE_NONE, 0, "f" },
   { "t_bool", t_bool_spirv, sizeof(t_bool_spirv), NULL,
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     false, R300_R2VB_TYPED_SOURCE_NONE, 0, "f" },
   { "t_sint_exact", t_sint_exact_spirv, sizeof(t_sint_exact_spirv), NULL,
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     true, R300_R2VB_TYPED_SOURCE_SINT, 'i', "fi" },
   { "t_uint_exact", t_uint_exact_spirv, sizeof(t_uint_exact_spirv), NULL,
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     true, R300_R2VB_TYPED_SOURCE_UINT, 'u', "fu" },
   { "t_sint_pos_outside", t_sint_pos_outside_spirv,
     sizeof(t_sint_pos_outside_spirv), NULL,
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_SIGNED_RANGE,
     true, R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
   { "t_sint_neg_outside", t_sint_neg_outside_spirv,
     sizeof(t_sint_neg_outside_spirv), NULL,
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_SIGNED_RANGE,
     true, R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
   { "t_uint_outside", t_uint_outside_spirv, sizeof(t_uint_outside_spirv),
     NULL, R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_UNSIGNED_RANGE,
     true, R300_R2VB_TYPED_SOURCE_UINT, 0, "" },
   { "t_uint_unbounded", t_uint_unbounded_spirv,
     sizeof(t_uint_unbounded_spirv), NULL,
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_UNSIGNED_RANGE,
     true, R300_R2VB_TYPED_SOURCE_UINT, 0, "" },
   { "t_sint_to_uint", t_sint_to_uint_spirv, sizeof(t_sint_to_uint_spirv),
     NULL, R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_MIXED_SIGNEDNESS,
     true, R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
   { "t_uint_to_sint", t_uint_to_sint_spirv, sizeof(t_uint_to_sint_spirv),
     NULL, R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_MIXED_SIGNEDNESS,
     true, R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
};

/* Stable names for the machine-readable row: enum renumbering must never
 * silently reinterpret archived census output. */
static const char *
plan_status_str(enum r300_r2vb_plan_status status)
{
   switch (status) {
   case R300_R2VB_PLAN_READY:             return "ready";
   case R300_R2VB_PLAN_SEMANTIC_REJECT:   return "semantic_reject";
   case R300_R2VB_PLAN_TRANSIENT_FAILURE: return "transient_failure";
   }
   return "unknown";
}

static const char *
typed_class_str(enum r300_r2vb_typed_source_class source_class)
{
   switch (source_class) {
   case R300_R2VB_TYPED_SOURCE_NONE: return "none";
   case R300_R2VB_TYPED_SOURCE_BOOL: return "bool";
   case R300_R2VB_TYPED_SOURCE_SINT: return "sint";
   case R300_R2VB_TYPED_SOURCE_UINT: return "uint";
   }
   return "unknown";
}

static void
carry_sig(const struct r300_r2vb_producer_plan *plan, char *buf, size_t len)
{
   unsigned n = 0;
   for (unsigned i = 0; i < plan->partition.num_bases && n + 1 < len; i++) {
      /* Keep the carry schema component-sized: a vector base contributes one
       * transport letter for every scalar lane, matching the retained-corpus
       * census formatter. */
      char type;
      switch (plan->partition.r2vb_transport[i]) {
      case R300_MP_R2VB_SINT:   type = 'i'; break;
      case R300_MP_R2VB_UINT:   type = 'u'; break;
      case R300_MP_R2VB_BOOL1:
      case R300_MP_R2VB_BOOL32: type = 'b'; break;
      default:                  type = 'f'; break;
      }
      unsigned width = plan->partition.bases[i]->num_components;
      for (unsigned component = 0;
           component < width && n + 1 < len; component++) {
         buf[n++] = type;
      }
   }
   buf[n] = '\0';
   for (char *a = buf; *a; a++)
      for (char *b = a + 1; *b; b++)
         if (*b < *a) {
            char t = *a;
            *a = *b;
            *b = t;
         }
}

/* Render the census row for one plan: the stable, order-independent view a
 * miner consumes and the determinism check compares. */
static void
format_row(const struct census_row *row, const char *space_name,
           const struct r300_r2vb_producer_plan *plan, char *buf, size_t len)
{
   char sig[R300_MP_MAX_CARRY_COMPS + 1] = "";
   if (plan->action == R300_R2VB_PLAN_SPLIT)
      carry_sig(plan, sig, sizeof(sig));
   snprintf(buf, len,
            "census specimen=%s space=%s status=%s action=%s primary=%s "
            "mask=0x%" PRIx64 " inputs=%u typed=%d class=%s carries=%s "
            "baseline=%u/%u/%u passA=%u/%u/%u passB=%u/%u/%u",
            row->name, space_name, plan_status_str(plan->status),
            r300_r2vb_plan_action_str(plan->action),
            r300_r2vb_plan_reason_str(plan->primary_reason),
            plan->observed_reason_mask, plan->num_position_inputs,
            plan->has_typed_source, typed_class_str(plan->typed_source_class),
            sig,
            plan->baseline.alu, plan->baseline.temps, plan->baseline.consts,
            plan->pass_a_cost.alu, plan->pass_a_cost.temps,
            plan->pass_a_cost.consts,
            plan->pass_b_cost.alu, plan->pass_b_cost.temps,
            plan->pass_b_cost.consts);
}

/* Recompile one plan-selected program under a named phase, printing and
 * transcripting its statistics.  Returns the number of statistics records
 * the compile produced; accumulates capture cleanliness into *clean. */
static unsigned
census_selected_phase(const struct census_row *row, const char *space_name,
                      const char *phase, const nir_shader *program,
                      bool print, char *transcript, size_t transcript_len,
                      bool *clean)
{
   nir_shader *clone = nir_shader_clone(NULL, program);
   if (!clone) {
      *clean = false;
      return 0;
   }
   stats_reset();
   struct r300_fs_admission_cost cost;
   r300_fs_measure_nir_admission(&g_context, clone, NULL,
                                 R300_FS_INPUT_R2VB_FLAT_VERTEX, &cost);
   if (print) {
      for (unsigned i = 0; i < g_stats_count; i++)
         printf("census-stats specimen=%s space=%s phase=%s seq=%u %s\n",
                row->name, space_name, phase, i, g_stats[i]);
   }
   transcript_append_stats(transcript, transcript_len, phase);
   *clean = *clean && !g_stats_overflow && !g_stats_truncated;
   ralloc_free(clone);
   return g_stats_count;
}

static void
run_row(const struct census_row *row, enum r300_r2vb_position_space space)
{
   const char *space_name =
      space == R300_R2VB_POSITION_WINDOW ? "window" : "clip";
   char label[96];
   char line_first[512], line_second[512];

   g_transcript[0][0] = '\0';
   g_transcript[1][0] = '\0';

   for (unsigned attempt = 0; attempt < 2; attempt++) {
      char *transcript = g_transcript[attempt];
      const size_t transcript_len = sizeof(g_transcript[0]);
      nir_shader *vs = row->build
                          ? row->build()
                          : prepare_module(row->spirv, row->spirv_size);
      snprintf(label, sizeof(label), "%s/%s specimen builds attempt %u",
               row->name, space_name, attempt + 1);
      CHECK(vs != NULL, label);
      if (!vs)
         return;

      stats_reset();
      struct r300_r2vb_producer_plan plan;
      bool ran = r300_r2vb_plan_producer(&g_context, vs, false, space, &plan);
      snprintf(label, sizeof(label), "%s/%s planner runs", row->name,
               space_name);
      if (attempt == 0)
         CHECK(ran, label);
      if (!ran) {
         ralloc_free(vs);
         return;
      }

      format_row(row, space_name, &plan,
                 attempt == 0 ? line_first : line_second,
                 sizeof(line_first));

      bool clean = !g_stats_overflow && !g_stats_truncated;
      unsigned walk_stats = g_stats_count;
      if (attempt == 0) {
         printf("%s\n", line_first);
         for (unsigned i = 0; i < g_stats_count; i++)
            printf("census-stats specimen=%s space=%s phase=walk seq=%u %s\n",
                   row->name, space_name, i, g_stats[i]);
      }
      transcript_append_stats(transcript, transcript_len, "walk");

      /* The plan-selected programs, recompiled under named phases: the
       * miner reads these records, and the walk sequence stays diagnostic. */
      unsigned selected_stats = 0;
      unsigned selected_pass_a_stats = 0;
      unsigned selected_pass_b_stats = 0;
      if (plan.action == R300_R2VB_PLAN_SINGLE && plan.candidate) {
         selected_stats += census_selected_phase(
            row, space_name, "selected-baseline", plan.candidate,
            attempt == 0, transcript, transcript_len, &clean);
      } else if (plan.action == R300_R2VB_PLAN_SPLIT && plan.candidate) {
         nir_shader *pass_a =
            r300_mp_build_carry_pass_a(plan.candidate, &plan.partition);
         nir_shader *pass_b = r300_mp_build_pos_pass_b(
            plan.candidate, &plan.partition, plan.num_position_inputs);
         if (pass_a) {
            r300_optimize_nir(pass_a, &g_screen.caps);
            selected_pass_a_stats = census_selected_phase(
               row, space_name, "selected-pass-a", pass_a, attempt == 0,
               transcript, transcript_len, &clean);
            selected_stats += selected_pass_a_stats;
            ralloc_free(pass_a);
         } else {
            clean = false;
         }
         if (pass_b) {
            r300_optimize_nir(pass_b, &g_screen.caps);
            selected_pass_b_stats = census_selected_phase(
               row, space_name, "selected-pass-b", pass_b, attempt == 0,
               transcript, transcript_len, &clean);
            selected_stats += selected_pass_b_stats;
            ralloc_free(pass_b);
         } else {
            clean = false;
         }
      }

      if (attempt == 0) {
         snprintf(label, sizeof(label), "%s/%s action %s", row->name,
                  space_name, r300_r2vb_plan_action_str(row->action));
         CHECK(plan.action == row->action, label);
         snprintf(label, sizeof(label), "%s/%s primary %s", row->name,
                  space_name,
                  r300_r2vb_plan_reason_str(row->primary_reason));
         CHECK(plan.primary_reason == row->primary_reason, label);
         snprintf(label, sizeof(label), "%s/%s typed producer contract",
                  row->name, space_name);
         CHECK(plan.has_typed_source == row->prepared_has_typed &&
                  plan.typed_source_class == row->typed_source_class,
               label);
         if (row->action == R300_R2VB_PLAN_SPLIT &&
             plan.action == R300_R2VB_PLAN_SPLIT) {
            char sig[R300_MP_MAX_CARRY_COMPS + 1];
            carry_sig(&plan, sig, sizeof(sig));
            bool has_required = row->required_carry == 0 ||
                                strchr(sig, row->required_carry) != NULL;
            bool all_allowed = true;
            for (const char *c = sig; *c; c++)
               if (!strchr(row->allowed_carry, *c))
                  all_allowed = false;
            snprintf(label, sizeof(label), "%s/%s carry {%s} within {%s}",
                     row->name, space_name, sig, row->allowed_carry);
            CHECK(has_required && all_allowed, label);
         }
         if (plan.action == R300_R2VB_PLAN_SINGLE ||
             plan.action == R300_R2VB_PLAN_SPLIT) {
            snprintf(label, sizeof(label), "%s/%s walk statistics captured",
                     row->name, space_name);
            CHECK(walk_stats > 0, label);
            if (plan.action == R300_R2VB_PLAN_SINGLE) {
               snprintf(label, sizeof(label),
                        "%s/%s selected baseline statistics captured",
                        row->name, space_name);
               CHECK(selected_stats > 0, label);
            } else {
               snprintf(label, sizeof(label), "%s/%s selected pass-A stats",
                        row->name, space_name);
               CHECK(selected_pass_a_stats > 0, label);
               snprintf(label, sizeof(label), "%s/%s selected pass-B stats",
                        row->name, space_name);
               CHECK(selected_pass_b_stats > 0, label);
            }
         }
         snprintf(label, sizeof(label), "%s/%s statistics capture clean",
                  row->name, space_name);
         CHECK(clean, label);
      }

      r300_r2vb_plan_release(&plan);
      ralloc_free(vs);
   }

   snprintf(label, sizeof(label), "%s/%s row deterministic", row->name,
            space_name);
   CHECK(strcmp(line_first, line_second) == 0, label);
   snprintf(label, sizeof(label), "%s/%s statistics deterministic",
            row->name, space_name);
   CHECK(strcmp(g_transcript[0], g_transcript[1]) == 0, label);
}

static void
check_fragment_pipeline_layout_guard(void)
{
   const VkPipelineShaderStageCreateInfo stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = VK_NULL_HANDLE,
      .pName = "main",
   };
   nir_shader *nir = NULL;
   VkResult result = r3v_prepare_shader_nir(&g_r3v_device, &stage_info, NULL,
                                            NULL, &nir);
   CHECK(result == VK_ERROR_FEATURE_NOT_PRESENT && nir == NULL,
         "fragment preparation rejects a missing pipeline layout");
}

static void
check_carry_signature_component_width(void)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT,
      g_screen.screen.nir_options[MESA_SHADER_FRAGMENT],
      "r2vb_census_carry_signature");
   struct r300_r2vb_producer_plan plan = {0};
   plan.partition.num_bases = 2;
   plan.partition.bases[0] = nir_undef(&b, 3, 32);
   plan.partition.bases[1] = nir_undef(&b, 1, 32);
   plan.partition.r2vb_transport[0] = R300_MP_R2VB_FLOAT;
   plan.partition.r2vb_transport[1] = R300_MP_R2VB_SINT;
   char signature[R300_MP_MAX_CARRY_COMPS + 1];
   carry_sig(&plan, signature, sizeof(signature));
   CHECK(strcmp(signature, "fffi") == 0,
         "census carry signature repeats transport per vector component");
   CHECK(strcmp(signature, "fi") != 0,
         "census carry signature rejects the per-base schema");
   ralloc_free(b.shader);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   fake_stack_init();

   check_fragment_pipeline_layout_guard();
   check_carry_signature_component_width();

   for (unsigned i = 0; i < ARRAY_SIZE(rows); i++) {
      run_row(&rows[i], R300_R2VB_POSITION_CLIP);
      run_row(&rows[i], R300_R2VB_POSITION_WINDOW);
   }

   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   glsl_type_singleton_decref();
   if (g_failures)
      printf("FAILED (%u)\n", g_failures);
   else
      printf("PASSED\n");
   return g_failures ? 1 : 0;
}
