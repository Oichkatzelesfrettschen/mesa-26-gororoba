/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_submit_preflight.h"

#include <string.h>

const char *
r3v_submit_refusal_name(enum r3v_submit_refusal refusal)
{
   static const char *const names[R3V_SUBMIT_REFUSAL_COUNT] = {
      [R3V_SUBMIT_ADMITTED] = "admitted",
      [R3V_SUBMIT_REFUSAL_MULTIPLE_TRANSPORT] = "multiple_transport",
      [R3V_SUBMIT_REFUSAL_MULTIPLE_ROUTE_CANDIDATES] =
         "multiple_route_candidates",
      [R3V_SUBMIT_REFUSAL_GPU_ONLY_NO_CANDIDATE] = "gpu_only_no_candidate",
      [R3V_SUBMIT_REFUSAL_UNROUTED_RECORDED_WORK] = "unrouted_recorded_work",
      [R3V_SUBMIT_REFUSAL_CANDIDATE_WITHOUT_TRANSPORT] =
         "candidate_without_transport",
      [R3V_SUBMIT_REFUSAL_MALFORMED_ROUTE_TABLE] = "malformed_route_table",
      [R3V_SUBMIT_REFUSAL_PHASE_ORDER] = "phase_order",
      [R3V_SUBMIT_REFUSAL_ABSENT_RECORD] = "absent_record",
   };
   return (unsigned)refusal < R3V_SUBMIT_REFUSAL_COUNT ? names[refusal]
                                                       : NULL;
}

enum r3v_submit_result_class
r3v_submit_refusal_result_class(enum r3v_submit_refusal refusal)
{
   switch (refusal) {
   case R3V_SUBMIT_ADMITTED:
      return R3V_SUBMIT_RESULT_SUCCESS;
   /* Every preflight verdict is a decision the driver takes before the
    * transport runs, so the boundary reports a declined command rather than
    * a lost queue. */
   case R3V_SUBMIT_REFUSAL_MULTIPLE_TRANSPORT:
   case R3V_SUBMIT_REFUSAL_MULTIPLE_ROUTE_CANDIDATES:
   case R3V_SUBMIT_REFUSAL_GPU_ONLY_NO_CANDIDATE:
   case R3V_SUBMIT_REFUSAL_UNROUTED_RECORDED_WORK:
   case R3V_SUBMIT_REFUSAL_CANDIDATE_WITHOUT_TRANSPORT:
   case R3V_SUBMIT_REFUSAL_MALFORMED_ROUTE_TABLE:
      return R3V_SUBMIT_RESULT_REFUSAL;
   /* An ordering break means a gate ran after a write reached application
    * memory, so the submit is past the point where declining is honest. */
   case R3V_SUBMIT_REFUSAL_PHASE_ORDER:
   case R3V_SUBMIT_REFUSAL_ABSENT_RECORD:
   case R3V_SUBMIT_REFUSAL_COUNT:
   default:
      return R3V_SUBMIT_RESULT_DEVICE_LOST;
   }
}

void
r3v_recorded_work_census_set(struct r3v_recorded_work_census *c,
                             uint32_t transfer_copies, uint32_t draws,
                             uint32_t dispatches, uint32_t query_ops,
                             uint32_t event_ops, uint32_t transport_ib_dwords)
{
   if (c == NULL)
      return;
   c->transfer_copies = transfer_copies;
   c->draws = draws;
   c->dispatches = dispatches;
   c->query_ops = query_ops;
   c->event_ops = event_ops;
   c->transport_ib_dwords = transport_ib_dwords;
}

uint32_t
r3v_recorded_work_census_total(const struct r3v_recorded_work_census *c)
{
   if (c == NULL)
      return 0;
   return c->transfer_copies + c->draws + c->dispatches + c->query_ops +
          c->event_ops;
}

bool
r3v_recorded_work_census_ordered(const struct r3v_recorded_work_census *c)
{
   return r3v_recorded_work_census_total(c) != 0;
}

