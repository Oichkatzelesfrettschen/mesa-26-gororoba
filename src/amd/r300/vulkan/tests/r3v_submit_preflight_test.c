/*
 * SPDX-License-Identifier: MIT
 *
 * The preparation layer's own verdicts, checked without a device.
 *
 * Three claims carry the file.  The census reproduces, over all thirty-two
 * combinations of its five ordered-work counters, the verdict the queue's
 * field list computed before it read a census, so the refactor moves no
 * submission.  The whole-submit preflight refuses each of its shapes for its
 * own named ground, and each refusal is reachable alone.  The transaction
 * admits a device-visible effect in the commit phase alone and refuses a
 * refusal that follows one, which is the rule "a refused submit leaves
 * application memory untouched" stated as a predicate.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r3v_submit_preflight.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The predicate the queue carried as an inline field list: a command buffer
 * requires submission-time ordering when any recorded kind is nonzero.  The
 * census form must agree with it everywhere, so the disjunction is written
 * out here and compared rather than trusted. */
static bool
field_list_ordered(uint32_t transfer_copies, uint32_t draws,
                   uint32_t dispatches, uint32_t query_ops,
                   uint32_t event_ops)
{
   return draws != 0 || dispatches != 0 || transfer_copies != 0 ||
          query_ops != 0 || event_ops != 0;
}

/* Thirty-two combinations: each of the five counters zero or nonzero.  The
 * verdict is a boolean over five independent fields, so the enumeration is
 * exhaustive rather than sampled, and a census that dropped a counter would
 * disagree on the four combinations that set only that one. */
static void
test_census_reproduces_the_field_list(void)
{
   for (unsigned mask = 0; mask < 32u; mask++) {
      const uint32_t transfer_copies = (mask & 1u) ? 3u : 0u;
      const uint32_t draws = (mask & 2u) ? 2u : 0u;
      const uint32_t dispatches = (mask & 4u) ? 1u : 0u;
      const uint32_t query_ops = (mask & 8u) ? 5u : 0u;
      const uint32_t event_ops = (mask & 16u) ? 4u : 0u;

      struct r3v_recorded_work_census census;
      memset(&census, 0xa5, sizeof(census));
      r3v_recorded_work_census_set(&census, transfer_copies, draws,
                                   dispatches, query_ops, event_ops, 64u);

      assert(census.transfer_copies == transfer_copies);
      assert(census.draws == draws);
      assert(census.dispatches == dispatches);
      assert(census.query_ops == query_ops);
      assert(census.event_ops == event_ops);
      assert(census.transport_ib_dwords == 64u);

      assert(r3v_recorded_work_census_ordered(&census) ==
             field_list_ordered(transfer_copies, draws, dispatches, query_ops,
                                event_ops));
      assert(r3v_recorded_work_census_total(&census) ==
             transfer_copies + draws + dispatches + query_ops + event_ops);
   }

   /* The recorded stream stays outside the ordered sum: a transport-only
    * command buffer carries a stream and no ordered work. */
   struct r3v_recorded_work_census transport_only;
   r3v_recorded_work_census_set(&transport_only, 0, 0, 0, 0, 0, 231u);
   assert(!r3v_recorded_work_census_ordered(&transport_only));
   assert(r3v_recorded_work_census_total(&transport_only) == 0);

   assert(r3v_recorded_work_census_total(NULL) == 0);
   assert(!r3v_recorded_work_census_ordered(NULL));
   r3v_recorded_work_census_set(NULL, 1, 1, 1, 1, 1, 1);
}

static struct r3v_recorded_work_census
transport_buffer(uint32_t dwords)
{
   struct r3v_recorded_work_census c;
   r3v_recorded_work_census_set(&c, 0, 0, 0, 0, 0, dwords);
   return c;
}

