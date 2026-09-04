/*
 * SPDX-License-Identifier: MIT
 *
 * The policy decides which executor performs an operation, and the
 * provenance records which one did and how far it travelled.  Both are
 * checkable without a device, and both carry the claims a hardware result
 * rests on, so they are checked here rather than inferred from a submission
 * that happened to work.
 *
 * The arms that matter are the refusals.  A closed gate reaches the host,
 * GPU_ONLY with no qualified route refuses rather than falling back, a use
 * mask naming two purposes reaches neither scan, a precommitted route runs
 * only under its own cached opt-in and says so in its provenance, and the
 * phase ladder advances one state at a time.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r3v_route_policy.h"

#include "util/macros.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The RB2D fill route's identity, read from the ledger so a table movement
 * moves this test with it rather than leaving it opening a gate no row
 * carries. */
static const struct r300_operation_route_row *
rb2d_fill_row(void)
{
   const struct r300_operation_route_row *row =
      r300_operation_route(R300_OPERATION_ROUTE_RB2D_CONST_FILL);
   assert(row != NULL && row->gate != NULL);
   assert(row->state == R300_OPERATION_ROUTE_PRECOMMITTED);
   return row;
}

static struct r3v_route_request
fill_request(enum r3v_execution_policy policy)
{
   return (struct r3v_route_request){
      .operation_id = R300_OPERATION_ID_CONSTFILL,
      .use = R300_ROUTE_USE_TRANSFER_BUFFER,
      .policy = policy,
      .byte_offset = 12u,
      .byte_size = 4096u,
      .element_bytes = 4u,
      .destination_device_visible = true,
      .destination_host_mapped = true,
   };
}

static void
test_policy_values(void)
{
   assert(r3v_execution_policy_from_value(NULL) == R3V_EXECUTION_AUTO);
   assert(r3v_execution_policy_from_value("gpu_only") ==
          R3V_EXECUTION_GPU_ONLY);
   assert(r3v_execution_policy_from_value("cpu_reference") ==
          R3V_EXECUTION_CPU_REFERENCE);
   /* An unset or empty value is the default an operator who asked for
    * nothing gets; every other unmatched value names no policy, so the
    * device refuses it rather than running under AUTO. */
   assert(r3v_execution_policy_from_value("") == R3V_EXECUTION_AUTO);
   assert(r3v_execution_policy_from_value("auto") == R3V_EXECUTION_AUTO);
   assert(r3v_execution_policy_from_value("GPU_ONLY") ==
          R3V_EXECUTION_POLICY_INVALID);
   assert(r3v_execution_policy_from_value("gpu_onl") ==
          R3V_EXECUTION_POLICY_INVALID);
   assert(r3v_execution_policy_from_value("gpu_only ") ==
          R3V_EXECUTION_POLICY_INVALID);
   assert(strcmp(r3v_execution_policy_name(R3V_EXECUTION_POLICY_INVALID),
                 "invalid") == 0);
   assert(r3v_execution_policy_from_value(" gpu_only") ==
          R3V_EXECUTION_POLICY_INVALID);

   for (unsigned i = 0; i <= R3V_EXECUTION_POLICY_INVALID; i++)
      assert(r3v_execution_policy_name((enum r3v_execution_policy)i) != NULL);
   assert(r3v_execution_policy_name(
             (enum r3v_execution_policy)(R3V_EXECUTION_POLICY_INVALID + 1)) ==
          NULL);
   for (unsigned i = 0; i <= R3V_ROUTE_DECISION_REFUSE; i++)
      assert(r3v_route_decision_name((enum r3v_route_decision)i) != NULL);
   assert(r3v_route_decision_name(
             (enum r3v_route_decision)(R3V_ROUTE_DECISION_REFUSE + 1)) ==
          NULL);
}

