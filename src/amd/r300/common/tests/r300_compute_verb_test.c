/*
 * SPDX-License-Identifier: MIT
 *
 * The compute verb ledger's well-formedness and precommitment: rows pass
 * the checker, calibrated mutations fail, catalog joins agree on their
 * semantic contracts, route states satisfy their evidence requirements,
 * and refusal classes and failure clauses carry their names.
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

struct catalog_binding_expectation {
   enum r300_compute_verb verb;
   enum r300_operation_id operation_id;
   enum r300_numeric_domain domain;
   enum r300_compute_verb_unit unit;
   enum r300_compute_verb_exactness exactness;
};

static const struct catalog_binding_expectation catalog_bindings[] = {
   { R300_COMPUTE_VERB_IDENTITY_MAP, R300_OPERATION_ID_IDENTITY_MAP,
     R300_NUM_DOMAIN_FP24_RTZ, R300_COMPUTE_VERB_UNIT_R2VB_CARRIER,
     R300_COMPUTE_VERB_FP24_EXACT_WINDOW },
   { R300_COMPUTE_VERB_CONST_FILL, R300_OPERATION_ID_CONSTFILL,
     R300_NUM_DOMAIN_RB3D_BLEND, R300_COMPUTE_VERB_UNIT_RB3D_CLEAR,
     R300_COMPUTE_VERB_BIT_EXACT },
   { R300_COMPUTE_VERB_BINARY_ARITHMETIC_MAP,
     R300_OPERATION_ID_BINARY_MAP,
     R300_NUM_DOMAIN_FP24_RTZ, R300_COMPUTE_VERB_UNIT_US_FP24_ALU,
     R300_COMPUTE_VERB_FP24_EXACT_WINDOW },
   { R300_COMPUTE_VERB_MULTITAP_GATHER,
     R300_OPERATION_ID_MULTITAP_GATHER,
     R300_NUM_DOMAIN_FP24_RTZ, R300_COMPUTE_VERB_UNIT_US_FP24_ALU,
     R300_COMPUTE_VERB_FP24_EXACT_WINDOW },
   { R300_COMPUTE_VERB_PREDICATED_STORE,
     R300_OPERATION_ID_PREDICATED_MASKED_STORE,
     R300_NUM_DOMAIN_FP24_RTZ, R300_COMPUTE_VERB_UNIT_TX_RB3D_COPY,
     R300_COMPUTE_VERB_BIT_EXACT },
   { R300_COMPUTE_VERB_MULTIPASS_SCAN,
     R300_OPERATION_ID_MULTIPASS_PING_PONG_SCAN,
     R300_NUM_DOMAIN_FP24_RTZ, R300_COMPUTE_VERB_UNIT_US_FP24_ALU,
     R300_COMPUTE_VERB_FP24_EXACT_WINDOW },
   { R300_COMPUTE_VERB_REDUCE, R300_OPERATION_ID_BLEND_ACC_REDUCTION,
     R300_NUM_DOMAIN_RB3D_BLEND, R300_COMPUTE_VERB_UNIT_RB3D_BLEND,
     R300_COMPUTE_VERB_FP24_EXACT_WINDOW },
   { R300_COMPUTE_VERB_SATURATING_DIFF,
     R300_OPERATION_ID_SATURATING_DIFF,
     R300_NUM_DOMAIN_RB3D_BLEND, R300_COMPUTE_VERB_UNIT_RB3D_BLEND,
     R300_COMPUTE_VERB_BIT_EXACT },
   { R300_COMPUTE_VERB_PARALLEL_4OUT_MAP,
     R300_OPERATION_ID_PARALLEL_4OUT_MAP,
     R300_NUM_DOMAIN_FP24_RTZ, R300_COMPUTE_VERB_UNIT_US_FP24_ALU,
     R300_COMPUTE_VERB_FP24_EXACT_WINDOW },
   { R300_COMPUTE_VERB_STENCIL_INVERT,
     R300_OPERATION_ID_STENCIL_INVERT_NOT,
     R300_NUM_DOMAIN_U8_STENCIL, R300_COMPUTE_VERB_UNIT_ZB_STENCIL,
     R300_COMPUTE_VERB_BIT_EXACT },
};

static bool
catalog_binding_set_valid(
   const struct catalog_binding_expectation *expectations,
   unsigned expectation_count, const struct r300_compute_verb_row *rows,
   uint32_t row_count, const char **reason)
{
   unsigned coverage[R300_COMPUTE_VERB_COUNT] = { 0 };

   if (row_count != R300_COMPUTE_VERB_COUNT) {
      *reason = "row count outside the verb enum";
      return false;
   }
   for (unsigned i = 0; i < expectation_count; i++) {
      if ((unsigned)expectations[i].verb >= row_count) {
         *reason = "expectation verb outside the verb enum";
         return false;
      }
      if (++coverage[expectations[i].verb] != 1) {
         *reason = "duplicate expectation verb";
         return false;
      }
      for (unsigned j = 0; j < i; j++) {
         if (expectations[j].operation_id == expectations[i].operation_id) {
            *reason = "duplicate expectation operation";
            return false;
         }
      }
   }
   for (uint32_t i = 0; i < row_count; i++) {
      const unsigned expected_coverage =
         rows[i].operation_id == R300_OPERATION_ID_NONE ? 0 : 1;
      if (coverage[i] != expected_coverage) {
         *reason = "catalog binding coverage disagrees";
         return false;
      }
   }
   *reason = NULL;
   return true;
}

static bool
catalog_status_is_silicon_backed(enum r300_vop_status status)
{
   return status == R300_VOP_HW_CONFIRMED ||
          status == R300_VOP_HW_CONFIRMED_CARRIER_PENDING ||
          status == R300_VOP_BOUNDARY;
}

static bool
catalog_binding_valid(const struct r300_compute_verb_row *row,
                      const struct r300_virtual_op_info *op,
                      const struct catalog_binding_expectation *expected,
                      const char **reason)
{
   if (row->operation_id != expected->operation_id ||
       op->operation_id != expected->operation_id) {
      *reason = "operation identity disagrees";
      return false;
   }
   if (op->domain != expected->domain) {
      *reason = "catalog domain disagrees with the operation contract";
      return false;
   }
   if (row->unit != expected->unit) {
      *reason = "verb unit disagrees with the operation contract";
      return false;
   }
   if (row->exactness != expected->exactness) {
      *reason = "verb exactness disagrees with the operation contract";
      return false;
   }
   const bool catalog_silicon = catalog_status_is_silicon_backed(op->status);
   const bool verb_silicon =
      row->evidence == R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED;
   if (catalog_silicon != verb_silicon) {
      *reason = "catalog status and verb evidence disagree";
      return false;
   }
   if ((r300_vop_status_is_carrier_pending(op->status) ||
        op->status == R300_VOP_REJECTED) &&
       row->gpu_route == R300_COMPUTE_VERB_ROUTE_EXECUTING) {
      *reason = "non-executable catalog status has an executing GPU route";
      return false;
   }
   if (row->gpu_route == R300_COMPUTE_VERB_ROUTE_EXECUTING &&
       !catalog_silicon) {
      *reason = "executing GPU route lacks a silicon-backed catalog op";
      return false;
   }
   *reason = NULL;
   return true;
}

/* Every catalog op named by the ledger resolves exactly once, and the ten
 * current joins agree on domain, unit, exactness, evidence, and route state.
 * Rows without a catalog operation remain explicitly NONE. */
