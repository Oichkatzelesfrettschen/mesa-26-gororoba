/*
 * SPDX-License-Identifier: MIT
 *
 * Separation-audit known-bad: one exported symbol from the forbidden
 * Gallium set, so the audit's symbol-table scan refuses this object
 * without any Gallium library existing in the build.
 */

#include "util/macros.h"

PUBLIC void *r300_screen_create(void *winsys);

PUBLIC void *
r300_screen_create(void *winsys)
{
   return winsys;
}
