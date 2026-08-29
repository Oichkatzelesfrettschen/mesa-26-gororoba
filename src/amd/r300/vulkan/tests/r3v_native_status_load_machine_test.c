/*
 * SPDX-License-Identifier: MIT
 *
 * Fakes-first validation of the serial status-load submitter machine.
 * Every hardware operation is a scripted fake, so each case knows the
 * exact transcript by construction: an accepted run proves the machine
 * walks the frozen ladder, and a rejected run proves the named gate
 * fires at its exact position with no later transition emitted.  With a
 * file argument, the happy-path run writes its merged two-sender
 * transcript for calibration against the offline protocol checker.
 */

#include "r3v_native_status_load_machine.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NONCE "b1946ac92492d2347c6235b4d2611184"
#define MAX_LINES 1600
#define MAX_DEFERRED_TRANSPORT_LINES 8

/* The submitter's whole-ladder outputs are checks against these exact
 * sequences, so the expected transcript is spelled once.
 */
static const char *const full_ladder[] = {
   "PREPARE",       "POISONED",       "ARMED",
   "QUEUE_SUBMIT_ENTER", "CS_IOCTL_ENTER", "CS_IOCTL_RETURN",
   "COMPLETION_WAIT_BEGIN", "COMPLETION_WAIT_RETURN",
   "QUEUE_SUBMIT_RETURN", "POST_SUBMIT_STATUS_CHECK_BEGIN",
   "POST_SUBMIT_STATUS_CHECK_END", "VERIFY_BEGIN", "VERIFY_END",
   "EVIDENCE_RETAINED", "REPOISONED",
};

enum fail_stage {
   FAIL_NONE,
   FAIL_POISON,
   FAIL_ARM,
   FAIL_SUBMIT,
   FAIL_TRANSPORT_TRACE,
   FAIL_TRANSPORT_OUT_OF_ORDER,
   FAIL_POST_SUBMIT_CHECK,
   FAIL_VERIFY_REFUSE,
   FAIL_VERIFY_MISMATCH,
   FAIL_VERIFY_MISMATCH_RETAIN,
   FAIL_RETAIN,
   FAIL_REPOISON,
};

struct fake {
   /* One shared clock keeps both senders' timestamps interleavable. */
   uint64_t now;
   uint64_t regress_at_call;
   uint64_t clock_calls;
   enum fail_stage fail_stage;
   uint32_t fail_iteration;
   int emit_failure;
   uint32_t emit_failures_remaining;
   char lines[MAX_LINES][R3V_STATUS_LOAD_MESSAGE_CAPACITY];
   uint32_t line_count;
   char deferred_transport_lines[MAX_DEFERRED_TRANSPORT_LINES]
      [R3V_STATUS_LOAD_MESSAGE_CAPACITY];
   uint32_t deferred_transport_line_count;
   bool defer_transport_emit;
   uint32_t transport_flush_count;
   uint64_t sampler_sequence;
};

static uint64_t
fake_now(void *ctx)
{
   struct fake *fake = ctx;
   fake->clock_calls++;
   if (fake->regress_at_call != 0 &&
       fake->clock_calls == fake->regress_at_call)
      fake->now = 100;
   else
      fake->now += 1000;
   return fake->now;
}

static int
fake_emit(void *ctx, const char *line)
{
   struct fake *fake = ctx;
   if (fake->emit_failure)
      return -1;
   if (fake->emit_failures_remaining != 0) {
      fake->emit_failures_remaining--;
      return -1;
   }
   if (fake->defer_transport_emit) {
      if (fake->deferred_transport_line_count >=
          MAX_DEFERRED_TRANSPORT_LINES)
         return -1;
      snprintf(fake->deferred_transport_lines[
                   fake->deferred_transport_line_count],
               sizeof(fake->deferred_transport_lines[0]), "%s", line);
      fake->deferred_transport_line_count++;
      return 0;
   }
   if (fake->line_count >= MAX_LINES)
      return -1;
   snprintf(fake->lines[fake->line_count],
            sizeof(fake->lines[fake->line_count]), "%s", line);
   fake->line_count++;
   return 0;
}