void
r3v_submit_census_add(struct r3v_submit_census *submit,
                      const struct r3v_recorded_work_census *buffer,
                      bool has_candidate, bool candidate_covers_work)
{
   if (submit == NULL || buffer == NULL)
      return;

   submit->command_buffers++;
   submit->executable_transport += buffer->transport_ib_dwords != 0;
   submit->recorded_work_total += r3v_recorded_work_census_total(buffer);
   if (!has_candidate)
      return;

   submit->route_candidates++;
   submit->candidates_without_transport += buffer->transport_ib_dwords == 0;
   submit->unrouted_work_buffers += !candidate_covers_work;
}

enum r3v_submit_refusal
r3v_submit_preflight_check(const struct r3v_submit_census *census,
                           bool retention_bound,
                           enum r3v_execution_policy policy,
                           const char **reason)
{
   const char *ignored = NULL;
   if (reason == NULL)
      reason = &ignored;
   *reason = NULL;

   if (census == NULL) {
      *reason = "submit census is absent";
      return R3V_SUBMIT_REFUSAL_ABSENT_RECORD;
   }

   /* A route candidate names the command buffer whose stream carries it, so
    * a candidate on a buffer with no stream has nothing to submit. */
   if (census->candidates_without_transport != 0) {
      *reason = "a route candidate names a command buffer with no recorded "
                "stream";
      return R3V_SUBMIT_REFUSAL_CANDIDATE_WITHOUT_TRANSPORT;
   }

   /* One authorization declares one stream digest and its evidence
    * directory disarms after one attempt, so a bound submit admits one
    * executable command buffer.  Refusing the whole submit up front keeps a
    * multi-buffer submit from executing its first buffer and then reporting
    * a refusal on the disarmed second. */
   if (retention_bound && census->executable_transport > 1) {
      *reason = "a retained submission carries one executable command "
                "buffer";
      return R3V_SUBMIT_REFUSAL_MULTIPLE_TRANSPORT;
   }

   /* The route admission covers one candidate per submit: the arming
    * verdict, the retained evidence, and the completion all describe one
    * transport. */
   if (census->route_candidates > 1) {
      *reason = "a submit carries one GPU route candidate";
      return R3V_SUBMIT_REFUSAL_MULTIPLE_ROUTE_CANDIDATES;
   }

   /* A candidate that covers part of its command buffer's recorded work
    * leaves the rest to the submission-time host path, which the route has
    * already hoisted its transport ahead of. */
   if (census->unrouted_work_buffers != 0) {
      *reason = "a routed command buffer carries recorded work the route "
                "does not cover";
      return R3V_SUBMIT_REFUSAL_UNROUTED_RECORDED_WORK;
   }

   /* The policy is the submit's.  A GPU_ONLY submit with no candidate
    * anywhere refuses here, ahead of every command buffer, so the refusal
    * arrives with application memory untouched. */
   if (policy == R3V_EXECUTION_GPU_ONLY && census->command_buffers != 0 &&
       census->route_candidates == 0) {
      *reason = "gpu_only: the submit carries no GPU route candidate";
      return R3V_SUBMIT_REFUSAL_GPU_ONLY_NO_CANDIDATE;
   }

   return R3V_SUBMIT_ADMITTED;
}

const char *
r3v_submit_phase_name(enum r3v_submit_phase phase)
{
   static const char *const names[] = { "prepare", "validate", "commit" };
   return (unsigned)phase <= R3V_SUBMIT_PHASE_COMMIT ? names[phase] : NULL;
}

void
r3v_submit_transaction_begin(struct r3v_submit_transaction *t)
{
   if (t == NULL)
      return;
   t->phase = R3V_SUBMIT_PHASE_PREPARE;
   t->effects = 0;
   t->refusal = R3V_SUBMIT_ADMITTED;
}

