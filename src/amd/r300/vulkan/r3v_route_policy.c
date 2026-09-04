/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_route_policy.h"

#include "util/macros.h"

#include <string.h>

enum r3v_execution_policy
r3v_execution_policy_from_value(const char *value)
{
   if (value == NULL || value[0] == '\0')
      return R3V_EXECUTION_AUTO;
   if (strcmp(value, "auto") == 0)
      return R3V_EXECUTION_AUTO;
   if (strcmp(value, "gpu_only") == 0)
      return R3V_EXECUTION_GPU_ONLY;
   if (strcmp(value, "cpu_reference") == 0)
      return R3V_EXECUTION_CPU_REFERENCE;
   return R3V_EXECUTION_POLICY_INVALID;
}

const char *
r3v_execution_policy_name(enum r3v_execution_policy p)
{
   static const char *const names[] = { "auto", "gpu_only", "cpu_reference",
                                        "invalid" };
   return (unsigned)p <= R3V_EXECUTION_POLICY_INVALID ? names[p] : NULL;
}

const char *
r3v_route_decision_name(enum r3v_route_decision d)
{
   static const char *const names[] = { "host", "gpu", "refuse" };
   return (unsigned)d <= R3V_ROUTE_DECISION_REFUSE ? names[d] : NULL;
}

const char *
r3v_execution_phase_name(enum r3v_execution_phase phase)
{
   static const char *const names[] = {
      "prepared",           "committed",          "ioctl_entered",
      "ioctl_accepted",     "completion_retired", "result_verified",
   };
   return (unsigned)phase <= R3V_EXECUTION_PHASE_RESULT_VERIFIED
             ? names[phase]
             : NULL;
}

