# Copyright © 2021 Emma Anholt
# SPDX-License-Identifier: MIT

libradeon_noop_drm_shim = shared_library(
  'radeon_noop_drm_shim',
  'radeon_noop_drm_shim.c',
  include_directories: [inc_include, inc_src, inc_amd],
  dependencies: dep_drm_shim,
  gnu_symbol_visibility : 'hidden',
  install : true,
)

if with_tests
  radeon_noop_drm_shim_test = executable(
    'radeon_noop_drm_shim_test',
    'radeon_noop_drm_shim.c',
    c_args: ['-DDRM_SHIM_TEST'],
    include_directories: [inc_include, inc_src, inc_amd],
    dependencies: dep_drm_shim_test,
    gnu_symbol_visibility: 'hidden',
  )

  # The state-token residue calibration pins the exact failure set of the
  # full-surface selector runs, so neither a new failure nor a silent
  # repair passes unnoticed; the signature file names the mechanism.
  drm_shim_residue_check = files(
    'tests/radeon_noop_drm_shim_residue_check.py',
  )[0]
  drm_shim_full_selector_signature = files(
    'tests/state_token_residue_full_selector.signature',
  )[0]

  foreach selector_test : [
    ['default', 'DEFAULT', '0x7140', 'RV515', 'identity'],
    ['rs482_name', 'RS482_5974', '0x5974', 'RS480', 'identity'],
    ['rs482_name_lowercase', 'rs482_5974', '0x5974', 'RS480', 'identity'],
    ['rs480_family', 'RS480', '0x5954', 'RS480', 'identity'],
    ['uppercase_uevent', '0x4E44', '0x4E44', 'R300', 'identity'],
  ]
    test(
      'radeon-noop-drm-shim-@0@'.format(selector_test[0]),
      radeon_noop_drm_shim_test,
      args: [
        selector_test[1],
        selector_test[2],
        selector_test[3],
        selector_test[4],
      ],
      suite: ['amd', 'drm-shim'],
    )
  endforeach

  foreach selector_test : [
    ['rs482_numeric', '0x5974', '0x5974', 'RS480'],
    ['cayman_vm', 'CAYMAN', '0x6700', 'CAYMAN'],
    ['tahiti_shader_array', 'TAHITI', '0x6780', 'TAHITI'],
  ]
    test(
      'radeon-noop-drm-shim-@0@-residue'.format(selector_test[0]),
      prog_python,
      args: [
        drm_shim_residue_check,
        '--signature', drm_shim_full_selector_signature,
        '--',
        radeon_noop_drm_shim_test,
        selector_test[1],
        selector_test[2],
        selector_test[3],
        'full',
      ],
      depends: [radeon_noop_drm_shim_test],
      suite: ['amd', 'drm-shim'],
    )
  endforeach

  foreach selector_test : [
    ['empty', ''],
    ['short', '0x597'],
    ['long', '0x05974'],
    ['trailing', '0x5974junk'],
    ['uppercase_prefix', '0X5974'],
    ['unknown', '0xFFFF'],
  ]
    test(
      'radeon-noop-drm-shim-reject-@0@'.format(selector_test[0]),
      radeon_noop_drm_shim_test,
      args: [selector_test[1], 'reject'],
      suite: ['amd', 'drm-shim'],
    )
  endforeach

  test(
    'radeon-noop-drm-shim-fork-parent-exit-first',
    radeon_noop_drm_shim_test,
    args: ['fork-parent-exit-first'],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )
  test(
    'radeon-noop-drm-shim-claimed-namespace-map-miss',
    radeon_noop_drm_shim_test,
    args: ['claimed-namespace-map-miss'],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )
  test(
    'radeon-noop-drm-shim-reaper-close-sweep',
    radeon_noop_drm_shim_test,
    args: ['reaper-close-sweep'],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )

  if cc.get_define(
    '__GLIBC__',
    prefix: '#include <features.h>',
  ) != ''
    libradeon_noop_drm_shim_legacy = shared_library(
      'radeon_noop_drm_shim_legacy',
      'radeon_noop_drm_shim.c',
      include_directories: [inc_include, inc_src, inc_amd],
      dependencies: dep_drm_shim_legacy_glibc_stat,
      gnu_symbol_visibility: 'hidden',
      install: false,
    )

    radeon_noop_drm_shim_preload_test = executable(
      'radeon_noop_drm_shim_preload_test',
      'radeon_noop_drm_shim_preload_test.c',
      include_directories: [inc_include, inc_src, inc_amd],
      dependencies: [dep_libdrm, dep_dl],
      gnu_symbol_visibility: 'hidden',
    )
  endif
