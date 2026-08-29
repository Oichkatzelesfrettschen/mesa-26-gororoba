/*
 * SPDX-License-Identifier: MIT
 *
 * Submitter-side state machine for the serial status-load cell.  The
 * machine walks each submission through the frozen RS482 status-event
 * ladder (PREPARE through REPOISONED), emits one JSONL barrier message
 * per transition, and refuses QUEUE_SUBMIT_ENTER unless the paired census
 * sampler has reached SAMPLER_READY and has not stopped.  Every
 * hardware-facing operation arrives through the ops table, so the whole
 * machine validates against fakes with no device present; the attended
 * runner later supplies the live operations under its own arming digest.
 */

#ifndef R3V_NATIVE_STATUS_LOAD_MACHINE_H
#define R3V_NATIVE_STATUS_LOAD_MACHINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The census sampler holds at most 64 records per block at the smallest
 * calibrated block size, and the serial cell keeps exactly one outstanding
 * submission, so the run bound matches one block of correlated windows.
 */
#define R3V_STATUS_LOAD_MAX_ITERATIONS 64u

/* run_nonce is the 32-lowercase-hex spelling shared with the sampler and
 * the offline transcript checker.
 */
#define R3V_STATUS_LOAD_NONCE_LENGTH 32u

/* Version 2 names the high-level queue call and its inner transport
 * boundaries separately.  Version 1 retained its original bytes and its
 * offline reader keeps their wrapper interpretation.
 */
#define R3V_STATUS_LOAD_PROTOCOL_MAGIC "0x52533445"
#define R3V_STATUS_LOAD_PROTOCOL_VERSION 2u

/* One formatted message line: eight short fields plus the newline stays
 * well inside this bound, and the formatter rejects overflow.
 */
#define R3V_STATUS_LOAD_MESSAGE_CAPACITY 320u

enum r3v_status_load_phase {
   R3V_STATUS_LOAD_RUNNING,
   R3V_STATUS_LOAD_COMPLETE,
   R3V_STATUS_LOAD_ABORTED,
};

struct r3v_status_load_machine;

/* The sampler's resting states as the submitter tracks them.  Mode
 * declarations and the submission window are orthogonal one-shot events
 * recorded beside these resting states.
 */
enum r3v_status_load_sampler {
   R3V_STATUS_LOAD_SAMPLER_ABSENT,
   R3V_STATUS_LOAD_SAMPLER_OPEN,
   R3V_STATUS_LOAD_SAMPLER_CALIBRATED,
   R3V_STATUS_LOAD_SAMPLER_READY,
   R3V_STATUS_LOAD_SAMPLER_STOPPED,
};

/* The submitter compares the sampler's declaration with the selected
 * census leg before it admits a queue submission.  UNDECLARED is the
 * initialization state and never describes a valid run.
 */
enum r3v_status_load_sampler_mode {
   R3V_STATUS_LOAD_SAMPLER_MODE_UNDECLARED,
   R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_PRESENT,
   R3V_STATUS_LOAD_SAMPLER_MODE_CENSUS_ABSENT,
};

enum r3v_status_load_transport_event {
   R3V_STATUS_LOAD_TRANSPORT_CS_IOCTL_ENTER,
   R3V_STATUS_LOAD_TRANSPORT_CS_IOCTL_RETURN,
   R3V_STATUS_LOAD_TRANSPORT_COMPLETION_WAIT_BEGIN,
   R3V_STATUS_LOAD_TRANSPORT_COMPLETION_WAIT_RETURN,
};

enum r3v_status_load_transport_phase {
   R3V_STATUS_LOAD_TRANSPORT_IDLE,
   R3V_STATUS_LOAD_TRANSPORT_EXPECT_CS_IOCTL_ENTER,
   R3V_STATUS_LOAD_TRANSPORT_EXPECT_CS_IOCTL_RETURN,
   R3V_STATUS_LOAD_TRANSPORT_EXPECT_COMPLETION_WAIT_BEGIN,
   R3V_STATUS_LOAD_TRANSPORT_EXPECT_COMPLETION_WAIT_RETURN,
   R3V_STATUS_LOAD_TRANSPORT_COMPLETE,
};

/* Hardware-facing operations, one per ladder transition that acts.  Each
 * returns 0 on success; a nonzero return aborts the run at the exact
 * ladder position its comment names.  verify() alone distinguishes a
 * negative refusal (the oracle could not judge; the run aborts before
 * VERIFY_END) from a positive mismatch (the oracle judged and failed;
 * VERIFY_END is emitted, then the run aborts).
 */
struct r3v_status_load_ops {
   void *ctx;
   int (*poison)(void *ctx, uint32_t iteration);
   int (*arm)(void *ctx, uint32_t iteration);
   int (*submit)(void *ctx, struct r3v_status_load_machine *machine,
                 uint32_t iteration);
   int (*post_submit_check)(void *ctx, uint32_t iteration);
   int (*verify)(void *ctx, uint32_t iteration);
   int (*retain)(void *ctx, uint32_t iteration);
   int (*repoison)(void *ctx, uint32_t iteration);
   uint64_t (*now_ns)(void *ctx);
   int (*emit)(void *ctx, const char *line);
};