static void
test_gate_closed_reaches_the_host(void)
{
   bool gates[R300_OPERATION_ROUTE_COUNT] = { false };
   const struct r300_operation_route_row *route = NULL;
   const char *reason = NULL;

   /* AUTO with every gate closed is the shipped path: the host performs the
    * fill and no route is named. */
   struct r3v_route_request request = fill_request(R3V_EXECUTION_AUTO);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_HOST);
   assert(route == NULL && reason != NULL);

   /* GPU_ONLY with the same closed gate refuses.  Reaching the host here
    * would make the policy a preference, and a preference proves nothing. */
   request = fill_request(R3V_EXECUTION_GPU_ONLY);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   assert(route == NULL);

   /* A NULL gate array is every gate closed, so a caller that supplies no
    * cache opens no route. */
   assert(r3v_route_policy_select(&request, NULL, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   /* CPU_REFERENCE reaches the host whatever the gates say. */
   gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = true;
   request = fill_request(R3V_EXECUTION_CPU_REFERENCE);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_HOST);
   assert(route == NULL);
}

static void
test_cached_gate_admits_the_precommitted_route(void)
{
   bool gates[R300_OPERATION_ROUTE_COUNT] = { false };
   const struct r300_operation_route_row *route = NULL;
   const char *reason = NULL;
   struct r3v_route_request request = fill_request(R3V_EXECUTION_GPU_ONLY);

   gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = true;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_GPU);
   assert(route == rb2d_fill_row());
   assert(route->unit == R300_EXECUTION_UNIT_RB2D_FILL);

   /* A gate belongs to one route: opening every other entry admits nothing
    * more, and closing this one closes the route. */
   for (unsigned r = 0; r < R300_OPERATION_ROUTE_COUNT; r++)
      gates[r] = r != R300_OPERATION_ROUTE_RB2D_CONST_FILL;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   memset(gates, 0, sizeof(gates));
   gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = true;

   /* The route serves one use.  A request naming the render attachment or a
    * storage buffer reaches it not at all, however open its gate stands:
    * applicability is the filter. */
   request.use = R300_ROUTE_USE_RENDER_ATTACHMENT;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   request.use = R300_ROUTE_USE_COMPUTE_STORAGE_BUFFER;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   /* A different operation reaches it not at all either. */
   request = fill_request(R3V_EXECUTION_GPU_ONLY);
   request.operation_id = R300_OPERATION_ID_IDENTITY_MAP;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
}

/* The use check stands ahead of both scans.  The promoted selector holds the
 * one-use rule itself, while the precommitted scan tests one bit of the mask
 * against a row's mask; without the hoisted check a request naming two
 * purposes would be refused by the first and admitted by the second, and the
 * open gate would carry a transfer route into a storage-buffer request. */
static void
test_use_mask_names_one_purpose(void)
{
   bool gates[R300_OPERATION_ROUTE_COUNT] = { false };
   const struct r300_operation_route_row *route = NULL;
   const char *reason = NULL;
   struct r3v_route_request request = fill_request(R3V_EXECUTION_GPU_ONLY);

   gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = true;

   request.use = (enum r300_operation_route_use)(
      R300_ROUTE_USE_TRANSFER_BUFFER | R300_ROUTE_USE_COMPUTE_STORAGE_BUFFER);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   assert(route == NULL && reason != NULL);

   request.use = (enum r300_operation_route_use)R300_ROUTE_USE_ALL;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   request.use = (enum r300_operation_route_use)0u;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   /* A bit outside the vocabulary refuses for the same reason: no row
    * serves it, and admitting it would let a later use land unreviewed. */
   request.use =
      (enum r300_operation_route_use)((uint32_t)R300_ROUTE_USE_ALL + 1u);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   /* AUTO refuses the same masks rather than falling to the host: a request
    * naming no single purpose has no host answer either. */
   request.policy = R3V_EXECUTION_AUTO;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   /* The single defined bit is admitted, so the check separates malformed
    * masks from the request the route serves. */
   request = fill_request(R3V_EXECUTION_GPU_ONLY);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_GPU);
}

/* The request's resource shape decides before the ledger does.  A range that
 * is not a whole number of elements describes no operation for either
 * executor, and a destination the device cannot reach disqualifies every GPU
 * row: without these the selector would answer GPU for a shape no route can
 * carry, and the caller would learn that only at emission. */
