/*
 * SPDX-License-Identifier: MIT
 *
 * The policy decides which executor performs an operation, and the
 * provenance records which one did.  Both are checkable without a device,
 * and both carry the claims a hardware result rests on, so they are
 * checked here rather than inferred from a submission that happened to
 * work.
 *
 * The arms that matter are the refusals.  A closed gate must reach the
 * host, GPU_ONLY with no qualified route must refuse rather than fall
 * back, and a precommitted route must run only under its own exact opt-in
 * and must say so in its provenance.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r3v_route_policy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The RB2D fill route's own gate, read from the ledger so a rename moves
 * this test with it rather than leaving it opening a gate nothing reads. */
static const char *
rb2d_fill_gate(void)
{
   const struct r300_operation_route_row *row =
      r300_operation_route(R300_OPERATION_ROUTE_RB2D_CONST_FILL);
   assert(row != NULL && row->gate != NULL);
   return row->gate;
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
      .destination_host_mapped = false,
   };
}

static void
test_policy_from_environment(void)
{
   unsetenv("R3V_EXECUTION_POLICY");
   assert(r3v_execution_policy_from_environment() == R3V_EXECUTION_AUTO);
   setenv("R3V_EXECUTION_POLICY", "gpu_only", 1);
   assert(r3v_execution_policy_from_environment() == R3V_EXECUTION_GPU_ONLY);
   setenv("R3V_EXECUTION_POLICY", "cpu_reference", 1);
   assert(r3v_execution_policy_from_environment() ==
          R3V_EXECUTION_CPU_REFERENCE);
   /* An unrecognized value leaves the default rather than opening a
    * stricter or looser path by accident. */
   setenv("R3V_EXECUTION_POLICY", "GPU_ONLY", 1);
   assert(r3v_execution_policy_from_environment() == R3V_EXECUTION_AUTO);
   setenv("R3V_EXECUTION_POLICY", "", 1);
   assert(r3v_execution_policy_from_environment() == R3V_EXECUTION_AUTO);
   unsetenv("R3V_EXECUTION_POLICY");

   for (unsigned i = 0; i <= R3V_EXECUTION_CPU_REFERENCE; i++)
      assert(r3v_execution_policy_name((enum r3v_execution_policy)i) != NULL);
   for (unsigned i = 0; i <= R3V_ROUTE_DECISION_REFUSE; i++)
      assert(r3v_route_decision_name((enum r3v_route_decision)i) != NULL);
}

static void
test_gate_closed_reaches_the_host(void)
{
   bool gates[R300_OPERATION_ROUTE_COUNT] = { false };
   const struct r300_operation_route_row *route = NULL;
   const char *reason = NULL;

   unsetenv(rb2d_fill_gate());

   /* AUTO with every gate closed is the shipped path: the host performs
    * the fill and no route is named. */
   struct r3v_route_request request = fill_request(R3V_EXECUTION_AUTO);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_HOST);
   assert(route == NULL && reason != NULL);

   /* GPU_ONLY with the same closed gate refuses.  It must not reach the
    * host: a policy that falls back proves nothing about the hardware. */
   request = fill_request(R3V_EXECUTION_GPU_ONLY);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   assert(route == NULL);

   /* CPU_REFERENCE reaches the host whatever the gates say. */
   setenv(rb2d_fill_gate(), "1", 1);
   request = fill_request(R3V_EXECUTION_CPU_REFERENCE);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_HOST);
   assert(route == NULL);
   unsetenv(rb2d_fill_gate());
}

