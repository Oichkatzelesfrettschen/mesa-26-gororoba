/* SPDX-License-Identifier: MIT
 *
 * The known-bad for the release-verdict audit: a test whose decisive
 * verdict disappears under NDEBUG despite carrying "#undef NDEBUG".
 *
 * assert re-expands on every inclusion of <assert.h> and reads NDEBUG as
 * it stood at that inclusion, so undefining NDEBUG after the include
 * changes nothing.  The verdict below compiles to ((void)0), the call
 * inside it never runs, and the binary reports success having judged
 * nothing.  A text search for "#undef NDEBUG" accepts this file, which
 * is why the audit reads the preprocessed translation unit instead.
 */

#define NDEBUG 1
#include <assert.h>
#undef NDEBUG

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
   printf("late-undef fixture: the verdict ran %d time(s)\n", calls);
   return 0;
}
