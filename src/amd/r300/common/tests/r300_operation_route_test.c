/*
 * SPDX-License-Identifier: MIT
 *
 * The operation route ledger: its population, every rule its checker states
 * refused on its own mutation, and the selector's fail-closed behavior when
 * a table offers two eligible routes or none.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_compute_verb.h"
#include "r300_operation_route.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MAX_ROUTES (R300_OPERATION_ROUTE_COUNT - 1)

/* The ledger's shape, pinned.  Two executing host routes, one executing GPU
 * route, fourteen candidates, no precommitted route; and the verb-level
 * aggregates the program reports.  A route landing moves these counts. */
static void
test_population(void)
{
   uint32_t count = 0;
   const struct r300_operation_route_row *rows =
      r300_operation_route_rows(&count);
   unsigned host_executing = 0, gpu_executing = 0;
   unsigned candidate = 0, precommitted = 0;
   unsigned retained_at_route_scope = 0, retained_at_raster_scope = 0;

   assert(rows != NULL && count == MAX_ROUTES && count == 17);
   for (uint32_t i = 0; i < count; i++) {
      const struct r300_operation_route_row *r = &rows[i];
      const bool exec = r->state == R300_OPERATION_ROUTE_EXECUTING;

      host_executing +=
         exec && r->executor == R300_OPERATION_ROUTE_EXECUTOR_HOST;
      gpu_executing +=
         exec && r->executor == R300_OPERATION_ROUTE_EXECUTOR_GPU;
      candidate += r->state == R300_OPERATION_ROUTE_CANDIDATE;
      precommitted += r->state == R300_OPERATION_ROUTE_PRECOMMITTED;
      if (r->evidence == R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED) {
         retained_at_route_scope +=
            r->evidence_scope ==
            R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL;
         retained_at_raster_scope +=
            r->evidence_scope == R300_COMPUTE_VERB_EVIDENCE_SCOPE_RASTER_CELL;
      }
      assert(r300_operation_route(r->route_id) == r);
   }

   assert(host_executing == 2);
   assert(gpu_executing == 1);
   assert(candidate == 14);
   assert(precommitted == 0);
   assert(retained_at_route_scope == 1);
   assert(retained_at_raster_scope == 13);

   /* The verb-level aggregates the same table yields. */
   uint32_t verbs = 0;
   const struct r300_compute_verb_row *verb_rows =
      r300_compute_verb_rows(&verbs);
   unsigned any_executing = 0, gpu_verbs = 0, none_executing = 0;
   for (uint32_t v = 0; v < verbs; v++) {
      const bool host = r300_operation_has_executing_route(
         verb_rows[v].operation_id, R300_OPERATION_ROUTE_EXECUTOR_HOST);
      const bool gpu = r300_operation_has_executing_route(
         verb_rows[v].operation_id, R300_OPERATION_ROUTE_EXECUTOR_GPU);
      any_executing += host || gpu;
      gpu_verbs += gpu;
      none_executing += !host && !gpu;
   }
   assert(verbs == 15);
   assert(any_executing == 2);
   assert(gpu_verbs == 1);
   assert(none_executing == 13);

   assert(r300_operation_route(R300_OPERATION_ROUTE_NONE) == NULL);
   assert(r300_operation_route(R300_OPERATION_ROUTE_COUNT) == NULL);
   assert(r300_operation_route((enum r300_operation_route_id)-1) == NULL);
}

/* The identity map's two routes differ in exactly the fields the split
 * exists to separate: one operation, two executors, two exactness bounds,
 * two evidence pairs.  The one-unit-per-verb table could not hold this. */