endif

libamdgpu_noop_drm_shim = shared_library(
  'amdgpu_noop_drm_shim',
  ['amdgpu_noop_drm_shim.c', '../common/amdgpu_devices.c'],
  include_directories: [inc_include, inc_src, inc_amd],
  dependencies: dep_drm_shim,
  install : true,
)

if with_tests and cc.get_define(
  '__GLIBC__',
  prefix: '#include <features.h>',
) != ''
  preload_modern_environment = environment({
    'LD_PRELOAD': libradeon_noop_drm_shim.full_path(),
    'DRM_SHIM_EXPECTED_DSO': libradeon_noop_drm_shim.full_path(),
    'RADEON_GPU_ID': '0x5974',
  })
  preload_legacy_environment = environment({
    'LD_PRELOAD': libradeon_noop_drm_shim_legacy.full_path(),
    'DRM_SHIM_EXPECTED_DSO': libradeon_noop_drm_shim_legacy.full_path(),
    'RADEON_GPU_ID': '0x5974',
  })

  test(
    'radeon-noop-drm-shim-preload-unloaded',
    radeon_noop_drm_shim_preload_test,
    args: ['unloaded', libradeon_noop_drm_shim.full_path()],
    depends: [libradeon_noop_drm_shim],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )
  # The preload runs carry the same state-token residue from the other
  # side: the exec-inherited child aborts.  They stay uncalibrated while
  # a synthetic sysfs open under parallel test load intermittently
  # reports EAGAIN (observed on /sys/dev/char/226:128/device/vendor in
  # the loaded-modern leg), which would make an exact-signature check
  # flaky.  The fd-alias adoption path is drm_shim_fd_adopt_raw_aliases;
  # the EAGAIN source under load is unattributed.
  test(
    'radeon-noop-drm-shim-preload-modern',
    radeon_noop_drm_shim_preload_test,
    args: ['loaded-modern'],
    env: preload_modern_environment,
    depends: [libradeon_noop_drm_shim],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )
  test(
    'radeon-noop-drm-shim-preload-legacy',
    radeon_noop_drm_shim_preload_test,
    args: ['loaded-legacy'],
    env: preload_legacy_environment,
    depends: [libradeon_noop_drm_shim_legacy],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )
  test(
    'radeon-noop-drm-shim-preload-cross-driver',
    radeon_noop_drm_shim_preload_test,
    args: ['cross-driver', libamdgpu_noop_drm_shim.full_path()],
    env: preload_modern_environment,
    depends: [libradeon_noop_drm_shim, libamdgpu_noop_drm_shim],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )

  foreach selector_test : [
    ['empty', ''],
    ['short', '0x597'],
    ['long', '0x05974'],
    ['trailing', '0x5974junk'],
    ['uppercase_prefix', '0X5974'],
    ['unknown', '0xFFFF'],
  ]
    test(
      'radeon-noop-drm-shim-preload-reject-@0@'.format(
        selector_test[0],
      ),
      radeon_noop_drm_shim_preload_test,
      args: [
        'reject-selector',
        libradeon_noop_drm_shim.full_path(),
        selector_test[1],
      ],
      depends: [libradeon_noop_drm_shim],
      timeout: 30,
      suite: ['amd', 'drm-shim'],
    )
  endforeach
endif

amdgpu_dump = executable(
  'amdgpu_dump_states',
  'amdgpu_dump_states.c',
  include_directories: [inc_include, inc_src, inc_amd],
  gnu_symbol_visibility: 'hidden',
  dependencies: [dep_libdrm],
  install: false,
)
