/* SPDX-License-Identifier: MIT
 *
 * The known-bad for the release-verdict audit: a test whose decisive
 * verdict disappears under its build's NDEBUG despite the source
 * carrying "#undef NDEBUG".
 *
 * assert re-expands on every inclusion of <assert.h> and reads NDEBUG as
 * it stood at that inclusion, so undefining it afterward changes
 * nothing.  The verdict below compiles to ((void)0), the call inside it
 * never runs, and the binary reports success having judged nothing.  A
 * text search for "#undef NDEBUG" accepts this file, which is why the
 * audit counts the assertion machinery the build discards instead.
 *
 * NDEBUG arrives from this target's own compile arguments, so the audit
 * reaches the same verdict on a debug profile and a release one.
 */

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
