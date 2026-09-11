#!/bin/sh
# Stock implicit layers use the loader's package paths; scoped selection chooses R3V.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
(
  VK_ADD_LAYER_PATH=/fixture/explicit
  VK_ADD_IMPLICIT_LAYER_PATH=/fixture/implicit
  export VK_ADD_LAYER_PATH VK_ADD_IMPLICIT_LAYER_PATH
  unset VK_DRIVER_FILES VK_ICD_FILENAMES
  . "$here/mesa-gororoba/mesa-gororoba-env.sh"
  . "$here/mesa-gororoba/mesa-gororoba-env.sh"
  [ "$VK_ADD_LAYER_PATH" = /fixture/explicit ]
  [ "$VK_ADD_IMPLICIT_LAYER_PATH" = /fixture/implicit ]
  [ "$VK_DRIVER_FILES" = /usr/share/vulkan/icd.d/r3v_icd.x86_64.json ]
  [ "$VK_ICD_FILENAMES" = "$VK_DRIVER_FILES" ]
)
echo 'stock Vulkan layer and scoped ICD environment: PASS'
