/*
 * SPDX-License-Identifier: MIT
 *
 * Submitter-side state machine for the serial status-load cell.  The
 * machine walks each submission through the frozen RS482 status-event
 * ladder (PREPARE through REPOISONED), emits one JSONL barrier message
 * per transition, and refuses IOCTL_ENTER unless the paired census
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

/* One formatted message line: eight short fields plus the newline stays
 * well inside this bound, and the formatter rejects overflow.
 */
#define R3V_STATUS_LOAD_MESSAGE_CAPACITY 320u

enum r3v_status_load_phase {
   R3V_STATUS_LOAD_RUNNING,
   R3V_STATUS_LOAD_COMPLETE,
   R3V_STATUS_LOAD_ABORTED,
};

/* The sampler's resting states as the submitter tracks them.
 * SAMPLER_ENTER_READ is a recorded event inside READY, so READY absorbs
 * it rather than adding a state.
 */
enum r3v_status_load_sampler {
   R3V_STATUS_LOAD_SAMPLER_ABSENT,
   R3V_STATUS_LOAD_SAMPLER_OPEN,
   R3V_STATUS_LOAD_SAMPLER_CALIBRATED,
   R3V_STATUS_LOAD_SAMPLER_READY,
   R3V_STATUS_LOAD_SAMPLER_STOPPED,
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
   int (*submit)(void *ctx, uint32_t iteration);
   int (*fence_wait)(void *ctx, uint32_t iteration);
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
   uint64_t last_sampler_ns;
   int have_sampler_ns;
   enum r3v_status_load_phase phase;
   const char *abort_reason;
};

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

/* Binds the ops table, the shared nonce, and the declared iteration
 * count (1 through R3V_STATUS_LOAD_MAX_ITERATIONS).  Returns 0, or -1
 * for a malformed nonce, an out-of-bound count, or a missing operation.
 */
int
r3v_status_load_machine_init(struct r3v_status_load_machine *machine,
                             const struct r3v_status_load_ops *ops,
                             const char *nonce, uint32_t iterations);

/* Feeds one observed sampler event by protocol state name.  An
 * out-of-order sampler transition or a sampler timestamp regression
 * aborts the run.  Returns 0 while the run keeps going.
 */
int
r3v_status_load_machine_sampler(struct r3v_status_load_machine *machine,
                                const char *state_name,
                                uint64_t timestamp_ns);

/* Runs one full submission through the ladder.  Returns 0 when the
 * iteration reached REPOISONED; nonzero means the run aborted at the
 * position named by the abort reason, and no later transition was
 * emitted.  A call past the declared count aborts a running machine;
 * a call after a terminal phase is refused with nothing emitted, so a
 * completed run keeps its verdict.
 */
int
r3v_status_load_machine_iterate(struct r3v_status_load_machine *machine);

/* Emits COMPLETE once every declared iteration has run.  A finish before
 * the declared count aborts instead: a short run must say ABORT, never
 * pass as complete.  Returns 0 on COMPLETE.
 */
int
r3v_status_load_machine_finish(struct r3v_status_load_machine *machine);

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