static void
test_submit_census_folds_each_buffer(void)
{
   struct r3v_submit_census submit = { 0 };
   struct r3v_recorded_work_census host_only;
   r3v_recorded_work_census_set(&host_only, 2, 0, 0, 1, 0, 0);
   const struct r3v_recorded_work_census routed = transport_buffer(26u);

   r3v_submit_census_add(&submit, &host_only, false, false);
   r3v_submit_census_add(&submit, &routed, true, true);

   assert(submit.command_buffers == 2);
   assert(submit.executable_transport == 1);
   assert(submit.route_candidates == 1);
   assert(submit.candidates_without_transport == 0);
   assert(submit.unrouted_work_buffers == 0);
   assert(submit.recorded_work_total == 3);

   /* A candidate on a buffer with no stream, and a candidate that leaves
    * recorded work behind, each land in their own counter. */
   struct r3v_submit_census flawed = { 0 };
   struct r3v_recorded_work_census streamless;
   r3v_recorded_work_census_set(&streamless, 1, 0, 0, 0, 0, 0);
   r3v_submit_census_add(&flawed, &streamless, true, true);
   assert(flawed.candidates_without_transport == 1);

   struct r3v_submit_census partial = { 0 };
   struct r3v_recorded_work_census mixed;
   r3v_recorded_work_census_set(&mixed, 1, 1, 0, 0, 0, 26u);
   r3v_submit_census_add(&partial, &mixed, true, false);
   assert(partial.unrouted_work_buffers == 1);

   /* An absent argument folds nothing rather than counting a buffer that
    * was never read. */
   struct r3v_submit_census untouched = { 0 };
   r3v_submit_census_add(&untouched, NULL, true, true);
   r3v_submit_census_add(NULL, &routed, true, true);
   assert(untouched.command_buffers == 0);
}

static void
test_preflight_admits_the_shipped_shapes(void)
{
   const char *reason = NULL;

   /* An empty submit admits: a submit with no command buffer has nothing to
    * route and nothing to refuse. */
   struct r3v_submit_census empty = { 0 };
   assert(r3v_submit_preflight_check(&empty, true, R3V_EXECUTION_GPU_ONLY,
                                     &reason) == R3V_SUBMIT_ADMITTED);

   /* One host-executed command buffer under AUTO: the shipped path. */
   struct r3v_submit_census host = { 0 };
   struct r3v_recorded_work_census work;
   r3v_recorded_work_census_set(&work, 4, 0, 0, 0, 0, 0);
   r3v_submit_census_add(&host, &work, false, false);
   assert(r3v_submit_preflight_check(&host, false, R3V_EXECUTION_AUTO,
                                     &reason) == R3V_SUBMIT_ADMITTED);

   /* Several host-executed buffers admit even under a retention bound: the
    * bound counts executable streams, and these carry none. */
   r3v_submit_census_add(&host, &work, false, false);
   r3v_submit_census_add(&host, &work, false, false);
   assert(host.command_buffers == 3 && host.executable_transport == 0);
   assert(r3v_submit_preflight_check(&host, true, R3V_EXECUTION_AUTO,
                                     &reason) == R3V_SUBMIT_ADMITTED);

   /* One routed buffer under GPU_ONLY admits: the policy asks for a
    * candidate and the submit carries one. */
   struct r3v_submit_census routed = { 0 };
   const struct r3v_recorded_work_census stream = transport_buffer(26u);
   r3v_submit_census_add(&routed, &stream, true, true);
   assert(r3v_submit_preflight_check(&routed, true, R3V_EXECUTION_GPU_ONLY,
                                     &reason) == R3V_SUBMIT_ADMITTED);
}

