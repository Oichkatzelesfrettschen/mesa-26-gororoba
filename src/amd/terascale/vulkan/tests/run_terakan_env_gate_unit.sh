#!/usr/bin/env bash
#
# Copyright (c) 2026 Terascale Functionalists
# SPDX-License-Identifier: MIT
#
# Acceptance matrix for terakan_env_gate_enabled() (the strict =1 helper
# in src/amd/terascale/vulkan/terakan_env.h).
#
# Builds a tiny stand-alone C driver and exercises the 10 documented
# input cases per row, asserting the expected boolean.  Does NOT use
# shell `eval` (brittle, lint-flagged); each row launches the driver
# directly with the configured environment.
#
# Portability:
#   - uses a portable mktemp template (no --suffix; --suffix is GNU-only)
#   - compiles with -std=c11 -Wall -Wextra -Werror to fail closed on
#     any compiler warning
#
# Usage:
#   bash run_terakan_env_gate_unit.sh
# Exit:
#   0 = all cases match expected; non-zero = at least one case wrong
#       or the driver failed to build.

set -u

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
HDR="${SELF_DIR}/../terakan_env.h"

# Portable mktemp templates: trailing 'X' fields are filled with random
# characters by both GNU and BSD coreutils.  No --suffix flag is used.
TMP_DRV="$(mktemp -t terakan_env_gate_drv.XXXXXX.c)"
TMP_BIN="$(mktemp -t terakan_env_gate_bin.XXXXXX)"
trap 'rm -f "${TMP_BIN}" "${TMP_DRV}"' EXIT

cat > "${TMP_DRV}" <<EOF
#include <stdio.h>
#include "${HDR}"
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    printf("%s\n", terakan_env_gate_enabled(argv[1]) ? "true" : "false");
    return 0;
}
EOF

# -std=c11 to guarantee stdbool.h + static-inline semantics across
# toolchains; -Werror to fail closed on any unexpected diagnostic.
"${CC:-cc}" -std=c11 -O0 -Wall -Wextra -Werror "${TMP_DRV}" -o "${TMP_BIN}" || {
    echo "build failed" >&2
    exit 2
}

ok=0
fail=0

# Run one acceptance case.  No `eval`: the runner sets the env var
# (or unsets it for the unset case) and execs the driver directly.
# Args: <description> <expected_text> <value_word>|<__UNSET__>
run() {
    local desc="$1"
    local expected="$2"
    local value="$3"
    local actual
    if [ "${value}" = "__UNSET__" ]; then
        # Run with TERAKAN_TESTVAR removed from the inherited env.
        actual="$(env -u TERAKAN_TESTVAR "${TMP_BIN}" TERAKAN_TESTVAR)"
    else
        # Env-prefix MUST go INSIDE the command substitution so it
        # applies to the subshell command, not to the outer assignment.
        # `VAR=val actual="$(cmd)"` is itself an assignment, so the
        # VAR=val prefix is treated as a shell-only assignment and does
        # NOT propagate to the subshell -- a bash gotcha.
        actual="$(TERAKAN_TESTVAR="${value}" "${TMP_BIN}" TERAKAN_TESTVAR)"
    fi
    if [ "${actual}" = "${expected}" ]; then
        printf "PASS  %-22s expected=%-5s actual=%s\n" "${desc}" "${expected}" "${actual}"
        ok=$((ok + 1))
    else
        printf "FAIL  %-22s expected=%-5s actual=%s\n" "${desc}" "${expected}" "${actual}"
        fail=$((fail + 1))
    fi
}

# 10-case acceptance matrix per terakan_env.h docstring (kept in sync).
run "unset"             "false"  "__UNSET__"
run "empty (VAR=)"      "false"  ""
run "VAR=0"             "false"  "0"
run "VAR=true"          "false"  "true"
run "VAR=yes"           "false"  "yes"
run "VAR=01"            "false"  "01"
run "VAR=1[space]"      "false"  "1 "
run "VAR=[space]1"      "false"  " 1"
# bash ANSI-C $'...' quoting produces literal '1' + newline byte
run "VAR=1[newline]"    "false"  $'1\n'
run "VAR=1 (only)"      "true"   "1"

echo
echo "summary: ${ok} pass, ${fail} fail"
[ "${fail}" -eq 0 ]
