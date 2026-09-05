/*
 * SPDX-License-Identifier: MIT
 *
 * The contract-evidence registry's obligations: each receipted shape is
 * admitted at its own width and refused above it -- V1 at one window
 * through one relocation site, V2 at two -- the shipped rows pass the
 * self-check, and mutated copies whose receipted maxima are zero fail it.
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
test_v2_admission_is_bounded_by_its_receipt(void)
{
   const struct r300_rb2d_contract_evidence *v2 =
      r300_rb2d_contract_evidence_find(R300_RB2D_CONTRACT_CONST_FILL_V2);
   assert(v2 != NULL);
   assert(v2->state == R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT);
   assert(v2->max_windows_receipted == 2u);
   assert(v2->max_reloc_sites_receipted == 2u);
   assert(strcmp(v2->artifact, "planned") != 0);

   /* The attended multiwindow run rebased the destination twice through
    * two relocation sites, so a narrower stream is inside what ran and
    * the third window is outside it. */
   for (uint32_t windows = 1u; windows <= 2u; windows++) {
      assert(r300_rb2d_contract_admitted(
         R300_RB2D_CONTRACT_CONST_FILL_V2, windows, windows,
         R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT));
   }
   assert(!r300_rb2d_contract_admitted(
      R300_RB2D_CONTRACT_CONST_FILL_V2, 3u, 3u,
      R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT));
   assert(!r300_rb2d_contract_admitted(
      R300_RB2D_CONTRACT_CONST_FILL_V2, 2u, 3u,
      R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT));
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
   mutated[R300_RB2D_CONTRACT_CONST_FILL_V2].max_windows_receipted = 0u;
   mutated[R300_RB2D_CONTRACT_CONST_FILL_V2].max_reloc_sites_receipted = 0u;
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
   test_v2_admission_is_bounded_by_its_receipt();
   test_self_check_and_its_calibration();
   test_names_and_lookup_bounds();
   printf("r300_rb2d_contract_evidence_test: ok\n");
   return 0;
}