static void
test_resource_shape_gates_the_gpu_decision(void)
{
   bool gates[R300_OPERATION_ROUTE_COUNT] = { false };
   const struct r300_operation_route_row *route = NULL;
   const char *reason = NULL;
   struct r3v_route_request request = fill_request(R3V_EXECUTION_GPU_ONLY);

   gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = true;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_GPU);

   /* A zero element width counts nothing. */
   request = fill_request(R3V_EXECUTION_GPU_ONLY);
   request.element_bytes = 0;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   request.policy = R3V_EXECUTION_AUTO;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   /* A range that is not a whole number of elements, and one that starts
    * off the element boundary, each refuse for either executor. */
   request = fill_request(R3V_EXECUTION_AUTO);
   request.byte_size = 4094u;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   request = fill_request(R3V_EXECUTION_AUTO);
   request.byte_offset = 13u;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   /* A destination the device cannot reach: GPU_ONLY refuses, and AUTO
    * takes the host path with no route named. */
   request = fill_request(R3V_EXECUTION_GPU_ONLY);
   request.destination_device_visible = false;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   assert(route == NULL && reason != NULL);
   request.policy = R3V_EXECUTION_AUTO;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_HOST);
   assert(route == NULL);
   /* CPU_REFERENCE reaches the host through its own clause, ahead of the
    * device-visibility question. */
   request.policy = R3V_EXECUTION_CPU_REFERENCE;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_HOST);

   /* A destination the host cannot map closes the host path: a reference
    * run that cannot touch the buffer is no reference, and an AUTO request
    * with no qualified GPU route has nowhere to fall. */
   request = fill_request(R3V_EXECUTION_CPU_REFERENCE);
   request.destination_host_mapped = false;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   request.policy = R3V_EXECUTION_AUTO;
   request.use = R300_ROUTE_USE_RENDER_ATTACHMENT;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   /* The same destination still reaches the open GPU route, because the
    * device can write it. */
   request.use = R300_ROUTE_USE_TRANSFER_BUFFER;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_GPU);

   /* A destination neither executor reaches has no answer at all. */
   request = fill_request(R3V_EXECUTION_AUTO);
   request.destination_device_visible = false;
   request.destination_host_mapped = false;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   request.policy = R3V_EXECUTION_CPU_REFERENCE;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   request.policy = R3V_EXECUTION_GPU_ONLY;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
}

static void
test_malformed_requests_refuse(void)
{
   bool gates[R300_OPERATION_ROUTE_COUNT] = { false };
   const struct r300_operation_route_row *route = NULL;
   const char *reason = NULL;

   gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = true;
   assert(r3v_route_policy_select(NULL, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   struct r3v_route_request request = fill_request(R3V_EXECUTION_AUTO);
   request.byte_size = 0;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   /* The operation is the join into the catalog, so one naming nothing and
    * one outside the catalog reach no executor.  Each policy asks the same
    * question, so each refuses. */
   for (unsigned pol = 0; pol <= R3V_EXECUTION_CPU_REFERENCE; pol++) {
      request = fill_request((enum r3v_execution_policy)pol);
      request.operation_id = R300_OPERATION_ID_NONE;
      assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
             R3V_ROUTE_DECISION_REFUSE);
      request.operation_id = (enum r300_operation_id)R300_OPERATION_ID_COUNT;
      assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
             R3V_ROUTE_DECISION_REFUSE);
   }

   request = fill_request((enum r3v_execution_policy)7);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
}