static void
test_identity_two_contracts(void)
{
   const struct r300_operation_route_row *host =
      r300_operation_route(R300_OPERATION_ROUTE_HOST_IDENTITY_MAP);
   const struct r300_operation_route_row *gpu =
      r300_operation_route(R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP);

   assert(host->operation_id == gpu->operation_id);
   assert(host->operation_id == R300_OPERATION_ID_IDENTITY_MAP);

   assert(host->executor == R300_OPERATION_ROUTE_EXECUTOR_HOST);
   assert(host->state == R300_OPERATION_ROUTE_EXECUTING);
   assert(host->unit == R300_EXECUTION_UNIT_HOST);
   assert(host->exactness == R300_COMPUTE_VERB_BIT_EXACT);
   assert(host->evidence == R300_COMPUTE_VERB_EVIDENCE_HOST);
   assert(host->evidence_scope ==
          R300_COMPUTE_VERB_EVIDENCE_SCOPE_HOST_EXECUTOR);
   assert(host->gate == NULL);
   assert(host->implementation_id == R300_OPERATION_IMPLEMENTATION_NONE);

   assert(gpu->executor == R300_OPERATION_ROUTE_EXECUTOR_GPU);
   assert(gpu->state == R300_OPERATION_ROUTE_EXECUTING);
   assert(gpu->unit == R300_EXECUTION_UNIT_R2VB_CARRIER);
   assert(gpu->exactness == R300_COMPUTE_VERB_FP24_EXACT_WINDOW);
   assert(gpu->index_class == R300_GRID_INDEX_LINEAR);
   assert(gpu->implementation_id ==
          R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER);
   assert(gpu->gpu_route_contract_id ==
          R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER);
   assert(gpu->admission_id == R300_ROUTE_ADMISSION_R2VB_FP24_IDENTITY);
   assert(gpu->evidence == R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED);
   assert(gpu->evidence_scope ==
          R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL);
   assert(gpu->gate != NULL);

   /* The ROP complement candidate carries the unit's documented encoding
    * rather than the host executor's tests: RB3D_ROPCNTL encodes INVERT and
    * no truth-table probe has run. */
   const struct r300_operation_route_row *rop =
      r300_operation_route(R300_OPERATION_ROUTE_RB3D_ROP_BITWISE_NOT);
   assert(rop->state == R300_OPERATION_ROUTE_CANDIDATE);
   assert(rop->evidence == R300_COMPUTE_VERB_EVIDENCE_SOURCE_GROUNDED);
   assert(rop->evidence_scope ==
          R300_COMPUTE_VERB_EVIDENCE_SCOPE_UNIT_CONTRACT);
}

#define REFUSES(reason_text)                                                 \
   do {                                                                      \
      assert(!r300_operation_route_rows_valid(m, count, &reason));            \
      assert(strcmp(reason, reason_text) == 0);                              \
      memcpy(m, rows, sizeof(*rows) * count);                                \
   } while (0)

static void
test_checker_calibration(void)
{
   uint32_t count = 0;
   const struct r300_operation_route_row *rows =
      r300_operation_route_rows(&count);
   struct r300_operation_route_row m[MAX_ROUTES];
   const char *reason = "unset";

   assert(r300_operation_route_rows_valid(rows, count, &reason));
   assert(reason == NULL);
   assert(!r300_operation_route_rows_valid(NULL, count, &reason));
   assert(strcmp(reason, "route table is empty") == 0);
   assert(!r300_operation_route_rows_valid(rows, 0, &reason));
   assert(strcmp(reason, "route table is empty") == 0);

   memcpy(m, rows, sizeof(m));

   m[0].route_id = R300_OPERATION_ROUTE_NONE;
   REFUSES("route identity outside the route enum");
   m[0].route_id = R300_OPERATION_ROUTE_COUNT;
   REFUSES("route identity outside the route enum");
   m[1].name = "";
   REFUSES("route lacks a diagnostic name");
   m[2].operation_id = R300_OPERATION_ID_NONE;
   REFUSES("route names no catalog operation");
   m[2].operation_id = R300_OPERATION_ID_COUNT;
   REFUSES("route names no catalog operation");
   m[3].executor = (enum r300_operation_route_executor)2;
   REFUSES("route executor outside the executor enum");
   m[3].state = (enum r300_operation_route_state)3;
   REFUSES("route state outside the state enum");
   m[4].unit = R300_EXECUTION_UNIT_COUNT;
   REFUSES("route unit outside the execution-unit enum");
   m[4].exactness = (enum r300_compute_verb_exactness)3;
   REFUSES("route exactness outside the exactness enum");
   m[4].tolerance = 0.5f;
   REFUSES("route tolerance disagrees with its exactness class");
   m[5].evidence = (enum r300_compute_verb_evidence)3;
   REFUSES("route evidence outside the evidence enum");
   m[5].evidence_scope = (enum r300_compute_verb_evidence_scope)4;
   REFUSES("route evidence scope outside the scope enum");

   /* Placement: a unit and a gate each belong to one executor. */
   m[1].unit = R300_EXECUTION_UNIT_HOST;
   REFUSES("GPU route runs on the host unit");
   m[0].unit = R300_EXECUTION_UNIT_RB3D_ROP;
   REFUSES("host route runs on a device unit");
   m[0].gate = "R3V_NATIVE_TEST_HOST_GATE";
   REFUSES("host route carries a gate");

   /* A GPU row left at the evidence enums' zero values would read as
    * host-tested; the checker refuses that rather than defaulting. */
   m[2].evidence = R300_COMPUTE_VERB_EVIDENCE_HOST;
   m[2].evidence_scope = R300_COMPUTE_VERB_EVIDENCE_SCOPE_HOST_EXECUTOR;
   REFUSES("GPU route claims host-executor evidence");
   m[0].evidence_scope =
      R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL;
   REFUSES("host route claims native-GPU-route evidence");

   /* Maturity: contracts move together with state, and an executing GPU
    * route names evidence for its own retained cell. */
   m[2].implementation_id =
      R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER;
   m[2].gpu_route_contract_id =
      R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER;
   m[2].admission_id = R300_ROUTE_ADMISSION_R2VB_FP24_IDENTITY;
   REFUSES("candidate route carries an implementation contract");
   m[2].state = R300_OPERATION_ROUTE_PRECOMMITTED;
   REFUSES("committed GPU route lacks implementation, contract, or "
           "admission");
   m[1].evidence_scope = R300_COMPUTE_VERB_EVIDENCE_SCOPE_UNIT_CONTRACT;
   REFUSES("executing GPU route lacks exact native-route evidence");

   /* Uniqueness. */
   m[3].route_id = m[2].route_id;
   REFUSES("route table repeats a route identity");
   m[3].name = m[2].name;
   REFUSES("route table repeats a diagnostic name");
   m[3].gate = m[2].gate;
   REFUSES("route table repeats a gate");

   /* Identity order is what r300_operation_route()'s subtraction depends
    * on: a swapped pair resolves every lookup for both to the other row. */
   {
      const struct r300_operation_route_row swap = m[4];
      m[4] = m[5];
      m[5] = swap;
   }
   REFUSES("routes out of identity order");

   /* Two executing routes for one operation and executor would leave the
    * selector choosing by table position.  The table refuses that shape
    * here rather than at the first dispatch that meets it -- the rule that
    * makes a second CONST_FILL route safe to add. */
   m[2].state = R300_OPERATION_ROUTE_EXECUTING;
   m[2].operation_id = R300_OPERATION_ID_IDENTITY_MAP;
   m[2].implementation_id =
      R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER;
   m[2].gpu_route_contract_id =
      R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER;
   m[2].admission_id = R300_ROUTE_ADMISSION_R2VB_FP24_IDENTITY;
   m[2].evidence = R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED;
   m[2].evidence_scope =
      R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL;
   REFUSES("two executing routes for one operation and executor");
}

