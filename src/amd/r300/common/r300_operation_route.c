/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_operation_route.h"

#include "util/macros.h"

#include <string.h>

#define ROUTE(id, n, op, exec, st, u, impl, contract, adm, idx, ex, tol, ev,  \
              scope, gate)                                                    \
   { R300_OPERATION_ROUTE_##id, n, R300_OPERATION_ID_##op,                    \
     R300_OPERATION_ROUTE_EXECUTOR_##exec, R300_OPERATION_ROUTE_##st,         \
     R300_EXECUTION_UNIT_##u, R300_OPERATION_IMPLEMENTATION_##impl,           \
     R300_GPU_ROUTE_CONTRACT_##contract, R300_ROUTE_ADMISSION_##adm,          \
     R300_GRID_INDEX_##idx, R300_COMPUTE_VERB_##ex, tol,                      \
     R300_COMPUTE_VERB_EVIDENCE_##ev,                                         \
     R300_COMPUTE_VERB_EVIDENCE_SCOPE_##scope, gate }

/* One row per route, grouped by operation.  The identity map carries the
 * two routes whose contracts differ: the host route moves every admitted
 * 32-bit record bit-exactly, and the R2VB carrier promises the FP24 exact
 * window because the record travels the US varying datapath as FP24.  The
 * thirteen raster candidates name a unit and a bound with no implementation
 * behind them, and their retained evidence reaches the unit's behavior in a
 * raster cell rather than the route.  The ROP complement candidate is
 * source-grounded rather than retained: RB3D_ROPCNTL bits 8-11 encode
 * INVERT as code 5, and no truth-table probe has run.
 */
static const struct r300_operation_route_row
   routes[R300_OPERATION_ROUTE_COUNT - 1] = {
      ROUTE(HOST_IDENTITY_MAP, "host_identity_map", IDENTITY_MAP, HOST,
            EXECUTING, HOST, NONE, NONE, NONE, LINEAR, BIT_EXACT, 0.0f, HOST,
            HOST_EXECUTOR, NULL),
      ROUTE(R2VB_IDENTITY_MAP, "r2vb_identity_map", IDENTITY_MAP, GPU,
            EXECUTING, R2VB_CARRIER, R2VB_FETCHED_IDENTITY_CARRIER,
            R2VB_COMPUTE_IDENTITY_CARRIER, R2VB_FP24_IDENTITY, LINEAR,
            FP24_EXACT_WINDOW, 0.0f, SILICON_RETAINED, NATIVE_GPU_ROUTE_CELL,
            "R3V_NATIVE_COMPUTE_IDENTITY_GPU_EXPERIMENTAL"),

      ROUTE(RB3D_CLEAR_CONST_FILL, "rb3d_clear_const_fill", CONSTFILL, GPU,
            CANDIDATE, RB3D_CLEAR, NONE, NONE, NONE, NONE, BIT_EXACT, 0.0f,
            SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_CONST_FILL_GPU_EXPERIMENTAL"),
      ROUTE(US_FP24_UNARY_AFFINE, "us_fp24_unary_affine", UNARY_AFFINE_MAP,
            GPU, CANDIDATE, US_FP24_ALU, NONE, NONE, NONE, NONE,
            FP24_EXACT_WINDOW, 0.0f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_AFFINE_GPU_EXPERIMENTAL"),
      ROUTE(US_FP24_BINARY_ARITHMETIC, "us_fp24_binary_arithmetic", BINARY_MAP,
            GPU, CANDIDATE, US_FP24_ALU, NONE, NONE, NONE, NONE,
            FP24_EXACT_WINDOW, 0.0f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_BINARY_GPU_EXPERIMENTAL"),
      ROUTE(US_FP24_UNARY_TRANSCENDENTAL, "us_fp24_unary_transcendental",
            UNARY_TRANSCENDENTAL_MAP, GPU, CANDIDATE, US_FP24_ALU, NONE, NONE,
            NONE, NONE, FP24_BOUNDED, 0.03f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_UNARY_TRANSCENDENTAL_GPU_EXPERIMENTAL"),
      ROUTE(US_FP24_BINARY_TRANSCENDENTAL, "us_fp24_binary_transcendental",
            BINARY_TRANSCENDENTAL_MAP, GPU, CANDIDATE, US_FP24_ALU, NONE, NONE,
            NONE, NONE, FP24_BOUNDED, 0.03f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_BINARY_TRANSCENDENTAL_GPU_EXPERIMENTAL"),
      ROUTE(RB3D_ROP_BITWISE_LOGIC, "rb3d_rop_bitwise_logic",
            BITWISE_LOGICOP_MAP, GPU, CANDIDATE, RB3D_ROP, NONE, NONE, NONE,
            NONE, BIT_EXACT, 0.0f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_BITWISE_GPU_EXPERIMENTAL"),
      ROUTE(US_FP24_MULTITAP, "us_fp24_multitap", MULTITAP_GATHER, GPU,
            CANDIDATE, US_FP24_ALU, NONE, NONE, NONE, COORD,
            FP24_EXACT_WINDOW, 0.0f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_MULTITAP_GPU_EXPERIMENTAL"),
      ROUTE(TX_RB3D_PREDICATED_STORE, "tx_rb3d_predicated_store",
            PREDICATED_MASKED_STORE, GPU, CANDIDATE, TX_RB3D_COPY, NONE, NONE,
            NONE, NONE, BIT_EXACT, 0.0f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_PREDICATED_GPU_EXPERIMENTAL"),
      ROUTE(US_FP24_MULTIPASS_SCAN, "us_fp24_multipass_scan",
            MULTIPASS_PING_PONG_SCAN, GPU, CANDIDATE, US_FP24_ALU, NONE, NONE,
            NONE, NONE, FP24_EXACT_WINDOW, 0.0f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_MULTIPASS_GPU_EXPERIMENTAL"),
      ROUTE(RB3D_BLEND_REDUCE, "rb3d_blend_reduce", BLEND_ACC_REDUCTION, GPU,
            CANDIDATE, RB3D_BLEND, NONE, NONE, NONE, NONE, FP24_EXACT_WINDOW,
            0.0f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_REDUCE_GPU_EXPERIMENTAL"),
      ROUTE(RB3D_BLEND_SATURATING_DIFF, "rb3d_blend_saturating_diff",
            SATURATING_DIFF, GPU, CANDIDATE, RB3D_BLEND, NONE, NONE, NONE,
            NONE, BIT_EXACT, 0.0f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_SATURATING_DIFF_GPU_EXPERIMENTAL"),
      ROUTE(US_FP24_PARALLEL_4OUT, "us_fp24_parallel_4out",
            PARALLEL_4OUT_MAP, GPU, CANDIDATE, US_FP24_ALU, NONE, NONE, NONE,
            NONE, FP24_EXACT_WINDOW, 0.0f, SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_PARALLEL_4OUT_GPU_EXPERIMENTAL"),
      ROUTE(ZB_STENCIL_INVERT, "zb_stencil_invert", STENCIL_INVERT_NOT, GPU,
            CANDIDATE, ZB_STENCIL, NONE, NONE, NONE, NONE, BIT_EXACT, 0.0f,
            SILICON_RETAINED, RASTER_CELL,
            "R3V_NATIVE_COMPUTE_STENCIL_INVERT_GPU_EXPERIMENTAL"),

      ROUTE(HOST_BITWISE_NOT, "host_bitwise_not", BITWISE_NOT_MAP, HOST,
            EXECUTING, HOST, NONE, NONE, NONE, LINEAR, BIT_EXACT, 0.0f, HOST,
            HOST_EXECUTOR, NULL),
      ROUTE(RB3D_ROP_BITWISE_NOT, "rb3d_rop_bitwise_not", BITWISE_NOT_MAP, GPU,
            CANDIDATE, RB3D_ROP, NONE, NONE, NONE, LINEAR, BIT_EXACT, 0.0f,
            SOURCE_GROUNDED, UNIT_CONTRACT,
            "R3V_NATIVE_COMPUTE_BITWISE_NOT_GPU_EXPERIMENTAL"),
};

#undef ROUTE

const struct r300_operation_route_row *
r300_operation_route_rows(uint32_t *count)
{
   if (count != NULL)
      *count = ARRAY_SIZE(routes);
   return routes;
}

const struct r300_operation_route_row *
r300_operation_route(enum r300_operation_route_id route_id)
{
   STATIC_ASSERT(ARRAY_SIZE(routes) == R300_OPERATION_ROUTE_COUNT - 1);
   if ((unsigned)route_id <= R300_OPERATION_ROUTE_NONE ||
       (unsigned)route_id >= R300_OPERATION_ROUTE_COUNT)
      return NULL;
   /* NONE occupies index 0 of the enum and no row, so the row index trails
    * the identity by one. */
   return &routes[(unsigned)route_id - 1];
}

uint32_t
r300_operation_route_count_for_operation(enum r300_operation_id operation_id)
{
   uint32_t n = 0;
   for (uint32_t i = 0; i < ARRAY_SIZE(routes); i++)
      n += routes[i].operation_id == operation_id;
   return n;
}

bool
r300_operation_has_executing_route(enum r300_operation_id operation_id,
                                   enum r300_operation_route_executor executor)
{
   for (uint32_t i = 0; i < ARRAY_SIZE(routes); i++) {
      if (routes[i].operation_id == operation_id &&
          routes[i].executor == executor &&
          routes[i].state == R300_OPERATION_ROUTE_EXECUTING)
         return true;
   }
   return false;
}

const struct r300_operation_route_row *
r300_operation_select_route_rows(const struct r300_operation_route_row *t,
                                 uint32_t count,
                                 enum r300_operation_id operation_id,
                                 enum r300_operation_route_executor executor,
                                 const bool *gate_state, const char **reason)
{
   const struct r300_operation_route_row *chosen = NULL;

   for (uint32_t i = 0; i < count; i++) {
      const struct r300_operation_route_row *row = &t[i];
      if (row->operation_id != operation_id || row->executor != executor ||
          row->state != R300_OPERATION_ROUTE_EXECUTING)
         continue;
      /* A gated route runs only under its own gate; a route with no gate
       * is the executor's default path. */
      if (row->gate != NULL &&
          (gate_state == NULL || !gate_state[row->route_id]))
         continue;
      if (chosen != NULL) {
         /* Table order is not a route policy: two eligible routes mean the
          * selector that would separate them has not been written. */
         if (reason != NULL)
            *reason = "two eligible routes and no selector policy";
         return NULL;
      }
      chosen = row;
   }

   if (chosen == NULL && reason != NULL)
      *reason = "no eligible executing route";
   return chosen;
}

const struct r300_operation_route_row *
r300_operation_select_route(enum r300_operation_id operation_id,
                            enum r300_operation_route_executor executor,
                            const bool *gate_state, const char **reason)
{
   return r300_operation_select_route_rows(routes, ARRAY_SIZE(routes),
                                           operation_id, executor, gate_state,
                                           reason);
}

const char *
r300_operation_route_executor_name(enum r300_operation_route_executor e)
{
   static const char *const names[] = { "host", "gpu" };
   return (unsigned)e <= R300_OPERATION_ROUTE_EXECUTOR_GPU ? names[e] : NULL;
}

const char *
r300_operation_route_state_name(enum r300_operation_route_state s)
{
   static const char *const names[] = { "candidate", "precommitted",
                                        "executing" };
   return (unsigned)s <= R300_OPERATION_ROUTE_EXECUTING ? names[s] : NULL;
}

const char *
r300_execution_unit_name(enum r300_execution_unit u)
{
   static const char *const names[R300_EXECUTION_UNIT_COUNT] = {
      "host",         "tx_rb3d_copy", "us_fp24_alu", "rb3d_blend",
      "rb3d_rop",     "zb_stencil",   "r2vb_carrier", "rb3d_clear",
   };
   return (unsigned)u < R300_EXECUTION_UNIT_COUNT ? names[u] : NULL;
}

const char *
r300_compute_verb_evidence_name(enum r300_compute_verb_evidence e)
{
   static const char *const names[] = { "host", "source_grounded",
                                        "silicon_retained" };
   return (unsigned)e <= R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED ? names[e]
                                                                    : NULL;
}

const char *
r300_compute_verb_evidence_scope_name(enum r300_compute_verb_evidence_scope s)
{
   static const char *const names[] = { "host_executor", "unit_contract",
                                        "raster_cell",
                                        "native_gpu_route_cell" };
   return (unsigned)s <= R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL
             ? names[s]
             : NULL;
}

const char *
r300_compute_verb_exactness_name(enum r300_compute_verb_exactness e)
{
   static const char *const names[] = { "bit_exact", "fp24_exact_window",
                                        "fp24_bounded" };
   return (unsigned)e <= R300_COMPUTE_VERB_FP24_BOUNDED ? names[e] : NULL;
}

bool
r300_operation_route_rows_valid(const struct r300_operation_route_row *t,
                                uint32_t count, const char **reason)
{
   const char *ignored = NULL;
   if (reason == NULL)
      reason = &ignored;
   *reason = NULL;

   if (t == NULL || count == 0) {
      *reason = "route table is empty";
      return false;
   }

   for (uint32_t i = 0; i < count; i++) {
      const struct r300_operation_route_row *r = &t[i];
      const bool gpu = r->executor == R300_OPERATION_ROUTE_EXECUTOR_GPU;
      const bool contracted = r->implementation_id !=
                                 R300_OPERATION_IMPLEMENTATION_NONE &&
                              r->gpu_route_contract_id !=
                                 R300_GPU_ROUTE_CONTRACT_NONE &&
                              r->admission_id != R300_ROUTE_ADMISSION_NONE;

      if ((unsigned)r->route_id <= R300_OPERATION_ROUTE_NONE ||
          (unsigned)r->route_id >= R300_OPERATION_ROUTE_COUNT) {
         *reason = "route identity outside the route enum";
         return false;
      }
      if (r->name == NULL || r->name[0] == '\0') {
         *reason = "route lacks a diagnostic name";
         return false;
      }
      if (r->operation_id == R300_OPERATION_ID_NONE ||
          (unsigned)r->operation_id >= R300_OPERATION_ID_COUNT) {
         *reason = "route names no catalog operation";
         return false;
      }
      if (r300_virtual_op_info_for_id(r->operation_id) == NULL) {
         *reason = "route operation does not resolve in the catalog";
         return false;
      }
      if ((unsigned)r->executor > R300_OPERATION_ROUTE_EXECUTOR_GPU) {
         *reason = "route executor outside the executor enum";
         return false;
      }
      if ((unsigned)r->state > R300_OPERATION_ROUTE_EXECUTING) {
         *reason = "route state outside the state enum";
         return false;
      }
      if ((unsigned)r->unit >= R300_EXECUTION_UNIT_COUNT) {
         *reason = "route unit outside the execution-unit enum";
         return false;
      }
      if ((unsigned)r->exactness > R300_COMPUTE_VERB_FP24_BOUNDED) {
         *reason = "route exactness outside the exactness enum";
         return false;
      }
      if ((r->exactness == R300_COMPUTE_VERB_FP24_BOUNDED) !=
          (r->tolerance > 0.0f)) {
         *reason = "route tolerance disagrees with its exactness class";
         return false;
      }
      if (!(r->tolerance >= 0.0f) || r->tolerance >= 1.0f) {
         *reason = "route tolerance outside [0, 1)";
         return false;
      }
      if ((unsigned)r->evidence >
          R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED) {
         *reason = "route evidence outside the evidence enum";
         return false;
      }
      if ((unsigned)r->evidence_scope >
          R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL) {
         *reason = "route evidence scope outside the scope enum";
         return false;
      }

      /* Placement rules: a unit belongs to one executor, and so does the
       * gate.  The host path is the executor's default and takes no opt-in,
       * so a gate on a host route would name an opt-in nothing reads. */
      if (gpu && r->unit == R300_EXECUTION_UNIT_HOST) {
         *reason = "GPU route runs on the host unit";
         return false;
      }
      if (!gpu && r->unit != R300_EXECUTION_UNIT_HOST) {
         *reason = "host route runs on a device unit";
         return false;
      }
      if (!gpu && r->gate != NULL) {
         *reason = "host route carries a gate";
         return false;
      }

      /* Evidence subject follows the executor: HOST_EXECUTOR reaches the
       * CPU executor's tests, and an exact native route cell reaches a GPU
       * route.  Without this a GPU row left at the enum's zero values would
       * read as host-tested. */
      if (gpu && (r->evidence == R300_COMPUTE_VERB_EVIDENCE_HOST ||
                  r->evidence_scope ==
                     R300_COMPUTE_VERB_EVIDENCE_SCOPE_HOST_EXECUTOR)) {
         *reason = "GPU route claims host-executor evidence";
         return false;
      }
      if (!gpu && r->evidence_scope ==
                     R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL) {
         *reason = "host route claims native-GPU-route evidence";
         return false;
      }

      /* Maturity rules: the three contracts move together, an executing GPU
       * route names evidence for its own retained cell, and raster-cell
       * evidence reaches a unit rather than a running route. */
      if (r->state == R300_OPERATION_ROUTE_CANDIDATE && contracted) {
         *reason = "candidate route carries an implementation contract";
         return false;
      }
      if (r->state != R300_OPERATION_ROUTE_CANDIDATE && gpu && !contracted) {
         *reason = "committed GPU route lacks implementation, contract, or "
                   "admission";
         return false;
      }
      if (r->state == R300_OPERATION_ROUTE_EXECUTING && gpu &&
          !(r->evidence == R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED &&
            r->evidence_scope ==
               R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL)) {
         *reason = "executing GPU route lacks exact native-route evidence";
         return false;
      }
      if (r->state == R300_OPERATION_ROUTE_EXECUTING &&
          r->evidence_scope == R300_COMPUTE_VERB_EVIDENCE_SCOPE_RASTER_CELL) {
         *reason = "raster-cell evidence attached to an executing route";
         return false;
      }

      for (uint32_t j = 0; j < i; j++) {
         if (t[j].route_id == r->route_id) {
            *reason = "route table repeats a route identity";
            return false;
         }
         if (strcmp(t[j].name, r->name) == 0) {
            *reason = "route table repeats a diagnostic name";
            return false;
         }
         if (t[j].gate != NULL && r->gate != NULL &&
             strcmp(t[j].gate, r->gate) == 0) {
            *reason = "route table repeats a gate";
            return false;
         }
      }
   }

   /* r300_operation_route() reaches a row by subtracting one from the
    * identity, NONE holding index 0 of the enum and no row, so a row out of
    * identity order would resolve every later lookup to a different route
    * and bind a dispatch to contracts it never named.  The lookup's
    * arithmetic is the rule. */
   for (uint32_t i = 0; i < count; i++) {
      if (t[i].route_id != (enum r300_operation_route_id)(i + 1)) {
         *reason = "routes out of identity order";
         return false;
      }
   }

   /* One executor's eligible set must be decidable without table order.
    * Two executing routes for one operation on one executor would leave the
    * selector choosing by position, so the table refuses that shape here
    * rather than at the first dispatch that meets it. */
   for (uint32_t i = 0; i < count; i++) {
      if (t[i].state != R300_OPERATION_ROUTE_EXECUTING)
         continue;
      for (uint32_t j = 0; j < i; j++) {
         if (t[j].state == R300_OPERATION_ROUTE_EXECUTING &&
             t[j].operation_id == t[i].operation_id &&
             t[j].executor == t[i].executor) {
            *reason = "two executing routes for one operation and executor";
            return false;
         }
      }
   }

   return true;
}