static void
test_execution_phase_ladder(void)
{
   const char *reason = NULL;

   for (unsigned i = 0; i <= R3V_EXECUTION_PHASE_RESULT_VERIFIED; i++)
      assert(r3v_execution_phase_name((enum r3v_execution_phase)i) != NULL);
   assert(r3v_execution_phase_name((enum r3v_execution_phase)(
             R3V_EXECUTION_PHASE_RESULT_VERIFIED + 1)) == NULL);

   /* The six phase names are the vocabulary a record reports; a rename
    * would move every retained provenance, so the spellings are pinned. */
   static const char *const spellings[] = {
      "prepared",       "committed",          "ioctl_entered",
      "ioctl_accepted", "completion_retired", "result_verified",
   };
   for (unsigned i = 0; i <= R3V_EXECUTION_PHASE_RESULT_VERIFIED; i++)
      assert(strcmp(r3v_execution_phase_name((enum r3v_execution_phase)i),
                    spellings[i]) == 0);

   /* The ladder climbs one state at a time from prepared to verified. */
   enum r3v_execution_phase phase = R3V_EXECUTION_PHASE_PREPARED;
   for (unsigned i = 1; i <= R3V_EXECUTION_PHASE_RESULT_VERIFIED; i++) {
      assert(r3v_execution_phase_advance(
         &phase, (enum r3v_execution_phase)i, &reason));
      assert(phase == (enum r3v_execution_phase)i);
   }

   /* A skip asserts a state nothing observed: a record cannot report a
    * retired completion without an accepted ioctl beneath it. */
   phase = R3V_EXECUTION_PHASE_COMMITTED;
   assert(!r3v_execution_phase_advance(
      &phase, R3V_EXECUTION_PHASE_COMPLETION_RETIRED, &reason));
   assert(phase == R3V_EXECUTION_PHASE_COMMITTED && reason != NULL);

   /* A repeat and a step back both refuse: the ladder records progress. */
   assert(!r3v_execution_phase_advance(
      &phase, R3V_EXECUTION_PHASE_COMMITTED, &reason));
   assert(!r3v_execution_phase_advance(
      &phase, R3V_EXECUTION_PHASE_PREPARED, &reason));
   assert(!r3v_execution_phase_advance(
      &phase, (enum r3v_execution_phase)(R3V_EXECUTION_PHASE_RESULT_VERIFIED +
                                         1), &reason));
   assert(!r3v_execution_phase_advance(NULL, R3V_EXECUTION_PHASE_COMMITTED,
                                       &reason));
   assert(phase == R3V_EXECUTION_PHASE_COMMITTED);
}

static struct r3v_execution_provenance
gpu_provenance(void)
{
   const struct r300_operation_route_row *row = rb2d_fill_row();
   return (struct r3v_execution_provenance){
      .operation_id = row->operation_id,
      .route_id = row->route_id,
      .unit = row->unit,
      .executor = row->executor,
      .route_state = row->state,
      .phase = R3V_EXECUTION_PHASE_IOCTL_ACCEPTED,
      .host_semantic_node = false,
      .device_submission = true,
      .experimental_admission = true,
      .ib_dwords = 26u,
      .relocation_count = 1u,
   };
}

static struct r3v_execution_provenance
host_provenance(void)
{
   const struct r300_operation_route_row *row =
      r300_operation_route(R300_OPERATION_ROUTE_HOST_TRANSFER_CONST_FILL);
   return (struct r3v_execution_provenance){
      .operation_id = row->operation_id,
      .route_id = row->route_id,
      .unit = row->unit,
      .executor = row->executor,
      .route_state = row->state,
      .phase = R3V_EXECUTION_PHASE_COMMITTED,
      .host_semantic_node = true,
      .device_submission = false,
   };
}

/* The one executing GPU route the ledger carries, for the arms that need a
 * promoted row rather than the precommitted fill route. */
static struct r3v_execution_provenance
executing_gpu_provenance(void)
{
   const struct r300_operation_route_row *row =
      r300_operation_route(R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP);
   return (struct r3v_execution_provenance){
      .operation_id = row->operation_id,
      .route_id = row->route_id,
      .unit = row->unit,
      .executor = row->executor,
      .route_state = row->state,
      .phase = R3V_EXECUTION_PHASE_IOCTL_ACCEPTED,
      .host_semantic_node = false,
      .device_submission = true,
      .experimental_admission = false,
      .ib_dwords = 231u,
      .relocation_count = 2u,
   };
}

