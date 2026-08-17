/*
 * SPDX-License-Identifier: MIT
 *
 * Serial status-load submitter state machine.  See the header for the
 * ladder contract; this file holds the transition order, the sampler
 * gate, and the absorbing abort.
 */

#include "r3v_native_status_load_machine.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int
r3v_status_load_format_message(char *buffer, size_t capacity,
                               const char *sender_role, const char *state,
                               const char *nonce, uint32_t submission_index,
                               uint64_t message_sequence,
                               uint64_t timestamp_ns)
{
   if (buffer == NULL || sender_role == NULL || state == NULL ||
       nonce == NULL)
      return -1;
   int length = snprintf(
      buffer, capacity,
      "{\"protocol_magic\": \"0x52533445\", \"protocol_version\": 1, "
      "\"run_nonce\": \"%s\", \"submission_index\": %" PRIu32 ", "
      "\"message_sequence\": %" PRIu64 ", \"timestamp_ns\": \"%" PRIu64
      "\", \"sender_role\": \"%s\", \"state\": \"%s\"}\n",
      nonce, submission_index, message_sequence, timestamp_ns, sender_role,
      state);
   if (length <= 0 || (size_t)length >= capacity)
      return -1;
   return length;
}

/* Marks the run aborted with the first reason and emits the ABORT
 * message on a best-effort basis: a failing sink cannot un-abort the
 * run, so its error is absorbed here.
 */
static void
machine_abort(struct r3v_status_load_machine *machine, const char *reason)
{
   if (machine->phase != R3V_STATUS_LOAD_RUNNING)
      return;
   machine->phase = R3V_STATUS_LOAD_ABORTED;
   machine->abort_reason = reason;

   char line[R3V_STATUS_LOAD_MESSAGE_CAPACITY];
   uint64_t now = machine->ops.now_ns(machine->ops.ctx);
   if (machine->have_submitter_ns && now < machine->last_submitter_ns)
      now = machine->last_submitter_ns;
   machine->last_submitter_ns = now;
   machine->have_submitter_ns = 1;
   machine->submitter_sequence++;
   uint32_t index =
      machine->next_iteration < machine->total_iterations
         ? machine->next_iteration
         : machine->total_iterations - 1;
   if (r3v_status_load_format_message(line, sizeof(line), "submitter",
                                      "ABORT", machine->nonce, index,
                                      machine->submitter_sequence, now) > 0)
      machine->ops.emit(machine->ops.ctx, line);
}

/* Emits one submitter ladder message.  A clock regression or a failing
 * sink aborts the run; the abort itself clamps the clock instead, so the
 * transcript's last message stays non-decreasing.
 */
static int
emit_submitter(struct r3v_status_load_machine *machine, const char *state,
               uint32_t submission_index)
{
   uint64_t now = machine->ops.now_ns(machine->ops.ctx);
   if (machine->have_submitter_ns && now < machine->last_submitter_ns) {
      machine_abort(machine, "submitter clock regressed");
      return -1;
   }
   machine->last_submitter_ns = now;
   machine->have_submitter_ns = 1;
   machine->submitter_sequence++;

   char line[R3V_STATUS_LOAD_MESSAGE_CAPACITY];
   if (r3v_status_load_format_message(line, sizeof(line), "submitter", state,
                                      machine->nonce, submission_index,
                                      machine->submitter_sequence, now) < 0) {
      machine_abort(machine, "message formatting failed");
      return -1;
   }
   if (machine->ops.emit(machine->ops.ctx, line) != 0) {
      machine_abort(machine, "message sink failed");
      return -1;
   }
   return 0;
}

