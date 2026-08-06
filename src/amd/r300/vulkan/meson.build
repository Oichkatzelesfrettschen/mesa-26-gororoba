# SPDX-License-Identifier: MIT

# Gallium roots resolved once: r3v links the r300 Gallium driver and its
# shader compiler, and a relative path repeated at every use site breaks
# silently when the driver directory moves.
inc_gallium_drivers = include_directories('../../../gallium/drivers')
inc_gallium_r300 = include_directories('../../../gallium/drivers/r300')
inc_gallium_r300_compiler = include_directories(
  '../../../gallium/drivers/r300/compiler')

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

# The Gallium arm appends to libr3v_files below; the native ICD builds from
# this loader-base snapshot.
r3v_loader_base_files = libr3v_files

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
    '../common/r300_fragment_binary.c',
  )
  libr3v_deps += [
    idep_nir,
    driver_r300,
  ]
  libr3v_inc += [
    inc_gallium_aux,
    inc_gallium_drivers,
    # r300/compiler/r300_nir.h (r300_optimize_nir) includes "r300_screen.h"
    # relative to the r300 driver dir, so put it directly on the include path.
    inc_gallium_r300,
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
      files('r3v_dp4_fs_nir.c', 'tests/r3v_static_offset_test.c',
            '../common/r300_fragment_binary.c'),
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
                                 '../common/r300_fragment_binary.c',
                                 'tests/r3v_r2vb_typed_route_oracle_test.c')],
      include_directories : [
        libr3v_inc, include_directories('.'),
        inc_gallium_aux, inc_gallium_drivers,
        inc_gallium_r300,
        inc_gallium_r300_compiler,
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
                                 '../common/r300_fragment_binary.c',
                                 'tests/r3v_r2vb_producer_census.c')],
      include_directories : [
        libr3v_inc, include_directories('.'),
        inc_gallium_aux, inc_gallium_drivers,
        inc_gallium_r300,
        inc_gallium_r300_compiler,
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

# Native ICD identity: libvulkan_r3v_native builds the loader-base files over
# the generic Radeon DRM transport with no Gallium runtime ownership.  The
# two ICDs coexist under one loader; each option produces its own library and
# manifest, and the separation-audit test proves the native library carries
# no Gallium entry symbol.
if get_option('r3v-native-backend')
  r3v_native_files = files(
    'r3v_native_arming.c',
    'r3v_native_cell.c',
    'r3v_native_cmd.c',
    'r3v_native_device.c',
    'r3v_native_memory.c',
    'r3v_native_queue.c',
    '../common/r300_first_draw_state.c',
    '../common/r300_fragment_binary.c',
    '../common/r300_tcl_bypass_triangle.c',
  )

  r3v_native_inc = [
    inc_include,
    inc_src,
    inc_gallium,
    # r300_reg.h for the fixed-cell emitter.
    include_directories('../../../gallium/drivers/r300'),
  ]
  r3v_native_deps = [
    dep_libdrm,
    idep_mesautil,
    idep_vulkan_runtime,
    idep_vulkan_util,
    idep_vulkan_wsi,
  ]

  # One compile of the native implementation feeds both the ICD shared
  # library and the triangle-cell harness; the ICD version script keeps
  # the loader surface at the three vk_icd* exports, so the harness links
  # the implementation directly instead of dlopening the ICD.  The static
  # library takes compile-only partial dependencies so the runtime and WSI
  # objects link once, at each consumer.
  r3v_native_compile_deps = []
  foreach _dep : r3v_native_deps
    r3v_native_compile_deps += _dep.partial_dependency(
      compile_args : true, includes : true, sources : true)
  endforeach

  libr3v_native_impl = static_library(
    'r3v_native_impl',
    [r3v_loader_base_files, r3v_native_files, r3v_entrypoints],
    include_directories : r3v_native_inc,
    dependencies : r3v_native_compile_deps,
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args, '-DR3V_NATIVE_BACKEND'],
    gnu_symbol_visibility : 'hidden',
  )

  libvulkan_r3v_native = shared_library(
    'vulkan_r3v_native',
    vs_module_defs : vulkan_api_def,
    link_whole : [libr3v_native_impl],
    dependencies : r3v_native_deps,
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

  native_icd_file_name = 'libvulkan_r3v_native.so'

  r3v_native_icd = custom_target(
    'r3v_native_icd',
    input : [vk_icd_gen, vk_api_xml],
    output : 'r3v_native_icd.@0@.json'.format(host_machine.cpu()),
    command : [
      prog_python, '@INPUT0@',
      '--api-version', '1.0',
      '--xml', '@INPUT1@',
      '--icd-lib-path', icd_lib_path,
      '--icd-filename', native_icd_file_name,
      '--out', '@OUTPUT@',
    ],
    build_by_default : true,
    install_dir : with_vulkan_icd_dir,
    install : true,
  )

  _native_dev_icd = custom_target(
    'r3v_native_devenv_icd',
    input : [vk_icd_gen, vk_api_xml],
    output : 'r3v_native_devenv_icd.@0@.json'.format(host_machine.cpu()),
    command : [
      prog_python, '@INPUT0@',
      '--api-version', '1.0',
      '--xml', '@INPUT1@',
      '--icd-lib-path', meson.current_build_dir(),
      '--icd-filename', native_icd_file_name,
      '--out', '@OUTPUT@',
    ],
    build_by_default : true,
    install : false,
  )

  # Executable form of the boundaries doc's dependency/symbol audit: the
  # native library defines no Gallium or winsys entry symbol, in any binding.
  prog_nm_r3v = find_program('nm', required : false)
  if with_tests and prog_nm_r3v.found()
    test(
      'r3v-native-gallium-separation',
      prog_python,
      args : [
        files('tests/r3v_native_separation_audit.py'),
        prog_nm_r3v.full_path(),
        libvulkan_r3v_native.full_path(),
      ],
      depends : libvulkan_r3v_native,
      suite : ['r3v'],
    )
    # Known-bad calibration: the Gallium-backed library carries
    # r300_screen_create, so the audit must fail on it.
    if get_option('r3v-gallium-backend') and with_gallium_r300
      test(
        'r3v-native-gallium-separation-known-bad',
        prog_python,
        args : [
          files('tests/r3v_native_separation_audit.py'),
          prog_nm_r3v.full_path(),
          libvulkan_r3v.full_path(),
        ],
        depends : libvulkan_r3v,
        should_fail : true,
        suite : ['r3v'],
      )
    endif

    # Dispatch-closure audit: the common overlay populates 1.0 bridge slots
    # whose downstream dispatch targets the native link set does not carry,
    # so a populated entrypoint can call through a null slot.  The selftest
    # proves both audit directions on synthetic tables; the baseline test
    # pins the known-open bridge families so the audit stays calibrated;
    # the enforce gate flips to a plain pass when the native table closes.
    r3v_native_entrypoint_audit_args = [
      files('tests/r3v_native_entrypoint_audit.py'),
      prog_nm_r3v.full_path(),
      libvulkan_r3v_native.full_path(),
      vk_api_xml,
      meson.project_source_root() / 'src' / 'vulkan' / 'runtime',
    ]
    test(
      'r3v-native-entrypoint-audit-selftest',
      prog_python,
      args : [files('tests/r3v_native_entrypoint_audit.py'), '--selftest'],
      suite : ['r3v'],
    )
    test(
      'r3v-native-entrypoint-audit-baseline',
      prog_python,
      args : [
        r3v_native_entrypoint_audit_args,
        '--expect-open',
        'BindImageMemory,GetImageMemoryRequirements,CmdBeginRenderPass,'
        + 'CmdBindVertexBuffers,CmdCopyBuffer,CmdPipelineBarrier',
      ],
      depends : libvulkan_r3v_native,
      suite : ['r3v'],
    )
    test(
      'r3v-native-entrypoint-closure',
      prog_python,
      args : [r3v_native_entrypoint_audit_args, '--enforce'],
      depends : libvulkan_r3v_native,
      should_fail : true,
      suite : ['r3v'],
    )
  endif

  # Attended cell: performs a live DRM_RADEON_CS on RS482 silicon under the
  # authorization and procedure in
  # docs/hardware/r3v-native-attended-cell-procedure.md.  It drives the
  # native ICD directly, so it builds wherever the native backend does; it
  # is built and never registered as a test, because running it is a
  # hardware authorization rather than a build-graph result.
  r3v_native_attended_cell = executable(
    'r3v_native_attended_cell',
    files('tests/r3v_native_attended_cell.c'),
    include_directories : [r3v_native_inc, include_directories('.')],
    dependencies : r3v_native_deps,
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
    build_by_default : true,
    install : false,
  )

  # Full-chain triangle-cell harness on the radeon noop drm-shim: the
  # closed-gate leg proves the fail-closed verdict and byte-identity of
  # the retained IB against the direct emitter, and the open-gate leg
  # proves the transport path with the shim absorbing DRM_RADEON_CS.
  if with_tests and with_tools.contains('drm-shim') and cc.get_define(
    '__GLIBC__', prefix : '#include <features.h>') != ''
    r3v_native_triangle_cell_harness = executable(
      'r3v_native_triangle_cell_harness',
      files('tests/r3v_native_triangle_cell_harness.c'),
      include_directories : r3v_native_inc,
      dependencies : [r3v_native_deps, dep_dl],
      # The entrypoint tables reference driver functions weakly, so the
      # whole implementation archive links in; plain link_with would skip
      # the objects only the dispatch tables name.
      link_whole : [libr3v_native_impl],
      link_with : [libradeon_drm_vk],
      c_args : [no_override_init_args],
      gnu_symbol_visibility : 'hidden',
    )
    r3v_native_cell_env = environment({
      'LD_PRELOAD' : libradeon_noop_drm_shim.full_path(),
      'RADEON_GPU_ID' : '0x5974',
      'DRM_SHIM_EXPECTED_DSO' : libradeon_noop_drm_shim.full_path(),
    })
    foreach cell_mode : ['closed', 'open']
      test(
        'r3v-native-triangle-cell-@0@'.format(cell_mode),
        r3v_native_triangle_cell_harness,
        args : [cell_mode],
        env : r3v_native_cell_env,
        depends : [libradeon_noop_drm_shim],
        timeout : 60,
        suite : ['r3v'],
      )
    endforeach
    # Arming-gate calibration: the complete fact set arms, and each
    # single-factor defect refuses with its own verdict.  The evaluator is
    # pure, so the test compiles it directly.
    test(
      'r3v-native-arming',
      executable(
        'r3v_native_arming_test',
        files('r3v_native_arming.c', 'tests/r3v_native_arming_test.c'),
        include_directories : [inc_include, inc_src,
                               include_directories('.')],
        gnu_symbol_visibility : 'hidden',
      ),
      suite : ['r3v'],
    )

    # Non-submitting arming runner: reports the cell digest and every
    # arming factor, then stops at the authorization boundary.  The test
    # leg runs it against a fresh directory with no declarations, so the
    # expected verdict is a refusal.
    r3v_native_arming_runner = executable(
      'r3v_native_arming_runner',
      files('r3v_native_arming.c', 'tests/r3v_native_arming_runner.c',
            '../common/r300_first_draw_state.c',
            '../common/r300_fragment_binary.c',
            '../common/r300_tcl_bypass_triangle.c'),
      include_directories : [r3v_native_inc, include_directories('.')],
      dependencies : [idep_mesautil],
      gnu_symbol_visibility : 'hidden',
    )
    test(
      'r3v-native-arming-runner-refuses-undeclared',
      prog_python,
      args : [
        files('tests/r3v_native_arming_runner_check.py'),
        r3v_native_arming_runner,
      ],
      suite : ['r3v'],
    )

    # Provider-attestation calibration: with the shim absent from
    # LD_PRELOAD, open and ioctl resolve into libc, and the harness
    # refuses before the hazard gate opens and before any Vulkan call.
    r3v_native_cell_unattested_env = environment({
      'RADEON_GPU_ID' : '0x5974',
      'DRM_SHIM_EXPECTED_DSO' : libradeon_noop_drm_shim.full_path(),
    })
    test(
      'r3v-native-triangle-cell-unattested',
      r3v_native_triangle_cell_harness,
      args : ['open'],
      env : r3v_native_cell_unattested_env,
      depends : [libradeon_noop_drm_shim],
      should_fail : true,
      timeout : 60,
      suite : ['r3v'],
    )
    # Exact-submit-object replay: the open-gate harness retains the object
    # the ioctl carried, and the kernel decision code replays those exact
    # bytes with malformed controls.  Runtime-gated on
    # R3V_KERNEL_REPLAY_TOOL (skip 77 when unset).
    test(
      'r3v-native-submit-object-replay',
      files('tests/run_submit_object_replay.sh')[0],
      args : [r3v_native_triangle_cell_harness],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endif
endif
