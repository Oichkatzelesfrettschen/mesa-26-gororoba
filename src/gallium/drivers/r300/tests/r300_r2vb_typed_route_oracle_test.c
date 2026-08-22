/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Route-chain oracle for the R2VB producer plan, SPIR-V-derived tier: the
 * typed-carry corpus modules enter as SPIR-V, lower to the vertex-shader
 * NIR shape r300g's planner consumes, and plan through the production
 * r300_r2vb_plan_producer on a fake SW-TCL screen and context.  The typed
 * split's original unreachability was a representation-boundary defect --
 * hand-built NIR tests were green while a SPIR-V-derived application VS
 * (literal constant-block address arithmetic, weak-ffma multiply-adds, a
 * 128-byte constant window lowered onto block 0) rejected at the route --
 * so this tier plans that representation on the exact modules the silicon
 * gate replays.
 *
 * The adapter below is the test's own front end.  spirv_to_nir runs with
 * the r300 vertex options (Vulkan environment, 32-bit index/offset buffer
 * addressing, the screen's SW-TCL NIR options whose keep_weak_ffma is the
 * representation fact the rows depend on); the module's 128-byte constant
 * block folds onto uniform block 0 and lowers to load_ubo_vec4 with a
 * literal block index, the form r300g's GL uniform-to-block-0 path feeds
 * the planner and nir_to_rc; vertex inputs take their dense attribute
 * slot as driver_location and outputs are assigned, as the state tracker
 * would have done before create_vs_state.
 *
 * Rows, in both clip and window space: the bounded-signed and
 * bounded-unsigned modules plan SPLIT with {f,i} and {f,u} carries; the
 * out-of-window signed and unsigned modules reject SIGNED_RANGE and
 * UNSIGNED_RANGE; the unbounded unsigned module rejects UNSIGNED_RANGE; and
 * the signedness-conflict modules reject MIXED_SIGNEDNESS.
 *
 * The two boolean rows pin the source-to-producer representation boundary.
 * The t_bool module converts its boolean early (float(k) before the chain),
 * so the restaged optimized position candidate the typed scan reads carries
 * no boolean marker: fixture source class boolean, prepared producer class
 * none, selected transport {f}.  The t_bool_carry module is hand-authored
 * SPIR-V assembly whose boolean stays live across every admissible cut into
 * a post-chain OpSelect between structurally unrelated arms, so in clip
 * space its prepared producer class is boolean and its carry must contain
 * b -- the row that licenses a BOOL transport claim from a live
 * carry_types token.  In window space r300_nir_lower_bool_to_float_fs
 * rewrites every position-feeding bcsel(fcmp) into fcsel_ge float form
 * (the divide and viewport arithmetic appended after position makes the
 * select's result an ALU operand, which arms the only-used-as-float rule),
 * so the window cell of the same module truthfully prepares to class none
 * with a float-only carry: boolean transport is reachable through the clip
 * route.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"

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

/* The planner reads the context's screen (caps and NIR options), the
 * fragment regalloc state, and the viewport.  has_tcl = false selects the
 * production SW-TCL tables. */
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

/* The corpus modules declare a 128-byte constant window (the Vulkan
 * push-constant block); r300 has one float constant file per stage, so the
 * window lowers onto uniform block 0 exactly as a GL uniform block does. */
#define CONSTANT_WINDOW_BYTES 128u

static const struct glsl_type *
block0_type(unsigned size_bytes)
{
   return glsl_array_type(glsl_vec4_type(), DIV_ROUND_UP(size_bytes, 16), 16);
}

/* Declare a sized uniform block 0: the compiler sizes RC constants from the
 * nir_var_mem_ubo interface declaration, and load_ubo alone carries no size. */
static void
declare_block0_ubo(nir_shader *nir, unsigned size_bytes)
{
   const struct glsl_type *ubo_type = block0_type(size_bytes);
   struct glsl_struct_field field = {
      .type = ubo_type,
      .name = "data",
      .location = -1,
   };
   nir_variable *ubo = nir_variable_create(nir, nir_var_mem_ubo, ubo_type,
                                           "block0_ubo");
   ubo->data.driver_location = 0;
   ubo->data.binding = 0;
   ubo->data.explicit_binding = 1;
   ubo->interface_type = glsl_interface_type(
      &field, 1, GLSL_INTERFACE_PACKING_STD430, false, "__block0_ubo");
   nir->info.num_ubos = MAX2(nir->info.num_ubos, 1);
   nir->info.first_ubo_is_default_ubo = true;
}

/* load_push_constant(offset) -> load_ubo(block 0, BASE + offset), so the
 * window flows through nir_lower_ubo_vec4 like any block-0 uniform. */
static void
lower_constant_window_to_block0(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      bool progress = false;
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_push_constant)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_def *off = intr->src[0].ssa;
            unsigned base = nir_intrinsic_base(intr);
            if (base)
               off = nir_iadd_imm(&b, off, base);
            nir_def *val = nir_load_ubo(
               &b, intr->def.num_components, intr->def.bit_size,
               nir_imm_int(&b, 0), off,
               .align_mul = nir_intrinsic_align_mul(intr),
               .align_offset = nir_intrinsic_align_offset(intr),
               .range_base = 0, .range = CONSTANT_WINDOW_BYTES);
            nir_def_rewrite_uses(&intr->def, val);
            nir_instr_remove(instr);
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
   declare_block0_ubo(nir, CONSTANT_WINDOW_BYTES);
}

