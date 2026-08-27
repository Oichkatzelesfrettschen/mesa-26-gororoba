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

  drm_shim_i686_test_args = [
    '-m32',
    '-march=i686',
    '-mtune=generic',
  ]
  drm_shim_i686_runtime_available = false
  if not meson.is_cross_build() or meson.can_run_host_binaries()
    drm_shim_i686_runtime_probe = cc.run(
      'int main(void) { return 0; }',
      args: drm_shim_i686_test_args,
      name: 'i686 C runtime for drm-shim ABI tests',
    )
    drm_shim_i686_runtime_available = (
      drm_shim_i686_runtime_probe.compiled() and
      drm_shim_i686_runtime_probe.returncode() == 0
    )
  endif
  if drm_shim_i686_runtime_available
    drm_shim_fcntl_large_lock_abi_test = executable(
      'drm_shim_fcntl_large_lock_abi_test',
      'tests/fcntl_large_lock_abi.c',
      c_args: drm_shim_i686_test_args,
      link_args: ['-m32'],
      include_directories: include_directories('.'),
    )
    test(
      'drm-shim-fcntl-large-lock-abi',
      drm_shim_fcntl_large_lock_abi_test,
      suite: ['drm-shim'],
    )
  else
    message('drm-shim i686 ABI test disabled: executable runtime unavailable')
  endif

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
