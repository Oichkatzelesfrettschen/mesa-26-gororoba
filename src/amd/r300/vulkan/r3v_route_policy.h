/*
 * SPDX-License-Identifier: MIT
 *
 * Route policy: which executor performs one Vulkan operation, and the
 * phased record of what actually did.
 *
 * The split follows the ledger's own.  r300_operation_route.h owns route
 * identity, executor, maturity, unit, contracts, evidence, and the semantic
 * uses a route serves; none of that knows a VkBuffer or a command type.
 * This file owns what the ledger cannot see: the command being recorded, the
 * resource shape, the requested execution policy, and the gates the device
 * cached at creation.  Common enumerates candidates and this chooses one, so
 * table order is never a policy.
 *
 * A precommitted route is admitted here rather than in common's selector.
 * The selector reaches EXECUTING routes only, which is what keeps an
 * unreceipted route out of every ordinary path; a route with an
 * implementation and no current-epoch receipt runs under its own exact
 * opt-in and reports that admission in its provenance, so an experimental
 * delivery stays distinguishable from a promoted one.
 *
 * The gate state arrives as an argument rather than through getenv: the
 * device reads every route gate once at creation, so one process's route
 * decision holds for the life of that device and a mid-process environment
 * mutation moves nothing.
 *
 * This translation unit names no VkResult.  A refusal is a typed verdict
 * plus a naming reason; the API boundary alone converts one into the
 * command's registry-legal result.
 */

#ifndef R3V_ROUTE_POLICY_H
#define R3V_ROUTE_POLICY_H

#include "amd/r300/common/r300_operation_route.h"

#include <stdbool.h>
#include <stdint.h>

/* What a caller asks of the driver.
 *
 * AUTO takes the fastest qualified route, which means the host unless an
 * exact route gate stands open: no crossover has been measured, and "GPU
 * whenever available" is a guess rather than a policy.  GPU_ONLY refuses
 * before any application-visible write when no qualified GPU route exists,
 * which is what makes a hardware claim checkable.  CPU_REFERENCE runs the
 * semantic reference implementation.
 *
 * A shadow-compare policy belongs here too and lands with the comparator
 * that implements it; an enumerant with no behavior behind it would answer
 * a request the driver cannot serve.
 */
enum r3v_execution_policy {
   R3V_EXECUTION_AUTO = 0,
   R3V_EXECUTION_GPU_ONLY,
   R3V_EXECUTION_CPU_REFERENCE,
};

/* Reads the policy a value names, defaulting to AUTO.  The values are the
 * enum's own lowercase names; anything else is AUTO, so a typo leaves the
 * default rather than opening a stricter or looser path.  The caller
 * supplies the value it cached, which keeps the environment read at device
 * creation beside the route gates.
 */
enum r3v_execution_policy r3v_execution_policy_from_value(const char *value);

const char *r3v_execution_policy_name(enum r3v_execution_policy p);

/* One operation on one resource shape, in terms the ledger can answer.  The
 * byte range and element width are the operation's, not the route's: a route
 * decides whether it can carry them, and refusing is its answer.
 *
 * The selector reads every field.  element_bytes counts the range: a range
 * that is not a whole number of elements starting on an element boundary
 * describes no operation and refuses for either executor.
 * destination_device_visible gates the device rows: a destination the device
 * cannot reach disqualifies every GPU route ahead of the ledger.  A route's
 * own admission -- the pitch grid, the offset grid, the segment bound a
 * carrier imposes -- belongs to that route and is checked where it is
 * emitted. */
struct r3v_route_request {
   enum r300_operation_id operation_id;
   enum r300_operation_route_use use;
   enum r3v_execution_policy policy;

   uint64_t byte_offset;
   uint64_t byte_size;
   uint32_t element_bytes;

   bool destination_device_visible;
   bool destination_host_mapped;
};

/* What the policy decided.  REFUSE is a verdict, not a fallback: a caller
 * that asked for GPU_ONLY and got REFUSE keeps its hands off the host path,
 * because the refusal is the whole point of the policy. */
enum r3v_route_decision {
   R3V_ROUTE_DECISION_HOST = 0,
   R3V_ROUTE_DECISION_GPU,
   R3V_ROUTE_DECISION_REFUSE,
};

const char *r3v_route_decision_name(enum r3v_route_decision d);

/* How far one execution has travelled, in the six states a submission
 * passes through.  Each names a distinct fact a reader needs separately: a
 * committed record has passed every refusal, an entered ioctl has reached
 * the kernel whatever it answers, an accepted ioctl has a command the CS
 * validator admitted, a retired completion has a fence the device signalled,
 * and a verified result has been compared against its oracle.  Collapsing
 * any pair loses the distinction between "the kernel took it" and "the
 * device finished it", which is the distinction a hardware claim rests on.
 *
 * The ladder is total for a device submission and advances one step at a
 * time.  A host route reaches COMMITTED and stops: the four states above it
 * name the ioctl and its completion, and a host route has neither.
 */
