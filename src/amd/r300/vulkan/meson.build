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
    'r3v-queue-map-range',
    executable(
      'r3v_queue_map_range_test',
      files('tests/r3v_queue_map_range_test.c'),
      include_directories : [libr3v_inc, include_directories('.')],
      dependencies : libr3v_deps,
      link_with : libr3v_link_with,
      c_args : libr3v_c_args,
      link_args : ['-Wl,--gc-sections'],
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
    'r3v_native_draw.c',
    'r3v_native_image.c',
    'r3v_native_identity.c',
    'r3v_native_memory.c',
    'r3v_native_object.c',
    'r3v_native_pipeline.c',
    'r3v_native_queue.c',
    'r3v_native_recording.c',
    'r3v_native_transfer.c',
    '../cpu/r300_cpu_vertex.c',
    '../common/r300_direct_write.c',
    '../common/r300_first_draw_state.c',
    '../common/r300_fragment_binary.c',
    '../common/r300_pm4_builder.c',
    '../common/r300_delivery_route.c',
    '../common/r300_r2vb_carrier_delivery.c',
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
    # Lifecycle-pair calibration on synthetic surfaces.  A --drop fixture
    # cannot reach this verdict: the dropped command fails the completeness
    # verdict in the same run, so a pair analysis that had stopped running
    # would still leave the fixture failing.  Each surface below reaches the
    # pair rule alone.
    foreach pair_case : [['complete', false],
                         ['missing-create', true],
                         ['missing-destroy', true],
                         ['explicit-complete', false],
                         ['explicit-missing-release', true],
                         ['plural-missing-release', true]]
      test(
        'r3v-native-lifecycle-pair-@0@'.format(pair_case[0]),
        prog_python,
        args : [files('tests/r3v_native_entrypoint_audit.py'),
                '--pair-fixture', pair_case[0]],
        should_fail : pair_case[1],
        suite : ['r3v'],
      )
    endforeach
    # Source headers carry the license grant and the file's description.  The
    # audit covers the complete native ownership surface -- the Vulkan lane,
    # the shared r300 common code, the Radeon DRM transport, and the native
    # build-infra scripts -- and each fixture mutates one clean header so a
    # rule that stopped reading its line fails its own calibration.  Imported
    # and upstream-verbatim source stays outside these roots; a file arriving
    # with an upstream header joins REAL_COPYRIGHT_HOLDERS by review.
    test(
      'r3v-source-header-audit',
      prog_python,
      args : [files('tests/r3v_source_header_audit.py'),
              meson.current_source_dir(),
              meson.project_source_root() / 'src/amd/r300/common',
              meson.project_source_root() / 'src/amd/radeon/drm_vk',
              meson.project_source_root() / 'build-infra/r3v'],
      suite : ['r3v'],
    )
    test(
      'r3v-source-header-audit-selftest',
      prog_python,
      args : [files('tests/r3v_source_header_audit.py'), '--selftest'],
      suite : ['r3v'],
    )
    foreach header_defect : ['missing-spdx', 'invented-copyright',
                             'ai-disclosure', 'missing-root']
      test(
        'r3v-source-header-audit-known-bad-@0@'.format(header_defect),
        prog_python,
        args : [files('tests/r3v_source_header_audit.py'),
                '--fixture', header_defect],
        should_fail : true,
        suite : ['r3v'],
      )
    endforeach
    # Qualification test-inventory gate: a build that silently registered
    # fewer tests must fail a qualification run rather than return a smaller
    # green suite.  The ordinary registration reports the inventory; the
    # zero-native fixture replays the build that once registered no native
    # tests and must refuse; the selftest proves both directions through the
    # same evaluation.  The fatal --qualification mode belongs to the
    # qualification wrapper, which supplies the replay and controls programs
    # this build cannot know.
    test(
      'r3v-qualification-inventory',
      prog_python,
      args : [files('tests/r3v_qualification_inventory.py'),
              meson.project_build_root()],
      suite : ['r3v'],
    )
    test(
      'r3v-qualification-inventory-selftest',
      prog_python,
      args : [files('tests/r3v_qualification_inventory.py'), '--selftest'],
      suite : ['r3v'],
    )
    test(
      'r3v-qualification-inventory-known-bad-zero-native',
      prog_python,
      args : [files('tests/r3v_qualification_inventory.py'),
              '--fixture', 'zero-native'],
      should_fail : true,
      suite : ['r3v'],
    )
    test(
      'r3v-native-entrypoint-closure',
      prog_python,
      args : [r3v_native_entrypoint_audit_args, '--enforce'],
      depends : libvulkan_r3v_native,
      suite : ['r3v'],
    )
    test(
      'r3v-native-public-surface-policy',
      prog_python,
      args : [r3v_native_entrypoint_audit_args, '--policy'],
      depends : libvulkan_r3v_native,
      suite : ['r3v'],
    )
    # Refusal-result legality spans the complete native core VkResult surface.
    # Removing the refusal result from one live command's registry set must
    # fail that verdict while the linked command surface remains intact.
    test(
      'r3v-native-refusal-result-known-bad-BeginCommandBuffer',
      prog_python,
      args : [r3v_native_entrypoint_audit_args, '--enforce',
              '--deny-refusal', 'BeginCommandBuffer'],
      depends : libvulkan_r3v_native,
      should_fail : true,
      suite : ['r3v'],
    )
    # Known-bad calibration for --enforce: removing one entrypoint from the
    # parsed surface must fail it.  Dropping a core 1.0 device command opens
    # the completeness verdict, and dropping a *2-form target opens the
    # closure verdict through the common bridge that dispatches into it.
    foreach dropped : ['CreateImage', 'BindBufferMemory2']
      test(
        'r3v-native-entrypoint-closure-known-bad-@0@'.format(dropped),
        prog_python,
        args : [r3v_native_entrypoint_audit_args, '--enforce',
                '--drop', dropped],
        depends : libvulkan_r3v_native,
        should_fail : true,
        suite : ['r3v'],
      )
    endforeach
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

  # Attended direct-write control: performs a live DRM_RADEON_CS on RS482
  # silicon under the authorization and procedure in
  # docs/hardware/r3v-native-direct-write-attended-procedure.md.  The
  # triangle attended cell keeps its own executable; each cell's digest
  # authorizes only its own stream.  Built and never registered as a
  # test, because running it is a hardware authorization rather than a
  # build-graph result.
  r3v_native_attended_direct_write = executable(
    'r3v_native_attended_direct_write',
    files('tests/r3v_native_attended_direct_write.c'),
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
    # The shim fixture as a dictionary so a test that needs one more
    # variable derives its environment instead of composing two.
    r3v_native_cell_vars = {
      'LD_PRELOAD' : libradeon_noop_drm_shim.full_path(),
      'RADEON_GPU_ID' : '0x5974',
      'DRM_SHIM_EXPECTED_DSO' : libradeon_noop_drm_shim.full_path(),
    }
    r3v_native_cell_env = environment(r3v_native_cell_vars)
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

    # Direct-write control harness on the same shim fixture: the 2D
    # solid-fill cell through the same recorder/queue/retention path,
    # armed by the control's own digest in the open leg.
    r3v_native_direct_write_harness = executable(
      'r3v_native_direct_write_harness',
      files('tests/r3v_native_direct_write_harness.c'),
      include_directories : r3v_native_inc,
      dependencies : [r3v_native_deps, dep_dl],
      link_whole : [libr3v_native_impl],
      link_with : [libradeon_drm_vk],
      c_args : [no_override_init_args],
      gnu_symbol_visibility : 'hidden',
    )
    foreach cell_mode : ['closed', 'open']
      test(
        'r3v-native-direct-write-@0@'.format(cell_mode),
        r3v_native_direct_write_harness,
        args : [cell_mode],
        env : r3v_native_cell_env,
        depends : [libradeon_noop_drm_shim],
        timeout : 60,
        suite : ['r3v'],
      )
    endforeach
    test(
      'r3v-native-submit-manifest',
      prog_python,
      args : [
        files('tests/r3v_native_submit_manifest_check.py'),
        r3v_native_direct_write_harness,
      ],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim, r3v_native_direct_write_harness],
      timeout : 60,
      suite : ['r3v'],
    )

    # The public-surface tables the sweeps measure, generated from the same
    # registry parse the policy audit reads.
    r3v_native_surface_table = custom_target(
      'r3v_native_surface_table',
      input : [files('tests/r3v_native_entrypoint_audit.py'), vk_api_xml],
      output : 'r3v_native_surface_table.c',
      command : [prog_python, '@INPUT0@', '--emit-c', '@INPUT1@', '@OUTPUT@'],
    )

    # Direct-table sweep: the ICD's own procaddr entrypoints with no loader
    # in the process, so a NULL names a table the driver built that way.
    r3v_native_direct_table_sweep = executable(
      'r3v_native_direct_table_sweep',
      [files('tests/r3v_native_direct_table_sweep.c'),
       r3v_native_surface_table],
      include_directories : [r3v_native_inc,
                             include_directories('tests')],
      dependencies : r3v_native_deps,
      link_whole : [libr3v_native_impl],
      link_with : [libradeon_drm_vk],
      c_args : [no_override_init_args],
      gnu_symbol_visibility : 'hidden',
    )
    test(
      'r3v-native-direct-table-sweep',
      r3v_native_direct_table_sweep,
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
    # Known-bad calibration for the absent legs, one per absent table: the
    # fixture appends a command the surface really provides as a row of the
    # named table, so the leak is judged by that table's own loop.  A leg that
    # stopped comparing its rows passes here, which is what gives all three
    # teeth against a promoted alias or a swapchain command reaching the
    # device table.
    foreach leak : [['higher-core', 'higher core:vkCmdDraw'],
                    ['promoted-alias', 'promoted alias:vkMapMemory'],
                    ['closed-extension', 'closed extension:vkQueueSubmit']]
      test(
        'r3v-native-direct-table-sweep-known-bad-@0@'.format(leak[0]),
        r3v_native_direct_table_sweep,
        env : environment(r3v_native_cell_vars + {
          'R3V_SURFACE_FIXTURE_LEAK' : leak[1],
        }),
        depends : [libradeon_noop_drm_shim],
        should_fail : true,
        timeout : 60,
        suite : ['r3v'],
      )
    endforeach

    # Surface-query-only WSI boundary: an X server backs the surface, so the
    # test registers where xcb and a virtual server are available and skips
    # at run time when no display answers.  Its absence stays visible as a
    # skip rather than a missing test.
    prog_xvfb = find_program('Xvfb', required : false)
    r3v_xvfb_wrapper = files('tests/run_under_xvfb.sh')
    if dep_xcb.found() and prog_xvfb.found()
      # The wrapper allocates the display through Xvfb -displayfd, returns
      # the wrapped binary's exact status, and reports infrastructure
      # failure as 125, so a should_fail expectation is satisfied only by
      # the binary's own verdict.  These four legs calibrate the wrapper
      # itself: status 0 and 1 pass through, 77 stays a skip, and a
      # startup failure refuses as 125 rather than any of those.
      test(
        'r3v-xvfb-wrapper-passthrough-success',
        r3v_xvfb_wrapper,
        args : ['true'],
        suite : ['r3v'],
      )
      test(
        'r3v-xvfb-wrapper-passthrough-failure',
        r3v_xvfb_wrapper,
        args : ['false'],
        should_fail : true,
        suite : ['r3v'],
      )
      test(
        'r3v-xvfb-wrapper-passthrough-skip',
        r3v_xvfb_wrapper,
        args : ['sh', '-c', 'exit 77'],
        suite : ['r3v'],
      )
      # The infrastructure verdict is exactly 125, asserted as an equality
      # rather than through should_fail, which any nonzero status but 77
      # would satisfy.
      test(
        'r3v-xvfb-wrapper-infrastructure-refusal',
        find_program('sh'),
        args : ['-c', 'R3V_XVFB_BINARY=/bin/false "$1" true; test $? -eq 125',
                'assert-125', r3v_xvfb_wrapper],
        suite : ['r3v'],
      )
      r3v_native_wsi_surface_contract = executable(
        'r3v_native_wsi_surface_contract',
        files('tests/r3v_native_wsi_surface_contract.c'),
        include_directories : r3v_native_inc,
        dependencies : [r3v_native_deps, dep_xcb],
        link_whole : [libr3v_native_impl],
        link_with : [libradeon_drm_vk],
        c_args : [no_override_init_args],
        gnu_symbol_visibility : 'hidden',
      )
      test(
        'r3v-native-wsi-surface-contract',
        r3v_xvfb_wrapper,
        args : [r3v_native_wsi_surface_contract.full_path()],
        env : r3v_native_cell_env,
        depends : [libradeon_noop_drm_shim],
        timeout : 120,
        suite : ['r3v'],
      )
      # Query-verdict calibration: the fixture replaces one loaded query
      # pointer with a stub that reports an error, and the run then reaches
      # the same result checks as the ordinary one.  A check that stopped
      # judging its result passes both, which is what these three catch.
      foreach wsi_query : ['capabilities', 'formats', 'modes']
        # --expect-status 1 turns the leg into a plain test only the
        # fixture's own failing verdict satisfies; an Xvfb infrastructure
        # failure surfaces as 125 instead of standing in for it.
        test(
          'r3v-native-wsi-surface-contract-known-bad-@0@'.format(wsi_query),
          r3v_xvfb_wrapper,
          args : ['--expect-status', '1',
                  r3v_native_wsi_surface_contract.full_path()],
          env : environment(r3v_native_cell_vars + {
            'R3V_WSI_FIXTURE_QUERY_ERROR' : wsi_query,
          }),
          depends : [libradeon_noop_drm_shim],
          timeout : 120,
          suite : ['r3v'],
        )
      endforeach
    endif

    # Loader black-box sweep: the real Vulkan loader resolves and calls the
    # built ICD, so it measures what an application linked against libvulkan
    # reaches.  It registers where that loader is present.
    dep_vulkan_loader = cc.find_library('vulkan', required : false)
    if dep_vulkan_loader.found()
      r3v_native_loader_env = environment(r3v_native_cell_vars + {
        'VK_DRIVER_FILES' : _native_dev_icd.full_path(),
      })
      test(
        'r3v-native-loader-sweep',
        executable(
          'r3v_native_loader_sweep',
          [files('tests/r3v_native_loader_sweep.c'),
           r3v_native_surface_table],
          include_directories : [inc_include, inc_src,
                                 include_directories('tests')],
          dependencies : [dep_vulkan_loader, idep_mesautil],
          c_args : [no_override_init_args],
        ),
        env : r3v_native_loader_env,
        depends : [libradeon_noop_drm_shim, libvulkan_r3v_native,
                   _native_dev_icd],
        timeout : 120,
        suite : ['r3v'],
      )
    endif
    # Arming-gate calibration: the complete fact set arms, and each
    # single-factor defect refuses with its own verdict.  The evaluator is
    # pure, so the test compiles it directly.
    test(
      'r3v-native-identity',
      executable(
        'r3v_native_identity_test',
        files('r3v_native_identity.c', 'tests/r3v_native_identity_test.c'),
        include_directories : [inc_include, inc_src,
                               include_directories('.')],
        dependencies : [idep_mesautil],
        gnu_symbol_visibility : 'hidden',
      ),
      suite : ['r3v'],
    )

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

    # Positive ARMED calibration through the collection seam: a
    # deterministic provider supplies every exact fact, the verdict is
    # ARMED with exit status zero, and each single-fact mutation refuses
    # through the same collect-then-evaluate path.  The binary links the
    # arming translation unit alone, so no Vulkan object, DRM fd, ioctl,
    # or attempt token can exist in the run.
    test(
      'r3v-native-arming-positive',
      executable(
        'r3v_native_arming_positive',
        files('r3v_native_arming.c', 'tests/r3v_native_arming_positive.c'),
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
            '../common/r300_pm4_builder.c',
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

    # Loader-only application gate: the complete public sequence of an
    # external Vulkan program, linked against the installed loader alone
    # and reaching the ICD only through its manifest.  The check wrapper
    # byte-compares the submit-retained ib.bin against the arming
    # runner's independent reference emission, so the recorded-cell
    # equality claim spans the loader boundary; the mutate fixture
    # calibrates that the comparison judges bytes.
    if dep_vulkan_loader.found()
      r3v_native_loader_application = executable(
        'r3v_native_loader_application',
        files('tests/r3v_native_loader_application.c'),
        include_directories : [inc_include, include_directories('.')],
        dependencies : [dep_vulkan_loader],
        c_args : [no_override_init_args],
      )
      r3v_native_loader_app_vars = r3v_native_cell_vars + {
        'VK_DRIVER_FILES' : _native_dev_icd.full_path(),
        'R3V_EXPECTED_ICD_DSO' : libvulkan_r3v_native.full_path(),
      }
      test(
        'r3v-native-loader-application',
        prog_python,
        args : [
          files('tests/r3v_native_loader_application_check.py'),
          r3v_native_loader_application,
          r3v_native_arming_runner,
        ],
        env : environment(r3v_native_loader_app_vars),
        depends : [libradeon_noop_drm_shim, libvulkan_r3v_native,
                   _native_dev_icd, r3v_native_loader_application,
                   r3v_native_arming_runner],
        timeout : 120,
        suite : ['r3v'],
      )
      # The known-bad legs assert the judged-mismatch status 1 exactly:
      # the check scripts report infrastructure failure as 2, so a
      # crash, a missing tool, or a failed reference emission refuses
      # the calibration instead of standing in for its verdict.
      test(
        'r3v-native-loader-application-known-bad-ib',
        find_program('sh'),
        args : [
          '-c', '"$1" "$2" "$3" "$4"; test $? -eq 1', 'assert-verdict',
          prog_python.full_path(),
          files('tests/r3v_native_loader_application_check.py'),
          r3v_native_loader_application.full_path(),
          r3v_native_arming_runner.full_path(),
        ],
        env : environment(r3v_native_loader_app_vars + {
          'R3V_LOADER_APP_FIXTURE_MUTATE_IB' : '1',
        }),
        depends : [libradeon_noop_drm_shim, libvulkan_r3v_native,
                   _native_dev_icd, r3v_native_loader_application,
                   r3v_native_arming_runner],
        timeout : 120,
        suite : ['r3v'],
      )

      # Readback-oracle calibration: each corrupt fixture writes one
      # deviating dword through the application's own mapping before
      # the sweep, and the leg asserts the oracle's own status 3
      # exactly, so a sweep that stopped judging bytes fails here.
      foreach oracle_leak : ['CORRUPT_FOOTPRINT', 'CORRUPT_TAIL']
        test(
          'r3v-native-loader-application-known-bad-@0@'.format(
            oracle_leak.to_lower()),
          find_program('sh'),
          args : [
            '-c',
            'd=$(mktemp -d) && R3V_NATIVE_MANIFEST_DIR="$d" "$1"; s=$?; ' +
            'rm -rf "$d"; test $s -eq 3',
            'assert-oracle',
            r3v_native_loader_application.full_path(),
          ],
          env : environment(r3v_native_loader_app_vars + {
            'R3V_LOADER_APP_FIXTURE_@0@'.format(oracle_leak) : '1',
          }),
          depends : [libradeon_noop_drm_shim, libvulkan_r3v_native,
                     _native_dev_icd, r3v_native_loader_application],
          timeout : 120,
          suite : ['r3v'],
        )
      endforeach

      # The application's black-box claim as an executable audit: no
      # driver symbol in any binding, and the Vulkan surface imported
      # from the loader.  The known-bad leg runs the same audit against
      # the triangle-cell harness, which links the implementation.
      if prog_nm_r3v.found()
        test(
          'r3v-native-loader-application-symbols',
          prog_python,
          args : [
            files('tests/r3v_native_loader_application_symbol_audit.py'),
            prog_nm_r3v.full_path(),
            r3v_native_loader_application.full_path(),
          ],
          depends : [r3v_native_loader_application],
          suite : ['r3v'],
        )
        test(
          'r3v-native-loader-application-symbols-known-bad',
          find_program('sh'),
          args : [
            '-c', '"$1" "$2" "$3" "$4"; test $? -eq 1', 'assert-verdict',
            prog_python.full_path(),
            files('tests/r3v_native_loader_application_symbol_audit.py'),
            prog_nm_r3v.full_path(),
            r3v_native_triangle_cell_harness.full_path(),
          ],
          depends : [r3v_native_triangle_cell_harness],
          suite : ['r3v'],
        )
      endif
    endif

    # Carrier-delivery harness: the stream-fed recorder reproduces the
    # frozen carrier bytes and cell IB for every F32 delivery shape;
    # recording-only, gate closed, no submission.
    test(
      'r3v-native-vertex-carrier',
      executable(
        'r3v_native_vertex_carrier_harness',
        files('tests/r3v_native_vertex_carrier_harness.c'),
        include_directories : r3v_native_inc,
        dependencies : r3v_native_deps,
        link_whole : [libr3v_native_impl],
        link_with : [libradeon_drm_vk],
        c_args : [no_override_init_args],
        gnu_symbol_visibility : 'hidden',
      ),
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )

    # Public recording surface on the drm-shim fixture: the
    # application-shaped render-pass/pipeline/draw sequence records the
    # qualified cell through public entry points, and every contract
    # deviation refuses.  The gate stays closed and no ioctl runs; the
    # harness drives the deferred draw, so the vertex gather and the
    # sentinel clear write host memory.
    test(
      'r3v-native-public-surface',
      executable(
        'r3v_native_public_surface_harness',
        files('tests/r3v_native_public_surface_harness.c'),
        include_directories : r3v_native_inc,
        dependencies : r3v_native_deps,
        link_whole : [libr3v_native_impl],
        link_with : [libradeon_drm_vk],
        c_args : [no_override_init_args],
        gnu_symbol_visibility : 'hidden',
      ),
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )

    # Non-submitting direct-write arming runner: reports the control
    # cell's digest and every arming factor, then stops at the
    # authorization boundary.  The check drives the refusal legs and the
    # wrong-cell negative against the triangle runner's digest.
    r3v_native_direct_write_arming_runner = executable(
      'r3v_native_direct_write_arming_runner',
      files('r3v_native_arming.c',
            'tests/r3v_native_direct_write_arming_runner.c',
            '../common/r300_direct_write.c',
            '../common/r300_first_draw_state.c',
            '../common/r300_fragment_binary.c',
            '../common/r300_pm4_builder.c',
            '../common/r300_tcl_bypass_triangle.c'),
      include_directories : [r3v_native_inc, include_directories('.')],
      dependencies : [idep_mesautil],
      gnu_symbol_visibility : 'hidden',
    )
    test(
      'r3v-native-direct-write-arming-runner',
      prog_python,
      args : [
        files('tests/r3v_native_direct_write_arming_runner_check.py'),
        r3v_native_direct_write_arming_runner,
        r3v_native_arming_runner,
      ],
      suite : ['r3v'],
    )

    # Positive ARMED calibration under the direct-write control's own
    # digest, through the collection seam; each single-fact mutation
    # refuses, and the triangle digest refuses as the wrong cell.
    test(
      'r3v-native-direct-write-arming-positive',
      executable(
        'r3v_native_direct_write_arming_positive',
        files('r3v_native_arming.c',
              'tests/r3v_native_direct_write_arming_positive.c',
              '../common/r300_direct_write.c',
              '../common/r300_first_draw_state.c',
              '../common/r300_fragment_binary.c',
              '../common/r300_pm4_builder.c',
              '../common/r300_tcl_bypass_triangle.c'),
        include_directories : [r3v_native_inc, include_directories('.')],
        dependencies : [idep_mesautil],
        gnu_symbol_visibility : 'hidden',
      ),
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
      'r3v-native-recording-poison',
      r3v_native_triangle_cell_harness,
      args : ['poison'],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
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
    # The direct-write control's exact-submit-object replay, through the
    # full CS parser/tracker.  Runtime-gated on R3V_CS_TRACK_REPLAY_TOOL
    # (skip 77 when unset).
    test(
      'r3v-native-direct-write-submit-object-replay',
      files('tests/run_direct_write_submit_object_replay.sh')[0],
      args : [r3v_native_direct_write_harness],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
    # Authority parity: one digest names the direct-write cell across the
    # standalone manifest writer, the arming runner, and the
    # queue-retained IB from both harness legs, and each attended runner
    # calls only its own cell's recorder.  The manifest writer builds
    # under the r300 Gallium test tree, so the check registers only where
    # that target exists.
    if is_variable('r300_direct_write_manifest')
      test(
        'r3v-native-direct-write-authority-parity',
        prog_python,
        args : [
          files('tests/r3v_native_direct_write_authority_parity.py'),
          r300_direct_write_manifest,
          r3v_native_direct_write_arming_runner,
          r3v_native_direct_write_harness,
          files('tests/r3v_native_attended_direct_write.c'),
          files('tests/r3v_native_attended_cell.c'),
        ],
        env : r3v_native_cell_env,
        depends : [libradeon_noop_drm_shim],
        timeout : 120,
        suite : ['r3v'],
      )
    endif
  endif
endif
