/*
 * SPDX-License-Identifier: MIT
 *
 * The compute verb ledger's well-formedness and precommitment: the
 * rows pass the checker, the checker refuses each calibrated
 * mutation, the identity map is the one verb whose CPU route executes
 * and the one job op maps onto it, no GPU route executes, and the
 * refusal classes and failure clauses carry their names.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_compute_verb.h"
#include "r300_numeric_domain.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
test_rows_well_formed(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);
   const char *reason = "unset";
   assert(count == R300_COMPUTE_VERB_COUNT);
   assert(r300_compute_verb_rows_valid(rows, count, &reason));
   assert(reason == NULL);
   for (uint32_t i = 0; i < count; i++) {
      assert(r300_compute_verb_row((enum r300_compute_verb)i) == &rows[i]);
      assert(strncmp(rows[i].gpu_gate, "R3V_NATIVE_COMPUTE_", 19) == 0);
      assert(strstr(rows[i].gpu_gate, "_GPU_EXPERIMENTAL") != NULL);
   }
   assert(r300_compute_verb_row(R300_COMPUTE_VERB_COUNT) == NULL);
}

/* Every catalog op a row names resolves in r300_virtual_op_catalog, so
 * the ledger inherits a domain and status the catalog owns rather than
 * restating them; the rows the catalog lacks say so with NULL. */
static void
test_catalog_binding(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);
   unsigned bound = 0;
   for (uint32_t i = 0; i < count; i++) {
      if (rows[i].catalog_op == NULL)
         continue;
      bool found = false;
      for (unsigned c = 0; c < r300_virtual_op_count(); c++) {
         if (strcmp(r300_virtual_op_catalog[c].op_name, rows[i].catalog_op) ==
             0)
            found = true;
      }
      if (!found)
         fprintf(stderr, "catalog op %s unresolved\n", rows[i].catalog_op);
      assert(found);
      bound++;
   }
   assert(bound == 10);
   assert(rows[R300_COMPUTE_VERB_IDENTITY_MAP].index_class ==
          R300_GRID_INDEX_LINEAR);
   assert(rows[R300_COMPUTE_VERB_MULTITAP_GATHER].index_class ==
          R300_GRID_INDEX_COORD);
}

/* Each rule of the checker refuses on a one-field mutation, and the
 * same table with the field restored passes again. */
static void
test_checker_calibration(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);
   struct r300_compute_verb_row mutated[R300_COMPUTE_VERB_COUNT];
   const char *reason = NULL;

   memcpy(mutated, rows, sizeof(mutated));
   assert(!r300_compute_verb_rows_valid(mutated, count - 1, &reason));
   assert(strcmp(reason, "row count outside the verb enum") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[1].verb = R300_COMPUTE_VERB_IDENTITY_MAP;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "rows out of enum order") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[2].name = rows[0].name;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "two rows share one name") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[3].gpu_gate = rows[0].gpu_gate;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "two rows share one gate") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_REDUCE].index_class =
      (enum r300_grid_index_class)(R300_GRID_INDEX_STRIDED + 1);
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "index class outside the grid-fold enum") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_IDENTITY_MAP].tolerance = 0.5f;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "tolerance disagrees with the exactness class") == 0);
   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_UNARY_TRANSCENDENTAL_MAP].tolerance = 0.0f;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "tolerance disagrees with the exactness class") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_BITWISE_LOGICOP_MAP].gpu_route =
      R300_COMPUTE_VERB_ROUTE_EXECUTING;
   mutated[R300_COMPUTE_VERB_BITWISE_LOGICOP_MAP].evidence =
      R300_COMPUTE_VERB_EVIDENCE_SOURCE_GROUNDED;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason,
                 "GPU route executing without retained silicon evidence") ==
          0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_CONST_FILL].gpu_route =
      R300_COMPUTE_VERB_ROUTE_PRECOMMITTED;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "host unit with a GPU route") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   assert(r300_compute_verb_rows_valid(mutated, count, &reason));
}

/* The precommitment the routes rest on: the identity map alone executes
 * on the CPU route and on a GPU route -- the R2VB carrier under the FP24
 * window, behind its exact gate -- every other GPU route is absent, and
 * the job op maps onto that row and onto nothing else. */
static void
test_precommitment(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);
   for (uint32_t i = 0; i < count; i++) {
      if (i == R300_COMPUTE_VERB_IDENTITY_MAP) {
         assert(rows[i].cpu_route == R300_COMPUTE_VERB_ROUTE_EXECUTING);
         assert(rows[i].gpu_route == R300_COMPUTE_VERB_ROUTE_EXECUTING);
      } else {
         assert(rows[i].cpu_route == R300_COMPUTE_VERB_ROUTE_ABSENT);
         assert(rows[i].gpu_route == R300_COMPUTE_VERB_ROUTE_ABSENT);
      }
   }
   const struct r300_compute_verb_row *identity =
      &rows[R300_COMPUTE_VERB_IDENTITY_MAP];
   assert(identity->unit == R300_COMPUTE_VERB_UNIT_R2VB_CARRIER);
   assert(identity->exactness == R300_COMPUTE_VERB_FP24_EXACT_WINDOW);
   assert(identity->gpu_route == R300_COMPUTE_VERB_ROUTE_EXECUTING);
   assert(strcmp(identity->gpu_gate,
                 "R3V_NATIVE_COMPUTE_IDENTITY_GPU_EXPERIMENTAL") == 0);

   const struct r300_compute_job job = { .op = R300_COMPUTE_JOB_OP_IDENTITY };
   assert(r300_compute_verb_for_job(&job) == identity);
   const struct r300_compute_job outside = { .op = 0x7f };
   assert(r300_compute_verb_for_job(&outside) == NULL);
   assert(r300_compute_verb_for_job(NULL) == NULL);
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
   test_catalog_binding();
   test_checker_calibration();
   test_precommitment();
   test_names();
   printf("r300_compute_verb_test: all checks passed\n");
   return 0;
}
