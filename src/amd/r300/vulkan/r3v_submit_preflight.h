/*
 * SPDX-License-Identifier: MIT
 *
 * Submit preflight: the recorded-work census, the whole-submit route
 * accounting, and the prepare -> validate -> commit transaction the
 * submission boundary runs inside.
 *
 * The transaction states one rule the queue has held in prose and never in
 * a predicate: every refusal lands before any device-visible effect, so a
 * refused submit leaves application memory as it found it.  A transaction
 * records each effect it applies and refuses a refusal that follows one, so
 * the ordering becomes a fact a test can break rather than a comment a
 * reader must trust.
 *
 * The census replaces a field list.  A predicate asking "does this command
 * buffer carry work besides the one route candidate" enumerated the command
 * buffer's recorded-work fields inline, so a work kind added to the command
 * buffer had to be added to every such list.  One census counts each kind
 * once and every predicate reads the total, which is what keeps a new work
 * kind from being silently dropped by a predicate that never learned about
 * it.
 *
 * The submit accounting is whole-submit rather than per command buffer.  An
 * authorization declares one stream and its evidence directory disarms after
 * one attempt, and an execution policy is a property of the submit a caller
 * issued, so counting route candidates one command buffer at a time would
 * admit a submit whose second buffer has nowhere to run.
 *
 * This translation unit names no VkResult.  A refusal is a typed verdict
 * plus a naming reason, and the API boundary alone converts a verdict into
 * the command's registry-legal result.
 */

#ifndef R3V_SUBMIT_PREFLIGHT_H
#define R3V_SUBMIT_PREFLIGHT_H

#include "r3v_route_policy.h"

#include "amd/r300/common/r300_operation_route.h"

#include <stdbool.h>
#include <stdint.h>

/* Every refusal the preparation layer produces, as a typed verdict.  The
 * result class below carries what the API boundary needs to answer a
 * caller; the verdict itself stays internal so a refusal is never confused
 * with the one result value the public surface reserves for a refused
 * command.
 */
enum r3v_submit_refusal {
   R3V_SUBMIT_ADMITTED = 0,
   /* A retained submission carries one executable command buffer, because
    * one authorization declares one stream digest. */
   R3V_SUBMIT_REFUSAL_MULTIPLE_TRANSPORT,
   /* Two command buffers in one submit each carry a GPU route candidate;
    * the route admission covers one. */
   R3V_SUBMIT_REFUSAL_MULTIPLE_ROUTE_CANDIDATES,
   /* GPU_ONLY over the whole submit: no command buffer carries a route
    * candidate, so the policy refuses instead of falling back. */
   R3V_SUBMIT_REFUSAL_GPU_ONLY_NO_CANDIDATE,
   /* A command buffer carries recorded work besides its route candidate,
    * whose submission-time semantics the route would hoist. */
   R3V_SUBMIT_REFUSAL_UNROUTED_RECORDED_WORK,
   /* A route candidate names a command buffer with no recorded stream. */
   R3V_SUBMIT_REFUSAL_CANDIDATE_WITHOUT_TRANSPORT,
   /* The route table the device caches its gates from is malformed. */
   R3V_SUBMIT_REFUSAL_MALFORMED_ROUTE_TABLE,
   /* A transaction applied a device-visible effect outside the commit
    * phase, or refused after one. */
   R3V_SUBMIT_REFUSAL_PHASE_ORDER,
   /* The caller named no census or no transaction: a defect in the calling
    * code rather than a shape the driver declines. */
   R3V_SUBMIT_REFUSAL_ABSENT_RECORD,
   R3V_SUBMIT_REFUSAL_COUNT,
};

const char *r3v_submit_refusal_name(enum r3v_submit_refusal refusal);

/* What the API boundary answers with.  The class names the resource or the
 * contract that failed; the boundary maps each class to the result its
 * command's registry entry permits, and the refusal class is the only one
 * that reaches the reserved refusal result.
 */
enum r3v_submit_result_class {
   R3V_SUBMIT_RESULT_SUCCESS = 0,
   /* The driver declines the command; the boundary spells this with the
    * one result the whole native refusal set shares. */
   R3V_SUBMIT_RESULT_REFUSAL,
   /* The submission is lost: an accepted authorization, an evidence write,
    * or a transport step failed past the point of return. */
   R3V_SUBMIT_RESULT_DEVICE_LOST,
};

enum r3v_submit_result_class
r3v_submit_refusal_result_class(enum r3v_submit_refusal refusal);

/* The recorded work one command buffer carries, one counter per kind the
 * command buffer records.  transport_ib_dwords is the recorded stream's
 * length: a command buffer with a stream submits, and one without it
 * executes its recorded work on the host at submission.
 *
 * r3v_recorded_work_census_ordered() below reads the five counters whose
 * semantics execute at submission time in recorded order.  The stream length stays outside
 * that sum: a recorded stream is the transport, not work the submission
 * boundary must order around it.
 */
struct r3v_recorded_work_census {
   uint32_t transfer_copies;
   uint32_t draws;
   uint32_t dispatches;
   uint32_t query_ops;
   uint32_t event_ops;
   uint32_t transport_ib_dwords;
};

/* Fills a census from one command buffer's counters.  The caller reads the
 * command buffer; this file stays free of Vulkan types, and the mapping from
 * a recorded field to its census counter is one argument each. */
