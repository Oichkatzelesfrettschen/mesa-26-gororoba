# Copyright © 2019 Broadcom
# Copyright © 2020 Intel Corporation
# SPDX-License-Identifier: MIT

drm_shim_sources = files(
  'device.c',
  'drm_shim.c',
)
drm_shim_dependencies = [dep_libdrm, idep_mesautil, dep_dl]

drm_shim = static_library(
  'drm_shim',
  drm_shim_sources,
  include_directories: [inc_include, inc_src],
  dependencies: drm_shim_dependencies,
  gnu_symbol_visibility : 'hidden',
)
dep_drm_shim = declare_dependency(
  link_with: drm_shim,
  dependencies: dep_libdrm,
)

if with_tests
  drm_shim_test = static_library(
    'drm_shim_test',
    drm_shim_sources,
    # The fortified client links an unfortified shim and exercises the
    # checker entry points as a cross-flag ABI boundary.
    c_args: ['-DDRM_SHIM_TEST', '-U_FORTIFY_SOURCE'],
    include_directories: [inc_include, inc_src],
    dependencies: drm_shim_dependencies,
    gnu_symbol_visibility: 'hidden',
  )
  dep_drm_shim_test = declare_dependency(
    link_with: drm_shim_test,
    dependencies: dep_libdrm,
  )

  if cc.get_define(
    '__GLIBC__',
    prefix: '#include <features.h>',
  ) != ''
    drm_shim_legacy_glibc_stat = static_library(
      'drm_shim_legacy_glibc_stat',
      drm_shim_sources,
      c_args: ['-DHAS_XSTAT=1', '-U_FORTIFY_SOURCE'],
      include_directories: [inc_include, inc_src],
      dependencies: drm_shim_dependencies,
      gnu_symbol_visibility: 'hidden',
    )
    dep_drm_shim_legacy_glibc_stat = declare_dependency(
      link_with: drm_shim_legacy_glibc_stat,
      dependencies: dep_libdrm,
    )
  endif
endif
