# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT

inc_amd = include_directories('.')

if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif

if with_amd_vk or with_gallium_radeonsi
  subdir('addrlib')
  subdir('common')
  if amd_with_llvm
    subdir('llvm')
  else
    libamd_common_llvm = []
  endif
  subdir('compiler')
endif

if with_amd_vk
  subdir('vulkan')
  if with_aco_tests
    subdir('compiler/tests')
  endif
endif

if with_gallium_radeonsi
  subdir('vpelib')
  subdir('gmlib')
  subdir('lanczoslib')
endif

# Terakan TeraScale Vulkan driver
if with_amd_terascale_vk or with_gallium_r600
  subdir('terascale/common')
  subdir('terascale/compiler')
endif

if with_amd_terascale_vk
  subdir('terascale/vulkan')
endif

if with_amd_r300_vk
  # When the Gallium backend is enabled, defer r300vk to src/meson.build
  # (after subdir('gallium')) so driver_r300 is in scope.  The
  # loader-only build (no Gallium) is processed here as before.
  if not (get_option('r300vk-gallium-backend') and with_gallium_r300)
    subdir('r300/vulkan/r300vk')
  endif
endif

if with_tools.contains('r300')
  subdir('r300/tools')
endif