static void
test_provenance(void)
{
   const char *reason = NULL;
   struct r3v_execution_provenance p = gpu_provenance();

   assert(r3v_execution_provenance_valid(&p, R3V_EXECUTION_GPU_ONLY, &reason));
   assert(r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   assert(!r3v_execution_provenance_valid(NULL, R3V_EXECUTION_AUTO, &reason));

   /* The three facts a hardware claim consists of, each refused alone. */
   p = gpu_provenance();
   p.host_semantic_node = true;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_GPU_ONLY,
                                          &reason));
   p = gpu_provenance();
   p.device_submission = false;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_GPU_ONLY,
                                          &reason));
   p = gpu_provenance();
   p.executor = R300_OPERATION_ROUTE_EXECUTOR_HOST;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_GPU_ONLY,
                                          &reason));

   /* An empty stream is no submission whatever the flags say. */
   p = gpu_provenance();
   p.ib_dwords = 0;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));

   /* Experimental admission and promotion imply each other in both
    * directions, so neither can be overstated.  The precommitted fill route
    * reports an experimental admission or refuses; the promoted carrier
    * route reports none or refuses. */
   p = gpu_provenance();
   p.experimental_admission = false;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   struct r3v_execution_provenance promoted = executing_gpu_provenance();
   assert(r3v_execution_provenance_valid(&promoted, R3V_EXECUTION_AUTO,
                                         &reason));
   assert(r3v_execution_provenance_valid(&promoted, R3V_EXECUTION_GPU_ONLY,
                                         &reason));
   promoted.experimental_admission = true;
   assert(!r3v_execution_provenance_valid(&promoted, R3V_EXECUTION_AUTO,
                                          &reason));

   /* A record naming a route is held to that route's row: a maturity, unit,
    * executor, or operation of its own describes a delivery the ledger does
    * not carry. */
   p = gpu_provenance();
   p.route_state = R300_OPERATION_ROUTE_EXECUTING;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   /* A candidate maturity on a precommitted row keeps the experimental
    * admission consistent, so only the ledger comparison refuses it. */
   p = gpu_provenance();
   p.route_state = R300_OPERATION_ROUTE_CANDIDATE;
   assert(p.experimental_admission);
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   p = gpu_provenance();
   p.unit = R300_EXECUTION_UNIT_RB3D_CLEAR;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   p = gpu_provenance();
   p.operation_id = R300_OPERATION_ID_IDENTITY_MAP;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   p = gpu_provenance();
   p.executor = R300_OPERATION_ROUTE_EXECUTOR_HOST;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   p = gpu_provenance();
   p.route_id = R300_OPERATION_ROUTE_COUNT;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));

   /* A host record that names no row keeps the ledger's zero values and
    * stays valid: naming a route is what invites the comparison. */
   struct r3v_execution_provenance anonymous = {
      .operation_id = R300_OPERATION_ID_CONSTFILL,
      .executor = R300_OPERATION_ROUTE_EXECUTOR_HOST,
      .phase = R3V_EXECUTION_PHASE_PREPARED,
      .host_semantic_node = true,
   };
   assert(r3v_execution_provenance_valid(&anonymous, R3V_EXECUTION_AUTO,
                                         &reason));
   /* An anonymous record claiming a device unit or a maturity asserts what
    * no row assigns it. */
   struct r3v_execution_provenance claimed = anonymous;
   claimed.unit = R300_EXECUTION_UNIT_RB2D_FILL;
   assert(!r3v_execution_provenance_valid(&claimed, R3V_EXECUTION_AUTO,
                                          &reason));
   claimed = anonymous;
   claimed.route_state = R300_OPERATION_ROUTE_EXECUTING;
   assert(!r3v_execution_provenance_valid(&claimed, R3V_EXECUTION_AUTO,
                                          &reason));

   /* An operation outside the catalog and an executor outside its enum
    * each refuse: the first names nothing a route realizes, and the second
    * would fall to the host branch and be compared against nothing.  A unit
    * or maturity outside its enum refuses through the row comparison or the
    * anonymous zero values above, which is why neither carries a bound of
    * its own. */
   struct r3v_execution_provenance wild = anonymous;
   wild.operation_id = (enum r300_operation_id)R300_OPERATION_ID_COUNT;
   assert(!r3v_execution_provenance_valid(&wild, R3V_EXECUTION_AUTO,
                                          &reason));
   wild = anonymous;
   wild.executor = (enum r300_operation_route_executor)7;
   assert(!r3v_execution_provenance_valid(&wild, R3V_EXECUTION_AUTO,
                                          &reason));
   wild = anonymous;
   wild.unit = (enum r300_execution_unit)R300_EXECUTION_UNIT_COUNT;
   assert(!r3v_execution_provenance_valid(&wild, R3V_EXECUTION_AUTO,
                                          &reason));
   wild = anonymous;
   wild.route_state = (enum r300_operation_route_state)9;
   assert(!r3v_execution_provenance_valid(&wild, R3V_EXECUTION_AUTO,
                                          &reason));
   wild = gpu_provenance();
   wild.unit = (enum r300_execution_unit)R300_EXECUTION_UNIT_COUNT;
   assert(!r3v_execution_provenance_valid(&wild, R3V_EXECUTION_AUTO,
                                          &reason));
   wild = gpu_provenance();
   wild.route_state = (enum r300_operation_route_state)9;
   assert(!r3v_execution_provenance_valid(&wild, R3V_EXECUTION_AUTO,
                                          &reason));

   /* The submission flag and the phase state one fact, so a record short of
    * the ioctl entry reporting a submission refuses, and one past it
    * reporting none refuses too. */
   p = gpu_provenance();
   p.phase = R3V_EXECUTION_PHASE_COMMITTED;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   p = gpu_provenance();
   p.device_submission = false;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   p = gpu_provenance();
   p.phase = R3V_EXECUTION_PHASE_RESULT_VERIFIED;
   assert(r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   p.phase = (enum r3v_execution_phase)(R3V_EXECUTION_PHASE_RESULT_VERIFIED +
                                        1);
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));

   /* A host record: it computed the result and submitted nothing. */
   struct r3v_execution_provenance host = host_provenance();
   assert(r3v_execution_provenance_valid(&host, R3V_EXECUTION_AUTO, &reason));
   assert(r3v_execution_provenance_valid(&host, R3V_EXECUTION_CPU_REFERENCE,
                                         &reason));
   /* Under GPU_ONLY the same record is the failure the policy exists to
    * catch. */
   assert(!r3v_execution_provenance_valid(&host, R3V_EXECUTION_GPU_ONLY,
                                          &reason));
   host.host_semantic_node = false;
   assert(!r3v_execution_provenance_valid(&host, R3V_EXECUTION_AUTO, &reason));

   /* A host route stops at its terminal phase: the four states above it
    * name the ioctl and its fence, and a host route enters neither. */
   host = host_provenance();
   assert(host.phase == R3V_EXECUTION_PHASE_HOST_TERMINAL);
   host.phase = R3V_EXECUTION_PHASE_IOCTL_ENTERED;
   host.device_submission = true;
   assert(!r3v_execution_provenance_valid(&host, R3V_EXECUTION_AUTO, &reason));
   host = host_provenance();
   host.phase = R3V_EXECUTION_PHASE_PREPARED;
   assert(r3v_execution_provenance_valid(&host, R3V_EXECUTION_AUTO, &reason));

   /* A GPU record under CPU_REFERENCE is the mirror failure. */
   p = gpu_provenance();
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_CPU_REFERENCE,
                                          &reason));

   /* A record naming no operation refuses whatever else it carries. */
   p = gpu_provenance();
   p.operation_id = R300_OPERATION_ID_NONE;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   p = gpu_provenance();
   p.route_id = R300_OPERATION_ROUTE_NONE;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
}

