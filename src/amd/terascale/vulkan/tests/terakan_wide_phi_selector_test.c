/*
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include "nir/terakan_nir_wide_phi.h"

#include "nir_builder.h"

#include <stdio.h>
#include <stdlib.h>

enum { test_case_count = 4 };

static nir_def *
build_case_modulo(nir_builder * const builder, nir_def * const value)
{
   return nir_umod(builder, value, nir_imm_int(builder, test_case_count));
}

static nir_def *
build_phi_chain(nir_builder * const builder, nir_def * const selector,
                nir_def * const replacement_selector, unsigned const replacement_case_index)
{
   nir_def * chain = nir_imm_int(builder, 100);

   for (unsigned case_index = 0; case_index < test_case_count; ++case_index) {
      nir_def * const case_selector =
         replacement_selector != NULL && case_index == replacement_case_index ? replacement_selector
                                                                              : selector;
      nir_def * const condition =
         nir_ior(builder, nir_imm_false(builder), nir_ieq_imm(builder, case_selector, case_index));
      nir_if * const case_if = nir_push_if(builder, condition);
      nir_def * const case_value = nir_imm_int(builder, 101 + case_index);
      nir_push_else(builder, case_if);
      nir_def * const previous_value = chain;
      nir_pop_if(builder, case_if);
      chain = nir_if_phi(builder, case_value, previous_value);
   }

   return chain;
}

static bool
test_selector_uses_phi_index_provenance(void)
{
   nir_builder builder =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL, "wide phi selector provenance");
   nir_def * const invocation_index = nir_load_local_invocation_index(&builder);
   nir_def * const unrelated_selector =
      build_case_modulo(&builder, nir_iadd_imm(&builder, invocation_index, 1));
   nir_def * const phi_selector = build_case_modulo(&builder, invocation_index);
   nir_def * const root_phi = build_phi_chain(&builder, phi_selector, NULL, 0);

   nir_def * const found_selector = terakan_nir_wide_phi_selector(root_phi, test_case_count);
   bool const passed = found_selector == phi_selector && found_selector != unrelated_selector;
   if (!passed)
      fputs("FAIL: unrelated same-divisor umod replaced the phi selector\n", stderr);

   ralloc_free(builder.shader);
   return passed;
}

static bool
test_selector_rejects_unrelated_same_divisor(void)
{
   nir_builder builder =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL, "wide phi selector rejection");
   nir_def * const invocation_index = nir_load_local_invocation_index(&builder);
   nir_def * const unrelated_selector =
      build_case_modulo(&builder, nir_iadd_imm(&builder, invocation_index, 1));
   nir_def * const unproven_selector =
      nir_iand_imm(&builder, invocation_index, test_case_count - 1);
   nir_def * const root_phi = build_phi_chain(&builder, unproven_selector, NULL, 0);

   nir_def * const found_selector = terakan_nir_wide_phi_selector(root_phi, test_case_count);
   bool const passed = found_selector == NULL;
   if (!passed) {
      fprintf(stderr, "FAIL: selector without phi provenance accepted unrelated umod %s\n",
              found_selector == unrelated_selector ? "candidate" : "definition");
   }

   ralloc_free(builder.shader);
   return passed;
}

static bool
test_selector_rejects_mixed_phi_provenance(void)
{
   nir_builder builder = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL,
                                                        "wide phi mixed selector rejection");
   nir_def * const invocation_index = nir_load_local_invocation_index(&builder);
   nir_def * const primary_selector = build_case_modulo(&builder, invocation_index);
   nir_def * const replacement_selector =
      build_case_modulo(&builder, nir_iadd_imm(&builder, invocation_index, 1));
   nir_def * const root_phi = build_phi_chain(&builder, primary_selector, replacement_selector, 2);

   bool const passed = terakan_nir_wide_phi_selector(root_phi, test_case_count) == NULL;
   if (!passed)
      fputs("FAIL: phi chain with mixed selector provenance was accepted\n", stderr);

   ralloc_free(builder.shader);
   return passed;
}

static bool
test_auto_segment_exact_opt_in(void)
{
   static char const * const rejected_values[] = {"", "0", "true", "01", "1 "};
   bool passed = true;

   unsetenv("TERAKAN_WIDE_PHI_AUTO_SEGMENT");
   if (terakan_nir_wide_phi_auto_segment_enabled())
      passed = false;

   for (unsigned value_index = 0;
        value_index < sizeof(rejected_values) / sizeof(rejected_values[0]); ++value_index) {
      setenv("TERAKAN_WIDE_PHI_AUTO_SEGMENT", rejected_values[value_index], 1);
      if (terakan_nir_wide_phi_auto_segment_enabled())
         passed = false;
   }

   setenv("TERAKAN_WIDE_PHI_AUTO_SEGMENT", "1", 1);
   if (!terakan_nir_wide_phi_auto_segment_enabled())
      passed = false;
   unsetenv("TERAKAN_WIDE_PHI_AUTO_SEGMENT");

   if (!passed)
      fputs("FAIL: wide-phi automatic segmentation is not exact-value gated\n", stderr);
   return passed;
}

int
main(void)
{
   if (!test_selector_uses_phi_index_provenance() ||
       !test_selector_rejects_unrelated_same_divisor() ||
       !test_selector_rejects_mixed_phi_provenance() || !test_auto_segment_exact_opt_in())
      return EXIT_FAILURE;

   puts("PASS: wide-phi selector provenance and exact opt-in");
   return EXIT_SUCCESS;
}
