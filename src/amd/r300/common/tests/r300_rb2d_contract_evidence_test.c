/*
 * SPDX-License-Identifier: MIT
 *
 * The contract-evidence registry's obligations: the receipted V1 shape is
 * admitted at its own width and refused above it, the V2 shape reaches no
 * silicon receipt at any width, the shipped rows pass the self-check, and a
 * mutated copy whose receipted V1 window count is zero fails it.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_rb2d_contract_evidence.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void
test_v1_admission_is_bounded_by_its_receipt(void)
{
   const struct r300_rb2d_contract_evidence *v1 =
      r300_rb2d_contract_evidence_find(R300_RB2D_CONTRACT_CONST_FILL_V1);
   assert(v1 != NULL);
   assert(v1->state == R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT);
   assert(v1->max_windows_receipted == 1u);
   assert(v1->max_reloc_sites_receipted == 1u);
   assert(strcmp(v1->artifact, "planned") != 0);

   assert(r300_rb2d_contract_admitted(
      R300_RB2D_CONTRACT_CONST_FILL_V1, 1u, 1u,
      R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT));
   /* The second window is the whole refusal: the receipted run emitted one
    * window through one relocation site, so a stream that rebases again is
    * outside what ran however strong the carrier's own evidence is. */
   assert(!r300_rb2d_contract_admitted(
      R300_RB2D_CONTRACT_CONST_FILL_V1, 2u, 2u,
      R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT));
   assert(!r300_rb2d_contract_admitted(
      R300_RB2D_CONTRACT_CONST_FILL_V1, 1u, 2u,
      R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT));
}

static void
test_v2_reaches_no_silicon_receipt(void)
{
   const struct r300_rb2d_contract_evidence *v2 =
      r300_rb2d_contract_evidence_find(R300_RB2D_CONTRACT_CONST_FILL_V2);
   assert(v2 != NULL);
   assert(v2->state == R300_RB2D_CONTRACT_EVIDENCE_KERNEL_REPLAY);
   assert(v2->max_windows_receipted == 0u);
   assert(v2->max_reloc_sites_receipted == 0u);

   for (uint32_t windows = 0; windows <= 4u; windows++) {
      assert(!r300_rb2d_contract_admitted(
         R300_RB2D_CONTRACT_CONST_FILL_V2, windows, windows,
         R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT));
   }
   /* The kernel replay it does hold reaches the empty stream alone, which
    * is what keeps a width claim out of a replay-only row. */
   assert(r300_rb2d_contract_admitted(
      R300_RB2D_CONTRACT_CONST_FILL_V2, 0u, 0u,
      R300_RB2D_CONTRACT_EVIDENCE_KERNEL_REPLAY));
   assert(!r300_rb2d_contract_admitted(
      R300_RB2D_CONTRACT_CONST_FILL_V2, 1u, 1u,
      R300_RB2D_CONTRACT_EVIDENCE_KERNEL_REPLAY));
}

static void
test_self_check_and_its_calibration(void)
{
   assert(r300_rb2d_contract_evidence_self_check() == 0);

   uint32_t count = 0;
   const struct r300_rb2d_contract_evidence *shipped =
      r300_rb2d_contract_evidence_rows(&count);
   assert(count == (uint32_t)R300_RB2D_CONTRACT_COUNT);
   assert(r300_rb2d_contract_evidence_rows_valid(shipped, count) == 0);

   struct r300_rb2d_contract_evidence mutated[R300_RB2D_CONTRACT_COUNT];
   memcpy(mutated, shipped, sizeof(mutated));
   mutated[R300_RB2D_CONTRACT_CONST_FILL_V1].max_windows_receipted = 0u;
   assert(r300_rb2d_contract_evidence_rows_valid(mutated, count) == -EINVAL);

   memcpy(mutated, shipped, sizeof(mutated));
   mutated[R300_RB2D_CONTRACT_CONST_FILL_V2].artifact = "planned";
   assert(r300_rb2d_contract_evidence_rows_valid(mutated, count) == -EINVAL);

   memcpy(mutated, shipped, sizeof(mutated));
   mutated[R300_RB2D_CONTRACT_CONST_FILL_V1].contract =
      R300_RB2D_CONTRACT_CONST_FILL_V2;
   assert(r300_rb2d_contract_evidence_rows_valid(mutated, count) == -EINVAL);

   assert(r300_rb2d_contract_evidence_rows_valid(NULL, count) == -EINVAL);
   assert(r300_rb2d_contract_evidence_rows_valid(shipped, count - 1u) ==
          -EINVAL);
}

static void
test_names_and_lookup_bounds(void)
{
   for (unsigned s = 0; s < R300_RB2D_CONTRACT_EVIDENCE_STATE_COUNT; s++) {
      assert(r300_rb2d_contract_evidence_state_name(
                (enum r300_rb2d_contract_evidence_state)s) != NULL);
   }
   assert(r300_rb2d_contract_evidence_state_name(
             R300_RB2D_CONTRACT_EVIDENCE_STATE_COUNT) == NULL);
   assert(r300_rb2d_contract_evidence_find(R300_RB2D_CONTRACT_COUNT) == NULL);
   assert(r300_rb2d_contract_evidence_find_in(NULL, 2u,
                                              R300_RB2D_CONTRACT_CONST_FILL_V1) ==
          NULL);
   assert(!r300_rb2d_contract_admitted(R300_RB2D_CONTRACT_COUNT, 0u, 0u,
                                       R300_RB2D_CONTRACT_EVIDENCE_PLANNED));
}

int
main(void)
{
   test_v1_admission_is_bounded_by_its_receipt();
   test_v2_reaches_no_silicon_receipt();
   test_self_check_and_its_calibration();
   test_names_and_lookup_bounds();
   printf("r300_rb2d_contract_evidence_test: ok\n");
   return 0;
}