/* A live transport hook timestamps and queues its line while the kernel
 * operation is active; the runner flushes all queued lines after the queue
 * call returns.  The fake mirrors that ordering so the machine test catches
 * a queue that emits transport lines into the ordinary sink immediately. */
static int
fake_flush_transport(struct fake *fake)
{
   const uint32_t queued = fake->deferred_transport_line_count;
   for (uint32_t i = 0; i < queued; i++) {
      if (fake->line_count >= MAX_LINES)
         return -1;
      snprintf(fake->lines[fake->line_count],
               sizeof(fake->lines[fake->line_count]), "%s",
               fake->deferred_transport_lines[i]);
      fake->line_count++;
   }
   fake->deferred_transport_line_count = 0;
   fake->transport_flush_count++;
   return 0;
}

static int
fake_stage(struct fake *fake, enum fail_stage stage, uint32_t iteration)
{
   return fake->fail_stage == stage && fake->fail_iteration == iteration
             ? -1
             : 0;
}

static int
fake_poison(void *ctx, uint32_t i)
{
   return fake_stage(ctx, FAIL_POISON, i);
}

static int
fake_arm(void *ctx, uint32_t i)
{
   return fake_stage(ctx, FAIL_ARM, i);
}

static int
fake_submit(void *ctx, struct r3v_status_load_machine *machine, uint32_t i)
{
   struct fake *fake = ctx;
   if (fake_stage(fake, FAIL_SUBMIT, i) != 0)
      return -1;
   if (fake_stage(fake, FAIL_TRANSPORT_TRACE, i) != 0)
      return 0;
   fake->defer_transport_emit = true;
   int result = 0;
   if (fake_stage(fake, FAIL_TRANSPORT_OUT_OF_ORDER, i) != 0) {
      result = r3v_status_load_machine_transport_event(
         machine, R3V_STATUS_LOAD_TRANSPORT_CS_IOCTL_RETURN);
   } else {
      static const enum r3v_status_load_transport_event events[] = {
         R3V_STATUS_LOAD_TRANSPORT_CS_IOCTL_ENTER,
         R3V_STATUS_LOAD_TRANSPORT_CS_IOCTL_RETURN,
         R3V_STATUS_LOAD_TRANSPORT_COMPLETION_WAIT_BEGIN,
         R3V_STATUS_LOAD_TRANSPORT_COMPLETION_WAIT_RETURN,
      };
      for (size_t e = 0; e < sizeof(events) / sizeof(events[0]); e++) {
         if (r3v_status_load_machine_transport_event(machine, events[e]) !=
             0) {
            result = -1;
            break;
         }
      }
   }
   fake->defer_transport_emit = false;
   if (fake_flush_transport(fake) != 0)
      return -1;
   return result;
}

static int
fake_post_submit_check(void *ctx, uint32_t i)
{
   return fake_stage(ctx, FAIL_POST_SUBMIT_CHECK, i);
}

static int
fake_verify(void *ctx, uint32_t i)
{
   struct fake *fake = ctx;
   if (fake->fail_stage == FAIL_VERIFY_REFUSE && fake->fail_iteration == i)
      return -1;
   if ((fake->fail_stage == FAIL_VERIFY_MISMATCH ||
        fake->fail_stage == FAIL_VERIFY_MISMATCH_RETAIN) &&
       fake->fail_iteration == i)
      return 1;
   return 0;
}

static int
fake_retain(void *ctx, uint32_t i)
{
   struct fake *fake = ctx;
   if (fake_stage(fake, FAIL_RETAIN, i) != 0 ||
       fake_stage(fake, FAIL_VERIFY_MISMATCH_RETAIN, i) != 0)
      return -1;
   return 0;
}

