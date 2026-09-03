/*
 * SPDX-License-Identifier: MIT
 *
 * Route policy: which executor performs one Vulkan operation, and the
 * record of what actually did.
 *
 * The split follows the ledger's own.  r300_operation_route.h owns route
 * identity, executor, maturity, unit, contracts, evidence, and the
 * semantic uses a route serves; none of that knows a VkBuffer or a command
 * type.  This file owns what the ledger cannot see: the command being
 * recorded, the resource shape, the requested execution policy, and the
 * gates the environment opens.  Common enumerates candidates and this
 * chooses one, so table order is never a policy.
 *
 * A precommitted route is admitted here rather than in common's selector.
 * The selector reaches EXECUTING routes only, which is what keeps an
 * unreceipted route out of every ordinary path; a route with an
 * implementation and no current-epoch receipt runs under its own exact
 * opt-in and reports that admission in its provenance, so an experimental
 * delivery is never mistaken for a promoted one.
 */

#ifndef R3V_ROUTE_POLICY_H
#define R3V_ROUTE_POLICY_H

#include "amd/r300/common/r300_operation_route.h"

#include <stdbool.h>
#include <stdint.h>

/* What a caller asks of the driver.
 *
 * AUTO takes the fastest qualified route, which today means the host
 * unless an exact route gate stands open: no crossover has been measured,
 * and "GPU whenever available" is a guess rather than a policy.
 * GPU_ONLY refuses before any application-visible write when no qualified
 * GPU route exists, which is what makes a hardware claim checkable.
 * CPU_REFERENCE runs the semantic reference implementation.
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

/* Reads the policy an environment names, defaulting to AUTO.  The values
 * are the enum's own lowercase names; anything else is AUTO, so a typo
 * leaves the default rather than opening a stricter or looser path. */
enum r3v_execution_policy r3v_execution_policy_from_environment(void);

const char *r3v_execution_policy_name(enum r3v_execution_policy p);

/* One operation on one resource shape, in terms the ledger can answer.
 * The byte range and element width are the operation's, not the route's:
 * a route decides whether it can carry them, and refusing is its answer. */
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
 * that asked for GPU_ONLY and got REFUSE must not then run the host path,
 * because the refusal is the whole point of the policy. */
enum r3v_route_decision {
   R3V_ROUTE_DECISION_HOST = 0,
   R3V_ROUTE_DECISION_GPU,
   R3V_ROUTE_DECISION_REFUSE,
};

const char *r3v_route_decision_name(enum r3v_route_decision d);

/* What executed, retained beside the result.
 *
 * host_semantic_node records whether the host computed the operation's
 * result.  The host still parses Vulkan, allocates, builds PM4, and calls
 * the ioctl on a GPU route; what it does not do is produce the bytes.  That
 * distinction is the whole content of a hardware claim, so it is a field
 * rather than a comment.
 */
struct r3v_execution_provenance {
   enum r300_operation_id operation_id;
   enum r300_operation_route_id route_id;
   enum r300_execution_unit unit;
   enum r300_operation_route_executor executor;
   enum r300_operation_route_state route_state;

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
 */
bool r3v_execution_provenance_valid(const struct r3v_execution_provenance *p,
                                    enum r3v_execution_policy policy,
                                    const char **reason);

/* Chooses the executor for one request.
 *
 * gate_state is indexed by route id, as r300_operation_select_route takes
 * it.  On R3V_ROUTE_DECISION_GPU, *route names the chosen row; on the other
 * decisions *route is NULL and *reason names the ground.
 *
 * Selection is fail-closed at every step: a malformed request, an operation
 * with no qualified route under GPU_ONLY, and two eligible GPU routes all
 * refuse rather than picking.
 */
enum r3v_route_decision
r3v_route_policy_select(const struct r3v_route_request *request,
                        const bool *gate_state,
                        const struct r300_operation_route_row **route,
                        const char **reason);

#endif /* R3V_ROUTE_POLICY_H */
