/* SPDX-License-Identifier: MIT */

#include "../r3v_native.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

static const size_t common_predicates[] = {
   offsetof(struct r3v_native_zmask_fast_clear_facts, platform_qualified),
   offsetof(struct r3v_native_zmask_fast_clear_facts,
            image_contract_qualified),
   offsetof(struct r3v_native_zmask_fast_clear_facts, combined_aspects),
   offsetof(struct r3v_native_zmask_fast_clear_facts, transfer_destination),
   offsetof(struct r3v_native_zmask_fast_clear_facts, binding_valid),
   offsetof(struct r3v_native_zmask_fast_clear_facts, layout_qualified),
   offsetof(struct r3v_native_zmask_fast_clear_facts,
            materialization_scratch_valid),
   offsetof(struct r3v_native_zmask_fast_clear_facts, fast_clear_plan_valid),
   offsetof(struct r3v_native_zmask_fast_clear_facts, command_scope_valid),
};

static struct r3v_native_zmask_fast_clear_facts
automatic_facts(void)
{
   return (struct r3v_native_zmask_fast_clear_facts){
      .automatic_qualified = true,
      .platform_qualified = true,
      .image_contract_qualified = true,
      .combined_aspects = true,
      .transfer_destination = true,
      .binding_valid = true,
      .layout_qualified = true,
      .materialization_scratch_valid = true,
      .fast_clear_plan_valid = true,
      .command_scope_valid = true,
      .automatic_source_valid = true,
   };
}

static void
test_automatic_predicates(void)
{
   struct r3v_native_zmask_fast_clear_facts facts = automatic_facts();
   assert(r3v_native_zmask_fast_clear_select_facts(&facts) ==
          R3V_NATIVE_ZMASK_FAST_CLEAR_AUTOMATIC);

   for (size_t index = 0u;
        index < sizeof(common_predicates) / sizeof(*common_predicates);
        index++) {
      struct r3v_native_zmask_fast_clear_facts mutation = facts;
      bool *predicate =
         (bool *)((char *)&mutation + common_predicates[index]);
      *predicate = false;
      assert(r3v_native_zmask_fast_clear_select_facts(&mutation) ==
             R3V_NATIVE_ZMASK_FAST_CLEAR_ORDINARY);
   }

   facts.automatic_qualified = false;
   assert(r3v_native_zmask_fast_clear_select_facts(&facts) ==
          R3V_NATIVE_ZMASK_FAST_CLEAR_ORDINARY);
   facts = automatic_facts();
   facts.automatic_source_valid = false;
   assert(r3v_native_zmask_fast_clear_select_facts(&facts) ==
          R3V_NATIVE_ZMASK_FAST_CLEAR_ORDINARY);
}

static void
test_experimental_and_priority(void)
{
   struct r3v_native_zmask_fast_clear_facts facts = automatic_facts();
   facts.automatic_qualified = false;
   facts.automatic_source_valid = false;
   facts.experimental_gate = true;
   facts.experimental_source_valid = true;
   assert(r3v_native_zmask_fast_clear_select_facts(&facts) ==
          R3V_NATIVE_ZMASK_FAST_CLEAR_EXPERIMENTAL);

   facts.experimental_source_valid = false;
   assert(r3v_native_zmask_fast_clear_select_facts(&facts) ==
          R3V_NATIVE_ZMASK_FAST_CLEAR_ORDINARY);
   facts = automatic_facts();
   facts.experimental_gate = true;
   facts.experimental_source_valid = true;
   assert(r3v_native_zmask_fast_clear_select_facts(&facts) ==
          R3V_NATIVE_ZMASK_FAST_CLEAR_AUTOMATIC);
}

static void
test_public_shape_fallbacks(void)
{
   struct r3v_native_zmask_fast_clear_facts facts = automatic_facts();
   facts.combined_aspects = false;
   assert(r3v_native_zmask_fast_clear_select_facts(&facts) ==
          R3V_NATIVE_ZMASK_FAST_CLEAR_ORDINARY);

   facts = automatic_facts();
   facts.image_contract_qualified = false;
   assert(r3v_native_zmask_fast_clear_select_facts(&facts) ==
          R3V_NATIVE_ZMASK_FAST_CLEAR_ORDINARY);
}

int
main(void)
{
   assert(r3v_native_zmask_fast_clear_select_facts(NULL) ==
          R3V_NATIVE_ZMASK_FAST_CLEAR_ORDINARY);
   const struct r3v_native_device production_device = {0};
   assert(!production_device.zmask_automatic_qualified);
   test_automatic_predicates();
   test_experimental_and_priority();
   test_public_shape_fallbacks();
   puts("r3v ZMASK fast-clear selector: OK");
   return 0;
}