static int
fake_repoison(void *ctx, uint32_t i)
{
   return fake_stage(ctx, FAIL_REPOISON, i);
}

static struct r3v_status_load_ops
fake_ops(struct fake *fake)
{
   return (struct r3v_status_load_ops){
      .ctx = fake,
      .poison = fake_poison,
      .arm = fake_arm,
      .submit = fake_submit,
      .post_submit_check = fake_post_submit_check,
      .verify = fake_verify,
      .retain = fake_retain,
      .repoison = fake_repoison,
      .now_ns = fake_now,
      .emit = fake_emit,
   };
}

/* Appends one sampler message to the merged transcript with the
 * sampler's own sequence counter, then feeds the same observation to the
 * machine, mirroring the live relay that parses the channel and reports
 * each sampler state as it retains the line.
 */
static int
sampler_event(struct fake *fake, struct r3v_status_load_machine *machine,
              const char *state, uint32_t submission_index)
{
   const uint64_t now = fake_now(fake);
   fake->sampler_sequence++;
   char line[R3V_STATUS_LOAD_MESSAGE_CAPACITY];
   if (r3v_status_load_format_message(line, sizeof(line), "sampler", state,
                                      NONCE, submission_index,
                                      fake->sampler_sequence, now) < 0)
      return -1;
   if (fake_emit(fake, line) != 0)
      return -1;
   return r3v_status_load_machine_sampler(machine, state, submission_index,
                                          now);
}

static int failures;

static void
fail(const char *name, const char *detail)
{
   printf("FAIL: %s: %s\n", name, detail);
   failures++;
}

static const char *
sampler_mode_state(enum r3v_status_load_sampler_mode mode)
{
   switch (mode) {
   case R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT:
      return "SAMPLER_MODE_CENSUS_PRESENT";
   case R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_ABSENT:
      return "SAMPLER_MODE_CENSUS_ABSENT";
   case R3V_STATUS_LOAD_SAMPLER_MODE_UNDECLARED:
      break;
   }
   return NULL;
}

static void
start_sampler(struct fake *fake, struct r3v_status_load_machine *machine,
              enum r3v_status_load_sampler_mode mode)
{
   if (r3v_status_load_machine_set_expected_sampler_mode(machine, mode) != 0)
      fail("sampler_start", "expected sampler mode could not be set");
   sampler_event(fake, machine, "SAMPLER_OPEN", 0);
   sampler_event(fake, machine, "SAMPLER_CALIBRATED", 0);
   sampler_event(fake, machine, sampler_mode_state(mode), 0);
   sampler_event(fake, machine, "SAMPLER_READY", 0);
}

/* Extracts the state name of transcript line n for the named sender;
 * NULL when the line is missing or belongs to the other sender.
 */
static const char *
line_state(const struct fake *fake, uint32_t n, const char *sender,
           char *out, size_t out_capacity)
{
   if (n >= fake->line_count)
      return NULL;
   const char *line = fake->lines[n];
   char role_needle[64];
   snprintf(role_needle, sizeof(role_needle), "\"sender_role\": \"%s\"",
            sender);
   if (strstr(line, role_needle) == NULL)
      return NULL;
   const char *state = strstr(line, "\"state\": \"");
   if (state == NULL)
      return NULL;
   state += strlen("\"state\": \"");
   const char *end = strchr(state, '"');
   if (end == NULL || (size_t)(end - state) >= out_capacity)
      return NULL;
   memcpy(out, state, (size_t)(end - state));
   out[end - state] = '\0';
   return out;
}

/* Collects the submitter's state sequence and compares it to the
 * expectation; any divergence or extra submitter traffic fails.
 */
