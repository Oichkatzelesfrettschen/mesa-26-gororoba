/*
 * SPDX-License-Identifier: MIT
 *
 * Compute verb ledger: each row binds one kernel shape the compute grammar
 * admits to the catalog operation it realizes.  Where and how that operation
 * runs is the route ledger's fact (r300_operation_route.h), joined by
 * operation identity.
 * docs/hardware/rs482-gpu-compute-substrate-atlas.md is the design authority
 * for a row's GPU route, holding the per-unit arithmetic and the probe each
 * row still owes.
 */

#include "r300_compute_verb.h"
#include "r300_operation_route.h"

#include <stddef.h>
#include <string.h>

#define ROW(v, n, op) \
   { R300_COMPUTE_VERB_##v, n, R300_OPERATION_ID_##op }

/* One row per kernel shape the direct SPIR-V admitter recognizes.  The
 * operation identity is the join into r300_operation_route.h, which holds
 * every route that realizes it.
 */
static const struct r300_compute_verb_row rows[R300_COMPUTE_VERB_COUNT] = {
   ROW(IDENTITY_MAP, "identity_map", IDENTITY_MAP),
   ROW(CONST_FILL, "const_fill", CONSTFILL),
   ROW(UNARY_AFFINE_MAP, "unary_affine_map", UNARY_AFFINE_MAP),
   ROW(BINARY_ARITHMETIC_MAP, "binary_arithmetic_map", BINARY_MAP),
   ROW(UNARY_TRANSCENDENTAL_MAP, "unary_transcendental_map",
       UNARY_TRANSCENDENTAL_MAP),
   ROW(BINARY_TRANSCENDENTAL_MAP, "binary_transcendental_map",
       BINARY_TRANSCENDENTAL_MAP),
   ROW(BITWISE_LOGICOP_MAP, "bitwise_logicop_map", BITWISE_LOGICOP_MAP),
   ROW(MULTITAP_GATHER, "multitap_gather", MULTITAP_GATHER),
   ROW(PREDICATED_STORE, "predicated_store", PREDICATED_MASKED_STORE),
   ROW(MULTIPASS_SCAN, "multipass_scan", MULTIPASS_PING_PONG_SCAN),
   ROW(REDUCE, "reduce", BLEND_ACC_REDUCTION),
   ROW(SATURATING_DIFF, "saturating_diff", SATURATING_DIFF),
   ROW(PARALLEL_4OUT_MAP, "parallel_4out_map", PARALLEL_4OUT_MAP),
   ROW(STENCIL_INVERT, "stencil_invert", STENCIL_INVERT_NOT),
   ROW(BITWISE_NOT_MAP, "bitwise_not_map", BITWISE_NOT_MAP),
};

#undef ROW

/* Both queue predicates project over the route ledger.  A verb's routes are
 * found through its operation identity, so a verb whose operation gains a
 * route moves these answers without the verb table changing at all. */
bool
r300_compute_dual_route_coverage_complete_rows(
   const struct r300_compute_verb_row *table, uint32_t count)
{
   if (count == 0)
      return false;
   for (uint32_t v = 0; v < count; v++) {
      if (!r300_operation_has_executing_route(
             table[v].operation_id, R300_OPERATION_ROUTE_EXECUTOR_HOST) ||
          !r300_operation_has_executing_route(
             table[v].operation_id, R300_OPERATION_ROUTE_EXECUTOR_GPU))
         return false;
   }
   return true;
}

bool
r300_compute_verb_queue_claim_rows(
   const struct r300_compute_verb_row *table, uint32_t count, bool gate_open)
{
   if (r300_compute_dual_route_coverage_complete_rows(table, count))
      return true;
   if (!gate_open)
      return false;
   for (uint32_t v = 0; v < count; v++) {
      if (r300_operation_has_executing_route(
             table[v].operation_id, R300_OPERATION_ROUTE_EXECUTOR_HOST))
         return true;
   }
   return false;
}

bool
r300_compute_dual_route_coverage_complete(void)
{
   return r300_compute_dual_route_coverage_complete_rows(
      rows, R300_COMPUTE_VERB_COUNT);
}

bool
r300_compute_verb_queue_claim(bool gate_open)
{
   return r300_compute_verb_queue_claim_rows(rows, R300_COMPUTE_VERB_COUNT,
                                             gate_open);
}

const struct r300_compute_verb_row *
r300_compute_verb_rows(uint32_t *count)
{
   *count = R300_COMPUTE_VERB_COUNT;
   return rows;
}

const struct r300_compute_verb_row *
r300_compute_verb_row(enum r300_compute_verb verb)
{
   if ((unsigned)verb >= R300_COMPUTE_VERB_COUNT)
      return NULL;
   return &rows[verb];
}

const struct r300_compute_verb_row *
r300_compute_verb_for_job(const struct r300_compute_job *job)
{
   if (job == NULL)
      return NULL;
   switch ((enum r300_compute_job_op)job->op) {
   case R300_COMPUTE_JOB_OP_IDENTITY:
      return &rows[R300_COMPUTE_VERB_IDENTITY_MAP];
   case R300_COMPUTE_JOB_OP_BITWISE_NOT:
      return &rows[R300_COMPUTE_VERB_BITWISE_NOT_MAP];
   }
   return NULL;
}

const char *
r300_compute_refusal_class_name(enum r300_compute_refusal_class c)
{
   static const char *const names[R300_COMPUTE_REFUSAL_CLASS_COUNT] = {
      "arbitrary_scatter",
      "image_store",
      "shared_memory_or_barrier",
      "general_atomic",
      "integer_shift",
   };
   return (unsigned)c < R300_COMPUTE_REFUSAL_CLASS_COUNT ? names[c] : NULL;
}

const char *
r300_compute_failure_clause_name(enum r300_compute_failure_clause c)
{
   static const char *const names[R300_COMPUTE_FAILURE_CLAUSE_COUNT] = {
      "refuse_at_admission",
      "exact_gate_per_verb",
      "no_fallback_after_submit",
      "refuse_before_write",
      "oracle_divergence_quarantines",
      "advertise_after_silicon",
   };
   return (unsigned)c < R300_COMPUTE_FAILURE_CLAUSE_COUNT ? names[c] : NULL;
}

bool
r300_compute_verb_rows_valid(const struct r300_compute_verb_row *table,
                             uint32_t count, const char **reason)
{
   if (table == NULL || count != R300_COMPUTE_VERB_COUNT) {
      *reason = "row count outside the verb enum";
      return false;
   }
   for (uint32_t i = 0; i < count; i++) {
      const struct r300_compute_verb_row *row = &table[i];
      if (row->verb != (enum r300_compute_verb)i) {
         *reason = "rows out of enum order";
         return false;
      }
      if (row->name == NULL || row->name[0] == '\0') {
         *reason = "row without a name";
         return false;
      }
      if (row->operation_id == R300_OPERATION_ID_NONE) {
         *reason = "compute verb lacks an operation identity";
         return false;
      }
      if ((unsigned)row->operation_id >= R300_OPERATION_ID_COUNT) {
         *reason = "operation identity outside the catalog enum";
         return false;
      }
      if (r300_virtual_op_info_for_id(row->operation_id) == NULL) {
         *reason = "operation identity does not resolve";
         return false;
      }
      for (uint32_t j = 0; j < i; j++) {
         if (strcmp(table[j].name, row->name) == 0) {
            *reason = "two rows share one name";
            return false;
         }
         if (table[j].operation_id == row->operation_id) {
            *reason = "two verbs share one operation identity";
            return false;
         }
      }
   }
   *reason = NULL;
   return true;
}