struct r3v_status_load_machine {
   struct r3v_status_load_ops ops;
   char nonce[R3V_STATUS_LOAD_NONCE_LENGTH + 1];
   uint32_t total_iterations;
   uint32_t next_iteration;
   uint64_t submitter_sequence;
   uint64_t last_submitter_ns;
   int have_submitter_ns;
   enum r3v_status_load_sampler sampler;
   enum r3v_status_load_sampler_mode sampler_mode;
   enum r3v_status_load_sampler_mode expected_sampler_mode;
   uint64_t last_sampler_ns;
   int have_sampler_ns;
   int sampler_window_pending;
   uint32_t sampler_window_index;
   int sampler_read_pending;
   uint32_t sampler_read_index;
   uint32_t transport_submission_index;
   enum r3v_status_load_transport_phase transport_phase;
   enum r3v_status_load_phase phase;
   const char *abort_reason;
};

/* Accepts the protocol's exact 32-character lowercase hexadecimal nonce. */
int r3v_status_load_nonce_valid(const char *nonce);

/* Formats one protocol message line (JSON object plus newline) with the
 * frozen field set.  Both sender roles use it, so the transcript carries
 * one spelling.  Returns the line length, or -1 when the buffer is short
 * or a field is malformed.
 */
int
r3v_status_load_format_message(char *buffer, size_t capacity,
                               const char *sender_role, const char *state,
                               const char *nonce, uint32_t submission_index,
                               uint64_t message_sequence,
                               uint64_t timestamp_ns);

/* Records one exact queue transport boundary for the iteration currently
 * inside QUEUE_SUBMIT_ENTER.  The function accepts each boundary once in
 * order and emits its transcript state; an unexpected callback aborts the
 * run, so a v2 transcript cannot substitute wrapper timing for the raw
 * DRM submission or completion wait.
 */
int
r3v_status_load_machine_transport_event(
   struct r3v_status_load_machine *machine,
   enum r3v_status_load_transport_event event);

/* Binds the ops table, the shared nonce, and the declared iteration
 * count (1 through R3V_STATUS_LOAD_MAX_ITERATIONS).  Each run selects one
 * census leg before the sampler handshake; the selected leg determines
 * whether each indexed submission also requires a read token.  Returns 0,
 * or -1 for a malformed nonce, an out-of-bound count, or a missing
 * operation.
 */
int
r3v_status_load_machine_init(struct r3v_status_load_machine *machine,
                             const struct r3v_status_load_ops *ops,
                             const char *nonce, uint32_t iterations);

/* Selects the census leg expected from the sampler.  The sampler must
 * declare the matching SAMPLER_MODE_CENSUS_PRESENT or
 * SAMPLER_MODE_CENSUS_ABSENT event after calibration; the machine derives
 * read-token requirements from that declaration.  Returns 0 when the
 * expectation is set on a running, unused machine.
 */
int
r3v_status_load_machine_set_expected_sampler_mode(
   struct r3v_status_load_machine *machine,
   enum r3v_status_load_sampler_mode mode);

/* Feeds one observed sampler event by protocol state name and its
 * 0-based submission index.  A mode declaration follows calibration and
 * must match the expected mode.  SAMPLER_ENTER_WINDOW is a one-shot
 * submission barrier; census-present runs follow it with an indexed
 * SAMPLER_ENTER_READ token, while census-absent runs omit the read token.
 * An out-of-order transition, duplicate token, index mismatch, or timestamp
 * regression aborts the run.  Returns 0 while the run keeps going.
 */
int
r3v_status_load_machine_sampler(struct r3v_status_load_machine *machine,
                                const char *state_name,
                                uint32_t submission_index,
                                uint64_t timestamp_ns);

/* Runs one full submission through the ladder.  The call consumes the
 * indexed SAMPLER_ENTER_WINDOW token and, for census-present mode, its
 * matching SAMPLER_ENTER_READ token before QUEUE_SUBMIT_ENTER.  Returns 0
 * when the iteration reached REPOISONED; nonzero means the run aborted at
 * the position named by the abort reason, and no later transition was
 * emitted.  A call past the declared count aborts a running machine; a call
 * after a terminal phase is refused with nothing emitted, so a completed
 * run keeps its verdict.
 */
int
r3v_status_load_machine_iterate(struct r3v_status_load_machine *machine);

/* Emits COMPLETE once every declared iteration has run.  A finish before
 * the declared count aborts instead: a short run must say ABORT, never
 * pass as complete.  Returns 0 on COMPLETE.
 */
int
r3v_status_load_machine_finish(struct r3v_status_load_machine *machine);

/* Aborts a running machine for a fault the ops table cannot express:
 * a barrier-channel death, a foreign nonce, or an operator interrupt.
 * The ABORT message is emitted once and the reason is retained; a
 * terminal machine is left unchanged.
 */
void
r3v_status_load_machine_fault(struct r3v_status_load_machine *machine,
                              const char *reason);

enum r3v_status_load_phase
r3v_status_load_machine_phase(const struct r3v_status_load_machine *machine);

/* The reason recorded by the first abort; empty string while none. */
const char *
r3v_status_load_machine_abort_reason(
   const struct r3v_status_load_machine *machine);

#ifdef __cplusplus
}
#endif

#endif /* R3V_NATIVE_STATUS_LOAD_MACHINE_H */