static void
expect_submitter_states(const char *name, const struct fake *fake,
                        const char *const *expected, uint32_t expected_count)
{
   uint32_t found = 0;
   for (uint32_t n = 0; n < fake->line_count; n++) {
      char state[64];
      if (line_state(fake, n, "submitter", state, sizeof(state)) == NULL)
         continue;
      if (found >= expected_count) {
         fail(name, "submitter emitted past the expected sequence");
         return;
      }
      if (strcmp(state, expected[found]) != 0) {
         printf("  position %u: %s, expected %s\n", found, state,
                expected[found]);
         fail(name, "submitter state diverged");
         return;
      }
      found++;
   }
   if (found != expected_count)
      fail(name, "submitter sequence ended short");
}

/* Builds the expected submitter sequence: N full ladders, then a
 * partial ladder prefix, then the terminal state when one is expected.
 */
static uint32_t
build_expected(const char **out, uint32_t full_iterations,
               uint32_t partial_states, const char *terminal)
{
   uint32_t count = 0;
   for (uint32_t i = 0; i < full_iterations; i++)
      for (uint32_t s = 0; s < 15; s++)
         out[count++] = full_ladder[s];
   for (uint32_t s = 0; s < partial_states; s++)
      out[count++] = full_ladder[s];
   if (terminal != NULL)
      out[count++] = terminal;
   return count;
}

/* Runs the standard shape: sampler up, ENTER_READ before each of
 * `iterations` iterate calls, sampler stop, finish.  Stops early the
 * moment the machine leaves RUNNING.
 */
static void
drive(struct fake *fake, struct r3v_status_load_machine *machine,
      uint32_t iterations, int stop_sampler_after, int call_finish)
{
   start_sampler(fake, machine,
                 R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT);
   for (uint32_t i = 0; i < iterations; i++) {
      if (r3v_status_load_machine_phase(machine) != R3V_STATUS_LOAD_RUNNING)
         return;
      sampler_event(fake, machine, "SAMPLER_ENTER_WINDOW", i);
      sampler_event(fake, machine, "SAMPLER_ENTER_READ", i);
      if (r3v_status_load_machine_iterate(machine) != 0)
         return;
   }
   if (stop_sampler_after &&
       r3v_status_load_machine_phase(machine) == R3V_STATUS_LOAD_RUNNING)
      sampler_event(fake, machine, "SAMPLER_STOPPED",
                    iterations > 0 ? iterations - 1 : 0);
   if (call_finish &&
       r3v_status_load_machine_phase(machine) == R3V_STATUS_LOAD_RUNNING)
      r3v_status_load_machine_finish(machine);
}

static void
expect_abort(const char *name, const struct fake *fake,
             const struct r3v_status_load_machine *machine,
             const char *reason_needle, uint32_t full_iterations,
             uint32_t partial_states)
{
   if (r3v_status_load_machine_phase(machine) != R3V_STATUS_LOAD_ABORTED) {
      fail(name, "run did not abort");
      return;
   }
   if (strstr(r3v_status_load_machine_abort_reason(machine),
              reason_needle) == NULL) {
      printf("  reason: %s\n", r3v_status_load_machine_abort_reason(machine));
      fail(name, "abort reason mismatch");
      return;
   }
   const char *expected[16 * 15 + 16];
   uint32_t count =
      build_expected(expected, full_iterations, partial_states, "ABORT");
   expect_submitter_states(name, fake, expected, count);
}