static void
test_preflight_refuses_each_shape(void)
{
   const char *reason = NULL;

   /* Two executable streams under a retention bound: one authorization
    * declares one stream digest, so the whole submit refuses ahead of its
    * first buffer rather than executing that one and reporting the disarmed
    * second. */
   struct r3v_submit_census two_streams = { 0 };
   const struct r3v_recorded_work_census stream = transport_buffer(26u);
   r3v_submit_census_add(&two_streams, &stream, false, false);
   r3v_submit_census_add(&two_streams, &stream, false, false);
   assert(r3v_submit_preflight_check(&two_streams, true, R3V_EXECUTION_AUTO,
                                     &reason) ==
          R3V_SUBMIT_REFUSAL_MULTIPLE_TRANSPORT);
   assert(reason != NULL);
   /* Without the bound the same submit admits: the refusal belongs to the
    * authorization, not to the shape. */
   assert(r3v_submit_preflight_check(&two_streams, false, R3V_EXECUTION_AUTO,
                                     &reason) == R3V_SUBMIT_ADMITTED);

   /* Two route candidates: the arming verdict, the retained evidence, and
    * the completion each describe one transport. */
   struct r3v_submit_census two_candidates = { 0 };
   r3v_submit_census_add(&two_candidates, &stream, true, true);
   r3v_submit_census_add(&two_candidates, &stream, true, true);
   assert(r3v_submit_preflight_check(&two_candidates, false,
                                     R3V_EXECUTION_AUTO, &reason) ==
          R3V_SUBMIT_REFUSAL_MULTIPLE_ROUTE_CANDIDATES);

   /* GPU_ONLY over a submit with no candidate anywhere.  Per command buffer
    * this submit's first buffer would run on the host and the policy would
    * report on the second; counted whole, it refuses with nothing run. */
   struct r3v_submit_census no_candidate = { 0 };
   struct r3v_recorded_work_census work;
   r3v_recorded_work_census_set(&work, 1, 0, 0, 0, 0, 0);
   r3v_submit_census_add(&no_candidate, &work, false, false);
   r3v_submit_census_add(&no_candidate, &work, false, false);
   assert(r3v_submit_preflight_check(&no_candidate, false,
                                     R3V_EXECUTION_GPU_ONLY, &reason) ==
          R3V_SUBMIT_REFUSAL_GPU_ONLY_NO_CANDIDATE);
   /* The same submit under AUTO takes the host path. */
   assert(r3v_submit_preflight_check(&no_candidate, false, R3V_EXECUTION_AUTO,
                                     &reason) == R3V_SUBMIT_ADMITTED);

   /* A routed buffer whose candidate leaves recorded work behind: the route
    * hoists the transport ahead of the submission boundary, so the residue
    * would execute out of its recorded order. */
   struct r3v_submit_census partial = { 0 };
   struct r3v_recorded_work_census mixed;
   r3v_recorded_work_census_set(&mixed, 2, 0, 1, 0, 0, 26u);
   r3v_submit_census_add(&partial, &mixed, true, false);
   assert(r3v_submit_preflight_check(&partial, false, R3V_EXECUTION_AUTO,
                                     &reason) ==
          R3V_SUBMIT_REFUSAL_UNROUTED_RECORDED_WORK);

   /* A candidate naming a buffer with no stream has nothing to submit. */
   struct r3v_submit_census streamless = { 0 };
   struct r3v_recorded_work_census host_only;
   r3v_recorded_work_census_set(&host_only, 1, 0, 0, 0, 0, 0);
   r3v_submit_census_add(&streamless, &host_only, true, true);
   assert(r3v_submit_preflight_check(&streamless, false, R3V_EXECUTION_AUTO,
                                     &reason) ==
          R3V_SUBMIT_REFUSAL_CANDIDATE_WITHOUT_TRANSPORT);

   /* An absent census is a defect in the calling code, so the verdict names
    * that rather than a shape the driver declines. */
   assert(r3v_submit_preflight_check(NULL, false, R3V_EXECUTION_AUTO,
                                     &reason) ==
          R3V_SUBMIT_REFUSAL_ABSENT_RECORD);
   assert(r3v_submit_refusal_result_class(R3V_SUBMIT_REFUSAL_ABSENT_RECORD) ==
          R3V_SUBMIT_RESULT_DEVICE_LOST);

   /* Every verdict spells a name, and the result class separates a declined
    * command from a lost queue. */
   for (unsigned i = 0; i < R3V_SUBMIT_REFUSAL_COUNT; i++)
      assert(r3v_submit_refusal_name((enum r3v_submit_refusal)i) != NULL);
   assert(r3v_submit_refusal_name(R3V_SUBMIT_REFUSAL_COUNT) == NULL);
   assert(r3v_submit_refusal_result_class(R3V_SUBMIT_ADMITTED) ==
          R3V_SUBMIT_RESULT_SUCCESS);
   assert(r3v_submit_refusal_result_class(
             R3V_SUBMIT_REFUSAL_GPU_ONLY_NO_CANDIDATE) ==
          R3V_SUBMIT_RESULT_REFUSAL);
   assert(r3v_submit_refusal_result_class(R3V_SUBMIT_REFUSAL_PHASE_ORDER) ==
          R3V_SUBMIT_RESULT_DEVICE_LOST);
}

