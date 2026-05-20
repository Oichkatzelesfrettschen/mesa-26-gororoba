#!/usr/bin/env bash
#
# Copyright (c) 2026 Terascale Functionalists
# SPDX-License-Identifier: MIT
#
# Acceptance matrix for terakan_env_gate_enabled() (the strict =1 helper
# in src/amd/terascale/vulkan/terakan_env.h).
#
# Builds a tiny stand-alone C driver, runs it under the 9 documented
# input cases, and asserts the expected boolean per row.
#
# Usage:
#   bash run_terakan_env_gate_unit.sh
# Exit:
#   0 = all cases match expected; non-zero = at least one case wrong.

set -u

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
HDR="${SELF_DIR}/../terakan_env.h"
TMP_BIN="$(mktemp --suffix=.bin)"
TMP_DRV="$(mktemp --suffix=.c)"
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

${CC:-cc} -O0 -Wall -Wextra "${TMP_DRV}" -o "${TMP_BIN}" || {
    echo "build failed" >&2
    exit 2
}

ok=0
fail=0
run() {
    local desc="$1" expected="$2" cmd="$3"
    actual="$(eval "$cmd")"
    if [ "${actual}" = "${expected}" ]; then
        printf "PASS  %-22s expected=%-5s actual=%s\n" "${desc}" "${expected}" "${actual}"
        ok=$((ok + 1))
    else
        printf "FAIL  %-22s expected=%-5s actual=%s\n" "${desc}" "${expected}" "${actual}"
        fail=$((fail + 1))
    fi
}

# 9-case acceptance matrix per terakan_env.h docstring
run "unset"            "false"  "unset TERAKAN_TESTVAR; \"${TMP_BIN}\" TERAKAN_TESTVAR"
run "empty (VAR=)"     "false"  "TERAKAN_TESTVAR= \"${TMP_BIN}\" TERAKAN_TESTVAR"
run "VAR=0"            "false"  "TERAKAN_TESTVAR=0 \"${TMP_BIN}\" TERAKAN_TESTVAR"
run "VAR=true"         "false"  "TERAKAN_TESTVAR=true \"${TMP_BIN}\" TERAKAN_TESTVAR"
run "VAR=yes"          "false"  "TERAKAN_TESTVAR=yes \"${TMP_BIN}\" TERAKAN_TESTVAR"
run "VAR=01"           "false"  "TERAKAN_TESTVAR=01 \"${TMP_BIN}\" TERAKAN_TESTVAR"
run "VAR=1[space]"     "false"  "TERAKAN_TESTVAR='1 ' \"${TMP_BIN}\" TERAKAN_TESTVAR"
run "VAR=[space]1"     "false"  "TERAKAN_TESTVAR=' 1' \"${TMP_BIN}\" TERAKAN_TESTVAR"
run "VAR=1 (only)"     "true"   "TERAKAN_TESTVAR=1 \"${TMP_BIN}\" TERAKAN_TESTVAR"

echo
echo "summary: ${ok} pass, ${fail} fail"
[ "${fail}" -eq 0 ]