/* Every block load names literal block 0: nir_lower_explicit_io rebuilds the
 * index as a vec construct, and nir_to_rc asserts a literal 0. */
static void
pin_block_index_zero(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      bool progress = false;
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_ubo &&
                intr->intrinsic != nir_intrinsic_load_ubo_vec4)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_src_rewrite(&intr->src[0], nir_imm_int(&b, 0));
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
}

/* Dense attribute slots: the draw path reads driver_location as the AOS
 * row index, and the state tracker assigns it before create_vs_state. */
static bool
assign_input_locations(nir_shader *nir)
{
   unsigned input_span = 0;
   nir_foreach_shader_in_variable(var, nir) {
      if (var->data.location < VERT_ATTRIB_GENERIC0)
         return false;
      const unsigned driver_location =
         var->data.location - VERT_ATTRIB_GENERIC0;
      const unsigned slots = glsl_count_attribute_slots(var->type, false);
      var->data.driver_location = driver_location;
      input_span = MAX2(input_span, driver_location + slots);
   }
   nir->num_inputs = input_span;
   return true;
}

/* One corpus module to the vertex-shader NIR the planner consumes. */
static nir_shader *
prepare_module(const uint32_t *words, size_t size_bytes)
{
   static const struct spirv_capabilities capabilities = {
      .Matrix = true,
      .Shader = true,
   };
   const struct spirv_to_nir_options options = {
      .environment = NIR_SPIRV_VULKAN,
      .capabilities = &capabilities,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,
      .skip_os_break_in_debug_build = true,
   };
   nir_shader *nir =
      spirv_to_nir(words, size_bytes / 4, NULL, MESA_SHADER_VERTEX, "main",
                   &options, g_screen.screen.nir_options[MESA_SHADER_VERTEX]);
   if (!nir)
      return NULL;
   nir_validate_shader(nir, "after spirv_to_nir");

   /* The SPIR-V module's function shape normalizes to one entry point with
    * straight-line body before any driver lowering: initializers land at
    * the top of their function, returns lower, calls inline, the other
    * entry points leave, struct copies split, and dead interface
    * variables go. */
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_cmat_call_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_shader_in | nir_var_shader_out | nir_var_system_value,
            NULL);
   NIR_PASS(_, nir, nir_propagate_invariant, false);

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
            nir_var_function_temp | nir_var_shader_out, UINT32_MAX);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);
   lower_constant_window_to_block0(nir);
   NIR_PASS(_, nir, nir_lower_explicit_io,
            nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);
   NIR_PASS(_, nir, nir_lower_ubo_vec4);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   pin_block_index_zero(nir);
   if (!assign_input_locations(nir)) {
      ralloc_free(nir);
      return NULL;
   }
   nir_assign_io_var_locations(nir, nir_var_shader_out);
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
   /* Prepared-producer expectation: the typed scan reads the restaged
    * optimized position candidate, so a fixture whose typed value converts
    * to float before the chain legitimately prepares to class none. */
   bool prepared_has_typed;
   enum r300_r2vb_typed_source_class source_class;
   /* SPLIT carry contract: the typed transport that must appear (0 when
    * either exact form is correct), and the letters the carry may consist
    * of. */
   char required_carry;
   const char *allowed_carry;
   /* r300_nir_lower_bool_to_float_fs rewrites position-feeding
    * bcsel(fcmp) into fcsel_ge float form once the window transform makes
    * the select an ALU operand, so a boolean-carry module's window cell
    * prepares to class none with a float-only carry. */
   bool window_bool_lowered;
};