/* The host route the ledger now carries for a linear transfer fill.  The
 * host executes it today, so the row states an executing fact and a caller
 * asking what fills a transfer buffer reads a route rather than a gap. */
static void
test_host_transfer_const_fill_row(void)
{
   const struct r300_operation_route_row *row =
      r300_operation_route(R300_OPERATION_ROUTE_HOST_TRANSFER_CONST_FILL);
   assert(row != NULL);
   assert(row->operation_id == R300_OPERATION_ID_CONSTFILL);
   assert(row->executor == R300_OPERATION_ROUTE_EXECUTOR_HOST);
   assert(row->state == R300_OPERATION_ROUTE_EXECUTING);
   assert(row->unit == R300_EXECUTION_UNIT_HOST);
   assert(row->uses == R300_ROUTE_USE_TRANSFER_BUFFER);
   assert(row->exactness == R300_COMPUTE_VERB_BIT_EXACT);
   /* The host path is the default and takes no opt-in. */
   assert(row->gate == NULL);

   /* Its use and the RB2D route's overlap, and only one of the two
    * executes, so the selector answers with the host route alone. */
   const char *reason = NULL;
   bool gates[R300_OPERATION_ROUTE_COUNT] = { false };
   const struct r300_operation_route_row *selected =
      r300_operation_select_route(R300_OPERATION_ID_CONSTFILL,
                                  R300_OPERATION_ROUTE_EXECUTOR_HOST,
                                  R300_ROUTE_USE_TRANSFER_BUFFER, gates,
                                  &reason);
   assert(selected == row);
}

