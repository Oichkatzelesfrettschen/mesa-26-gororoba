# SPDX-License-Identifier: MIT

r3v_entrypoints = custom_target(
  'r3v_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['r3v_entrypoints.h', 'r3v_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@',
    '--xml', '@INPUT1@',
    '--proto', '--weak',
    '--out-h', '@OUTPUT0@',
    '--out-c', '@OUTPUT1@',
    '--prefix', 'r3v',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

libr3v_files = files(
  'r3v_instance.c',
  'r3v_physical_device.c',
  'r3v_cpu_sync.c',
)

libr3v_deps = [
  dep_libdrm,
  idep_mesautil,
  idep_vulkan_runtime,
  idep_vulkan_util,
  # Both lanes call wsi_device_init/finish and register the WSI
  # entrypoint tables, so the full WSI dependency belongs to the base
  # library, not the Gallium arm.
  idep_vulkan_wsi,
]

libr3v_inc = [
  inc_include,
  inc_src,
  # Header-only: pipe/p_defines.h enums (PIPE_TEXTURE_*, PIPE_BIND_*) appear
  # in format-query signatures the loader-only lane compiles too; the
  # Gallium LIBRARY dependency stays gated below.
  inc_gallium,
]

libr3v_link_with = []
libr3v_c_args = [no_override_init_args]

# Gallium-backed device and submit path.  Gated behind r3v-gallium-backend
# so the loader-only R0 build (no Gallium dependency) continues to work.
# with_gallium_r300 is set by the parent meson.build when gallium-drivers
# includes r300; guard both options so the build fails clearly if only one
# is set.
# Processed from src/meson.build AFTER subdir('gallium') so that driver_r300
# and libgallium are in scope.
if get_option('r3v-gallium-backend') and not with_gallium_r300
  error('r3v-gallium-backend requires gallium-drivers to include r300; ' +
        'add r300 to -Dgallium-drivers or disable -Dr3v-gallium-backend')
endif
if get_option('r3v-gallium-backend') and with_gallium_r300
  libr3v_files += files(
    'r3v_device.c',
    'r3v_queue.c',
    'r3v_memory.c',
    'r3v_buffer.c',
    'r3v_image.c',
    'r3v_shader_module.c',
    'r3v_render_pass.c',
    'r3v_framebuffer.c',
    'r3v_cmd_buffer.c',
    'r3v_descriptor.c',
    'r3v_identity_map.c',
    'r3v_dp4_fs_nir.c',
    'r3v_pipeline.c',
    'r3v_object.c',
  )
  libr3v_deps += [
    idep_nir,
    driver_r300,
  ]
  libr3v_inc += [
    inc_gallium_aux,
    include_directories('../../../../gallium/drivers'),
    # r300/compiler/r300_nir.h (r300_optimize_nir) includes "r300_screen.h"
    # relative to the r300 driver dir, so put it directly on the include path.
    include_directories('../../../../gallium/drivers/r300'),
  ]
  # libgallium provides Gallium auxiliary symbols (u_upload_alloc_ref,
  # u_default_texture_subdata, etc.) that libr300.a references but does
  # not define.  Must be in link_with (not dependencies) because meson
  # tracks it as a static_library object, not a declare_dependency.
  # libgalliumvl resolves the g3dvl video backend symbols the r300 screen and
  # context now reference (vl_create_mpeg12_decoder via r300_video.c,
  # vl_video_buffer_is_format_supported via r300_screen.c); --gc-sections drops
  # the decode kernels when the codec is disabled.
  libr3v_link_with += [libgallium, libgalliumvl]
  libr3v_c_args += ['-DR3V_GALLIUM_BACKEND']
endif

libvulkan_r3v = shared_library(
  'vulkan_r3v',
  [libr3v_files, r3v_entrypoints],
  vs_module_defs : vulkan_api_def,
  include_directories : libr3v_inc,
  dependencies : libr3v_deps,
  link_with : libr3v_link_with,
  c_args : libr3v_c_args,
  link_args : [
    ld_args_build_id,
    ld_args_bsymbolic,
    ld_args_gc_sections,
    vulkan_icd_link_args,
  ],
  link_depends : vulkan_icd_link_depends,
  gnu_symbol_visibility : 'hidden',
  install : true,
)

# Build-time guard for the DP4 compute-as-raster fragment shader shape: the FS
# NIR builder must give the 2D sampler a 2-component coordinate (a vec4 coord
# trips nir_build_tex_struct's assert and aborted every DP4 compute pipeline
# create on an asserts-enabled build).  Compiles the extracted builder against
# NIR alone, no pipe_context.
if with_tests and get_option('r3v-gallium-backend') and with_gallium_r300
  test(
    'r3v-dp4-fs-nir',
    executable(
      'r3v_dp4_fs_nir_test',
      files('r3v_dp4_fs_nir.c', 'tests/r3v_dp4_fs_nir_test.c'),
      include_directories : [inc_include, inc_src, include_directories('.')],
      dependencies : [idep_nir, idep_mesautil],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
  test(
    'r3v-render-pass-feedback',
    executable(
      'r3v_render_pass_feedback_test',
      files('tests/r3v_render_pass_feedback_test.c'),
      include_directories : [libr3v_inc, include_directories('.')],
      dependencies : libr3v_deps,
      link_with : libr3v_link_with,
      c_args : libr3v_c_args,
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
  test(
    'r3v-static-offset',
    executable(
      'r3v_static_offset_test',
      files('r3v_dp4_fs_nir.c', 'tests/r3v_static_offset_test.c'),
      include_directories : [libr3v_inc, include_directories('.')],
      dependencies : libr3v_deps,
      link_with : libr3v_link_with,
      c_args : libr3v_c_args,
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
  test(
    'r3v-descriptor',
    executable(
      'r3v_descriptor_test',
      [r3v_entrypoints[0], files('r3v_descriptor.c',
                                'tests/r3v_descriptor_test.c')],
      include_directories : [libr3v_inc, include_directories('.')],
      dependencies : libr3v_deps,
      link_with : libr3v_link_with,
      c_args : libr3v_c_args,
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
  test(
    'r3v-format-features',
    executable(
      'r3v_format_features_test',
      files('tests/r3v_format_features_test.c'),
      include_directories : [libr3v_inc, include_directories('.')],
      dependencies : libr3v_deps,
      link_with : libr3v_link_with,
      c_args : libr3v_c_args,
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
endif

# ICD JSON manifest.  The loader discovers ICDs by scanning
# /usr/share/vulkan/icd.d/ for *.json files and following the
# library_path entry.  vk_icd_gen emits the canonical form so the
# version negotiation matches what r3v_instance.c implements.
icd_lib_path = join_paths(get_option('prefix'), get_option('libdir'))
# Route-chain oracle for the R2VB producer plan, frontend-integration tier:
# the typed-carry corpus SPIR-V through the production r3v preparation
# (r3v_prepare_shader_nir) into the r300 producer planner, both position
# spaces.  Compiles the pipeline translation unit directly so the oracle and
# the driver share one preparation.
if with_tests and get_option('r3v-gallium-backend') and with_gallium_r300
  test(
    'r3v-r2vb-typed-route-oracle',
    executable(
      'r3v_r2vb_typed_route_oracle_test',
      [r3v_entrypoints[0], files('r3v_pipeline.c', 'r3v_dp4_fs_nir.c',
                                 'tests/r3v_r2vb_typed_route_oracle_test.c')],
      include_directories : [
        libr3v_inc, include_directories('.'),
        inc_gallium_aux, inc_gallium_drivers,
        include_directories('../../../../gallium/drivers/r300'),
        include_directories('../../../../gallium/drivers/r300/compiler'),
      ],
      dependencies : libr3v_deps + [idep_nir],
      link_with : libr3v_link_with + [libr300],
      c_args : libr3v_c_args,
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
endif

# Producer resource census: every corpus specimen (typed-carry SPIR-V plus
# synthetic fitting and over-budget anchors) through the production plan
# chain, emitting one machine-readable row per specimen with the plan cost
# vectors and the RC statistics of every successful compile.  Verdict
# expectations reproduce the route-chain oracle, so a drifting census fails
# instead of mining wrong data.
if with_tests and get_option('r3v-gallium-backend') and with_gallium_r300
  test(
    'r3v-r2vb-producer-census',
    executable(
      'r3v_r2vb_producer_census',
      [r3v_entrypoints[0], files('r3v_pipeline.c', 'r3v_dp4_fs_nir.c',
                                 'tests/r3v_r2vb_producer_census.c')],
      include_directories : [
        libr3v_inc, include_directories('.'),
        inc_gallium_aux, inc_gallium_drivers,
        include_directories('../../../../gallium/drivers/r300'),
        include_directories('../../../../gallium/drivers/r300/compiler'),
      ],
      dependencies : libr3v_deps + [idep_nir],
      link_with : libr3v_link_with + [libr300],
      c_args : libr3v_c_args,
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
endif

icd_file_name = 'libvulkan_r3v.so'

r3v_icd = custom_target(
  'r3v_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'r3v_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.0',
    '--xml', '@INPUT1@',
    '--icd-lib-path', icd_lib_path,
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)

# Devenv ICD: when running meson devenv, point the loader at the build
# directory copy of the library so
# `VK_ICD_FILENAMES=<build>/r3v_devenv_icd.<cpu>.json` picks up the
# in-tree build (see `_dev_icdname`).
_dev_icdname = 'r3v_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  'r3v_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.0',
    '--xml', '@INPUT1@',
    '--icd-lib-path', meson.current_build_dir(),
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install : false,
)

devenv.append('VK_ICD_FILENAMES', _dev_icd.full_path())