/* The silicon gate's required rows: T0 SPLIT float-carry (early
 * conversion), boolean-carry SPLIT with b; T1/T2 SPLIT {f,i}; T3 SPLIT
 * {f,u}; T4/T5 REJECT SIGNED_RANGE; T6/T7 REJECT UNSIGNED_RANGE; T8/T9
 * REJECT MIXED_SIGNEDNESS.  The signedness-conflict modules read SINT as
 * the source class because their consumer casts mark the signed domain. */
static const struct corpus_row rows[] = {
   { "t_bool", t_bool_spirv, sizeof(t_bool_spirv),
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     false, R300_R2VB_TYPED_SOURCE_NONE, 0, "f" },
   { "t_bool_carry", t_bool_carry_spirv, sizeof(t_bool_carry_spirv),
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     true, R300_R2VB_TYPED_SOURCE_BOOL, 'b', "bf", true },
   { "t_sint_exact", t_sint_exact_spirv, sizeof(t_sint_exact_spirv),
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     true, R300_R2VB_TYPED_SOURCE_SINT, 'i', "fi" },
   { "t_uint_exact", t_uint_exact_spirv, sizeof(t_uint_exact_spirv),
     R300_R2VB_PLAN_SPLIT, R300_R2VB_PLAN_OK,
     true, R300_R2VB_TYPED_SOURCE_UINT, 'u', "fu" },
   { "t_sint_pos_outside", t_sint_pos_outside_spirv,
     sizeof(t_sint_pos_outside_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_SIGNED_RANGE,
     true, R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
   { "t_sint_neg_outside", t_sint_neg_outside_spirv,
     sizeof(t_sint_neg_outside_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_SIGNED_RANGE,
     true, R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
   { "t_uint_outside", t_uint_outside_spirv, sizeof(t_uint_outside_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_UNSIGNED_RANGE,
     true, R300_R2VB_TYPED_SOURCE_UINT, 0, "" },
   { "t_uint_unbounded", t_uint_unbounded_spirv,
     sizeof(t_uint_unbounded_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_UNSIGNED_RANGE,
     true, R300_R2VB_TYPED_SOURCE_UINT, 0, "" },
   { "t_sint_to_uint", t_sint_to_uint_spirv, sizeof(t_sint_to_uint_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_MIXED_SIGNEDNESS,
     true, R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
   { "t_uint_to_sint", t_uint_to_sint_spirv, sizeof(t_uint_to_sint_spirv),
     R300_R2VB_PLAN_REJECT, R300_R2VB_PLAN_MIXED_SIGNEDNESS,
     true, R300_R2VB_TYPED_SOURCE_SINT, 0, "" },
};

static void
run_row(const struct corpus_row *row, enum r300_r2vb_position_space space)
{
   const char *space_name =
      space == R300_R2VB_POSITION_WINDOW ? "window" : "clip";
   char label[96];

   nir_shader *vs = prepare_module(row->spirv, row->spirv_size);
   snprintf(label, sizeof(label), "%s/%s prepares to vertex NIR", row->name,
            space_name);
   CHECK(vs != NULL, label);
   if (!vs)
      return;

   /* Window-space boolean lowering: the per-space expectation of a
    * boolean-carry module. */
   bool prepared_has_typed = row->prepared_has_typed;
   enum r300_r2vb_typed_source_class source_class = row->source_class;
   char required_carry = row->required_carry;
   const char *allowed_carry = row->allowed_carry;
   if (row->window_bool_lowered && space == R300_R2VB_POSITION_WINDOW) {
      prepared_has_typed = false;
      source_class = R300_R2VB_TYPED_SOURCE_NONE;
      required_carry = 0;
      allowed_carry = "f";
   }

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

   snprintf(label, sizeof(label), "%s/%s prepared typed-source class",
            row->name, space_name);
   CHECK(plan.has_typed_source == prepared_has_typed &&
            plan.typed_source_class == source_class,
         label);

   if (row->action == R300_R2VB_PLAN_SPLIT &&
       plan.action == R300_R2VB_PLAN_SPLIT) {
      char sig[R300_MP_MAX_CARRY_COMPS + 1];
      carry_sig(&plan, sig, sizeof(sig));
      bool has_required = required_carry == 0 ||
                          strchr(sig, required_carry) != NULL;
      bool all_allowed = true;
      for (const char *c = sig; *c; c++)
         if (!strchr(allowed_carry, *c))
            all_allowed = false;
      snprintf(label, sizeof(label), "%s/%s carry {%s} within {%s}",
               row->name, space_name, sig, allowed_carry);
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
   if (g_failures)
      printf("FAILED (%u)\n", g_failures);
   else
      printf("PASSED\n");
   return g_failures ? 1 : 0;
}
