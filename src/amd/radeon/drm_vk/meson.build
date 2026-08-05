# SPDX-License-Identifier: MIT

# Generic Radeon DRM transport for Vulkan drivers: GEM buffer objects, PRIME
# sharing, relocation aggregation, DRM_RADEON_CS argument construction, and
# finite completion waits.  The layer carries radeon_drm.h mechanics only;
# R300 and Evergreen register or packet knowledge stays with each driver.
files_radeon_drm_vk = files(
  'radeon_drm_vk_bo.c',
  'radeon_drm_vk_completion.c',
  'radeon_drm_vk_cs.c',
  'radeon_drm_vk_device.c',
  'radeon_drm_vk_ioctl.c',
  'radeon_drm_vk_reloc.c',
)

inc_radeon_drm_vk = include_directories('.')

libradeon_drm_vk = static_library(
  'radeon_drm_vk',
  files_radeon_drm_vk,
  include_directories : [inc_include, inc_src],
  dependencies : [dep_libdrm, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

if with_tests
  foreach t : ['bo', 'cs', 'reloc']
    test(
      'radeon-drm-vk-' + t,
      executable(
        'radeon_drm_vk_@0@_test'.format(t),
        files('tests/radeon_drm_vk_@0@_test.c'.format(t)),
        include_directories : [inc_include, inc_src, inc_radeon_drm_vk],
        link_with : [libradeon_drm_vk],
        dependencies : [dep_libdrm, idep_mesautil],
        gnu_symbol_visibility : 'hidden',
      ),
      suite : ['radeon-drm-vk'],
    )
  endforeach
endif
