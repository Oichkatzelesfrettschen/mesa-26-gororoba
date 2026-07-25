/*
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_ENV_H
#define TERAKAN_ENV_H

#include <stdbool.h>
#include <stdlib.h>

/* Strict experimental/developer gate: only the exact string "1"
 * enables.  Production-facing options should NOT use this helper --
 * they need Mesa's broader option parser (debug_get_bool_option /
 * driconf) with boolean-truthy semantics.  This is for env knobs
 * that exist only to gate ad-hoc experiments and that must be safe
 * against shells / CI that pre-export variable names with empty or
 * falsey values.
 *
 * Acceptance matrix (the test under
 * src/amd/terascale/vulkan/tests/run_terakan_env_gate_unit.sh):
 *   unset       -> false
 *   VAR=        -> false  (empty)
 *   VAR=0       -> false
 *   VAR=true    -> false
 *   VAR=yes     -> false
 *   VAR=01      -> false
 *   VAR="1 "    -> false  (trailing space rejected)
 *   VAR=" 1"    -> false  (leading space rejected)
 *   VAR="1\n"   -> false  (trailing newline rejected)
 *   VAR=1       -> true   (exact two-byte string '1' + NUL)
 */
static inline bool
terakan_env_gate_enabled(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && v[0] == '1' && v[1] == '\0';
}

#endif /* TERAKAN_ENV_H */
