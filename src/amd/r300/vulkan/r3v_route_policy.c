/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_route_policy.h"

#include <stdlib.h>
#include <string.h>

/* Every hazardous or route-selecting opt-in in R3V opens on the
 * exact string "1", so an unset, empty, or zero value holds it closed. */
static bool
exact_opt_in(const char *name)
{
   const char *value = name != NULL ? getenv(name) : NULL;
   return value != NULL && strcmp(value, "1") == 0;
}

enum r3v_execution_policy
r3v_execution_policy_from_environment(void)
{
   const char *value = getenv("R3V_EXECUTION_POLICY");
   if (value == NULL)
      return R3V_EXECUTION_AUTO;
   if (strcmp(value, "gpu_only") == 0)
      return R3V_EXECUTION_GPU_ONLY;
   if (strcmp(value, "cpu_reference") == 0)
      return R3V_EXECUTION_CPU_REFERENCE;
   return R3V_EXECUTION_AUTO;
}

const char *
r3v_execution_policy_name(enum r3v_execution_policy p)
{
   static const char *const names[] = { "auto", "gpu_only", "cpu_reference" };
   return (unsigned)p <= R3V_EXECUTION_CPU_REFERENCE ? names[p] : NULL;
}

const char *
r3v_route_decision_name(enum r3v_route_decision d)
{
   static const char *const names[] = { "host", "gpu", "refuse" };
   return (unsigned)d <= R3V_ROUTE_DECISION_REFUSE ? names[d] : NULL;
}

bool
r3v_execution_provenance_valid(const struct r3v_execution_provenance *p,
                               enum r3v_execution_policy policy,
                               const char **reason)
{
   const char *ignored = NULL;
   if (reason == NULL)
      reason = &ignored;
   *reason = NULL;

   if (p == NULL) {
      *reason = "provenance is absent";
      return false;
   }
   if (p->operation_id == R300_OPERATION_ID_NONE) {
      *reason = "provenance names no operation";
      return false;
   }

   const bool gpu = p->executor == R300_OPERATION_ROUTE_EXECUTOR_GPU;
   const bool executing = p->route_state == R300_OPERATION_ROUTE_EXECUTING;

   /* A host route names no route row, so its identity, unit, and state are
    * the ledger's zero values and only the executor and the semantic node
    * carry meaning. */
   if (!gpu) {
      if (!p->host_semantic_node) {
         *reason = "host route computed nothing";
         return false;
      }
      if (p->device_submission) {
         *reason = "host route reports a device submission";
         return false;
      }
   } else {
      if (p->route_id == R300_OPERATION_ROUTE_NONE) {
         *reason = "GPU route names no route identity";
         return false;
      }
      if (p->ib_dwords == 0) {
         *reason = "GPU route submitted an empty stream";
         return false;
      }
   }

   /* An experimental admission and a route that has not been promoted
    * imply each other, in both directions: a promoted route reporting
    * itself experimental would understate what it claims, and a
    * precommitted route reporting itself promoted would overstate it. */
   if (gpu && p->experimental_admission == executing) {
      *reason = executing ? "executing route reports an experimental "
                            "admission"
                          : "unpromoted route reports a promoted admission";
      return false;
   }

   switch (policy) {
   case R3V_EXECUTION_GPU_ONLY:
      /* The hardware claim, stated as the three facts that make it true. */
      if (p->host_semantic_node) {
         *reason = "GPU_ONLY: the host computed the result";
         return false;
      }
      if (!gpu) {
         *reason = "GPU_ONLY: the route runs on the host";
         return false;
      }
      if (!p->device_submission) {
         *reason = "GPU_ONLY: no submission reached the device";
         return false;
      }
      break;
   case R3V_EXECUTION_CPU_REFERENCE:
      if (gpu) {
         *reason = "CPU_REFERENCE: a GPU route executed";
         return false;
      }
      break;
   case R3V_EXECUTION_AUTO:
      break;
   default:
      *reason = "policy outside the policy enum";
      return false;
   }

   return true;
}

enum r3v_route_decision
r3v_route_policy_select(const struct r3v_route_request *request,
                        const bool *gate_state,
                        const struct r300_operation_route_row **route,
                        const char **reason)
{
   const struct r300_operation_route_row *ignored_route = NULL;
   const char *ignored_reason = NULL;
   if (route == NULL)
      route = &ignored_route;
   if (reason == NULL)
      reason = &ignored_reason;
   *route = NULL;
   *reason = NULL;

   if (request == NULL) {
      *reason = "route request is absent";
      return R3V_ROUTE_DECISION_REFUSE;
   }
   if (request->byte_size == 0) {
      *reason = "route request names an empty range";
      return R3V_ROUTE_DECISION_REFUSE;
   }
   if ((unsigned)request->policy > R3V_EXECUTION_CPU_REFERENCE) {
      *reason = "route request names no execution policy";
      return R3V_ROUTE_DECISION_REFUSE;
   }
   if (request->policy == R3V_EXECUTION_CPU_REFERENCE) {
      *reason = "cpu_reference policy";
      return R3V_ROUTE_DECISION_HOST;
   }

   /* A promoted route first: the selector reaches EXECUTING rows only, and
    * one that answers is the route this operation takes with no
    * experimental admission behind it. */
   const struct r300_operation_route_row *promoted =
      r300_operation_select_route(request->operation_id,
                                  R300_OPERATION_ROUTE_EXECUTOR_GPU,
                                  request->use, gate_state, reason);
   if (promoted != NULL) {
      *route = promoted;
      return R3V_ROUTE_DECISION_GPU;
   }

   /* Then a precommitted route under its own exact gate.  The row carries
    * an implementation, a route contract, and an admission contract; what
    * it lacks is a current-epoch receipt, so it runs only where an operator
    * asked for it by name. */
   const struct r300_operation_route_row *candidate = NULL;
   uint32_t rows = 0;
   const struct r300_operation_route_row *all =
      r300_operation_route_rows(&rows);
   for (uint32_t i = 0; i < rows; i++) {
      const struct r300_operation_route_row *r = &all[i];
      if (r->operation_id != request->operation_id ||
          r->executor != R300_OPERATION_ROUTE_EXECUTOR_GPU ||
          r->state != R300_OPERATION_ROUTE_PRECOMMITTED ||
          (r->uses & (uint32_t)request->use) == 0 || r->gate == NULL)
         continue;
      if (!exact_opt_in(r->gate))
         continue;
      if (candidate != NULL) {
         /* Two open experimental gates for one purpose leave the choice to
          * table order, which is not a policy here either. */
         *reason = "two precommitted routes admitted for one use";
         return R3V_ROUTE_DECISION_REFUSE;
      }
      candidate = r;
   }

   if (candidate != NULL) {
      *route = candidate;
      return R3V_ROUTE_DECISION_GPU;
   }

   if (request->policy == R3V_EXECUTION_GPU_ONLY) {
      /* The refusal is the policy working.  Falling back here would make
       * GPU_ONLY a preference, and a preference proves nothing. */
      if (*reason == NULL)
         *reason = "gpu_only: no qualified GPU route";
      return R3V_ROUTE_DECISION_REFUSE;
   }

   *reason = "no qualified GPU route; the host path is the default";
   return R3V_ROUTE_DECISION_HOST;
}
