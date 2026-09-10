#!/usr/bin/env sh
# Verify that sourcing a Mesa loader environment preserves caller functions.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)

check_environment_file() (
  environment_file=$1

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
check_environment_file "$here/mesa-gororoba-debug-optimized/mesa-gororoba-env.sh"

check_profile_does_not_force_vulkan_selection() (
  unset VK_DRIVER_FILES VK_ICD_FILENAMES
  # shellcheck disable=SC1090
  . "$here/mesa-gororoba/mesa-gororoba-profile.sh"
  if [ "${VK_DRIVER_FILES+x}" = x ] || [ "${VK_ICD_FILENAMES+x}" = x ]; then
    echo "interactive profile forced Vulkan driver selection" >&2
    exit 1
  fi
)

check_development_wrapper_path_and_overrides() (
  MESA_INSTALL_PREFIX=/opt/mesa-loader-fixture
  export MESA_INSTALL_PREFIX
  unset VK_DRIVER_FILES VK_ICD_FILENAMES
  # shellcheck disable=SC1090
  . "$here/mesa-gororoba/mesa-gororoba-env.sh"
  expected="$MESA_INSTALL_PREFIX/share/vulkan/icd.d/r3v_icd.x86_64.json"
  [ "${VK_DRIVER_FILES:-}" = "$expected" ] || {
    echo "development wrapper selected an unexpected VK_DRIVER_FILES path" >&2
    exit 1
  }
  [ "${VK_ICD_FILENAMES:-}" = "$expected" ] || {
    echo "development wrapper selected an unexpected VK_ICD_FILENAMES path" >&2
    exit 1
  }

  VK_DRIVER_FILES=/caller/driver.json
  VK_ICD_FILENAMES=/caller/icd.json
  export VK_DRIVER_FILES VK_ICD_FILENAMES
  # shellcheck disable=SC1090
  . "$here/mesa-gororoba/mesa-gororoba-env.sh"
  [ "$VK_DRIVER_FILES" = /caller/driver.json ] || {
    echo "development wrapper replaced caller VK_DRIVER_FILES" >&2
    exit 1
  }
  [ "$VK_ICD_FILENAMES" = /caller/icd.json ] || {
    echo "development wrapper replaced caller VK_ICD_FILENAMES" >&2
    exit 1
  }
)

check_profile_does_not_force_vulkan_selection
check_development_wrapper_path_and_overrides

echo "loader environment caller-function scope: PASS"