bool
r3v_execution_phase_advance(enum r3v_execution_phase *phase,
                            enum r3v_execution_phase next,
                            const char **reason)
{
   const char *ignored = NULL;
   if (reason == NULL)
      reason = &ignored;
   *reason = NULL;

   if (phase == NULL) {
      *reason = "phase advance names no record";
      return false;
   }
   if (r3v_execution_phase_name(*phase) == NULL ||
       r3v_execution_phase_name(next) == NULL) {
      *reason = "phase outside the execution ladder";
      return false;
   }
   /* One step at a time: the ladder's whole content is that each state
    * proves the one below it ran, and a skip asserts a state nothing
    * observed. */
   if ((unsigned)next != (unsigned)*phase + 1u) {
      *reason = next <= *phase ? "phase advance does not move forward"
                               : "phase advance skips a state";
      return false;
   }
   *phase = next;
   return true;
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
   if (p->operation_id == R300_OPERATION_ID_NONE ||
       (unsigned)p->operation_id >= R300_OPERATION_ID_COUNT) {
      *reason = "provenance names no operation";
      return false;
   }
   if (r3v_execution_phase_name(p->phase) == NULL) {
      *reason = "provenance phase outside the execution ladder";
      return false;
   }
   /* The executor is bounded before it is read.  Everything below asks
    * whether it is GPU, so a third value would fall to the host branch and
    * be compared against nothing; the unit and the maturity need no bound
    * of their own, because a record naming a route is compared field for
    * field against its row and one naming none is held to the ledger's zero
    * values, and an out-of-range value fails both. */
   if ((unsigned)p->executor > R300_OPERATION_ROUTE_EXECUTOR_GPU) {
      *reason = "provenance executor outside the executor enum";
      return false;
   }

   const bool gpu = p->executor == R300_OPERATION_ROUTE_EXECUTOR_GPU;
   const bool executing = p->route_state == R300_OPERATION_ROUTE_EXECUTING;
   const bool entered_kernel =
      p->phase >= R3V_EXECUTION_PHASE_IOCTL_ENTERED;

   /* A record that names a route is held to that route's own row.  The
    * ledger owns the operation, unit, executor, and maturity a route id
    * stands for, so a record carrying its own values for them would let a
    * fabricated identity read as a delivery the ledger never describes. */
   if (p->route_id != R300_OPERATION_ROUTE_NONE) {
      const struct r300_operation_route_row *row =
         r300_operation_route(p->route_id);
      if (row == NULL) {
         *reason = "provenance names a route identity the ledger lacks";
         return false;
      }
      if (row->operation_id != p->operation_id) {
         *reason = "provenance operation disagrees with its route row";
         return false;
      }
      if (row->executor != p->executor) {
         *reason = "provenance executor disagrees with its route row";
         return false;
      }
      if (row->unit != p->unit) {
         *reason = "provenance unit disagrees with its route row";
         return false;
      }
      if (row->state != p->route_state) {
         *reason = "provenance maturity disagrees with its route row";
         return false;
      }
   } else {
      /* A record that names no route makes no claim a row could back, so it
       * carries the ledger's zero values for the fields a row owns: the
       * host unit and the unpromoted maturity.  A device unit or a
       * promotion here would assert what no route row assigns it. */
      if (p->unit != R300_EXECUTION_UNIT_HOST) {
         *reason = "provenance names no route yet claims a device unit";
         return false;
      }
      if (p->route_state != R300_OPERATION_ROUTE_CANDIDATE) {
         *reason = "provenance names no route yet claims a maturity";
         return false;
      }
   }

   /* The submission flag and the phase state one fact, so they agree in
    * both directions: a record past the ioctl entry made a submission, and
    * a record short of it made none. */
   if (p->device_submission != entered_kernel) {
      *reason = entered_kernel
                   ? "provenance entered the ioctl without a submission"
                   : "provenance reports a submission before the ioctl";
      return false;
   }

   /* A host route names no route row, so its identity, unit, and state are
    * the ledger's zero values and only the executor, the semantic node, and
    * the phase carry meaning. */
   if (!gpu) {
      if (!p->host_semantic_node) {
         *reason = "host route computed nothing";
         return false;
      }
      if (p->phase > R3V_EXECUTION_PHASE_HOST_TERMINAL) {
         *reason = "host route reports a phase past its terminal state";
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

   /* An experimental admission and a route that has not been promoted imply
    * each other, in both directions: a promoted route reporting itself
    * experimental would understate what it claims, and a precommitted route
    * reporting itself promoted would overstate it. */
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
      /* The submission itself is not asked here.  device_submission and the
       * phase already agree above, so a record past the ioctl carries the
       * submission and one short of it carries none; demanding the flag at
       * every phase refused the prepared record this policy exists to
       * admit.  GPU_ONLY's own content is the two facts above, and the
       * refusal that replaces a host fallback lives at the route decision.
       */
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

bool
r3v_route_automatic_selection_admitted_in(
   const enum r300_operation_route_id *admitted, uint32_t count,
   enum r300_operation_route_id id)
{
   if (admitted == NULL)
      return false;
   for (uint32_t i = 0; i < count; i++) {
      if (admitted[i] == id)
         return true;
   }
   return false;
}

/* The admitted set is empty: no crossover has been measured for any route,
 * so AUTO takes none of them without an explicit request. */
static const enum r300_operation_route_id automatic_selection_admitted[1] = {
   R300_OPERATION_ROUTE_NONE,
};

bool
r3v_route_automatic_selection_admitted(enum r300_operation_route_id id)
{
   if (id == R300_OPERATION_ROUTE_NONE)
      return false;
   return r3v_route_automatic_selection_admitted_in(
      automatic_selection_admitted,
      ARRAY_SIZE(automatic_selection_admitted), id);
}

/* One defined purpose, the same rule r300_operation_select_route holds.  A
 * request performs one operation for one purpose; a mask spanning two would
 * let a scan match on whichever bit a row happens to carry. */
static bool
one_defined_use(enum r300_operation_route_use use)
{
   const uint32_t bits = (uint32_t)use;
   return bits != 0 && (bits & (bits - 1)) == 0 &&
          (bits & ~(uint32_t)R300_ROUTE_USE_ALL) == 0;
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
   /* The operation is the join into the route catalog, so a request naming
    * none or naming one outside it reaches no row on either executor and
    * describes no semantic the host could run either. */
   if (request->operation_id == R300_OPERATION_ID_NONE ||
       (unsigned)request->operation_id >= R300_OPERATION_ID_COUNT) {
      *reason = "route request names no catalog operation";
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
   /* The use check stands ahead of both scans below.  The promoted selector
    * holds this rule itself, and the precommitted scan tests one bit of the
    * mask against a row's own mask, so a request naming two purposes would
    * pass the second scan on either bit while the first refused it. */
   if (!one_defined_use(request->use)) {
      *reason = "route request names other than one defined use";
      return R3V_ROUTE_DECISION_REFUSE;
   }
   /* The range is a whole number of elements starting on an element
    * boundary.  A route carries elements rather than loose bytes, so a
    * request whose range cannot be counted in them describes no operation
    * for either executor and refuses rather than falling to the host. */
   if (request->element_bytes == 0) {
      *reason = "route request names a zero element width";
      return R3V_ROUTE_DECISION_REFUSE;
   }
   if (request->byte_size % request->element_bytes != 0 ||
       request->byte_offset % request->element_bytes != 0) {
      *reason = "route request names a range outside its element grid";
      return R3V_ROUTE_DECISION_REFUSE;
   }
   /* The two reach flags say which executors can touch the destination.  A
    * host decision needs the host's own mapping, so a destination neither
    * executor reaches has no answer at all and refuses here rather than
    * naming an executor that cannot write it. */
   if (!request->destination_host_mapped &&
       !request->destination_device_visible) {
      *reason = "neither executor reaches the destination";
      return R3V_ROUTE_DECISION_REFUSE;
   }
   if (request->policy == R3V_EXECUTION_CPU_REFERENCE) {
      if (!request->destination_host_mapped) {
         *reason = "cpu_reference: the destination is not host mapped";
         return R3V_ROUTE_DECISION_REFUSE;
      }
      *reason = "cpu_reference policy";
      return R3V_ROUTE_DECISION_HOST;
   }

   /* A device route writes through the device's own view of the
    * destination, so a destination the device cannot reach disqualifies
    * every GPU row before the ledger is consulted.  Under GPU_ONLY that is
    * the refusal the policy exists to produce; under AUTO the host path
    * carries the operation. */
   if (!request->destination_device_visible) {
      if (request->policy == R3V_EXECUTION_GPU_ONLY) {
         *reason = "gpu_only: the destination is not device visible";
         return R3V_ROUTE_DECISION_REFUSE;
      }
      *reason = "the destination is not device visible; the host path "
                "carries it";
      return R3V_ROUTE_DECISION_HOST;
   }
   /* A device-visible destination the host cannot map keeps the host path
    * closed, so an AUTO request that finds no qualified GPU route below has
    * nowhere to fall. */

   /* A promoted route first: the selector reaches EXECUTING rows only, and
    * one that answers is the route this operation takes with no
    * experimental admission behind it. */
   const struct r300_operation_route_row *promoted =
      r300_operation_select_route(request->operation_id,
                                  R300_OPERATION_ROUTE_EXECUTOR_GPU,
                                  request->use, gate_state, reason);
   if (promoted != NULL) {
      /* An ungated promoted route under AUTO is the one decision nobody
       * named: the ledger says the route delivers, and the caller asked
       * for the fastest executor rather than for this one.  Automatic
       * selection is the separate fact that answers it, and it is withheld
       * until a crossover measures where the device wins.  A gate the
       * operator opened and a GPU_ONLY the caller wrote are both explicit,
       * so neither consults it.
       */
      if (request->policy == R3V_EXECUTION_AUTO && promoted->gate == NULL &&
          !r3v_route_automatic_selection_admitted(promoted->route_id)) {
         if (!request->destination_host_mapped) {
            *reason = "automatic selection is withheld and the destination "
                      "is not host mapped";
            return R3V_ROUTE_DECISION_REFUSE;
         }
         *reason = "automatic selection for this route is withheld until a "
                   "crossover is measured; the host path carries it";
         return R3V_ROUTE_DECISION_HOST;
      }
      *route = promoted;
      return R3V_ROUTE_DECISION_GPU;
   }

   /* Then a precommitted route under its own cached gate.  The row carries
    * an implementation, a route contract, and an admission contract; what
    * it lacks is a current-epoch receipt, so it runs only where an operator
    * asked for it by name at device creation. */
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
      if (gate_state == NULL || !gate_state[r->route_id])
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

   if (!request->destination_host_mapped) {
      *reason = "no qualified GPU route and the destination is not host "
                "mapped";
      return R3V_ROUTE_DECISION_REFUSE;
   }
   *reason = "no qualified GPU route; the host path is the default";
   return R3V_ROUTE_DECISION_HOST;
}