static void
test_exact_gate_admits_the_precommitted_route(void)
{
   bool gates[R300_OPERATION_ROUTE_COUNT] = { false };
   const struct r300_operation_route_row *route = NULL;
   const char *reason = NULL;
   struct r3v_route_request request = fill_request(R3V_EXECUTION_GPU_ONLY);

   /* The gate opens on the exact value "1" and on nothing else, so an
    * operator cannot arm a hardware path with a truthy-looking string. */
   static const char *const closed[] = { "0", "", "true", "yes", "2", " 1" };
   for (unsigned i = 0; i < sizeof(closed) / sizeof(closed[0]); i++) {
      setenv(rb2d_fill_gate(), closed[i], 1);
      assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
             R3V_ROUTE_DECISION_REFUSE);
   }

   setenv(rb2d_fill_gate(), "1", 1);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_GPU);
   assert(route == r300_operation_route(R300_OPERATION_ROUTE_RB2D_CONST_FILL));
   assert(route->state == R300_OPERATION_ROUTE_PRECOMMITTED);
   assert(route->unit == R300_EXECUTION_UNIT_RB2D_FILL);

   /* The route serves one use.  A request naming the render attachment
    * reaches it not at all, however open its gate stands: applicability is
    * the filter. */
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

   unsetenv(rb2d_fill_gate());
}

static void
test_malformed_requests_refuse(void)
{
   bool gates[R300_OPERATION_ROUTE_COUNT] = { false };
   const struct r300_operation_route_row *route = NULL;
   const char *reason = NULL;

   setenv(rb2d_fill_gate(), "1", 1);
   assert(r3v_route_policy_select(NULL, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   struct r3v_route_request request = fill_request(R3V_EXECUTION_AUTO);
   request.byte_size = 0;
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);

   request = fill_request((enum r3v_execution_policy)7);
   assert(r3v_route_policy_select(&request, gates, &route, &reason) ==
          R3V_ROUTE_DECISION_REFUSE);
   unsetenv(rb2d_fill_gate());
}

static struct r3v_execution_provenance
gpu_provenance(void)
{
   const struct r300_operation_route_row *row =
      r300_operation_route(R300_OPERATION_ROUTE_RB2D_CONST_FILL);
   return (struct r3v_execution_provenance){
      .operation_id = row->operation_id,
      .route_id = row->route_id,
      .unit = row->unit,
      .executor = row->executor,
      .route_state = row->state,
      .host_semantic_node = false,
      .device_submission = true,
      .experimental_admission = true,
      .ib_dwords = 26u,
      .relocation_count = 1u,
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
    * directions, so neither can be overstated. */
   p = gpu_provenance();
   p.experimental_admission = false;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   p = gpu_provenance();
   p.route_state = R300_OPERATION_ROUTE_EXECUTING;
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));
   p.experimental_admission = false;
   assert(r3v_execution_provenance_valid(&p, R3V_EXECUTION_AUTO, &reason));

   /* A host record: it computed the result and submitted nothing. */
   struct r3v_execution_provenance host = {
      .operation_id = R300_OPERATION_ID_CONSTFILL,
      .executor = R300_OPERATION_ROUTE_EXECUTOR_HOST,
      .host_semantic_node = true,
      .device_submission = false,
   };
   assert(r3v_execution_provenance_valid(&host, R3V_EXECUTION_AUTO, &reason));
   assert(r3v_execution_provenance_valid(&host, R3V_EXECUTION_CPU_REFERENCE,
                                         &reason));
   /* Under GPU_ONLY the same record is the failure the policy exists to
    * catch. */
   assert(!r3v_execution_provenance_valid(&host, R3V_EXECUTION_GPU_ONLY,
                                          &reason));
   host.host_semantic_node = false;
   assert(!r3v_execution_provenance_valid(&host, R3V_EXECUTION_AUTO, &reason));
   host.host_semantic_node = true;
   host.device_submission = true;
   assert(!r3v_execution_provenance_valid(&host, R3V_EXECUTION_AUTO, &reason));

   /* A GPU record under CPU_REFERENCE is the mirror failure. */
   p = gpu_provenance();
   assert(!r3v_execution_provenance_valid(&p, R3V_EXECUTION_CPU_REFERENCE,
                                          &reason));
}

int
main(void)
{
   test_policy_from_environment();
   test_gate_closed_reaches_the_host();
   test_exact_gate_admits_the_precommitted_route();
   test_malformed_requests_refuse();
   test_provenance();
   printf("r3v_route_policy_test: all checks passed\n");
   return 0;
}
