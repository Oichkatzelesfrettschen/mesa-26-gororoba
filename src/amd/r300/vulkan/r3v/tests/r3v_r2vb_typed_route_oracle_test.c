/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Route-chain oracle for the R2VB producer plan, frontend-integration tier:
 * the typed-carry corpus SPIR-V modules through the production r3v
 * preparation (r3v_prepare_shader_nir: vk_spirv_to_nir with r3v's options
 * plus the full r3v vertex lowering chain) into the same producer planner the
 * driver caches.  The typed split's original unreachability was a
 * representation-boundary defect -- synthetic NIR tests were green while the
 * SPIR-V-derived application VS (literal UBO address arithmetic, weak-ffma
 * multiply-adds, push constants lowered onto constant block 0) rejected at
 * the route -- so this tier plans the exact representation the route
 * consumes, on the exact modules the silicon gate replays.
 *
 * The screen carries the production SW-TCL options
 * (r300_screen_init_nir_options with has_tcl = false selects the gallivm
 * vertex table, whose keep_weak_ffma is the load-bearing representation
 * fact), so the prepared NIR matches what a created screen produces.
 *
 * Rows, in both clip and window space: the bool, bounded-signed, and
 * bounded-unsigned modules plan SPLIT with {f,b}, {f,i}, {f,u} carries; the
 * out-of-window signed and unsigned modules reject SIGNED_RANGE and
 * UNSIGNED_RANGE; the unbounded unsigned module rejects UNSIGNED_RANGE; and
 * the signedness-conflict modules reject MIXED_SIGNEDNESS.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"

#include "r3v_device.h"
#include "r3v_pipeline.h"
#include "r3v_private.h"
#include "r3v_shader_module.h"

#include "vulkan/runtime/vk_instance.h"
#include "vulkan/runtime/vk_physical_device.h"

#include "r300_context.h"
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

/* The preparation reads device->vk.physical (base SPIR-V capabilities from a
 * zeroed physical device) and device->screen (caps and NIR options); the
 * planner additionally reads the r300 context's screen, regalloc state, and
 * viewport.  has_tcl = false selects the production SW-TCL tables. */
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

   /* The SPIR-V capability derivation reads the instance's requested API
    * version; everything else stays at the zeroed (base-capability)
    * defaults, matching a 1.0 instance with no extensions. */
   memset(&g_vk_instance, 0, sizeof(g_vk_instance));
   g_vk_instance.app_info.api_version = VK_API_VERSION_1_0;
   memset(&g_vk_pdev, 0, sizeof(g_vk_pdev));
   g_vk_pdev.instance = &g_vk_instance;
   memset(&g_r3v_device, 0, sizeof(g_r3v_device));
   g_r3v_device.vk.physical = &g_vk_pdev;
   g_r3v_device.screen = (struct pipe_screen *)&g_screen;
}

/* Run one corpus module through the production preparation to the bound
 * application VS NIR.  The module wraps into an r3v_shader_module exactly as
 * vkCreateShaderModule stores it. */
static nir_shader *
prepare_module(const uint32_t *words, size_t size_bytes)
{
   struct r3v_shader_module *mod =
      calloc(1, sizeof(*mod) + size_bytes);
   if (!mod)
      return NULL;
   /* The handle cast validates the object type vk_object_base_init would
    * stamp. */
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
   if (result != VK_SUCCESS)
      return NULL;
   return nir;
}

/* Sorted one-letter carry signature (f/i/u/b per transported base), the
 * order-independent form of the split trace encoding. */
