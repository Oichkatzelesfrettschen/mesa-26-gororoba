#!/bin/sh
# setup-distcc.sh -- verify ccache+distcc+sccache toolchain health.
#
# With the meson-native-file canonical wiring (c/cpp/rust in
# configs/<profile>.meson, CCACHE_PREFIX=distcc in env/btver1.env),
# there is no /tmp/distcc-wrap/ wrapper to regenerate anymore.
# This script now just pre-flights the tools so a missing binary
# fails LOUDLY before a long meson-configure run.
#
# The historical wrapper generation was an anti-pattern; see
# ../../steinmarder/docs/workspace/ccache-sccache-wiring.md for the RCA.

set -eu

missing=0
for tool in ccache distcc sccache rustc rustfmt clang-21 clang++-21; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf '  MISSING  %s\n' "$tool" >&2
        missing=$((missing + 1))
    fi
done

if [ "$missing" -gt 0 ]; then
    printf '\n%d required tool(s) missing.  Install them before running make.\n' \
        "$missing" >&2
    exit 1
fi

printf 'ccache:   %s\n' "$(ccache --version | head -1)"
printf 'distcc:   %s\n' "$(distcc --version | head -1)"
printf 'sccache:  %s\n' "$(sccache --version)"
printf 'rustc:    %s\n' "$(rustc --version)"
printf 'rustfmt:  %s\n' "$(rustfmt --version)"
printf 'clang-21: %s\n' "$(clang-21 --version | head -1)"
printf '\ndistcc hosts: %s\n' "$(paste -sd ' ' ~/.distcc/hosts 2>/dev/null || echo '(~/.distcc/hosts missing)')"

if [ -r "$HOME/.distcc/hosts" ] &&
    grep -Eq '(^|[[:space:]])[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+([/:[:space:]]|$)' "$HOME/.distcc/hosts"; then
    printf '\nraw IPv4 address found in ~/.distcc/hosts; use mDNS hostnames such as *.local.\n' >&2
    exit 1
fi

# Clean up the legacy wrapper dir if it exists from prior build-infra revisions.
if [ -d /tmp/distcc-wrap ]; then
    rm -rf /tmp/distcc-wrap
    printf 'cleaned legacy: /tmp/distcc-wrap\n'
fi

printf '\nall tools present; canonical wiring is in configs/*.meson [binaries].\n'