/* Automatic selection is a separate fact from a route's maturity.  A
 * receipt promotes a route; it does not measure where the device beats the
 * host store loop, and AUTO takes an unnamed route only on that
 * measurement.  The predicate is calibrated over an explicit admitted set,
 * so both answers are exercised, and the shipped set is empty because no
 * crossover has been measured for any route.
 */
static void
test_automatic_selection_is_separate_from_maturity(void)
{
   static const enum r300_operation_route_id admitted[] = {
      R300_OPERATION_ROUTE_RB2D_CONST_FILL,
   };
   assert(r3v_route_automatic_selection_admitted_in(
      admitted, ARRAY_SIZE(admitted), R300_OPERATION_ROUTE_RB2D_CONST_FILL));
   assert(!r3v_route_automatic_selection_admitted_in(
      admitted, ARRAY_SIZE(admitted),
      R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP));
   assert(!r3v_route_automatic_selection_admitted_in(NULL, 1,
      R300_OPERATION_ROUTE_RB2D_CONST_FILL));

   /* The shipped set: no route is taken by automatic selection, the RB2D
    * fill route included, so its only door is its own gate. */
   uint32_t rows = 0;
   const struct r300_operation_route_row *table =
      r300_operation_route_rows(&rows);
   for (uint32_t i = 0; i < rows; i++) {
      assert(!r3v_route_automatic_selection_admitted(table[i].route_id));
   }
   assert(!r3v_route_automatic_selection_admitted(R300_OPERATION_ROUTE_NONE));
}

/* The withheld-automatic-selection branch governs a promoted route reached
 * with no gate of its own.  Every GPU row the ledger carries is gated, so
 * that branch has no live subject; this holds that fact rather than leaving
 * it an accident, and the first ungated GPU promotion fails here and makes
 * the branch live.
 */
static void
test_every_gpu_row_is_gated(void)
{
   uint32_t rows = 0;
   const struct r300_operation_route_row *table =
      r300_operation_route_rows(&rows);
   assert(rows != 0);
   for (uint32_t i = 0; i < rows; i++) {
      if (table[i].executor != R300_OPERATION_ROUTE_EXECUTOR_GPU)
         continue;
      assert(table[i].gate != NULL && table[i].gate[0] != '\0');
   }
}

int
main(void)
{
   test_policy_values();
   test_automatic_selection_is_separate_from_maturity();
   test_every_gpu_row_is_gated();
   test_gate_closed_reaches_the_host();
   test_cached_gate_admits_the_precommitted_route();
   test_use_mask_names_one_purpose();
   test_resource_shape_gates_the_gpu_decision();
   test_malformed_requests_refuse();
   test_execution_phase_ladder();
   test_provenance();
   test_host_transfer_const_fill_row();
   printf("r3v_route_policy_test: all checks passed\n");
   return 0;
}