static void
carry_sig(const struct r300_r2vb_producer_plan *plan, char *buf, size_t len)
{
   unsigned n = 0;
   for (unsigned i = 0; i < plan->partition.num_bases && n + 1 < len; i++) {
      switch (plan->partition.r2vb_transport[i]) {
      case R300_MP_R2VB_SINT:   buf[n++] = 'i'; break;
      case R300_MP_R2VB_UINT:   buf[n++] = 'u'; break;
      case R300_MP_R2VB_BOOL1:
      case R300_MP_R2VB_BOOL32: buf[n++] = 'b'; break;
      default:                  buf[n++] = 'f'; break;
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

struct corpus_row {
   const char *name;
   const uint32_t *spirv;
   size_t spirv_size;
   enum r300_r2vb_plan_action action;
   enum r300_r2vb_plan_reason primary_reason;
   enum r300_r2vb_typed_source_class source_class;
   /* SPLIT carry contract: the typed transport that must appear (0 when
    * either exact form is correct), and the letters the carry may consist
    * of.  The deterministic candidate ordering selects the narrowest
    * admissible cut, and glslang's select-to-float form converts the bool
    * module's value with b2f32 before any admissible cut, so its carry may
    * ride as the exact 0/1 float; the BOOL1 transport mechanism itself is
    * pinned by the unit-tier oracle and the producer-split test. */
   char required_carry;
   const char *allowed_carry;
};

/* The silicon gate's required rows: T0 SPLIT {f,b}; T1/T2 SPLIT {f,i};
 * T3 SPLIT {f,u}; T4/T5 REJECT SIGNED_RANGE; T6/T7 REJECT UNSIGNED_RANGE;
 * T8/T9 REJECT MIXED_SIGNEDNESS.  The signedness-conflict modules read
 * SINT as the whole-program source class because their consumer casts mark
 * the signed domain. */
static const struct corpus_row rows[] = {
   { "t_bool", t_bool_spirv, sizeof(t_bool_spirv),
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     R300_R2VB_TYPED_SOURCE_BOOL, 0, "bf" },
   { "t_sint_exact", t_sint_exact_spirv, sizeof(t_sint_exact_spirv),
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     R300_R2VB_TYPED_SOURCE_SINT, 'i', "fi" },
   { "t_uint_exact", t_uint_exact_spirv, sizeof(t_uint_exact_spirv),
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     R300_R2VB_TYPED_SOURCE_UINT, 'u', "fu" },
   { "t_sint_pos_outside", t_sint_pos_outside_spirv,
     sizeof(t_sint_pos_outside_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_SIGNED_RANGE,
     R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
   { "t_sint_neg_outside", t_sint_neg_outside_spirv,
     sizeof(t_sint_neg_outside_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_SIGNED_RANGE,
     R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
   { "t_uint_outside", t_uint_outside_spirv, sizeof(t_uint_outside_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_UNSIGNED_RANGE,
     R300_R2VB_TYPED_SOURCE_UINT, 0, "" },
   { "t_uint_unbounded", t_uint_unbounded_spirv,
     sizeof(t_uint_unbounded_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_UNSIGNED_RANGE,
     R300_R2VB_TYPED_SOURCE_UINT, 0, "" },
   { "t_sint_to_uint", t_sint_to_uint_spirv, sizeof(t_sint_to_uint_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_MIXED_SIGNEDNESS,
     R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
   { "t_uint_to_sint", t_uint_to_sint_spirv, sizeof(t_uint_to_sint_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_MIXED_SIGNEDNESS,
     R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
};

static void
run_row(const struct corpus_row *row, enum r300_r2vb_position_space space)
{
   const char *space_name =
      space == R300_R2VB_POSITION_WINDOW ? "window" : "clip";
   char label[96];

   nir_shader *vs = prepare_module(row->spirv, row->spirv_size);
   snprintf(label, sizeof(label), "%s/%s prepares through r3v", row->name,
            space_name);
   CHECK(vs != NULL, label);
   if (!vs)
      return;

   struct r300_r2vb_producer_plan plan;
   bool ran = r300_r2vb_plan_producer(&g_context, vs, false, space, &plan);
   printf("    %s/%s action=%s primary=%s mask=0x%" PRIx64
          " typed=%d class=%d bases=%u\n",
          row->name, space_name, r300_r2vb_plan_action_str(plan.action),
          r300_r2vb_plan_reason_str(plan.primary_reason),
          plan.observed_reason_mask, plan.has_typed_source,
          plan.typed_source_class, plan.partition.num_bases);
   snprintf(label, sizeof(label), "%s/%s planner runs", row->name,
            space_name);
   CHECK(ran, label);

   snprintf(label, sizeof(label), "%s/%s action %s", row->name, space_name,
            r300_r2vb_plan_action_str(row->action));
   CHECK(plan.action == row->action, label);

   snprintf(label, sizeof(label), "%s/%s primary %s", row->name, space_name,
            r300_r2vb_plan_reason_str(row->primary_reason));
   CHECK(plan.primary_reason == row->primary_reason, label);

   snprintf(label, sizeof(label), "%s/%s typed-source class", row->name,
            space_name);
   CHECK(plan.has_typed_source &&
            plan.typed_source_class == row->source_class,
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

   r300_r2vb_plan_release(&plan);
   ralloc_free(vs);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   fake_stack_init();

   for (unsigned i = 0; i < ARRAY_SIZE(rows); i++) {
      printf("%s\n", rows[i].name);
      run_row(&rows[i], R300_R2VB_POSITION_CLIP);
      run_row(&rows[i], R300_R2VB_POSITION_WINDOW);
   }

   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   glsl_type_singleton_decref();
   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