int
main(int argc, char **argv)
{
   struct fake fake;
   struct r3v_status_load_machine machine;
   struct r3v_status_load_ops ops;

   /* Init gates: iteration bound, nonce shape, missing operation. */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   if (r3v_status_load_machine_init(&machine, &ops, NONCE, 0) == 0)
      fail("init_bound_low", "zero iterations accepted");
   if (r3v_status_load_machine_init(&machine, &ops, NONCE,
                                    R3V_STATUS_LOAD_MAX_ITERATIONS + 1) == 0)
      fail("init_bound_high", "65 iterations accepted");
   if (r3v_status_load_machine_init(&machine, &ops, NONCE,
                                    R3V_STATUS_LOAD_MAX_ITERATIONS) != 0)
      fail("init_bound_max", "64 iterations rejected");
   if (r3v_status_load_machine_init(&machine, &ops, "short", 1) == 0)
      fail("init_nonce_short", "malformed nonce accepted");
   if (r3v_status_load_machine_init(
          &machine, &ops, "B1946AC92492D2347C6235B4D2611184", 1) == 0)
      fail("init_nonce_case", "uppercase nonce accepted");
   ops.submit = NULL;
   if (r3v_status_load_machine_init(&machine, &ops, NONCE, 1) == 0)
      fail("init_missing_op", "missing submit accepted");

   /* The expected mode is a one-shot configuration and rejects the
    * initialization sentinel or a second selection.
    */
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   if (r3v_status_load_machine_set_expected_sampler_mode(
          &machine, R3V_STATUS_LOAD_SAMPLER_MODE_UNDECLARED) == 0)
      fail("mode_bound", "undeclared sampler mode accepted");
   if (r3v_status_load_machine_set_expected_sampler_mode(
          &machine, R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT) != 0 ||
       r3v_status_load_machine_set_expected_sampler_mode(
          &machine, R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_ABSENT) == 0)
      fail("mode_one_shot", "sampler mode selection was reusable");

   /* Happy path: two full iterations, sampler stop, COMPLETE. */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 2);
   drive(&fake, &machine, 2, 1, 1);
   if (r3v_status_load_machine_phase(&machine) != R3V_STATUS_LOAD_COMPLETE)
      fail("happy_path", "run did not complete");
   {
      const char *expected[16 * 15 + 16];
      uint32_t count = build_expected(expected, 2, 0, "COMPLETE");
      expect_submitter_states("happy_path", &fake, expected, count);
      /* Submitter sequences are strictly monotonic from 1 and the merged
       * transcript carries both roles.
       */
      uint32_t submitter_lines = 0, sampler_lines = 0;
      for (uint32_t n = 0; n < fake.line_count; n++) {
         char state[64];
         if (strstr(fake.lines[n],
                    "\"protocol_magic\": \"0x52533445\",") == NULL)
            fail("happy_path", "message did not carry protocol magic");
         if (strstr(fake.lines[n], "\"protocol_version\": 2,") == NULL)
            fail("happy_path", "message did not carry protocol version 2");
         if (line_state(&fake, n, "submitter", state, sizeof(state)) != NULL)
            submitter_lines++;
         if (line_state(&fake, n, "sampler", state, sizeof(state)) != NULL)
            sampler_lines++;
      }
      if (submitter_lines != count)
         fail("happy_path", "submitter line count mismatch");
      if (sampler_lines != 4 + 2 * 2 + 1)
         fail("happy_path", "sampler line count mismatch");
      if (argc > 1) {
         FILE *out = fopen(argv[1], "w");
         if (out == NULL) {
            fail("transcript_write", "cannot open the output path");
         } else {
            int write_failed = 0;
            for (uint32_t n = 0; n < fake.line_count; n++) {
               if (fputs(fake.lines[n], out) == EOF) {
                  write_failed = 1;
                  break;
               }
            }
            if (write_failed)
               fail("transcript_write", "writing a transcript line failed");
            if (fclose(out) != 0)
               fail("transcript_close", "closing the transcript failed");
         }
      }
   }

   /* A sink that rejects one attempted line recovers for ABORT.  The
    * undelivered line's sequence is reused, so the retained transcript has
    * a contiguous per-sender sequence with no phantom message.
    */
   memset(&fake, 0, sizeof(fake));
   fake.emit_failures_remaining = 1;
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   r3v_status_load_machine_iterate(&machine);
   expect_abort("one_shot_sink_failure", &fake, &machine, "sink failed", 0,
                0);
   if (fake.line_count != 1 ||
       strstr(fake.lines[0], "\"message_sequence\": 1") == NULL)
      fail("one_shot_sink_failure", "ABORT did not reuse sequence one");

   /* iterate() past COMPLETE aborts instead of emitting a ladder. */
   if (r3v_status_load_machine_iterate(&machine) == 0)
      fail("iterate_after_complete", "iteration after COMPLETE accepted");

   /* Sampler never ready: the run refuses before QUEUE_SUBMIT_ENTER. */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   if (r3v_status_load_machine_set_expected_sampler_mode(
          &machine, R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT) != 0)
      fail("sampler_not_ready", "expected sampler mode could not be set");
   sampler_event(&fake, &machine, "SAMPLER_OPEN", 0);
   sampler_event(&fake, &machine, "SAMPLER_CALIBRATED", 0);
   r3v_status_load_machine_iterate(&machine);
   expect_abort("sampler_not_ready", &fake, &machine, "never reached ready",
                0, 3);

   /* Sampler stopped between iterations: the second submission refuses. */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 2);
   start_sampler(&fake, &machine,
                 R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_WINDOW", 0);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_READ", 0);
   r3v_status_load_machine_iterate(&machine);
   sampler_event(&fake, &machine, "SAMPLER_STOPPED", 0);
   r3v_status_load_machine_iterate(&machine);
   expect_abort("sampler_stopped_midrun", &fake, &machine,
                "sampler stopped", 1, 3);

   /* ENTER_READ is a one-shot capability tied to the next iteration. */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 2);
   start_sampler(&fake, &machine,
                 R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_WINDOW", 0);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_READ", 0);
   r3v_status_load_machine_iterate(&machine);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_WINDOW", 1);
   r3v_status_load_machine_iterate(&machine);
   expect_abort("sampler_token_reuse", &fake, &machine,
                "ENTER_READ missing", 1, 3);

   /* A window for a later iteration cannot open an earlier submission. */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   start_sampler(&fake, &machine,
                 R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_WINDOW", 1);
   expect_abort("sampler_token_index", &fake, &machine,
                "window submission index differs", 0, 0);

   /* Duplicate ENTER_READ events are protocol faults, even when the
    * duplicate carries the same index.
    */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   start_sampler(&fake, &machine,
                 R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_WINDOW", 0);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_READ", 0);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_READ", 0);
   expect_abort("sampler_token_duplicate", &fake, &machine,
                "duplicate ENTER_READ", 0, 0);

   /* The census-absent control consumes the common window without a read
    * token while retaining the sampler-ready and sampler-stopped barriers.
    */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   start_sampler(&fake, &machine,
                 R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_ABSENT);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_WINDOW", 0);
   if (r3v_status_load_machine_iterate(&machine) != 0 ||
       r3v_status_load_machine_finish(&machine) != 0 ||
       r3v_status_load_machine_phase(&machine) != R3V_STATUS_LOAD_COMPLETE)
      fail("census_absent_gate", "control run did not complete");

   /* A sampler mode must be declared after calibration and must agree
    * with the submitter's selected census leg.
    */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   sampler_event(&fake, &machine, "SAMPLER_OPEN", 0);
   sampler_event(&fake, &machine, "SAMPLER_CALIBRATED", 0);
   sampler_event(&fake, &machine, "SAMPLER_MODE_CENSUS_PRESENT", 0);
   expect_abort("mode_without_expectation", &fake, &machine,
                "expectation missing", 0, 0);

   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   if (r3v_status_load_machine_set_expected_sampler_mode(
          &machine, R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_ABSENT) != 0)
      fail("mode_mismatch", "expected sampler mode could not be set");
   sampler_event(&fake, &machine, "SAMPLER_OPEN", 0);
   sampler_event(&fake, &machine, "SAMPLER_CALIBRATED", 0);
   sampler_event(&fake, &machine, "SAMPLER_MODE_CENSUS_PRESENT", 0);
   expect_abort("mode_mismatch", &fake, &machine, "differs", 0, 0);

   /* The shared window is mandatory for an observed read and a read is
    * forbidden for the census-absent control.
    */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   start_sampler(&fake, &machine,
                 R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_READ", 0);
   expect_abort("read_without_window", &fake, &machine,
                "without ENTER_WINDOW", 0, 0);

   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   start_sampler(&fake, &machine,
                 R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_ABSENT);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_WINDOW", 0);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_READ", 0);
   expect_abort("read_in_absent_mode", &fake, &machine,
                "census-absent mode", 0, 0);

   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   start_sampler(&fake, &machine,
                 R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_ABSENT);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_WINDOW", 0);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_WINDOW", 0);
   expect_abort("window_duplicate", &fake, &machine,
                "duplicate ENTER_WINDOW", 0, 0);

   /* Operation failures, each at its exact ladder position. */
   static const struct {
      const char *name;
      enum fail_stage stage;
      const char *reason;
      uint32_t partial_states;
   } operation_cases[] = {
      { "poison_failure", FAIL_POISON, "poison failed", 1 },
      { "arm_failure", FAIL_ARM, "arming failed", 2 },
      { "submit_failure", FAIL_SUBMIT, "queue submission failed", 4 },
      { "transport_trace_incomplete", FAIL_TRANSPORT_TRACE,
        "transport trace ended", 4 },
      { "transport_trace_out_of_order", FAIL_TRANSPORT_OUT_OF_ORDER,
        "transport trace transition out of order", 4 },
      { "post_submit_check_failure", FAIL_POST_SUBMIT_CHECK,
        "post-submit status check failed", 10 },
      { "verify_refusal", FAIL_VERIFY_REFUSE, "verification refused", 12 },
      { "verify_mismatch", FAIL_VERIFY_MISMATCH, "verification mismatch",
        14 },
      { "mismatch_retention_failure", FAIL_VERIFY_MISMATCH_RETAIN,
        "retention failed", 13 },
      { "retention_failure", FAIL_RETAIN, "retention failed", 13 },
      { "repoison_failure", FAIL_REPOISON, "repoison failed", 14 },
   };
   for (size_t c = 0;
        c < sizeof(operation_cases) / sizeof(operation_cases[0]); c++) {
      memset(&fake, 0, sizeof(fake));
      fake.fail_stage = operation_cases[c].stage;
      fake.fail_iteration = 1;
      ops = fake_ops(&fake);
      r3v_status_load_machine_init(&machine, &ops, NONCE, 2);
      drive(&fake, &machine, 2, 0, 0);
      expect_abort(operation_cases[c].name, &fake, &machine,
                   operation_cases[c].reason, 1,
                   operation_cases[c].partial_states);
   }

   /* A submitter clock regression aborts rather than emitting a message
    * the offline checker would reject.
    */
   memset(&fake, 0, sizeof(fake));
   fake.regress_at_call = 8;
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   drive(&fake, &machine, 1, 0, 0);
   if (r3v_status_load_machine_phase(&machine) != R3V_STATUS_LOAD_ABORTED ||
       strstr(r3v_status_load_machine_abort_reason(&machine),
              "clock regressed") == NULL)
      fail("clock_regression", "regressed clock did not abort");

   /* An out-of-order sampler transition is a barrier fault. */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   if (r3v_status_load_machine_set_expected_sampler_mode(
          &machine, R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT) != 0)
      fail("sampler_out_of_order", "expected sampler mode could not be set");
   sampler_event(&fake, &machine, "SAMPLER_OPEN", 0);
   sampler_event(&fake, &machine, "SAMPLER_READY", 0);
   if (r3v_status_load_machine_phase(&machine) != R3V_STATUS_LOAD_ABORTED ||
       strstr(r3v_status_load_machine_abort_reason(&machine),
              "before sampler mode") == NULL)
      fail("sampler_out_of_order", "READY before CALIBRATED accepted");

   /* ENTER_READ before READY is equally out of order. */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   if (r3v_status_load_machine_set_expected_sampler_mode(
          &machine, R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT) != 0)
      fail("enter_read_early", "expected sampler mode could not be set");
   sampler_event(&fake, &machine, "SAMPLER_OPEN", 0);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_READ", 0);
   if (r3v_status_load_machine_phase(&machine) != R3V_STATUS_LOAD_ABORTED)
      fail("enter_read_early", "ENTER_READ before READY accepted");

   /* finish() before the declared count aborts; a short run never
    * reads as COMPLETE.
    */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 2);
   start_sampler(&fake, &machine,
                 R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_WINDOW", 0);
   sampler_event(&fake, &machine, "SAMPLER_ENTER_READ", 0);
   r3v_status_load_machine_iterate(&machine);
   if (r3v_status_load_machine_finish(&machine) == 0 ||
       r3v_status_load_machine_phase(&machine) != R3V_STATUS_LOAD_ABORTED)
      fail("early_finish", "finish before the declared count accepted");

   /* A failing message sink aborts the run at its first emission. */
   memset(&fake, 0, sizeof(fake));
   fake.emit_failure = 1;
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   r3v_status_load_machine_iterate(&machine);
   if (r3v_status_load_machine_phase(&machine) != R3V_STATUS_LOAD_ABORTED ||
       strstr(r3v_status_load_machine_abort_reason(&machine),
              "sink failed") == NULL)
      fail("sink_failure", "failing sink did not abort");

   /* An external fault aborts a running machine with the caller's
    * reason; a completed machine keeps its verdict.
    */
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   r3v_status_load_machine_fault(&machine, "sampler channel closed");
   if (r3v_status_load_machine_phase(&machine) != R3V_STATUS_LOAD_ABORTED ||
       strstr(r3v_status_load_machine_abort_reason(&machine),
              "channel closed") == NULL)
      fail("external_fault", "fault did not abort with its reason");
   memset(&fake, 0, sizeof(fake));
   ops = fake_ops(&fake);
   r3v_status_load_machine_init(&machine, &ops, NONCE, 1);
   drive(&fake, &machine, 1, 1, 1);
   r3v_status_load_machine_fault(&machine, "late fault");
   if (r3v_status_load_machine_phase(&machine) != R3V_STATUS_LOAD_COMPLETE)
      fail("fault_after_complete", "fault re-labeled a completed run");

   /* The formatter refuses a short buffer instead of truncating. */
   {
      char tiny[32];
      if (r3v_status_load_format_message(tiny, sizeof(tiny), "submitter",
                                         "PREPARE", NONCE, 0, 1, 1000) >= 0)
         fail("format_capacity", "short buffer accepted");
   }
   {
      char line[R3V_STATUS_LOAD_MESSAGE_CAPACITY];
      if (r3v_status_load_format_message(line, sizeof(line), "sampler\"",
                                         "SAMPLER_READY", NONCE, 0, 1,
                                         1000) >= 0)
         fail("format_sender_role", "quoted sender role accepted");
      if (r3v_status_load_format_message(line, sizeof(line), "sampler",
                                         "SAMPLER_READY\"",
                                         NONCE, 0, 1, 1000) >= 0)
         fail("format_state", "quoted state accepted");
      if (r3v_status_load_format_message(line, sizeof(line), "sampler",
                                         "SAMPLER_READY", "bad\"nonce", 0,
                                         1, 1000) >= 0)
         fail("format_nonce", "quoted nonce accepted");
   }

   if (failures) {
      printf("status-load machine calibration: %d failures\n", failures);
      return 1;
   }
   printf("status-load machine calibration: 31 cases pass\n");
   return 0;
}
