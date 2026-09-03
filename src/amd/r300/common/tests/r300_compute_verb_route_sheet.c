/*
 * SPDX-License-Identifier: MIT
 *
 * Render the compute-verb ledger as a route sheet for external tooling and
 * for a reader deciding what executes.  The ledger's own C table orders a
 * row by declaration convenience, so a reader scanning it meets the evidence
 * strength before the route status and reads a retained-bundle token as a
 * working route; thirteen rows carry SILICON_RETAINED at raster-cell scope
 * with no route at all.  The sheet fixes the reading order: route status
 * leads, the implementation and contract that would carry a route follow,
 * and the evidence columns arrive last as a pair, strength beside subject.
 *
 * The emitter refuses a row it cannot name in full, so a field outside its
 * enum ends the sheet instead of printing an empty column that reads as an
 * absent property.
 */

#include "r300_compute_verb.h"

#include <stdio.h>

/* Route status leads; evidence strength and scope arrive together and last. */
static const char *const HEADER =
   "schema_version\tverb\tcpu_route\tgpu_route\timplementation_id\t"
   "contract_id\tunit\texactness\ttolerance\tevidence\tevidence_scope\tgate\n";

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

      if (printf("1\t%s\t%s\t%s\t%u\t%u\t%s\t%s\t%.9g\t%s\t%s\t%s\n",
                 row->name, cpu, gpu, (unsigned)row->implementation_id,
                 (unsigned)row->gpu_route_contract_id, unit, exact,
                 (double)row->tolerance, evidence, scope, row->gpu_gate) < 0)
         return 1;
   }

   return fflush(stdout) == 0 && !ferror(stdout) ? 0 : 1;
}