enum r3v_execution_phase {
   R3V_EXECUTION_PHASE_PREPARED = 0,
   R3V_EXECUTION_PHASE_COMMITTED,
   R3V_EXECUTION_PHASE_IOCTL_ENTERED,
   R3V_EXECUTION_PHASE_IOCTL_ACCEPTED,
   R3V_EXECUTION_PHASE_COMPLETION_RETIRED,
   R3V_EXECUTION_PHASE_RESULT_VERIFIED,
};

const char *r3v_execution_phase_name(enum r3v_execution_phase phase);

/* The highest phase a host route reaches; the states above it name the
 * ioctl and the fence. */
#define R3V_EXECUTION_PHASE_HOST_TERMINAL R3V_EXECUTION_PHASE_COMMITTED

/* Advances *phase to next, writing true when next is exactly one step ahead.
 * A skip, a repeat, a step back, and a value outside the ladder all write
 * false with *reason naming the refusal, so a record that never entered the
 * ioctl cannot report a retired completion. */
bool r3v_execution_phase_advance(enum r3v_execution_phase *phase,
                                 enum r3v_execution_phase next,
                                 const char **reason);

/* What executed, retained beside the result.
 *
 * host_semantic_node records whether the host computed the operation's
 * result.  The host still parses Vulkan, allocates, builds PM4, and calls
 * the ioctl on a GPU route; what it does not do is produce the bytes.  That
 * distinction is the whole content of a hardware claim, so it is a field
 * rather than a comment.
 *
 * phase carries how far the execution travelled, so a reader separates a
 * record the policy admitted from one the device retired.
 */
struct r3v_execution_provenance {
   enum r300_operation_id operation_id;
   enum r300_operation_route_id route_id;
   enum r300_execution_unit unit;
   enum r300_operation_route_executor executor;
   enum r300_operation_route_state route_state;
   enum r3v_execution_phase phase;

   bool host_semantic_node;
   bool device_submission;
   /* The route carries an implementation and no current-epoch receipt, and
    * ran because its exact gate stood open. */
   bool experimental_admission;

   uint32_t ib_dwords;
   uint32_t relocation_count;
};

/* Holds a provenance record to the policy that produced it, returning true
 * or false with *reason naming the first rule it breaks.
 *
 * Under GPU_ONLY the rules are the hardware claim itself: the host computed
 * nothing, the route runs on the GPU, and a submission reached the device.
 * Across every policy, an experimental admission and a non-executing route
 * state imply each other, so a promoted route cannot report itself
 * experimental and a precommitted one cannot report itself promoted.
 *
 * The phase binds to the same facts: device_submission holds exactly at
 * IOCTL_ENTERED and above, and a host route stays at or below its terminal
 * phase, so a record cannot claim a kernel entry its executor never makes.
 *
 * A record naming a route identity is held to that route's ledger row --
 * operation, executor, unit, and maturity -- because the ledger owns those
 * facts and a record carrying its own copies could describe a delivery no
 * route performs.  A host record that names no row leaves route_id NONE and
 * keeps the ledger's zero values for the fields a row owns, so it asserts
 * no unit and no maturity it cannot back, and a value outside either enum
 * fails that comparison the same way.
 */
bool r3v_execution_provenance_valid(const struct r3v_execution_provenance *p,
                                    enum r3v_execution_policy policy,
                                    const char **reason);

/* Chooses the executor for one request.
 *
 * gate_state is indexed by route id, as r300_operation_select_route takes
 * it, and covers precommitted rows as well as executing ones: the device
 * caches every route's gate at creation, so this function reads no
 * environment.  On R3V_ROUTE_DECISION_GPU, *route names the chosen row; on
 * the other decisions *route is NULL and *reason names the ground.
 *
 * Selection is fail-closed at every step: a malformed request, a use mask
 * naming other than one defined purpose, a range outside its element grid,
 * an operation with no qualified route under GPU_ONLY, and two eligible GPU
 * routes all refuse rather than picking.  The use and shape checks run ahead
 * of both the promoted selector and the precommitted scan, so a request that
 * spans two purposes or counts no elements reaches neither.
 */
enum r3v_route_decision
r3v_route_policy_select(const struct r3v_route_request *request,
                        const bool *gate_state,
                        const struct r300_operation_route_row **route,
                        const char **reason);

#endif /* R3V_ROUTE_POLICY_H */
