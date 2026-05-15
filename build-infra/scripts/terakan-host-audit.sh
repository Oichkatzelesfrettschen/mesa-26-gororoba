#!/bin/sh
# terakan-host-audit.sh -- verify the Terakan host/build lane before a long build.
#
# WHY: missing tools and stale distcc policy waste hours on x130e.
# WHAT: check the selected Meson profile, host env, toolchain, Python modules,
#       pkg-config deps, and ccache/distcc-pump split.
# HOW: run through `make audit PROFILE=<profile> HOSTENV=<hostenv>`.

set -eu

PROFILE=terakan-distcc-no-rusticl
HOSTENV=btver1-ccache-no-pump

while [ "$#" -gt 0 ]; do
    case "$1" in
        --profile)
            PROFILE="${2:?missing value for --profile}"
            shift 2
            ;;
        --hostenv)
            HOSTENV="${2:?missing value for --hostenv}"
            shift 2
            ;;
        -h|--help)
            printf 'usage: %s [--profile NAME] [--hostenv NAME]\n' "$0"
            exit 0
            ;;
        *)
            printf 'unknown argument: %s\n' "$1" >&2
            exit 2
            ;;
    esac
done

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INFRA_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
TOPSRC=$(CDPATH= cd -- "$INFRA_DIR/.." && pwd)
CONFIG="$INFRA_DIR/configs/$PROFILE.meson"
ENVFILE="$INFRA_DIR/env/$HOSTENV.env"

failures=0
warnings=0

fail()
{
    printf 'FAIL  %s\n' "$*" >&2
    failures=$((failures + 1))
}

warn()
{
    printf 'WARN  %s\n' "$*" >&2
    warnings=$((warnings + 1))
}

ok()
{
    printf 'OK    %s\n' "$*"
}

need_command()
{
    if command -v "$1" >/dev/null 2>&1; then
        ok "command $1"
    else
        fail "missing command $1"
    fi
}

need_file()
{
    if [ -e "$1" ]; then
        ok "path $1"
    else
        fail "missing path $1"
    fi
}

need_pkg_config()
{
    if pkg-config --exists "$1" 2>/dev/null; then
        ok "pkg-config $1"
    else
        fail "missing pkg-config dependency $1"
    fi
}

printf 'Terakan host audit\n'
printf 'profile: %s\n' "$PROFILE"
printf 'hostenv: %s\n' "$HOSTENV"
printf 'topsrc:  %s\n\n' "$TOPSRC"

need_file "$CONFIG"
need_file "$ENVFILE"

if grep -Eq 'gallium-rusticl[[:space:]]*=[[:space:]]*true' "$CONFIG"; then
    RUSTICL_ENABLED=1
else
    RUSTICL_ENABLED=0
fi

for tool in meson ninja pkg-config python3 clang-21 clang++-21 ccache distcc; do
    need_command "$tool"
done

if [ "$RUSTICL_ENABLED" -eq 1 ]; then
    need_file "/home/eirikr/.local/bin/sccache"
    need_file "/home/eirikr/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc"
else
    if [ -x "/home/eirikr/.local/bin/sccache" ]; then
        ok "optional Rust sccache path"
    else
        warn "optional /home/eirikr/.local/bin/sccache missing; required only for Rusticl recovery profiles"
    fi
    if [ -x "/home/eirikr/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc" ]; then
        ok "optional stable rustc path"
    else
        warn "optional stable rustc path missing; required only for Rusticl recovery profiles"
    fi
fi

if python3 -c 'import mako, ply' >/dev/null 2>&1; then
    ok "python modules mako ply"
else
    fail "missing Python module mako or ply"
fi

if python3 - <<'PY' >/dev/null 2>&1
import sys
if sys.version_info >= (3, 12):
    import packaging
PY
then
    ok "python packaging requirement"
else
    fail "Python 3.12+ needs module packaging"
fi

for pc in \
    libdrm \
    xcb-dri3 \
    xcb-present \
    xshmfence \
    x11-xcb \
    xrandr \
    xcb-randr \
    xcb-sync \
    xcb-xfixes \
    wayland-client \
    wayland-server \
    wayland-protocols \
    vulkan; do
    need_pkg_config "$pc"
done

if command -v glslangValidator >/dev/null 2>&1; then
    ok "command glslangValidator"
else
    fail "missing command glslangValidator"
fi

if [ -r "$HOME/.distcc/hosts" ]; then
    if grep -Eq '(^|[[:space:]])[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+([/:[:space:]]|$)' "$HOME/.distcc/hosts"; then
        fail "raw IPv4 address found in ~/.distcc/hosts; use mDNS hostnames"
    else
        ok "distcc hosts use hostname policy"
    fi
else
    warn "~/.distcc/hosts is missing; distcc builds may run locally only"
fi

if [ "$HOSTENV" = "btver1-ccache-no-pump" ]; then
    if grep -q 'export CCACHE_PREFIX="/usr/bin/distcc"' "$ENVFILE"; then
        ok "warm lane uses CCACHE_PREFIX=/usr/bin/distcc"
    else
        fail "warm lane must use CCACHE_PREFIX=/usr/bin/distcc"
    fi
    if grep -q 'unset DISTCC_PUMP' "$ENVFILE"; then
        ok "warm lane clears distcc-pump state"
    else
        fail "warm lane must clear distcc-pump state"
    fi
fi

if [ "$HOSTENV" = "btver1-distcc-pump" ]; then
    if grep -q 'unset CCACHE_PREFIX' "$ENVFILE"; then
        ok "pump lane removes ccache prefix"
    else
        fail "pump lane must unset CCACHE_PREFIX"
    fi
    if grep -q ',cpp' "$ENVFILE"; then
        ok "pump lane derives cpp host options"
    else
        fail "pump lane must derive ,cpp host options"
    fi
fi

if [ "$RUSTICL_ENABLED" -eq 1 ]; then
    printf '\nRusticl recovery lane checks\n'
    need_command bindgen
    need_command rustfmt
    if command -v llvm-config-21 >/dev/null 2>&1; then
        ok "command llvm-config-21"
    else
        warn "llvm-config-21 not found; confirm llvm-21-dev/libclang-21-dev manually"
    fi
else
    ok "profile disables Rusticl for Terakan daily lane"
fi

printf '\nDebian package hints for missing daily-lane tools:\n'
printf '  meson ninja-build clang-21 pkg-config ccache distcc sccache\n'
printf '  libdrm-dev libxcb-dri3-dev libxcb-present-dev libxshmfence-dev\n'
printf '  libx11-xcb-dev libxrandr-dev libxcb-randr0-dev libxcb-sync-dev\n'
printf '  libxcb-xfixes0-dev libwayland-dev wayland-protocols python3-mako\n'
printf '  python3-ply python3-packaging libvulkan-dev glslang-tools\n'
if [ "$RUSTICL_ENABLED" -eq 1 ]; then
    printf '  rust-bindgen llvm-21-dev libclang-21-dev\n'
fi

if [ "$failures" -gt 0 ]; then
    printf '\nTerakan host audit failed: %d failure(s), %d warning(s).\n' "$failures" "$warnings" >&2
    exit 1
fi

printf '\nTerakan host audit passed with %d warning(s).\n' "$warnings"