static void
test_catalog_binding(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);
   const unsigned binding_count =
      sizeof(catalog_bindings) / sizeof(catalog_bindings[0]);
   const char *reason = NULL;
   assert(catalog_binding_set_valid(catalog_bindings, binding_count, rows,
                                    count, &reason));
   assert(reason == NULL);

   for (unsigned i = 0; i < binding_count; i++) {
      const struct catalog_binding_expectation *expected = &catalog_bindings[i];
      const struct r300_compute_verb_row *row = &rows[expected->verb];
      const struct r300_virtual_op_info *op =
         r300_virtual_op_info_for_id(expected->operation_id);
      reason = NULL;
      assert(op != NULL);
      assert(catalog_binding_valid(row, op, expected, &reason));
      assert(reason == NULL);
   }

   assert(rows[R300_COMPUTE_VERB_IDENTITY_MAP].index_class ==
          R300_GRID_INDEX_LINEAR);
   assert(rows[R300_COMPUTE_VERB_MULTITAP_GATHER].index_class ==
          R300_GRID_INDEX_COORD);
}

static void
test_catalog_binding_calibration(void)
{
   const struct catalog_binding_expectation *expected = &catalog_bindings[3];
   const struct r300_compute_verb_row *row =
      r300_compute_verb_row(expected->verb);
   const struct r300_virtual_op_info *op =
      r300_virtual_op_info_for_id(expected->operation_id);
   struct r300_compute_verb_row mutated_row = *row;
   struct r300_virtual_op_info mutated_op = *op;
   struct catalog_binding_expectation
      mutated_bindings[sizeof(catalog_bindings) / sizeof(catalog_bindings[0])];
   const unsigned binding_count =
      sizeof(catalog_bindings) / sizeof(catalog_bindings[0]);
   uint32_t row_count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&row_count);
   const char *reason = NULL;

   assert(op != NULL);

   mutated_row.operation_id = R300_OPERATION_ID_BINARY_MAP;
   assert(!catalog_binding_valid(&mutated_row, op, expected, &reason));
   assert(strcmp(reason, "operation identity disagrees") == 0);

   mutated_row = *row;
   mutated_row.unit = R300_COMPUTE_VERB_UNIT_TX_RB3D_COPY;
   assert(!catalog_binding_valid(&mutated_row, op, expected, &reason));
   assert(strcmp(reason, "verb unit disagrees with the operation contract") == 0);

   mutated_row = *row;
   mutated_row.exactness = R300_COMPUTE_VERB_BIT_EXACT;
   assert(!catalog_binding_valid(&mutated_row, op, expected, &reason));
   assert(strcmp(reason, "verb exactness disagrees with the operation contract") == 0);

   mutated_op.domain = R300_NUM_DOMAIN_U7_DOT;
   assert(!catalog_binding_valid(row, &mutated_op, expected, &reason));
   assert(strcmp(reason,
                 "catalog domain disagrees with the operation contract") == 0);

   mutated_op = *op;
   mutated_row = *row;
   mutated_row.evidence = R300_COMPUTE_VERB_EVIDENCE_SOURCE_GROUNDED;
   assert(!catalog_binding_valid(&mutated_row, &mutated_op, expected, &reason));
   assert(strcmp(reason, "catalog status and verb evidence disagree") == 0);

   mutated_op.status = R300_VOP_NUMERIC_DERIVED;
   mutated_row = *row;
   assert(!catalog_binding_valid(&mutated_row, &mutated_op, expected, &reason));
   assert(strcmp(reason, "catalog status and verb evidence disagree") == 0);

   mutated_row = *row;
   mutated_op.status = R300_VOP_HW_CONFIRMED_CARRIER_PENDING;
   mutated_row.gpu_route = R300_COMPUTE_VERB_ROUTE_EXECUTING;
   assert(!catalog_binding_valid(&mutated_row, &mutated_op, expected, &reason));
   assert(strcmp(reason,
                 "non-executable catalog status has an executing GPU route") == 0);

   mutated_op.status = R300_VOP_REJECTED;
   mutated_row.evidence = R300_COMPUTE_VERB_EVIDENCE_SOURCE_GROUNDED;
   assert(!catalog_binding_valid(&mutated_row, &mutated_op, expected, &reason));
   assert(strcmp(reason,
                 "non-executable catalog status has an executing GPU route") == 0);

   mutated_op.status = R300_VOP_NUMERIC_DERIVED;
   assert(!catalog_binding_valid(&mutated_row, &mutated_op, expected, &reason));
   assert(strcmp(reason,
                 "executing GPU route lacks a silicon-backed catalog op") == 0);

   memcpy(mutated_bindings, catalog_bindings, sizeof(mutated_bindings));
   mutated_bindings[1].verb = mutated_bindings[0].verb;
   assert(!catalog_binding_set_valid(mutated_bindings, binding_count, rows,
                                     row_count, &reason));
   assert(strcmp(reason, "duplicate expectation verb") == 0);

   memcpy(mutated_bindings, catalog_bindings, sizeof(mutated_bindings));
   mutated_bindings[1].operation_id = mutated_bindings[0].operation_id;
   assert(!catalog_binding_set_valid(mutated_bindings, binding_count, rows,
                                     row_count, &reason));
   assert(strcmp(reason, "duplicate expectation operation") == 0);

   assert(!catalog_binding_set_valid(catalog_bindings, binding_count - 1,
                                     rows, row_count, &reason));
   assert(strcmp(reason, "catalog binding coverage disagrees") == 0);
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
   mutated[R300_COMPUTE_VERB_CONST_FILL].operation_id =
      R300_OPERATION_ID_COUNT;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "operation identity outside the catalog enum") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_CONST_FILL].implementation_id =
      R300_OPERATION_IMPLEMENTATION_COUNT;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason,
                 "implementation identity outside the implementation enum") ==
          0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_CONST_FILL].gpu_route_contract_id =
      R300_GPU_ROUTE_CONTRACT_COUNT;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "route contract outside the route-contract enum") ==
          0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_IDENTITY_MAP].implementation_id =
      R300_OPERATION_IMPLEMENTATION_NONE;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "implementation and route contract are partial") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_IDENTITY_MAP].gpu_route_contract_id =
      R300_GPU_ROUTE_CONTRACT_NONE;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "implementation and route contract are partial") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_CONST_FILL].cpu_route =
      (enum r300_compute_verb_route_status)
         (R300_COMPUTE_VERB_ROUTE_EXECUTING + 1);
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "route status outside the route-status enum") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_CONST_FILL].implementation_id =
      R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER;
   mutated[R300_COMPUTE_VERB_CONST_FILL].gpu_route_contract_id =
      R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason,
                 "absent GPU route carries an implementation contract") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_CONST_FILL].gpu_route =
      R300_COMPUTE_VERB_ROUTE_PRECOMMITTED;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "GPU route lacks an implementation contract") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_BITWISE_LOGICOP_MAP].gpu_route =
      R300_COMPUTE_VERB_ROUTE_PRECOMMITTED;
   mutated[R300_COMPUTE_VERB_BITWISE_LOGICOP_MAP].implementation_id =
      R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER;
   mutated[R300_COMPUTE_VERB_BITWISE_LOGICOP_MAP].gpu_route_contract_id =
      R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "GPU route lacks an operation identity") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_REDUCE].index_class =
      (enum r300_grid_index_class)(R300_GRID_INDEX_STRIDED + 1);
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "index class outside the grid-fold enum") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_REDUCE].unit =
      (enum r300_compute_verb_unit)(R300_COMPUTE_VERB_UNIT_RB3D_CLEAR + 1);
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "unit outside the compute-verb enum") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_REDUCE].exactness =
      (enum r300_compute_verb_exactness)(R300_COMPUTE_VERB_FP24_BOUNDED + 1);
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "exactness outside the exactness enum") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_IDENTITY_MAP].tolerance = 0.5f;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "tolerance disagrees with the exactness class") == 0);
   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_UNARY_TRANSCENDENTAL_MAP].tolerance = 0.0f;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "tolerance disagrees with the exactness class") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_CONST_FILL].evidence =
      (enum r300_compute_verb_evidence)
         (R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED + 1);
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "evidence outside the evidence enum") == 0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_IDENTITY_MAP].evidence =
      R300_COMPUTE_VERB_EVIDENCE_SOURCE_GROUNDED;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason,
                 "GPU route executing without retained silicon evidence") ==
          0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_IDENTITY_MAP].operation_id =
      R300_OPERATION_ID_QFMSUB;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason,
                 "executing GPU route has an inadmissible catalog status") ==
          0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_CONST_FILL].gpu_route =
      R300_COMPUTE_VERB_ROUTE_PRECOMMITTED;
   mutated[R300_COMPUTE_VERB_CONST_FILL].implementation_id =
      R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER;
   mutated[R300_COMPUTE_VERB_CONST_FILL].gpu_route_contract_id =
      R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "operation and implementation contract disagree") ==
          0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_CONST_FILL].operation_id =
      R300_OPERATION_ID_IDENTITY_MAP;
   mutated[R300_COMPUTE_VERB_CONST_FILL].gpu_route =
      R300_COMPUTE_VERB_ROUTE_EXECUTING;
   mutated[R300_COMPUTE_VERB_CONST_FILL].implementation_id =
      R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER;
   mutated[R300_COMPUTE_VERB_CONST_FILL].gpu_route_contract_id =
      R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER;
   assert(!r300_compute_verb_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "implementation contract and verb shape disagree") ==
          0);

   memcpy(mutated, rows, sizeof(mutated));
   mutated[R300_COMPUTE_VERB_IDENTITY_MAP].unit =
      R300_COMPUTE_VERB_UNIT_HOST;
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
   assert(identity->operation_id == R300_OPERATION_ID_IDENTITY_MAP);
   assert(identity->implementation_id ==
          R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER);
   assert(identity->gpu_route_contract_id ==
          R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER);
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
   test_catalog_binding_calibration();
   test_checker_calibration();
   test_precommitment();
   test_names();
   printf("r300_compute_verb_test: all checks passed\n");
   return 0;
}
