/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_compute_verb.h"

#include <stddef.h>
#include <string.h>

#define ROW(v, n, cat, idx, u, ex, tol, cpu, gpu, ev, gate)                \
   { R300_COMPUTE_VERB_##v, n, cat, R300_GRID_INDEX_##idx,                 \
     R300_COMPUTE_VERB_UNIT_##u, R300_COMPUTE_VERB_##ex, tol,              \
     R300_COMPUTE_VERB_ROUTE_##cpu, R300_COMPUTE_VERB_ROUTE_##gpu,         \
     R300_COMPUTE_VERB_EVIDENCE_##ev, gate }

/* The identity map's GPU route executes on the R2VB producer carrier
 * (r300_compute_identity_carrier.h) under its exact gate, whose US
 * datapath narrows to FP24: the route promises the FP24 exact window
 * and refuses outside it, while the CPU route moves every 32-bit
 * pattern.  The transcendental tolerances are the bounds
 * the retained RS482 deliveries held (smooth-monotone functions near
 * 1e-3, range-reduced sin and cos near 2e-2, pow and div near 1e-3).
 */
static const struct r300_compute_verb_row rows[R300_COMPUTE_VERB_COUNT] = {
   ROW(IDENTITY_MAP, "identity_map", "IDENTITY_MAP", LINEAR, R2VB_CARRIER, FP24_EXACT_WINDOW, 0.0f,
       EXECUTING, EXECUTING, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_IDENTITY_GPU_EXPERIMENTAL"),
   ROW(CONST_FILL, "const_fill", "CONSTFILL", NONE, HOST, BIT_EXACT, 0.0f, ABSENT, ABSENT,
       SILICON_RETAINED, "R3V_NATIVE_COMPUTE_CONST_FILL_GPU_EXPERIMENTAL"),
   ROW(UNARY_AFFINE_MAP, "unary_affine_map", NULL, NONE, US_FP24_ALU, FP24_EXACT_WINDOW,
       0.0f, ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_AFFINE_GPU_EXPERIMENTAL"),
   ROW(BINARY_ARITHMETIC_MAP, "binary_arithmetic_map", "BINARY_MAP",
       NONE, US_FP24_ALU,
       FP24_EXACT_WINDOW, 0.0f, ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_BINARY_GPU_EXPERIMENTAL"),
   ROW(UNARY_TRANSCENDENTAL_MAP, "unary_transcendental_map", NULL, NONE, US_FP24_ALU,
       FP24_BOUNDED, 0.03f, ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_UNARY_TRANSCENDENTAL_GPU_EXPERIMENTAL"),
   ROW(BINARY_TRANSCENDENTAL_MAP, "binary_transcendental_map", NULL,
       NONE, US_FP24_ALU,
       FP24_BOUNDED, 0.03f, ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_BINARY_TRANSCENDENTAL_GPU_EXPERIMENTAL"),
   ROW(BITWISE_LOGICOP_MAP, "bitwise_logicop_map", NULL, NONE, RB3D_ROP, BIT_EXACT, 0.0f,
       ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_BITWISE_GPU_EXPERIMENTAL"),
   ROW(MULTITAP_GATHER, "multitap_gather", "MULTITAP_GATHER", COORD, TX_RB3D_COPY, FP24_EXACT_WINDOW,
       0.0f, ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_MULTITAP_GPU_EXPERIMENTAL"),
   ROW(PREDICATED_STORE, "predicated_store", "PREDICATED_MASKED_STORE",
       NONE, TX_RB3D_COPY, BIT_EXACT, 0.0f,
       ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_PREDICATED_GPU_EXPERIMENTAL"),
   ROW(MULTIPASS_SCAN, "multipass_scan", "MULTIPASS_PING_PONG_SCAN", NONE, US_FP24_ALU, FP24_EXACT_WINDOW, 0.0f,
       ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_MULTIPASS_GPU_EXPERIMENTAL"),
   ROW(REDUCE, "reduce", "BLEND_ACC_REDUCTION", NONE, RB3D_BLEND, FP24_EXACT_WINDOW, 0.0f, ABSENT, ABSENT,
       SILICON_RETAINED, "R3V_NATIVE_COMPUTE_REDUCE_GPU_EXPERIMENTAL"),
   ROW(SATURATING_DIFF, "saturating_diff", "SATURATING_DIFF", NONE, RB3D_BLEND, BIT_EXACT, 0.0f,
       ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_SATURATING_DIFF_GPU_EXPERIMENTAL"),
   ROW(PARALLEL_4OUT_MAP, "parallel_4out_map", "PARALLEL_4OUT_MAP", NONE, US_FP24_ALU,
       FP24_EXACT_WINDOW, 0.0f, ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_PARALLEL_4OUT_GPU_EXPERIMENTAL"),
   ROW(STENCIL_INVERT, "stencil_invert", "STENCIL_INVERT_NOT", NONE, ZB_STENCIL, BIT_EXACT, 0.0f,
       ABSENT, ABSENT, SILICON_RETAINED,
       "R3V_NATIVE_COMPUTE_STENCIL_INVERT_GPU_EXPERIMENTAL"),
};

#undef ROW

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
      const bool bounded = row->exactness == R300_COMPUTE_VERB_FP24_BOUNDED;
      if (bounded ? !(row->tolerance > 0.0f) : row->tolerance != 0.0f) {
         *reason = "tolerance disagrees with the exactness class";
         return false;
      }
      if (row->gpu_route == R300_COMPUTE_VERB_ROUTE_EXECUTING &&
          row->evidence != R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED) {
         *reason = "GPU route executing without retained silicon evidence";
         return false;
      }
      if (row->unit == R300_COMPUTE_VERB_UNIT_HOST &&
          row->gpu_route != R300_COMPUTE_VERB_ROUTE_ABSENT) {
         *reason = "host unit with a GPU route";
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