static void
test_transaction_orders_refusal_before_effect(void)
{
   const char *reason = NULL;
   struct r3v_submit_transaction t;

   for (unsigned i = 0; i <= R3V_SUBMIT_PHASE_COMMIT; i++)
      assert(r3v_submit_phase_name((enum r3v_submit_phase)i) != NULL);
   assert(r3v_submit_phase_name(
             (enum r3v_submit_phase)(R3V_SUBMIT_PHASE_COMMIT + 1)) == NULL);

   /* A transaction refuses in prepare and in validate with nothing applied,
    * which is what leaves the caller's memory as it found it. */
   r3v_submit_transaction_begin(&t);
   assert(t.phase == R3V_SUBMIT_PHASE_PREPARE);
   assert(r3v_submit_transaction_reversible(&t));
   assert(r3v_submit_transaction_refuse(
      &t, R3V_SUBMIT_REFUSAL_MULTIPLE_TRANSPORT, &reason));
   assert(t.refusal == R3V_SUBMIT_REFUSAL_MULTIPLE_TRANSPORT);

   r3v_submit_transaction_begin(&t);
   assert(r3v_submit_transaction_advance(&t, R3V_SUBMIT_PHASE_VALIDATE,
                                         &reason));
   assert(r3v_submit_transaction_refuse(
      &t, R3V_SUBMIT_REFUSAL_GPU_ONLY_NO_CANDIDATE, &reason));

   /* The commit phase alone admits a device-visible effect: an effect in
    * prepare or validate would put bytes in application memory ahead of a
    * gate that can still refuse. */
   r3v_submit_transaction_begin(&t);
   assert(!r3v_submit_transaction_record_effect(&t, &reason));
   assert(t.refusal == R3V_SUBMIT_REFUSAL_PHASE_ORDER && reason != NULL);
   assert(t.effects == 0);

   r3v_submit_transaction_begin(&t);
   assert(r3v_submit_transaction_advance(&t, R3V_SUBMIT_PHASE_VALIDATE,
                                         &reason));
   assert(!r3v_submit_transaction_record_effect(&t, &reason));
   assert(t.effects == 0);

   r3v_submit_transaction_begin(&t);
   assert(r3v_submit_transaction_advance(&t, R3V_SUBMIT_PHASE_VALIDATE,
                                         &reason));
   assert(r3v_submit_transaction_advance(&t, R3V_SUBMIT_PHASE_COMMIT,
                                         &reason));
   assert(r3v_submit_transaction_record_effect(&t, &reason));
   assert(t.effects == 1 && !r3v_submit_transaction_reversible(&t));

   /* A refusal after an effect breaks the transaction's own rule, so the
    * verdict becomes the ordering break rather than the one named. */
   assert(!r3v_submit_transaction_refuse(
      &t, R3V_SUBMIT_REFUSAL_MULTIPLE_TRANSPORT, &reason));
   assert(t.refusal == R3V_SUBMIT_REFUSAL_PHASE_ORDER);
   assert(reason != NULL);

   /* Validation cannot be skipped: a transaction reaches commit through it
    * or not at all. */
   r3v_submit_transaction_begin(&t);
   assert(!r3v_submit_transaction_advance(&t, R3V_SUBMIT_PHASE_COMMIT,
                                          &reason));
   assert(t.phase == R3V_SUBMIT_PHASE_PREPARE);
   assert(t.refusal == R3V_SUBMIT_REFUSAL_PHASE_ORDER);

   /* A repeat, a step back, and a phase outside the enum all refuse. */
   r3v_submit_transaction_begin(&t);
   assert(!r3v_submit_transaction_advance(&t, R3V_SUBMIT_PHASE_PREPARE,
                                          &reason));
   assert(r3v_submit_transaction_advance(&t, R3V_SUBMIT_PHASE_VALIDATE,
                                         &reason));
   assert(!r3v_submit_transaction_advance(&t, R3V_SUBMIT_PHASE_PREPARE,
                                          &reason));
   assert(!r3v_submit_transaction_advance(
      &t, (enum r3v_submit_phase)(R3V_SUBMIT_PHASE_COMMIT + 1), &reason));

   /* A verdict of admitted is no refusal, and neither is a value outside
    * the enum. */
   r3v_submit_transaction_begin(&t);
   assert(!r3v_submit_transaction_refuse(&t, R3V_SUBMIT_ADMITTED, &reason));
   assert(!r3v_submit_transaction_refuse(&t, R3V_SUBMIT_REFUSAL_COUNT,
                                         &reason));

   assert(!r3v_submit_transaction_advance(NULL, R3V_SUBMIT_PHASE_VALIDATE,
                                          &reason));
   assert(!r3v_submit_transaction_record_effect(NULL, &reason));
   assert(!r3v_submit_transaction_refuse(
      NULL, R3V_SUBMIT_REFUSAL_PHASE_ORDER, &reason));
   assert(!r3v_submit_transaction_reversible(NULL));
   r3v_submit_transaction_begin(NULL);
}

/* The device caches one gate value per route identity, so the table decides
 * the shape of that array.  The shipped table is well-formed; the refusals
 * are calibrated on mutated copies, which is the only input that reaches
 * them. */
