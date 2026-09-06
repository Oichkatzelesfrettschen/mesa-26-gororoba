/*
 * SPDX-License-Identifier: MIT
 *
 * Reading the bounded measurement declaration into one session.
 *
 * The declaration is a file an operator writes before any device exists.
 * It is read once, at device creation, into memory the caller owns; the
 * digest covers exactly those bytes and the parse reads exactly those
 * bytes, so the identity a campaign publishes names the declaration the
 * session actually opened.  Hashing a path and reopening it to parse
 * would let the file change between the two reads.
 *
 * This interface takes no device.  The deployment below carries every
 * fact the checks compare, so each of the load's boundaries -- the open,
 * the bounded read, the text allocation, the parse, the epoch, the
 * platform, the route, and the session open -- is reachable from a test
 * with a temporary file and no Vulkan.
 *
 * Reading a declaration opens a predicate and grants nothing.  The route
 * checks below hold the declared executor to what the build carries and
 * what this device already selected; they open no gate, lower no evidence
 * floor, and resolve no competing pair by preference.
 */

#ifndef R3V_MEASUREMENT_DECLARATION_H
#define R3V_MEASUREMENT_DECLARATION_H

#include "r3v_measurement_session.h"

#include "amd/r300/common/r300_chip_identity.h"
#include "amd/r300/common/r300_operation_route.h"

#include <stdbool.h>
#include <stdint.h>

/* What the load resolved.  ABSENT preserves ordinary behavior, because no
 * declaration was named; every other value refuses the device.  An
 * unreadable or malformed declaration is a defect in what the operator
 * wrote, and a failed allocation is a shortage of memory, so the two
 * carry different results to the caller rather than one standing in for
 * the other. */
enum r3v_measurement_declaration_status {
   R3V_MEASUREMENT_DECLARATION_ABSENT = 0,
   R3V_MEASUREMENT_DECLARATION_OPENED,
   R3V_MEASUREMENT_DECLARATION_REFUSED,
   R3V_MEASUREMENT_DECLARATION_NO_MEMORY,
};

/* Every fact the declaration is held against, collected before the load
 * so the load itself reads the filesystem once and the environment never.
 */
struct r3v_measurement_deployment {
   uint32_t pci_vendor_id;
   uint32_t pci_device_id;
   /* The running kernel release and radeon module srcversion, read
    * through the arming provider.  An empty string is an unreadable fact
    * and refuses at the epoch check. */
   const char *kernel_release;
   const char *module_srcversion;
   /* The board this device resolved, which the declaration's platform
    * name must select.  A PCI id names a die class several boards share,
    * so the epoch check's pair is a consistency check and this is the
    * specimen. */
   enum r300_platform_id platform_id;
   const struct r300_operation_route_row *routes;
   uint32_t route_count;
   /* Whether each route's own opt-in stands open, indexed by enum
    * r300_operation_route_id.  It arrives already decided, from
    * r3v_route_gate_state_from_cache, which owns the rule that a gate
    * opens on the literal "1" and on nothing else.  Restating that rule
    * here would put a second reader of the same cache on the fail-open
    * side of it. */
   const bool *route_gate_open;
   uint32_t route_gate_count;
   /* A test refuses the text allocation here to reach the no-memory arm.
    * Production leaves it false. */
   bool refuse_text_allocation;
};

/* Reads the declaration at `path` and opens `session` over it.
 *
 * `session` has passed through `r3v_measurement_session_init`, once,
 * before its first load; this call never resets it, because a load that
 * cleared a live session would restore the allowance that session spent.
 * A second load over an open session refuses and leaves its bindings and
 * counters standing.  `*reason` names the first rule that refused and is
 * NULL on ABSENT and OPENED.  Every rule this file owns names itself with
 * a string literal; the one refusal that does not is a closed session,
 * whose reason points into `session` and lives as long as it does.
 * Nothing durable is written: no attempt token, no binding, no consumed
 * execution.
 */
enum r3v_measurement_declaration_status
r3v_measurement_declaration_open(
   struct r3v_measurement_session *session, const char *path,
   const struct r3v_measurement_deployment *deployment, const char **reason);

#endif /* R3V_MEASUREMENT_DECLARATION_H */