bool
r3v_submit_transaction_advance(struct r3v_submit_transaction *t,
                               enum r3v_submit_phase next,
                               const char **reason)
{
   const char *ignored = NULL;
   if (reason == NULL)
      reason = &ignored;
   *reason = NULL;

   if (t == NULL) {
      *reason = "phase advance names no transaction";
      return false;
   }
   if (r3v_submit_phase_name(next) == NULL) {
      *reason = "phase outside the submit phases";
      return false;
   }
   if ((unsigned)next != (unsigned)t->phase + 1u) {
      *reason = next <= t->phase
                   ? "phase advance does not move forward"
                   : "phase advance skips the validation phase";
      t->refusal = R3V_SUBMIT_REFUSAL_PHASE_ORDER;
      return false;
   }
   t->phase = next;
   return true;
}

bool
r3v_submit_transaction_record_effect(struct r3v_submit_transaction *t,
                                     const char **reason)
{
   const char *ignored = NULL;
   if (reason == NULL)
      reason = &ignored;
   *reason = NULL;

   if (t == NULL) {
      *reason = "effect names no transaction";
      return false;
   }
   if (t->phase != R3V_SUBMIT_PHASE_COMMIT) {
      *reason = "a device-visible effect precedes the commit phase";
      t->refusal = R3V_SUBMIT_REFUSAL_PHASE_ORDER;
      return false;
   }
   t->effects++;
   return true;
}

bool
r3v_submit_transaction_reversible(const struct r3v_submit_transaction *t)
{
   return t != NULL && t->effects == 0;
}

bool
r3v_submit_transaction_refuse(struct r3v_submit_transaction *t,
                              enum r3v_submit_refusal refusal,
                              const char **reason)
{
   const char *ignored = NULL;
   if (reason == NULL)
      reason = &ignored;
   *reason = NULL;

   if (t == NULL) {
      *reason = "refusal names no transaction";
      return false;
   }
   if (refusal == R3V_SUBMIT_ADMITTED ||
       r3v_submit_refusal_name(refusal) == NULL) {
      *reason = "refusal names no verdict";
      return false;
   }
   /* The transaction's whole rule: a refusal that follows a device-visible
    * effect cannot leave application memory as it found it, so the verdict
    * is the ordering break rather than the one the caller named. */
   if (!r3v_submit_transaction_reversible(t)) {
      *reason = "a refusal follows a device-visible effect";
      t->refusal = R3V_SUBMIT_REFUSAL_PHASE_ORDER;
      return false;
   }
   t->refusal = refusal;
   return true;
}

bool
r3v_route_table_admits_device(const struct r300_operation_route_row *t,
                              uint32_t count, const char **reason)
{
   const char *ignored = NULL;
   if (reason == NULL)
      reason = &ignored;
   *reason = NULL;

   if (!r300_operation_route_rows_valid(t, count, reason))
      return false;

   /* An empty gate name reads from getenv("") at device creation, which
    * names no variable and leaves the route's opt-in undecidable.  The
    * ledger's validator requires a diagnostic name and bounds every route
    * identity, so the gate string is the one field the device's own cache
    * depends on that the ledger does not constrain. */
   for (uint32_t i = 0; i < count; i++) {
      if (t[i].gate != NULL && t[i].gate[0] == '\0') {
         *reason = "route gate carries an empty variable name";
         return false;
      }
   }
   return true;
}

bool
r3v_route_gate_state_from_cache(const char *const *cached_values,
                                bool *gate_state, uint32_t count)
{
   if (gate_state == NULL || count < R300_OPERATION_ROUTE_COUNT)
      return false;

   memset(gate_state, 0, count * sizeof(*gate_state));
   if (cached_values == NULL)
      return true;
   /* A route gate opens on the literal "1" and on nothing else.  The
    * device's own reader already stores that value or NULL, and the rule is
    * restated here because this is where a cached string becomes an open
    * gate: an entry holding "0", "", or any other text would otherwise arm
    * a hazardous route by being present. */
   for (uint32_t r = 0; r < R300_OPERATION_ROUTE_COUNT; r++)
      gate_state[r] = cached_values[r] != NULL &&
                      strcmp(cached_values[r], "1") == 0;
   return true;
}