static void
test_route_table_admission(void)
{
   const char *reason = NULL;
   uint32_t count = 0;
   const struct r300_operation_route_row *rows =
      r300_operation_route_rows(&count);

   assert(r3v_route_table_admits_device(rows, count, &reason));
   assert(reason == NULL);

   struct r300_operation_route_row mutated[R300_OPERATION_ROUTE_COUNT];
   assert(count <= R300_OPERATION_ROUTE_COUNT);

   /* An empty gate name reads from an unnamed variable, leaving the route's
    * opt-in undecidable.  The ledger's validator requires a diagnostic name
    * and permits any gate string, so this rule is the device's own. */
   memcpy(mutated, rows, count * sizeof(*rows));
   mutated[R300_OPERATION_ROUTE_RB2D_CONST_FILL - 1].gate = "";
   assert(!r3v_route_table_admits_device(mutated, count, &reason));
   assert(reason != NULL);
   /* A named gate on the same row admits, so the refusal reads the string
    * rather than the row. */
   mutated[R300_OPERATION_ROUTE_RB2D_CONST_FILL - 1].gate = "R3V_A_GATE";
   assert(r3v_route_table_admits_device(mutated, count, &reason));

   /* The ledger's own well-formedness comes first: a repeated identity and
    * an identity outside the enum both refuse through
    * r300_operation_route_rows_valid, which is what lets the device size its
    * gate cache by that enum. */
   memcpy(mutated, rows, count * sizeof(*rows));
   mutated[1].route_id = mutated[0].route_id;
   assert(!r3v_route_table_admits_device(mutated, count, &reason));
   memcpy(mutated, rows, count * sizeof(*rows));
   mutated[0].route_id = R300_OPERATION_ROUTE_COUNT;
   assert(!r3v_route_table_admits_device(mutated, count, &reason));

   /* An empty table names no route at all. */
   assert(!r3v_route_table_admits_device(rows, 0, &reason));
   assert(!r3v_route_table_admits_device(NULL, count, &reason));
}

static void
test_gate_state_from_cache(void)
{
   const char *cached[R300_OPERATION_ROUTE_COUNT] = { NULL };
   bool gates[R300_OPERATION_ROUTE_COUNT];

   cached[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = "1";
   memset(gates, true, sizeof(gates));
   assert(r3v_route_gate_state_from_cache(cached, gates,
                                          R300_OPERATION_ROUTE_COUNT));
   for (unsigned r = 0; r < R300_OPERATION_ROUTE_COUNT; r++)
      assert(gates[r] == (r == R300_OPERATION_ROUTE_RB2D_CONST_FILL));

   /* The gate opens on the literal "1" and on nothing else: a cached value
    * that is merely present arms no hazardous route. */
   static const char *const closed[] = { "0",    "",  "true",
                                         "yes",  "2", " 1",
                                         "1 ",   "01" };
   for (unsigned i = 0; i < sizeof(closed) / sizeof(closed[0]); i++) {
      cached[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = closed[i];
      memset(gates, true, sizeof(gates));
      assert(r3v_route_gate_state_from_cache(cached, gates,
                                             R300_OPERATION_ROUTE_COUNT));
      for (unsigned r = 0; r < R300_OPERATION_ROUTE_COUNT; r++)
         assert(!gates[r]);
   }
   cached[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = "1";

   /* An array short of the ledger cannot hold every identity, so the fill
    * refuses instead of leaving entries the selector would index. */
   assert(!r3v_route_gate_state_from_cache(cached, gates,
                                           R300_OPERATION_ROUTE_COUNT - 1));
   assert(!r3v_route_gate_state_from_cache(cached, NULL,
                                           R300_OPERATION_ROUTE_COUNT));

   /* No cache is every gate closed. */
   memset(gates, true, sizeof(gates));
   assert(r3v_route_gate_state_from_cache(NULL, gates,
                                          R300_OPERATION_ROUTE_COUNT));
   for (unsigned r = 0; r < R300_OPERATION_ROUTE_COUNT; r++)
      assert(!gates[r]);
}

int
main(void)
{
   test_census_reproduces_the_field_list();
   test_submit_census_folds_each_buffer();
   test_preflight_admits_the_shipped_shapes();
   test_preflight_refuses_each_shape();
   test_transaction_orders_refusal_before_effect();
   test_route_table_admission();
   test_gate_state_from_cache();
   printf("r3v_submit_preflight_test: all checks passed\n");
   return 0;
}
