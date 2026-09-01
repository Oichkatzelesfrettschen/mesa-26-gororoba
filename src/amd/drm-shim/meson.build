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

  # Calibrates the checker itself against known-good and known-bad
  # inputs: the recorded signature replayed verbatim passes, a foreign
  # vendor id (0x10de) reported through the shim's identity-override
  # comparison fails even though digit-run normalization alone would
  # collapse it onto the shim's own vendor (0x1002), and a decimal-only
  # value elsewhere in the failure set keeps its normalization.
  test(
    'radeon-noop-drm-shim-residue-check-identity-calibration',
    prog_python,
    args: [files('tests/radeon_noop_drm_shim_residue_check_selftest.py')],
    suite: ['amd', 'drm-shim'],
  )

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
    'radeon-noop-drm-shim-fd-identity-publication',
    radeon_noop_drm_shim_test,
    args: ['fd-identity-publication'],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )
  test(
    'radeon-noop-drm-shim-fork-child-witness-release',
    radeon_noop_drm_shim_test,
    args: ['fork-child-witness-release'],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )
  test(
    'radeon-noop-drm-shim-fork-child-identity-repair',
    radeon_noop_drm_shim_test,
    args: ['fork-child-identity-repair'],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )
  test(
    'radeon-noop-drm-shim-witness-fd-reuse',
    radeon_noop_drm_shim_test,
    args: ['witness-fd-reuse'],
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
  test(
    'radeon-noop-drm-shim-exact-identity-or-refuse',
    radeon_noop_drm_shim_test,
    args: ['fd-exact-identity-or-refuse'],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )
  test(
    'radeon-noop-drm-shim-write-only-state-token-readable-witness',
    radeon_noop_drm_shim_test,
    args: ['write-only-state-token-readable-witness'],
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
  # flaky.  The fd-alias adoption path is drm_shim_fd_adopt_raw_aliases
  # (rg --fixed-strings drm_shim_fd_adopt_raw_aliases
  # src/drm-shim/device.c); the EAGAIN source under load is unattributed.
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
  test(
    'radeon-noop-drm-shim-preload-changed-device',
    radeon_noop_drm_shim_preload_test,
    args: ['changed-device'],
    env: preload_modern_environment,
    depends: [libradeon_noop_drm_shim],
    timeout: 30,
    suite: ['amd', 'drm-shim'],
  )
  test(
    'radeon-noop-drm-shim-preload-malformed-token',
    radeon_noop_drm_shim_preload_test,
    args: ['malformed-token'],
    env: preload_modern_environment,
    depends: [libradeon_noop_drm_shim],
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
