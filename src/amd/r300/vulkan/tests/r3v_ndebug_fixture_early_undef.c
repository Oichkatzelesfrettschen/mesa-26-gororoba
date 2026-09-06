/* SPDX-License-Identifier: MIT
 *
 * The known-good for the release-verdict audit: a test whose verdict
 * stays active under its build's NDEBUG.
 *
 * Undefining NDEBUG before <assert.h> is included is what reaches
 * assert's expansion, so the verdict below keeps its call and the
 * translation unit keeps every assertion its source stakes.
 */

#undef NDEBUG
#include <assert.h>

#include <stdio.h>

static int calls;

static int
verdict(void)
{
   calls++;
   return 0;
}

int
main(void)
{
   assert(verdict() == 0);
   printf("early-undef fixture: the verdict ran %d time(s)\n", calls);
   return calls == 1 ? 0 : 1;
}
