/*
 * SPDX-License-Identifier: MIT
 *
 * The compute verb ledger's semantic surface: fifteen kernel shapes, each
 * bound to one catalog operation, and the two queue predicates that project
 * over the route ledger.  Route facts -- unit, exactness, evidence, gate --
 * are r300_operation_route_test's subject.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_compute_verb.h"
#include "r300_operation_route.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
test_rows_well_formed(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);
   const char *reason = "unset";

   assert(rows != NULL && count == R300_COMPUTE_VERB_COUNT && count == 15);
   assert(r300_compute_verb_rows_valid(rows, count, &reason));
   assert(reason == NULL);

   for (uint32_t i = 0; i < count; i++) {
      assert(rows[i].verb == (enum r300_compute_verb)i);
      assert(rows[i].name != NULL && rows[i].name[0] != '\0');
      assert(rows[i].operation_id != R300_OPERATION_ID_NONE);
      assert(r300_virtual_op_info_for_id(rows[i].operation_id) != NULL);
      assert(r300_compute_verb_row((enum r300_compute_verb)i) == &rows[i]);
   }
   assert(r300_compute_verb_row(R300_COMPUTE_VERB_COUNT) == NULL);
}

/* Every rule the checker states, each refused on its own mutation, so a
 * rule that stops holding is a failing arm rather than a silent pass. */
static void
test_checker_calibration(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);
   struct r300_compute_verb_row mutated[R300_COMPUTE_VERB_COUNT];
   const char *reason = NULL;

   assert(!r300_compute_verb_rows_valid(NULL, count, &reason));
   assert(strcmp(reason, "row count outside the verb enum") == 0);
   assert(!r300_compute_verb_rows_valid(rows, count - 1, &reason));
   assert(strcmp(reason, "row count outside the verb enum") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[3].verb = R300_COMPUTE_VERB_IDENTITY_MAP;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "rows out of enum order") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[2].name = "";
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "row without a name") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[4].operation_id = R300_OPERATION_ID_NONE;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "compute verb lacks an operation identity") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[5].operation_id = R300_OPERATION_ID_COUNT;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "operation identity outside the catalog enum") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[6].name = mutated[1].name;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "two rows share one name") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[7].operation_id = mutated[1].operation_id;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "two verbs share one operation identity") == 0);
}

/* The two predicates read the route ledger through operation identity, so a
 * verb's answer moves when its operation's routes move and not otherwise. */
static void
test_queue_projection(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);

   /* Coverage asks for an executing host route and an executing GPU route
    * on every verb.  Thirteen operations have neither, so it stands false
    * and the unconditional compute bit stays unreachable. */
   assert(!r300_compute_dual_route_coverage_complete());
   assert(!r300_compute_dual_route_coverage_complete_rows(rows, count));
   assert(!r300_compute_dual_route_coverage_complete_rows(rows, 0));

   /* The gated claim asks only that some verb's operation runs on the host,
    * which identity_map and bitwise_not_map both do. */
   assert(!r300_compute_verb_queue_claim(false));
   assert(r300_compute_verb_queue_claim(true));
   assert(!r300_compute_verb_queue_claim_rows(rows, 0, true));

   /* Coverage is a property of the table it is asked about.  A one-row
    * table over identity_map is fully covered, because that operation owns
    * both an executing host route and an executing GPU route, so the claim
    * stands there without a gate -- which is exactly what makes coverage
    * over the whole fifteen-row ledger the ratchet it is. */
   struct r300_compute_verb_row one = rows[R300_COMPUTE_VERB_IDENTITY_MAP];
   assert(r300_compute_dual_route_coverage_complete_rows(&one, 1));
   assert(r300_compute_verb_queue_claim_rows(&one, 1, false));
   assert(r300_compute_verb_queue_claim_rows(&one, 1, true));

   /* bitwise_not_map runs on the host alone, so its one-row table is not
    * covered and claims only under the gate. */
   one = rows[R300_COMPUTE_VERB_BITWISE_NOT_MAP];
   assert(!r300_compute_dual_route_coverage_complete_rows(&one, 1));
   assert(!r300_compute_verb_queue_claim_rows(&one, 1, false));
   assert(r300_compute_verb_queue_claim_rows(&one, 1, true));

   /* An operation with no executing route at all claims nothing either way. */
   one = rows[R300_COMPUTE_VERB_CONST_FILL];
   assert(!r300_compute_verb_queue_claim_rows(&one, 1, true));
   assert(!r300_compute_verb_queue_claim_rows(&one, 1, false));
}

/* The verb-to-operation join every consumer follows to reach a route. */
static void
test_operation_join(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);

   for (uint32_t i = 0; i < count; i++) {
      const uint32_t routes =
         r300_operation_route_count_for_operation(rows[i].operation_id);
      /* Every verb reaches at least one route; identity_map and
       * bitwise_not_map reach two, a host route beside a device one. */
      assert(routes >= 1);
      assert(routes == (rows[i].verb == R300_COMPUTE_VERB_IDENTITY_MAP ||
                                rows[i].verb == R300_COMPUTE_VERB_BITWISE_NOT_MAP
                           ? 2u
                           : 1u));
   }

   assert(rows[R300_COMPUTE_VERB_IDENTITY_MAP].operation_id ==
          R300_OPERATION_ID_IDENTITY_MAP);
   assert(rows[R300_COMPUTE_VERB_CONST_FILL].operation_id ==
          R300_OPERATION_ID_CONSTFILL);
}

static void
test_names(void)
{
   for (unsigned c = 0; c < R300_COMPUTE_REFUSAL_CLASS_COUNT; c++)
      assert(r300_compute_refusal_class_name(
                (enum r300_compute_refusal_class)c) != NULL);
   assert(r300_compute_refusal_class_name(R300_COMPUTE_REFUSAL_CLASS_COUNT) ==
          NULL);
   for (unsigned c = 0; c < R300_COMPUTE_FAILURE_CLAUSE_COUNT; c++)
      assert(r300_compute_failure_clause_name(
                (enum r300_compute_failure_clause)c) != NULL);
   assert(r300_compute_failure_clause_name(
             R300_COMPUTE_FAILURE_CLAUSE_COUNT) == NULL);
   assert(strcmp(r300_compute_refusal_class_name(
                    R300_COMPUTE_REFUSAL_INTEGER_SHIFT),
                 "integer_shift") == 0);
   assert(strcmp(r300_compute_failure_clause_name(
                    R300_COMPUTE_FAILURE_ORACLE_DIVERGENCE_QUARANTINES),
                 "oracle_divergence_quarantines") == 0);
}

int
main(void)
{
   test_rows_well_formed();
   test_checker_calibration();
   test_queue_projection();
   test_operation_join();
   test_names();
   printf("r300_compute_verb_test: all checks passed\n");
   return 0;
}
