#!/usr/bin/env sh
# Verify that sourcing a Mesa loader environment preserves caller functions.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)

check_environment_file() (
  environment_file=$1
  MESA_INSTALL_PREFIX=/opt/mesa-loader-environment-fixture
  export MESA_INSTALL_PREFIX

  mesa_prepend_path() {
    printf '%s\n' caller-definition
  }

  # shellcheck disable=SC1090
  . "$environment_file"
  # shellcheck disable=SC1090
  . "$environment_file"

  if [ "$(mesa_prepend_path)" != caller-definition ]; then
    echo "caller function changed after sourcing $environment_file" >&2
    exit 1
  fi
)

test_directory=$(mktemp -d)
trap 'rm -rf -- "$test_directory"' EXIT HUP INT TERM
known_bad_environment_file="$test_directory/deletes-caller-function.sh"
printf '%s\n' 'unset -f mesa_prepend_path' > "$known_bad_environment_file"

if check_environment_file "$known_bad_environment_file" >/dev/null 2>&1; then
  echo "known-bad loader preserved a caller function" >&2
  exit 1
fi
echo "loader environment caller-function deletion: REJECTED"

check_environment_file "$here/mesa-gororoba/mesa-gororoba-env.sh"
check_environment_file "$here/mesa-gororoba-debug-optimized/mesa-gororoba-debug-optimized-env.sh"

echo "loader environment caller-function scope: PASS"
