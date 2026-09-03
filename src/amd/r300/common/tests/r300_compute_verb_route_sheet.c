/*
 * SPDX-License-Identifier: MIT
 *
 * Render the compute-verb ledger as a route sheet for external tooling and
 * for a reader deciding what executes.  r300_operation_registry_export
 * carries the operation join surface and excludes route state by design, so
 * no export reaches the ledger's route and evidence columns and a consumer
 * asking what executes reads the C table or nothing.  This sheet is that
 * export.
 *
 * Two properties carry it.  Evidence strength prints beside its scope in
 * every row, so a SILICON_RETAINED token never stands alone: fourteen rows
 * carry that strength and thirteen of them reach a raster cell rather than a
 * route.  And the emitter refuses a row it cannot name in full, so a field
 * outside its enum ends the sheet instead of printing an empty column that
 * reads as an absent property.
 *
 * operation_id is the join key: r300_numeric_domain.h holds the operation
 * strings to diagnostics and forbids their use as joins, and the names do
 * diverge (verb const_fill carries operation CONSTFILL).
 */

#include "r300_compute_verb.h"

#include <stdio.h>

/* Route status leads; evidence strength and scope arrive together and last. */
static const char *const HEADER =
   "schema_version\tverb\toperation_id\tcpu_route\tgpu_route\t"
   "implementation_id\tcontract_id\tunit\texactness\ttolerance\tevidence\t"
   "evidence_scope\tgate\n";

static const char *
exactness_name(enum r300_compute_verb_exactness e)
{
   switch (e) {
   case R300_COMPUTE_VERB_BIT_EXACT:
      return "bit_exact";
   case R300_COMPUTE_VERB_FP24_EXACT_WINDOW:
      return "fp24_exact_window";
   case R300_COMPUTE_VERB_FP24_BOUNDED:
      return "fp24_bounded";
   }
   return NULL;
}

int
main(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);
   const char *reason = NULL;

   /* A sheet renders the ledger the checker admits, never a table the
    * checker would refuse. */
   if (rows == NULL || count == 0 ||
       !r300_compute_verb_rows_valid(rows, count, &reason)) {
      fprintf(stderr, "route-sheet: ledger invalid: %s\n",
              reason != NULL ? reason : "no rows");
      return 1;
   }

   if (printf("%s", HEADER) < 0)
      return 1;

   for (uint32_t i = 0; i < count; i++) {
      const struct r300_compute_verb_row *row = &rows[i];
      const char *cpu = r300_compute_verb_route_status_name(row->cpu_route);
      const char *gpu = r300_compute_verb_route_status_name(row->gpu_route);
      const char *unit = r300_compute_verb_unit_name(row->unit);
      const char *exact = exactness_name(row->exactness);
      const char *evidence = r300_compute_verb_evidence_name(row->evidence);
      const char *scope =
         r300_compute_verb_evidence_scope_name(row->evidence_scope);

      if (row->name == NULL || cpu == NULL || gpu == NULL || unit == NULL ||
          exact == NULL || evidence == NULL || scope == NULL ||
          row->gpu_gate == NULL) {
         fprintf(stderr, "route-sheet: row %u has an unnameable field\n", i);
         return 1;
      }

      if (printf("1\t%s\t%u\t%s\t%s\t%u\t%u\t%s\t%s\t%.9g\t%s\t%s\t%s\n",
                 row->name, (unsigned)row->operation_id, cpu, gpu,
                 (unsigned)row->implementation_id,
                 (unsigned)row->gpu_route_contract_id, unit, exact,
                 (double)row->tolerance, evidence, scope, row->gpu_gate) < 0)
         return 1;
   }

   return fflush(stdout) == 0 && !ferror(stdout) ? 0 : 1;
}