#undef REFUSES

static void
test_selector(void)
{
   uint32_t count = 0;
   const struct r300_operation_route_row *rows =
      r300_operation_route_rows(&count);
   bool gates[R300_OPERATION_ROUTE_COUNT] = { false };
   const char *reason = NULL;

   /* Every gate closed: the host route is the default and needs none, and
    * the gated GPU route is unreachable. */
   assert(r300_operation_select_route(R300_OPERATION_ID_IDENTITY_MAP,
                                      R300_OPERATION_ROUTE_EXECUTOR_HOST,
                                      gates, &reason) ==
          r300_operation_route(R300_OPERATION_ROUTE_HOST_IDENTITY_MAP));
   assert(r300_operation_select_route(R300_OPERATION_ID_IDENTITY_MAP,
                                      R300_OPERATION_ROUTE_EXECUTOR_GPU, gates,
                                      &reason) == NULL);
   assert(strcmp(reason, "no eligible executing route") == 0);
   assert(r300_operation_select_route(R300_OPERATION_ID_IDENTITY_MAP,
                                      R300_OPERATION_ROUTE_EXECUTOR_GPU, NULL,
                                      &reason) == NULL);

   /* The exact route's own gate selects it. */
   gates[R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP] = true;
   assert(r300_operation_select_route(R300_OPERATION_ID_IDENTITY_MAP,
                                      R300_OPERATION_ROUTE_EXECUTOR_GPU, gates,
                                      &reason) ==
          r300_operation_route(R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP));

   /* Every other gate open selects nothing: a gate belongs to one route,
    * and an open gate on a candidate route opens no execution. */
   memset(gates, 0, sizeof(gates));
   for (uint32_t r = 0; r < count; r++) {
      if (rows[r].route_id != R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP)
         gates[rows[r].route_id] = true;
   }
   assert(r300_operation_select_route(R300_OPERATION_ID_IDENTITY_MAP,
                                      R300_OPERATION_ROUTE_EXECUTOR_GPU, gates,
                                      &reason) == NULL);
   assert(r300_operation_select_route(R300_OPERATION_ID_CONSTFILL,
                                      R300_OPERATION_ROUTE_EXECUTOR_GPU, gates,
                                      &reason) == NULL);
   assert(strcmp(reason, "no eligible executing route") == 0);
   /* An operation with no host route selects nothing on the host either. */
   assert(r300_operation_select_route(R300_OPERATION_ID_CONSTFILL,
                                      R300_OPERATION_ROUTE_EXECUTOR_HOST,
                                      gates, &reason) == NULL);

   /* Two eligible routes fail closed rather than letting table order pick.
    * The shipped table cannot hold that shape, so the arm runs on a mutated
    * copy through the _rows form. */
   struct r300_operation_route_row m[MAX_ROUTES];
   memcpy(m, rows, sizeof(*rows) * count);
   for (uint32_t r = 0; r < count; r++) {
      if (m[r].route_id == R300_OPERATION_ROUTE_RB3D_CLEAR_CONST_FILL) {
         m[r].state = R300_OPERATION_ROUTE_EXECUTING;
         m[r].operation_id = R300_OPERATION_ID_IDENTITY_MAP;
         m[r].gate = NULL;
      }
   }
   memset(gates, 0, sizeof(gates));
   gates[R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP] = true;
   assert(r300_operation_select_route_rows(
             m, count, R300_OPERATION_ID_IDENTITY_MAP,
             R300_OPERATION_ROUTE_EXECUTOR_GPU, gates, &reason) == NULL);
   assert(strcmp(reason, "two eligible routes and no selector policy") == 0);
}