int
r3v_status_load_machine_init(struct r3v_status_load_machine *machine,
                             const struct r3v_status_load_ops *ops,
                             const char *nonce, uint32_t iterations)
{
   if (machine == NULL || ops == NULL || nonce == NULL)
      return -1;
   if (ops->poison == NULL || ops->arm == NULL || ops->submit == NULL ||
       ops->fence_wait == NULL || ops->verify == NULL ||
       ops->retain == NULL || ops->repoison == NULL || ops->now_ns == NULL ||
       ops->emit == NULL)
      return -1;
   if (iterations < 1 || iterations > R3V_STATUS_LOAD_MAX_ITERATIONS)
      return -1;
   if (strlen(nonce) != R3V_STATUS_LOAD_NONCE_LENGTH)
      return -1;
   for (size_t i = 0; i < R3V_STATUS_LOAD_NONCE_LENGTH; i++) {
      const char c = nonce[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return -1;
   }

   memset(machine, 0, sizeof(*machine));
   machine->ops = *ops;
   memcpy(machine->nonce, nonce, R3V_STATUS_LOAD_NONCE_LENGTH + 1);
   machine->total_iterations = iterations;
   machine->sampler = R3V_STATUS_LOAD_SAMPLER_ABSENT;
   machine->phase = R3V_STATUS_LOAD_RUNNING;
   machine->abort_reason = "";
   return 0;
}

int
r3v_status_load_machine_sampler(struct r3v_status_load_machine *machine,
                                const char *state_name, uint64_t timestamp_ns)
{
   if (machine->phase != R3V_STATUS_LOAD_RUNNING)
      return -1;
   if (machine->have_sampler_ns && timestamp_ns < machine->last_sampler_ns) {
      machine_abort(machine, "sampler clock regressed");
      return -1;
   }

   enum r3v_status_load_sampler next;
   if (strcmp(state_name, "SAMPLER_OPEN") == 0)
      next = R3V_STATUS_LOAD_SAMPLER_OPEN;
   else if (strcmp(state_name, "SAMPLER_CALIBRATED") == 0)
      next = R3V_STATUS_LOAD_SAMPLER_CALIBRATED;
   else if (strcmp(state_name, "SAMPLER_READY") == 0)
      next = R3V_STATUS_LOAD_SAMPLER_READY;
   else if (strcmp(state_name, "SAMPLER_ENTER_READ") == 0)
      next = R3V_STATUS_LOAD_SAMPLER_READY;
   else if (strcmp(state_name, "SAMPLER_STOPPED") == 0)
      next = R3V_STATUS_LOAD_SAMPLER_STOPPED;
   else {
      machine_abort(machine, "sampler sent an unknown state");
      return -1;
   }

   /* The sampler climbs OPEN, CALIBRATED, READY in order and may stop
    * from any of them; ENTER_READ belongs to READY alone.  Anything else
    * is a barrier fault the run stops on.
    */
   int valid;
   if (next == R3V_STATUS_LOAD_SAMPLER_STOPPED)
      valid = machine->sampler != R3V_STATUS_LOAD_SAMPLER_ABSENT &&
              machine->sampler != R3V_STATUS_LOAD_SAMPLER_STOPPED;
   else if (strcmp(state_name, "SAMPLER_ENTER_READ") == 0)
      valid = machine->sampler == R3V_STATUS_LOAD_SAMPLER_READY;
   else
      valid = (int)next == (int)machine->sampler + 1;
   if (!valid) {
      machine_abort(machine, "sampler transition out of order");
      return -1;
   }

   machine->sampler = next;
   machine->last_sampler_ns = timestamp_ns;
   machine->have_sampler_ns = 1;
   return 0;
}

int
r3v_status_load_machine_iterate(struct r3v_status_load_machine *machine)
{
   if (machine->phase != R3V_STATUS_LOAD_RUNNING) {
      machine_abort(machine, "iterate after a terminal phase");
      return -1;
   }
   if (machine->next_iteration >= machine->total_iterations) {
      machine_abort(machine, "iterate beyond the declared count");
      return -1;
   }
   const uint32_t i = machine->next_iteration;
   void *ctx = machine->ops.ctx;

   if (emit_submitter(machine, "PREPARE", i) != 0)
      return -1;
   if (machine->ops.poison(ctx, i) != 0) {
      machine_abort(machine, "poison failed");
      return -1;
   }
   if (emit_submitter(machine, "POISONED", i) != 0)
      return -1;
   if (machine->ops.arm(ctx, i) != 0) {
      machine_abort(machine, "arming failed");
      return -1;
   }
   if (emit_submitter(machine, "ARMED", i) != 0)
      return -1;

   /* The sampler gate: submission is refused, never attempted, when the
    * sampler is gone or was never ready, so the transcript carries ABORT
    * here instead of an IOCTL_ENTER the checker would reject.
    */
   if (machine->sampler == R3V_STATUS_LOAD_SAMPLER_STOPPED) {
      machine_abort(machine, "sampler stopped before submission");
      return -1;
   }
   if (machine->sampler != R3V_STATUS_LOAD_SAMPLER_READY) {
      machine_abort(machine, "sampler never reached ready");
      return -1;
   }

   if (emit_submitter(machine, "IOCTL_ENTER", i) != 0)
      return -1;
   const int submit_status = machine->ops.submit(ctx, i);
   /* The ioctl returned either way; only its verdict differs. */
   if (emit_submitter(machine, "IOCTL_RETURN", i) != 0)
      return -1;
   if (submit_status != 0) {
      machine_abort(machine, "submission ioctl failed");
      return -1;
   }

   if (emit_submitter(machine, "FENCE_WAIT_BEGIN", i) != 0)
      return -1;
   if (machine->ops.fence_wait(ctx, i) != 0) {
      machine_abort(machine, "fence wait failed");
      return -1;
   }
   if (emit_submitter(machine, "FENCE_SIGNALED", i) != 0)
      return -1;

   if (emit_submitter(machine, "VERIFY_BEGIN", i) != 0)
      return -1;
   const int verify_status = machine->ops.verify(ctx, i);
   if (verify_status < 0) {
      machine_abort(machine, "verification refused its inputs");
      return -1;
   }
   if (emit_submitter(machine, "VERIFY_END", i) != 0)
      return -1;
   if (verify_status > 0) {
      machine_abort(machine, "verification mismatch");
      return -1;
   }

   if (machine->ops.retain(ctx, i) != 0) {
      machine_abort(machine, "evidence retention failed");
      return -1;
   }
   if (emit_submitter(machine, "EVIDENCE_RETAINED", i) != 0)
      return -1;
   if (machine->ops.repoison(ctx, i) != 0) {
      machine_abort(machine, "repoison failed");
      return -1;
   }
   if (emit_submitter(machine, "REPOISONED", i) != 0)
      return -1;

   machine->next_iteration = i + 1;
   return 0;
}

int
r3v_status_load_machine_finish(struct r3v_status_load_machine *machine)
{
   if (machine->phase != R3V_STATUS_LOAD_RUNNING) {
      machine_abort(machine, "finish after a terminal phase");
      return -1;
   }
   if (machine->next_iteration < machine->total_iterations) {
      machine_abort(machine, "finish before the declared iterations");
      return -1;
   }
   if (emit_submitter(machine, "COMPLETE",
                      machine->total_iterations - 1) != 0)
      return -1;
   machine->phase = R3V_STATUS_LOAD_COMPLETE;
   return 0;
}

void
r3v_status_load_machine_fault(struct r3v_status_load_machine *machine,
                              const char *reason)
{
   machine_abort(machine, reason);
}

enum r3v_status_load_phase
r3v_status_load_machine_phase(const struct r3v_status_load_machine *machine)
{
   return machine->phase;
}

const char *
r3v_status_load_machine_abort_reason(
   const struct r3v_status_load_machine *machine)
{
   return machine->abort_reason;
}
