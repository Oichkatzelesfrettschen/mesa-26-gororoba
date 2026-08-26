#!/usr/bin/env sh
# Verify that the debug launcher exposes its opt-scoped Vulkan layers once.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd -P)
environment_file="$HERE/mesa-gororoba-debug-optimized/mesa-gororoba-debug-optimized-env.sh"
fixture_prefix=/opt/mesa-vulkan-layer-environment-fixture

(
  GOROROBA_MESA_PREFIX=$fixture_prefix
  VK_ADD_LAYER_PATH=/usr/local/share/vulkan/explicit_layer.d
  VK_ADD_IMPLICIT_LAYER_PATH=/usr/local/share/vulkan/implicit_layer.d
  export GOROROBA_MESA_PREFIX VK_ADD_LAYER_PATH VK_ADD_IMPLICIT_LAYER_PATH

  . "$environment_file"
  . "$environment_file"

  expected_explicit="$fixture_prefix/share/vulkan/explicit_layer.d:/usr/local/share/vulkan/explicit_layer.d"
  expected_implicit="$fixture_prefix/share/vulkan/implicit_layer.d:/usr/local/share/vulkan/implicit_layer.d"

  if [ "$VK_ADD_LAYER_PATH" != "$expected_explicit" ]; then
    echo "explicit Vulkan layer path mismatch: $VK_ADD_LAYER_PATH" >&2
    exit 1
  fi
  if [ "$VK_ADD_IMPLICIT_LAYER_PATH" != "$expected_implicit" ]; then
    echo "implicit Vulkan layer path mismatch: $VK_ADD_IMPLICIT_LAYER_PATH" >&2
    exit 1
  fi
)

echo "Vulkan layer launcher environment: PASS"