/* Restoring a per-verb gate array cannot represent the ledger: two routes
 * for one operation carry two gates, and a verb-indexed array holds one.
 * The counts below are what such an array would lose. */
static void
test_per_verb_gate_array_cannot_represent(void)
{
   uint32_t count = 0;
   const struct r300_operation_route_row *rows =
      r300_operation_route_rows(&count);
   uint32_t verbs = 0;
   const struct r300_compute_verb_row *verb_rows =
      r300_compute_verb_rows(&verbs);
   unsigned gated = 0;

   for (uint32_t r = 0; r < count; r++)
      gated += rows[r].gate != NULL;

   /* Fifteen gated routes over fifteen verbs looks representable until the
    * operations are counted: bitwise_not_map's operation carries a gated
    * GPU route beside an ungated host route, and identity_map's carries the
    * executing gated one, so two verbs own two routes each and a
    * verb-indexed array is one slot short of the routes it must gate. */
   assert(gated == 15);
   assert(count > verbs);
   for (uint32_t v = 0; v < verbs; v++) {
      const uint32_t n =
         r300_operation_route_count_for_operation(verb_rows[v].operation_id);
      assert(n >= 1);
   }
   assert(r300_operation_route_count_for_operation(
             R300_OPERATION_ID_IDENTITY_MAP) == 2);
   assert(r300_operation_route_count_for_operation(
             R300_OPERATION_ID_BITWISE_NOT_MAP) == 2);
}

static void
test_names(void)
{
   for (unsigned u = 0; u < R300_EXECUTION_UNIT_COUNT; u++)
      assert(r300_execution_unit_name((enum r300_execution_unit)u) != NULL);
   assert(r300_execution_unit_name(R300_EXECUTION_UNIT_COUNT) == NULL);

   assert(strcmp(r300_operation_route_executor_name(
                    R300_OPERATION_ROUTE_EXECUTOR_GPU),
                 "gpu") == 0);
   assert(r300_operation_route_executor_name(
             (enum r300_operation_route_executor)2) == NULL);
   assert(strcmp(r300_operation_route_state_name(
                    R300_OPERATION_ROUTE_CANDIDATE),
                 "candidate") == 0);
   assert(r300_operation_route_state_name(
             (enum r300_operation_route_state)3) == NULL);
   assert(strcmp(r300_compute_verb_evidence_name(
                    R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED),
                 "silicon_retained") == 0);
   assert(r300_compute_verb_evidence_name(
             (enum r300_compute_verb_evidence)3) == NULL);
   assert(strcmp(r300_compute_verb_evidence_scope_name(
                    R300_COMPUTE_VERB_EVIDENCE_SCOPE_RASTER_CELL),
                 "raster_cell") == 0);
   assert(r300_compute_verb_evidence_scope_name(
             (enum r300_compute_verb_evidence_scope)4) == NULL);
   assert(strcmp(r300_compute_verb_exactness_name(
                    R300_COMPUTE_VERB_FP24_BOUNDED),
                 "fp24_bounded") == 0);
   assert(r300_compute_verb_exactness_name(
             (enum r300_compute_verb_exactness)3) == NULL);
}

int
main(void)
{
   test_population();
   test_identity_two_contracts();
   test_checker_calibration();
   test_selector();
   test_per_verb_gate_array_cannot_represent();
   test_names();
   printf("r300_operation_route_test: all checks passed\n");
   return 0;
}
