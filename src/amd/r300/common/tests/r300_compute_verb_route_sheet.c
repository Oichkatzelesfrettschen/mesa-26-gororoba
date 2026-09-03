/*
 * SPDX-License-Identifier: MIT
 *
 * Render the operation route ledger as a sheet for external tooling and for
 * a reader deciding what executes.  r300_operation_registry_export carries
 * the operation join surface and excludes route state by design, so no
 * export reaches the route and evidence columns and a consumer asking what
 * executes reads the C table or nothing.  This sheet is that export.
 *
 * Three properties carry it.  Route status leads, so what executes is read
 * before what was retained.  Evidence strength prints beside its scope in
 * every row, so a SILICON_RETAINED token never stands alone: fourteen rows
 * carry that strength and thirteen of them reach a raster cell rather than a
 * route.  And the emitter refuses a row it cannot name in full, so a field
 * outside its enum ends the sheet instead of printing an empty column that
 * reads as an absent property.
 *
 * operation_id is the join key: r300_numeric_domain.h holds the operation
 * strings to diagnostics and forbids their use as joins, and the names do
 * diverge (verb const_fill carries operation CONSTFILL).  One operation may
 * carry several routes separated by the uses they serve, so the route
 * identity is the sheet's own key and the operation is what joins it to the
 * verb and catalog exports.
 */

#include "r300_compute_verb.h"
#include "r300_operation_route.h"

#include <stdio.h>

static const char *const HEADER =
   "schema_version\troute_id\troute\toperation_id\texecutor\tstate\t"
   "unit\tuses\timplementation_id\tcontract_id\tadmission_id\t"
   "exactness\ttolerance\tevidence\tevidence_scope\tgate\n";

int
main(void)
{
   uint32_t count = 0;
   const struct r300_operation_route_row *rows =
      r300_operation_route_rows(&count);
   const char *reason = NULL;

   /* A sheet renders the ledger the checker admits, never a table the
    * checker would refuse. */
   if (rows == NULL || count == 0 ||
       !r300_operation_route_rows_valid(rows, count, &reason)) {
      fprintf(stderr, "route-sheet: ledger invalid: %s\n",
              reason != NULL ? reason : "no rows");
      return 1;
   }

   if (printf("%s", HEADER) < 0)
      return 1;

   for (uint32_t i = 0; i < count; i++) {
      const struct r300_operation_route_row *row = &rows[i];
      const char *executor =
         r300_operation_route_executor_name(row->executor);
      const char *state = r300_operation_route_state_name(row->state);
      const char *unit = r300_execution_unit_name(row->unit);
      const char *exact = r300_compute_verb_exactness_name(row->exactness);
      const char *evidence = r300_compute_verb_evidence_name(row->evidence);
      const char *scope =
         r300_compute_verb_evidence_scope_name(row->evidence_scope);
      /* Applicability is a route fact a consumer needs: two routes for one
       * operation are separated by the uses they serve, so a sheet without
       * the column would show CONSTFILL reaching two GPU routes with no
       * statement of which caller each answers. */
      char uses[96];
      r300_operation_route_use_names(row->uses, uses, sizeof(uses));

      if (row->name == NULL || executor == NULL || state == NULL ||
          unit == NULL || exact == NULL || evidence == NULL || scope == NULL) {
         fprintf(stderr, "route-sheet: row %u has an unnameable field\n", i);
         return 1;
      }

      if (printf("1\t%u\t%s\t%u\t%s\t%s\t%s\t%s\t%u\t%u\t%u\t%s\t%.9g"
                 "\t%s\t%s\t%s\n",
                 (unsigned)row->route_id, row->name,
                 (unsigned)row->operation_id, executor, state, unit, uses,
                 (unsigned)row->implementation_id,
                 (unsigned)row->gpu_route_contract_id,
                 (unsigned)row->admission_id, exact, (double)row->tolerance,
                 evidence, scope,
                 row->gate != NULL ? row->gate : "-") < 0)
         return 1;
   }

   return fflush(stdout) == 0 && !ferror(stdout) ? 0 : 1;
}
