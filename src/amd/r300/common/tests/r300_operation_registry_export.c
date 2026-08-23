/*
 * SPDX-License-Identifier: MIT
 *
 * Emit the stable, API-neutral operation join surface for external evidence
 * tooling.  This test-only program exports structural authority only: evidence,
 * route state, proof narrative, and retained-bundle provenance are deliberately
 * absent.
 */

#include "r300_numeric_domain.h"

#include <stdio.h>

int
main(void)
{
   if (printf("schema_version\toperation_id\toperation_name\t"
              "numeric_domain\n") < 0)
      return 1;

   for (unsigned value = R300_OPERATION_ID_NONE + 1;
        value < R300_OPERATION_ID_COUNT; value++) {
      const enum r300_operation_id id = (enum r300_operation_id)value;
      const struct r300_virtual_op_info *operation =
         r300_virtual_op_info_for_id(id);
      if (operation == NULL || operation->operation_id != id ||
          operation->op_name == NULL || operation->op_name[0] == '\0' ||
          (unsigned)operation->domain >= R300_NUM_DOMAIN_COUNT)
         return 1;

      const struct r300_numeric_domain_info *domain =
         r300_numeric_domain_info(operation->domain);
      if (domain == NULL || domain->domain != operation->domain ||
          domain->name == NULL || domain->name[0] == '\0')
         return 1;

      if (printf("1\t%u\t%s\t%s\n", value, operation->op_name,
                 domain->name) < 0)
         return 1;
   }

   return fflush(stdout) == 0 && !ferror(stdout) ? 0 : 1;
}
