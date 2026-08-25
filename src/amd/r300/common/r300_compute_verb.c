/*
 * SPDX-License-Identifier: MIT
 *
 * Compute verb ledger: each row binds one operation to the pipeline unit that
 * realizes it, its exactness bound, and its route state.
 * docs/hardware/rs482-gpu-compute-substrate-atlas.md is the design authority
 * for a row's GPU route, holding the per-unit arithmetic and the probe each
 * row still owes.
 */

#include "r300_compute_verb.h"

#include <stddef.h>
#include <string.h>

#define ROW(v, n, op, impl, contract, idx, u, ex, tol, cpu, gpu, ev, scope,  \
            gate)                                                           \
   { R300_COMPUTE_VERB_##v, n, R300_OPERATION_ID_##op,                      \
     R300_OPERATION_IMPLEMENTATION_##impl,                                  \
     R300_GPU_ROUTE_CONTRACT_##contract, R300_GRID_INDEX_##idx,             \
     R300_COMPUTE_VERB_UNIT_##u, R300_COMPUTE_VERB_##ex, tol,               \
     R300_COMPUTE_VERB_ROUTE_##cpu, R300_COMPUTE_VERB_ROUTE_##gpu,          \
     R300_COMPUTE_VERB_EVIDENCE_##ev,                                       \
     R300_COMPUTE_VERB_EVIDENCE_SCOPE_##scope, gate }

/* The identity map's GPU route executes on the R2VB producer carrier
 * (r300_compute_identity_carrier.h) under its exact gate, whose US
 * datapath narrows to FP24: the route promises the FP24 exact window
 * and refuses outside it, while the CPU route moves every 32-bit
 * pattern.  The transcendental tolerances are the bounds
 * the retained RS482 deliveries held (smooth-monotone functions near
 * 1e-3, range-reduced sin and cos near 2e-2, pow and div near 1e-3).
 */
static const struct r300_compute_verb_row rows[R300_COMPUTE_VERB_COUNT] = {
   ROW(IDENTITY_MAP, "identity_map", IDENTITY_MAP,
       R2VB_FETCHED_IDENTITY_CARRIER, R2VB_COMPUTE_IDENTITY_CARRIER,
       LINEAR, R2VB_CARRIER, FP24_EXACT_WINDOW, 0.0f,
       EXECUTING, EXECUTING, SILICON_RETAINED, NATIVE_GPU_ROUTE_CELL,
       "R3V_NATIVE_COMPUTE_IDENTITY_GPU_EXPERIMENTAL"),
   ROW(CONST_FILL, "const_fill", CONSTFILL, NONE, NONE, NONE, RB3D_CLEAR,
       BIT_EXACT, 0.0f, ABSENT, ABSENT,
       SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_CONST_FILL_GPU_EXPERIMENTAL"),
   ROW(UNARY_AFFINE_MAP, "unary_affine_map", UNARY_AFFINE_MAP,
       NONE, NONE, NONE,
       US_FP24_ALU, FP24_EXACT_WINDOW,
       0.0f, ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_AFFINE_GPU_EXPERIMENTAL"),
   ROW(BINARY_ARITHMETIC_MAP, "binary_arithmetic_map", BINARY_MAP,
       NONE, NONE, NONE, US_FP24_ALU,
       FP24_EXACT_WINDOW, 0.0f, ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_BINARY_GPU_EXPERIMENTAL"),
   ROW(UNARY_TRANSCENDENTAL_MAP, "unary_transcendental_map",
       UNARY_TRANSCENDENTAL_MAP,
       NONE, NONE, NONE, US_FP24_ALU,
       FP24_BOUNDED, 0.03f, ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_UNARY_TRANSCENDENTAL_GPU_EXPERIMENTAL"),
   ROW(BINARY_TRANSCENDENTAL_MAP, "binary_transcendental_map",
       BINARY_TRANSCENDENTAL_MAP,
       NONE, NONE, NONE, US_FP24_ALU,
       FP24_BOUNDED, 0.03f, ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_BINARY_TRANSCENDENTAL_GPU_EXPERIMENTAL"),
   ROW(BITWISE_LOGICOP_MAP, "bitwise_logicop_map", BITWISE_LOGICOP_MAP,
       NONE, NONE, NONE,
       RB3D_ROP, BIT_EXACT, 0.0f,
       ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_BITWISE_GPU_EXPERIMENTAL"),
   ROW(MULTITAP_GATHER, "multitap_gather", MULTITAP_GATHER, NONE, NONE,
       COORD, US_FP24_ALU, FP24_EXACT_WINDOW,
       0.0f, ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_MULTITAP_GPU_EXPERIMENTAL"),
   ROW(PREDICATED_STORE, "predicated_store", PREDICATED_MASKED_STORE,
       NONE, NONE, NONE, TX_RB3D_COPY, BIT_EXACT, 0.0f,
       ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_PREDICATED_GPU_EXPERIMENTAL"),
   ROW(MULTIPASS_SCAN, "multipass_scan", MULTIPASS_PING_PONG_SCAN,
       NONE, NONE, NONE, US_FP24_ALU, FP24_EXACT_WINDOW, 0.0f,
       ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_MULTIPASS_GPU_EXPERIMENTAL"),
   ROW(REDUCE, "reduce", BLEND_ACC_REDUCTION, NONE, NONE, NONE, RB3D_BLEND,
       FP24_EXACT_WINDOW, 0.0f, ABSENT, ABSENT,
       SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_REDUCE_GPU_EXPERIMENTAL"),
   ROW(SATURATING_DIFF, "saturating_diff", SATURATING_DIFF, NONE, NONE,
       NONE, RB3D_BLEND, BIT_EXACT, 0.0f,
       ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_SATURATING_DIFF_GPU_EXPERIMENTAL"),
   ROW(PARALLEL_4OUT_MAP, "parallel_4out_map", PARALLEL_4OUT_MAP, NONE, NONE,
       NONE, US_FP24_ALU,
       FP24_EXACT_WINDOW, 0.0f, ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_PARALLEL_4OUT_GPU_EXPERIMENTAL"),
   ROW(STENCIL_INVERT, "stencil_invert", STENCIL_INVERT_NOT, NONE, NONE,
       NONE, ZB_STENCIL, BIT_EXACT, 0.0f,
       ABSENT, ABSENT, SILICON_RETAINED, RASTER_CELL,
       "R3V_NATIVE_COMPUTE_STENCIL_INVERT_GPU_EXPERIMENTAL"),
   /* The word complement is exact in every bit, so the host executor
    * realizes it outright and the row's evidence is that executor.
    * The ROP INVERT carrier the RB3D ROP domain names still awaits its
    * truth-table probe, so the GPU route stays absent and the unit is
    * the host.
    */
   ROW(BITWISE_NOT_MAP, "bitwise_not_map", BITWISE_NOT_MAP, NONE, NONE,
       LINEAR, HOST, BIT_EXACT, 0.0f,
       EXECUTING, ABSENT, HOST, HOST_EXECUTOR,
       "R3V_NATIVE_COMPUTE_BITWISE_NOT_GPU_EXPERIMENTAL"),
};

#undef ROW

bool
r300_compute_verb_queue_conformant_rows(
   const struct r300_compute_verb_row *table, uint32_t count)
{
   if (count == 0)
      return false;
   for (uint32_t v = 0; v < count; v++) {
      if (table[v].cpu_route != R300_COMPUTE_VERB_ROUTE_EXECUTING ||
          table[v].gpu_route != R300_COMPUTE_VERB_ROUTE_EXECUTING)
         return false;
   }
   return true;
}

bool
r300_compute_verb_queue_claim_rows(
   const struct r300_compute_verb_row *table, uint32_t count, bool gate_open)
{
   if (r300_compute_verb_queue_conformant_rows(table, count))
      return true;
   if (!gate_open)
      return false;
   for (uint32_t v = 0; v < count; v++) {
      if (table[v].cpu_route == R300_COMPUTE_VERB_ROUTE_EXECUTING)
         return true;
   }
   return false;
}

bool
r300_compute_verb_queue_conformant(void)
{
   return r300_compute_verb_queue_conformant_rows(rows,
                                                  R300_COMPUTE_VERB_COUNT);
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
      if (row->name == NULL || row->name[0] == '\0' || row->gpu_gate == NULL ||
          row->gpu_gate[0] == '\0') {
         *reason = "row without a name or a gate";
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
      const struct r300_virtual_op_info *operation =
         r300_virtual_op_info_for_id(row->operation_id);
      if (operation == NULL) {
         *reason = "operation identity does not resolve";
         return false;
      }
      if ((unsigned)row->implementation_id >=
          R300_OPERATION_IMPLEMENTATION_COUNT) {
         *reason = "implementation identity outside the implementation enum";
         return false;
      }
      if ((unsigned)row->gpu_route_contract_id >=
          R300_GPU_ROUTE_CONTRACT_COUNT) {
         *reason = "route contract outside the route-contract enum";
         return false;
      }
      const bool has_implementation =
         row->implementation_id != R300_OPERATION_IMPLEMENTATION_NONE;
      const bool has_contract =
         row->gpu_route_contract_id != R300_GPU_ROUTE_CONTRACT_NONE;
      if (has_implementation != has_contract) {
         *reason = "implementation and route contract are partial";
         return false;
      }
      if ((unsigned)row->cpu_route > R300_COMPUTE_VERB_ROUTE_EXECUTING ||
          (unsigned)row->gpu_route > R300_COMPUTE_VERB_ROUTE_EXECUTING) {
         *reason = "route status outside the route-status enum";
         return false;
      }
      if (row->gpu_route == R300_COMPUTE_VERB_ROUTE_ABSENT &&
          has_implementation) {
         *reason = "absent GPU route carries an implementation contract";
         return false;
      }
      if (row->gpu_route != R300_COMPUTE_VERB_ROUTE_ABSENT &&
          !has_implementation) {
         *reason = "GPU route lacks an implementation contract";
         return false;
      }
      for (uint32_t j = 0; j < i; j++) {
         if (strcmp(table[j].name, row->name) == 0) {
            *reason = "two rows share one name";
            return false;
         }
         if (strcmp(table[j].gpu_gate, row->gpu_gate) == 0) {
            *reason = "two rows share one gate";
            return false;
         }
      }
      if ((unsigned)row->index_class > R300_GRID_INDEX_STRIDED) {
         *reason = "index class outside the grid-fold enum";
         return false;
      }
      if ((unsigned)row->unit > R300_COMPUTE_VERB_UNIT_RB3D_CLEAR) {
         *reason = "unit outside the compute-verb enum";
         return false;
      }
      if ((unsigned)row->exactness > R300_COMPUTE_VERB_FP24_BOUNDED) {
         *reason = "exactness outside the exactness enum";
         return false;
      }
      const bool bounded = row->exactness == R300_COMPUTE_VERB_FP24_BOUNDED;
      if (bounded ? !(row->tolerance > 0.0f) : row->tolerance != 0.0f) {
         *reason = "tolerance disagrees with the exactness class";
         return false;
      }
      if ((unsigned)row->evidence >
          R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED) {
         *reason = "evidence outside the evidence enum";
         return false;
      }
      if ((unsigned)row->evidence_scope >
          R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL) {
         *reason = "evidence scope outside the evidence-scope enum";
         return false;
      }
      const bool evidence_scope_matches =
         (row->evidence == R300_COMPUTE_VERB_EVIDENCE_HOST &&
          row->evidence_scope ==
             R300_COMPUTE_VERB_EVIDENCE_SCOPE_HOST_EXECUTOR) ||
         (row->evidence == R300_COMPUTE_VERB_EVIDENCE_SOURCE_GROUNDED &&
          row->evidence_scope ==
             R300_COMPUTE_VERB_EVIDENCE_SCOPE_UNIT_CONTRACT) ||
         (row->evidence == R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED &&
          (row->evidence_scope ==
              R300_COMPUTE_VERB_EVIDENCE_SCOPE_RASTER_CELL ||
           row->evidence_scope ==
              R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL));
      if (!evidence_scope_matches) {
         *reason = "evidence strength and scope disagree";
         return false;
      }
      if (row->evidence_scope ==
             R300_COMPUTE_VERB_EVIDENCE_SCOPE_RASTER_CELL &&
          row->gpu_route == R300_COMPUTE_VERB_ROUTE_EXECUTING) {
         *reason = "raster-cell evidence attached to an executing GPU route";
         return false;
      }
      if (row->evidence_scope ==
             R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL &&
          row->gpu_route == R300_COMPUTE_VERB_ROUTE_ABSENT) {
         *reason = "native-route-cell evidence without a proposed GPU route";
         return false;
      }
      if (row->gpu_route == R300_COMPUTE_VERB_ROUTE_EXECUTING &&
          (row->evidence != R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED ||
           row->evidence_scope !=
              R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL)) {
         *reason = "executing GPU route lacks retained native-route-cell evidence";
         return false;
      }
      if (row->gpu_route == R300_COMPUTE_VERB_ROUTE_EXECUTING &&
          operation->status != R300_VOP_HW_CONFIRMED &&
          operation->status != R300_VOP_BOUNDARY) {
         *reason = "executing GPU route has an inadmissible catalog status";
         return false;
      }
      if (has_implementation &&
          (row->operation_id != R300_OPERATION_ID_IDENTITY_MAP ||
           row->implementation_id !=
              R300_OPERATION_IMPLEMENTATION_R2VB_FETCHED_IDENTITY_CARRIER ||
           row->gpu_route_contract_id !=
              R300_GPU_ROUTE_CONTRACT_R2VB_COMPUTE_IDENTITY_CARRIER)) {
         *reason = "operation and implementation contract disagree";
         return false;
      }
      if (row->unit == R300_COMPUTE_VERB_UNIT_HOST &&
          row->gpu_route != R300_COMPUTE_VERB_ROUTE_ABSENT) {
         *reason = "host unit with a GPU route";
         return false;
      }
      if (has_implementation &&
          (row->verb != R300_COMPUTE_VERB_IDENTITY_MAP ||
           row->index_class != R300_GRID_INDEX_LINEAR ||
           row->unit != R300_COMPUTE_VERB_UNIT_R2VB_CARRIER ||
           row->exactness != R300_COMPUTE_VERB_FP24_EXACT_WINDOW ||
           row->cpu_route != R300_COMPUTE_VERB_ROUTE_EXECUTING)) {
         *reason = "implementation contract and verb shape disagree";
         return false;
      }
      if (row->unit != R300_COMPUTE_VERB_UNIT_HOST &&
          row->gpu_route == R300_COMPUTE_VERB_ROUTE_ABSENT &&
          row->cpu_route == R300_COMPUTE_VERB_ROUTE_ABSENT &&
          row->evidence == R300_COMPUTE_VERB_EVIDENCE_HOST) {
         *reason = "unit row with no route and no evidence";
         return false;
      }
   }
   *reason = NULL;
   return true;
}