void r3v_recorded_work_census_set(struct r3v_recorded_work_census *c,
                                  uint32_t transfer_copies, uint32_t draws,
                                  uint32_t dispatches, uint32_t query_ops,
                                  uint32_t event_ops,
                                  uint32_t transport_ib_dwords);

/* The total recorded work, over the kinds that carry submission-time
 * ordering.  A predicate asking whether a command buffer holds work besides
 * one route candidate subtracts that candidate's own count from this. */
uint32_t
r3v_recorded_work_census_total(const struct r3v_recorded_work_census *c);

/* Whether the command buffer carries work whose semantics execute in
 * recorded order at submission.  A route that hoists a command buffer's
 * transport ahead of the submission boundary reaches a buffer with none. */
bool
r3v_recorded_work_census_ordered(const struct r3v_recorded_work_census *c);

/* The whole submit, counted once.  route_candidates counts command buffers
 * carrying a candidate for a GPU route; unrouted_work_buffers counts those
 * whose recorded work exceeds what their candidate covers. */
struct r3v_submit_census {
   uint32_t command_buffers;
   uint32_t executable_transport;
   uint32_t route_candidates;
   uint32_t candidates_without_transport;
   uint32_t unrouted_work_buffers;
   uint32_t recorded_work_total;
};

/* Folds one command buffer's census into the submit's.  has_candidate says
 * the caller's route policy admitted a GPU route for this buffer, and
 * candidate_covers_work says the candidate accounts for every counted work
 * item; a candidate covering three of a buffer's four recorded items leaves
 * the fourth unrouted and the buffer counted here. */
void r3v_submit_census_add(struct r3v_submit_census *submit,
                           const struct r3v_recorded_work_census *buffer,
                           bool has_candidate, bool candidate_covers_work);

/* The whole-submit verdict, taken before any command buffer runs.
 *
 * retention_bound says an authorization or a plan session binds this submit
 * to one executable command buffer.  The policy is the submit's, so a
 * GPU_ONLY submit with no route candidate anywhere refuses here rather than
 * running its first buffer on the host and reporting the policy failure on
 * the second.
 */
enum r3v_submit_refusal
r3v_submit_preflight_check(const struct r3v_submit_census *census,
                           bool retention_bound,
                           enum r3v_execution_policy policy,
                           const char **reason);

/* The three phases of one submit.
 *
 * PREPARE builds every fallible thing: relocation lists, digests, evidence
 * artifacts, the completion buffer, the command stream.  VALIDATE runs every
 * gate: the route policy, the arming verdict, the plan admission.  COMMIT
 * alone touches application memory, the ioctl, and the fence.  A refusal
 * belongs to the first two phases, which is what leaves a refused submit's
 * target bytes as it found them.
 */
enum r3v_submit_phase {
   R3V_SUBMIT_PHASE_PREPARE = 0,
   R3V_SUBMIT_PHASE_VALIDATE,
   R3V_SUBMIT_PHASE_COMMIT,
};

const char *r3v_submit_phase_name(enum r3v_submit_phase phase);

/* One submit's transaction record.  effects counts the device-visible
 * effects the commit phase applied, and refusal holds the first verdict the
 * transaction refused with. */
struct r3v_submit_transaction {
   enum r3v_submit_phase phase;
   uint32_t effects;
   enum r3v_submit_refusal refusal;
};

void r3v_submit_transaction_begin(struct r3v_submit_transaction *t);

/* Advances to next, which is exactly one phase ahead.  A skip, a repeat, and
 * a step back all refuse, so a transaction cannot reach the commit phase
 * without passing validation. */
bool r3v_submit_transaction_advance(struct r3v_submit_transaction *t,
                                    enum r3v_submit_phase next,
                                    const char **reason);

/* Records one device-visible effect.  The commit phase alone admits one:
 * an effect in prepare or validate would put bytes in application memory
 * ahead of a gate that can still refuse. */
bool r3v_submit_transaction_record_effect(struct r3v_submit_transaction *t,
                                          const char **reason);

/* Refuses the transaction with a verdict.  A refusal after an effect breaks
 * the transaction's own rule and returns false with *reason naming it, so a
 * caller that reordered a gate past a write finds out here. */
bool r3v_submit_transaction_refuse(struct r3v_submit_transaction *t,
                                   enum r3v_submit_refusal refusal,
                                   const char **reason);

/* Whether the transaction may still refuse without leaving application
 * memory changed. */
bool r3v_submit_transaction_reversible(const struct r3v_submit_transaction *t);

/* Whether a route table admits a device.
 *
 * The ledger's own well-formedness comes first; it bounds every route
 * identity to the enum the device's cached gate array is sized by, so the
 * one field left is the gate string that array's entries are read from.  A
 * gate naming no variable leaves its route's opt-in undecidable, and the
 * refusal lands at device creation rather than at the first submission that
 * reads a gate.
 */
bool r3v_route_table_admits_device(const struct r300_operation_route_row *t,
                                   uint32_t count, const char **reason);

/* Fills gate_state[] from the device's cached gate values: an entry holds
 * true when that route's cached value is the literal "1", the exact opt-in
 * every route gate takes.  A cached "0", an empty string, and any other text
 * leave the gate closed, so a value's presence is not consent.  count is the
 * array length in both directions and must cover every route identity, so
 * the selector's index is in range by construction.  Returns false when the
 * array is too short to hold the ledger. */
bool r3v_route_gate_state_from_cache(const char *const *cached_values,
                                     bool *gate_state, uint32_t count);

#endif /* R3V_SUBMIT_PREFLIGHT_H */
