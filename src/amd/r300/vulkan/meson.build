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

# The loader-base files: instance, physical device, and the CPU-timeline
# sync type; the native ICD below builds them over the Radeon DRM transport.
r3v_loader_base_files = files(
  'r3v_instance.c',
  'r3v_physical_device.c',
  'r3v_cpu_sync.c',
)

# Memory-property contract: the native heap stays under the RS48x platform
# ceiling and every advertised property keeps its heap and host-mapping
# backing.  The checker is a pure function over the reported table, so this
# test runs in every configuration and calibrates on known-bad tables.
if with_tests
  test(
    'r3v-memory-properties-contract',
    executable(
      'r3v_memory_properties_contract_test',
      files('tests/r3v_memory_properties_contract_test.c'),
      include_directories : [inc_include, include_directories('.')],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )

  # Native pipeline front end: the reference cell modules admit
  # through the direct SPIR-V word-stream reader by meaning, admitted
  # jobs execute to exact bytes, and inadmissible or malformed streams
  # refuse by shape.  The admitter is plain C over words, so the test
  # carries no compiler-stack dependency.
  test(
    'r3v-native-pipeline-frontend',
    executable(
      'r3v_native_pipeline_frontend_test',
      files('tests/r3v_native_pipeline_frontend_test.c',
            '../common/r300_vertex_spirv.c',
            '../cpu/r300_cpu_vertex.c', '../cpu/r300_cpu_vertex_job.c'),
      include_directories : [inc_include, inc_src,
                             include_directories('.')],
      dependencies : [dep_m],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )

  # Direct SPIR-V compute admission: the reference identity-map module
  # admits by meaning and executes to exact bytes, the reference
  # scatter module refuses, and malformed word streams refuse by
  # bounds.  The admitter is plain C over words, so the test carries no
  # compiler-stack dependency.
  test(
    'r3v-native-compute-frontend',
    executable(
      'r3v_native_compute_frontend_test',
      files('tests/r3v_native_compute_frontend_test.c',
            '../common/r300_compute_spirv.c',
            '../cpu/r300_cpu_compute_job.c'),
      include_directories : [inc_include, inc_src,
                             include_directories('.')],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
endif

icd_lib_path = join_paths(get_option('prefix'), get_option('libdir'))
# The r3v ICD: libvulkan_r3v builds the loader-base files over the generic
# Radeon DRM transport with no Gallium runtime ownership; the
# separation-audit test proves the library carries no Gallium entry symbol.
r3v_native_files = files(
  'r3v_native_arming.c',
  'r3v_native_cell.c',
  'r3v_native_cmd.c',
  'r3v_native_compute.c',
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
)

# The ICD compiles with no Gallium include root: PIPE_FORMAT_* comes from
# util/format through idep_mesautil, and the separation-audit tests prove
# the sources, compile commands, and linked library stay free of Gallium
# includes and symbols.
r3v_native_inc = [
  inc_include,
  inc_src,
  inc_amd_common,
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
  link_with : [libradeon_drm_vk, libr300_common, libr300_cpu],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
)

libvulkan_r3v = shared_library(
  'vulkan_r3v',
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

_dev_icd = custom_target(
  'r3v_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'r3v_devenv_icd.@0@.json'.format(host_machine.cpu()),
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
devenv.append('VK_DRIVER_FILES', _dev_icd.full_path())

# Executable form of the boundaries doc's dependency/symbol audit: the
# native library defines no Gallium or winsys entry symbol, in any binding.
prog_nm_r3v = find_program('nm', required : false)
if with_tests
  r300_common_boundary_audit = files(
    '../common/tests/r300_common_boundary_audit.py')
  test(
    'r3v-native-gallium-separation-selftest',
    prog_python,
    args : [files('tests/r3v_native_separation_audit.py'), '--selftest'],
    suite : ['r3v'],
  )
  test(
    'r3v-native-common-boundary-sources',
    prog_python,
    args : [r300_common_boundary_audit, '--sources', 'native']
           + r3v_loader_base_files + r3v_native_files + r300_cpu_files
           + [r3v_entrypoints[0], r3v_entrypoints[1]]
           + files(
      'r3v_cpu_sync.h',
      'r3v_format.h',
      'r3v_instance.h',
      'r3v_memory_properties_contract.h',
      'r3v_native.h',
      'r3v_native_arming.h',
      'r3v_native_identity.h',
      'r3v_native_reference_spirv.h',
      'r3v_physical_device.h',
      'r3v_private.h',
      ),
    depends : r3v_entrypoints,
    suite : ['r3v'],
  )

  # Executable form of the boundaries doc's completion criterion that
  # feature and extension tables come from implemented behavior: every
  # device extension the native branch advertises names the native
  # route that executes it, and the native feature set stays empty.
  test(
    'r3v-native-advertised-surface-selftest',
    prog_python,
    args : [files('tests/r3v_native_advertised_surface_audit.py'),
            '--selftest'],
    suite : ['r3v'],
  )
  test(
    'r3v-native-advertised-surface',
    prog_python,
    args : [files('tests/r3v_native_advertised_surface_audit.py'),
            '--source', files('r3v_physical_device.c')],
    suite : ['r3v'],
  )
  # Source layer: the ICD's sources carry no Gallium include and no
  # Gallium runtime identifier on any line the compile keeps.
  test(
    'r3v-native-gallium-separation-sources',
    prog_python,
    args : [files('tests/r3v_native_separation_audit.py'), '--sources']
           + r3v_loader_base_files + r3v_native_files + r300_cpu_files
           + files(
      'r3v_cpu_sync.h',
      'r3v_format.h',
      'r3v_instance.h',
      'r3v_memory_properties_contract.h',
      'r3v_native.h',
      'r3v_native_arming.h',
      'r3v_native_identity.h',
      'r3v_native_reference_spirv.h',
      'r3v_physical_device.h',
    ),
    suite : ['r3v'],
  )
  # Compile-command layer: no object of libr3v_native_impl compiles
  # with a Gallium include dir; fails closed when the database or the
  # native objects are absent.
  test(
    'r3v-native-gallium-separation-compile-commands',
    prog_python,
    args : [
      files('tests/r3v_native_separation_audit.py'),
      '--compile-commands',
      meson.global_build_root() / 'compile_commands.json',
      'r3v_native_impl',
    ],
    depends : libr3v_native_impl,
    suite : ['r3v'],
  )
  # The native implementation links libr300_cpu directly.  Its compile
  # command receives the same Gallium-independence gate as native-owned
  # objects, so sharing the exact benchmarked object preserves separation.
  test(
    'r3v-native-gallium-separation-cpu-compile-commands',
    prog_python,
    args : [
      files('tests/r3v_native_separation_audit.py'),
      '--compile-commands',
      meson.global_build_root() / 'compile_commands.json',
      'libr300_cpu',
    ],
    depends : libr300_cpu,
    suite : ['r3v'],
  )
endif
prog_readelf_r3v = find_program('readelf', required : false)
if with_tests and prog_readelf_r3v.found()
  # Dynamic layer: the linked ICD's dynamic section names no Gallium
  # library; fails closed when the artifact is absent.
  test(
    'r3v-native-gallium-separation-dynamic',
    prog_python,
    args : [
      files('tests/r3v_native_separation_audit.py'),
      '--dynamic',
      prog_readelf_r3v.full_path(),
      libvulkan_r3v.full_path(),
    ],
    depends : libvulkan_r3v,
    suite : ['r3v'],
  )
endif
if with_tests and prog_nm_r3v.found()
  # Undefined references are the object-level boundary: an inline or
  # generated source path cannot hide a reachable NIR/Gallium dependency.
  test(
    'r3v-native-common-boundary-objects',
    prog_python,
    args : [
      r300_common_boundary_audit,
      '--objects',
      prog_nm_r3v.full_path(),
      libr3v_native_impl.full_path(),
    ],
    depends : libr3v_native_impl,
    suite : ['r3v'],
  )
  # The implementation archive check catches undefined forbidden
  # dependencies before resolution; this final-artifact leg separately
  # proves that none remain unresolved after the installed ICD is composed.
  # The adjacent full-symbol separation audit covers forbidden definitions.
  test(
    'r3v-native-common-boundary-final-dso',
    prog_python,
    args : [
      r300_common_boundary_audit,
      '--objects',
      prog_nm_r3v.full_path(),
      libvulkan_r3v.full_path(),
    ],
    depends : libvulkan_r3v,
    suite : ['r3v'],
  )
  test(
    'r3v-native-gallium-separation',
    prog_python,
    args : [
      files('tests/r3v_native_separation_audit.py'),
      prog_nm_r3v.full_path(),
      libvulkan_r3v.full_path(),
    ],
    depends : libvulkan_r3v,
    suite : ['r3v'],
  )
  # Known-bad calibration: a synthetic shared object exporting
  # r300_screen_create, one symbol of the audit's forbidden set, so the
  # audit refuses it with no Gallium library in the build.
  r3v_native_separation_known_bad = shared_library(
    'r3v_native_separation_known_bad',
    files('tests/fixtures/r3v_native_separation_known_bad.c'),
    include_directories : [inc_include, inc_src],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-gallium-separation-known-bad',
    prog_python,
    args : [
      files('tests/r3v_native_separation_audit.py'),
      '--expect-status', '1',
      prog_nm_r3v.full_path(),
      r3v_native_separation_known_bad.full_path(),
    ],
    depends : r3v_native_separation_known_bad,
    suite : ['r3v'],
  )

  # Dispatch-closure audit: the common overlay populates 1.0 bridge slots
  # whose downstream dispatch targets the native link set does not carry,
  # so a populated entrypoint can call through a null slot.  The selftest
  # proves both audit directions on synthetic tables; the baseline test
  # pins the known-open bridge families so the audit stays calibrated;
  # the enforce gate flips to a plain pass when the native table closes.
  r3v_native_entrypoint_audit_args = [
    files('tests/r3v_native_entrypoint_audit.py'),
    prog_nm_r3v.full_path(),
    libvulkan_r3v.full_path(),
    vk_api_xml,
    meson.project_source_root() / 'src' / 'vulkan' / 'runtime',
  ]
  test(
    'r3v-native-entrypoint-audit-selftest',
    prog_python,
    args : [files('tests/r3v_native_entrypoint_audit.py'), '--selftest'],
    suite : ['r3v'],
  )
  # The merged native table is closed, so the calibrated baseline is the
  # exact empty open-slot set.  The known-bad leg expects one slot but opens
  # two, proving that unexpected additions fail the baseline comparator.
  test(
    'r3v-native-entrypoint-audit-baseline',
    prog_python,
    args : [r3v_native_entrypoint_audit_args, '--baseline', ''],
    depends : libvulkan_r3v,
    suite : ['r3v'],
  )
  test(
    'r3v-native-entrypoint-audit-baseline-known-bad-extra',
    prog_python,
    args : [r3v_native_entrypoint_audit_args, '--baseline',
            'BindBufferMemory', '--drop', 'BindBufferMemory2',
            '--drop', 'CreateRenderPass2'],
    depends : libvulkan_r3v,
    should_fail : true,
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
    depends : libvulkan_r3v,
    suite : ['r3v'],
  )
  test(
    'r3v-native-public-surface-policy',
    prog_python,
    args : [r3v_native_entrypoint_audit_args, '--policy'],
    depends : libvulkan_r3v,
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
    depends : libvulkan_r3v,
    should_fail : true,
    suite : ['r3v'],
  )
  test(
    'r3v-native-refusal-result-known-bad-FlushMappedMemoryRanges',
    prog_python,
    args : [r3v_native_entrypoint_audit_args, '--enforce',
            '--deny-refusal', 'FlushMappedMemoryRanges'],
    depends : libvulkan_r3v,
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
      depends : libvulkan_r3v,
      should_fail : true,
      suite : ['r3v'],
    )
  endforeach
endif

# Source-header auditing uses Python source inspection and keeps its
# known-bad fixtures available when the optional nm-based symbol audit is
# unavailable.
if with_tests
  test(
    'r3v-source-header-audit',
    prog_python,
    args : [files('tests/r3v_source_header_audit.py'),
            meson.current_source_dir(),
            meson.project_source_root() / 'src/amd/r300/common',
            meson.project_source_root() / 'src/amd/r300/cpu',
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

  # Rename and identity ratchet: every surviving pre-rename or lane-identity
  # spelling is a ledger row (path, token, replacement, owner, removal
  # condition) and a row leaves with its spelling, so the count only falls.
  test(
    'r3v-rename-ratchet',
    prog_python,
    args : [files('tests/r3v_rename_ratchet.py'),
            meson.project_source_root()],
    suite : ['r3v'],
  )
  test(
    'r3v-rename-ratchet-selftest',
    prog_python,
    args : [files('tests/r3v_rename_ratchet.py'), '--selftest'],
    suite : ['r3v'],
  )
  foreach header_defect : ['missing-spdx', 'invented-copyright',
                           'invented-project-collective-copyright',
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

# Attended depth control: performs a live DRM_RADEON_CS on RS482
# silicon drawing the two-triangle depth-test cell, then retains the
# color target and the depth surface and classifies both oracles.  Its
# first run is the B14 depth-control workload debut, unobserved by any
# reader.  Built and never registered as a test, because running it is
# a hardware authorization rather than a build-graph result.
r3v_native_attended_zb_depth_control = executable(
  'r3v_native_attended_zb_depth_control',
  files('tests/r3v_native_attended_zb_depth_control.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended producer-only cell: performs a live DRM_RADEON_CS on RS482
# silicon, writing the vertex carrier alone -- no visible target and no
# re-ingest -- so the CPU read-back of the carrier against the delivery
# identity, with the poison retained past the delivered extent, is the
# whole verdict.  Built and never registered as a test, because running
# it is a hardware authorization rather than a build-graph result.
r3v_native_attended_producer = executable(
  'r3v_native_attended_producer',
  files('tests/r3v_native_attended_producer.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

r3v_native_attended_reingest = executable(
  'r3v_native_attended_reingest',
  files('tests/r3v_native_attended_reingest.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended FLOAT_4 + FLOAT_2 tuple cell: performs a live DRM_RADEON_CS
# on RS482 silicon, fetching the two-array vertex stream through the
# PSC XY01 selector into the carrier -- the read-back judges the
# silicon's expansion of each two-dword record to (x, y, 0, 1).  Built
# and never registered as a test, because running it is a hardware
# authorization rather than a build-graph result.
r3v_native_attended_float2_tuple = executable(
  'r3v_native_attended_float2_tuple',
  files('tests/r3v_native_attended_float2_tuple.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended public GPU-producer cell: drives the application-shaped
# Vulkan surface so the composed route -- producer over the
# application's records ahead of the consumer cell -- executes on
# silicon, with the driver's carrier read-back and this runner's
# color-target oracle as two independent verdicts.  Built and never
# registered as a test, because running it is a hardware authorization
# rather than a build-graph result.
r3v_native_attended_public_gpu_producer = executable(
  'r3v_native_attended_public_gpu_producer',
  files('tests/r3v_native_attended_public_gpu_producer.c',
        '../common/r300_r2vb_public_route.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended route dispatch timing cell: one declared-route submission
# with CLOCK_MONOTONIC_RAW readings at the recording boundary and
# around the synchronous vkQueueSubmit, so the CPU and GPU delivery
# routes earn comparable timing rows on verified bytes.  Built and
# never registered as a test, because running it is a hardware
# authorization rather than a build-graph result.
r3v_native_attended_route_dispatch_timing = executable(
  'r3v_native_attended_route_dispatch_timing',
  files('tests/r3v_native_attended_route_dispatch_timing.c',
        '../common/r300_r2vb_public_route.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended serial status-load cell: submits the frozen tuple stream up
# to the declared serial bound while the paired-status census samples,
# coordinated over a SOCK_SEQPACKET barrier channel by the status-load
# submitter machine.  Built and never registered as a test, because
# running it is a hardware authorization rather than a build-graph
# result.
r3v_native_attended_status_load = executable(
  'r3v_native_attended_status_load_float2_tuple',
  files('tests/r3v_native_attended_status_load_float2_tuple.c',
        'tests/r3v_native_status_load_machine.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

if with_tests
  # Pure arming checks compile without drm-shim or an X server. Keeping
  # them at the native backend boundary preserves calibration in native
  # builds that omit the optional runtime harnesses.
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
  # The producer cell's recording contract on the host: the installed
  # stream, the carrier relocation's two GTT domains, the poison
  # contract's positive and negative sides, and the arming gate's cell
  # kind.  Installation and emission touch no device, so the checks run
  # with no DRM node present.
  test(
    'r3v-native-producer-cell',
    executable(
      'r3v_native_producer_cell_test',
      files('tests/r3v_native_producer_cell_test.c'),
      include_directories : [r3v_native_inc, include_directories('.')],
      dependencies : r3v_native_deps,
      link_whole : [libr3v_native_impl],
      link_with : [libradeon_drm_vk],
      c_args : [no_override_init_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
  # The tuple cell's recording contract on the host: the installed
  # stream, the carrier's two GTT domains beside the read-only vertex
  # relocation, the arming gate's tuple kind, and the vertex-stream
  # capacity boundary the recorder's allocation gate mirrors.
  test(
    'r3v-native-float2-tuple-cell',
    executable(
      'r3v_native_float2_tuple_cell_test',
      files('tests/r3v_native_float2_tuple_cell_test.c'),
      include_directories : [r3v_native_inc, include_directories('.')],
      dependencies : r3v_native_deps,
      link_whole : [libr3v_native_impl],
      link_with : [libradeon_drm_vk],
      c_args : [no_override_init_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
  r3v_native_queue_status_test = executable(
    'r3v_native_queue_status_test',
    files('tests/r3v_native_queue_status_test.c'),
    include_directories : r3v_native_inc,
    dependencies : r3v_native_deps,
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-queue-status',
    r3v_native_queue_status_test,
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
  # The serial status-load submitter machine validates entirely against
  # scripted fakes: the queue-call and raw transport ladder, the
  # sampler-ready gate before QUEUE_SUBMIT_ENTER, and the absorbing abort,
  # with no device present.
  test(
    'r3v-native-status-load-machine',
    executable(
      'r3v_native_status_load_machine_test',
      files('tests/r3v_native_status_load_machine.c',
            'tests/r3v_native_status_load_machine_test.c'),
      include_directories : [inc_include, inc_src,
                             include_directories('.')],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )

  # The runner emits the contract-prefixed reference cell and stops before
  # any Vulkan or DRM operation. Its refusal calibration is independent of
  # the optional shim harness.
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
endif

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

  # Producer-cell harness on the same shim fixture: the R2VB producer
  # pass through the same recorder/queue/retention path.  The shim
  # absorbs DRM_RADEON_CS and executes nothing, so the open leg holds an
  # accepted transport beside a carrier that still carries the
  # recorder's poison; the geometry leg binds a carrier outside the
  # frozen layout so the producer kind's geometry predicate is the one
  # arming factor that refuses.
  r3v_native_producer_cell_harness = executable(
    'r3v_native_producer_cell_harness',
    files('tests/r3v_native_producer_cell_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : [r3v_native_deps, dep_dl],
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach cell_mode : ['closed', 'open', 'geometry']
    test(
      'r3v-native-producer-cell-@0@'.format(cell_mode),
      r3v_native_producer_cell_harness,
      args : [cell_mode],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach

  # FLOAT_4 + FLOAT_2 tuple harness on the same shim fixture: two
  # command relocations plus completion cross the recorder/queue path.
  # The shim absorbs DRM_RADEON_CS, so the open leg pairs completed
  # transport with an unchanged poison carrier and byte-identical
  # read-only vertex source.  The geometry leg isolates the tuple
  # vertex-allocation extent factor.
  r3v_native_float2_tuple_cell_harness = executable(
    'r3v_native_float2_tuple_cell_harness',
    files('tests/r3v_native_float2_tuple_cell_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : [r3v_native_deps, dep_dl],
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach cell_mode : ['closed', 'open', 'geometry']
    test(
      'r3v-native-float2-tuple-cell-@0@'.format(cell_mode),
      r3v_native_float2_tuple_cell_harness,
      args : [cell_mode],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  # Serial-cell resubmission harness on the same shim fixture: one
  # recorded serial command buffer crosses vkQueueSubmit three times
  # under a declared bound of three, so every per-submission queue step
  # runs again over the same command buffer, and the exhausted bound
  # refuses the fourth.
  r3v_native_serial_cell_harness = executable(
    'r3v_native_serial_cell_harness',
    files('tests/r3v_native_serial_cell_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : [r3v_native_deps, dep_dl],
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-serial-cell-resubmission',
    r3v_native_serial_cell_harness,
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )

  # Burst-cell harness on the same shim fixture: the admission leg covers
  # the four-member composition and one-shot token, while the lifetime legs
  # drive the surviving native prepare/commit/reset state through distinct
  # command-buffer, submit-shape, pre-ioctl refusal, and teardown paths.
  r3v_native_burst_cell_harness = executable(
    'r3v_native_burst_cell_harness',
    files('tests/r3v_native_burst_cell_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : [r3v_native_deps, dep_dl],
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-burst-cell-admission',
    r3v_native_burst_cell_harness,
    args : ['admission'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  foreach lifetime_leg : [
    ['commit', 'commit'],
    ['reset', 'reset'],
    ['different-command-buffer', 'different-buffer'],
    ['submit-shape', 'submit-shape'],
    ['trace-refusal', 'trace-refusal'],
    ['teardown', 'teardown'],
  ]
    test(
      'r3v-native-submit-lifetime-@0@'.format(lifetime_leg[0]),
      r3v_native_burst_cell_harness,
      args : [lifetime_leg[1]],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  foreach known_bad_leg : [
    ['reset-release', 'known-bad-reset'],
    ['command-buffer-binding', 'known-bad-binding'],
    ['teardown-release', 'known-bad-teardown'],
  ]
    test(
      'r3v-native-submit-lifetime-known-bad-@0@'.format(known_bad_leg[0]),
      r3v_native_burst_cell_harness,
      args : [known_bad_leg[1]],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      should_fail : true,
      suite : ['r3v'],
    )
  endforeach

  # The harness owns a fresh manifest directory, so inherited evidence
  # paths cannot supply stale fixed-name artifacts to its verdict.
  test(
    'r3v-native-float2-tuple-cell-external-manifest-ignored',
    r3v_native_float2_tuple_cell_harness,
    args : ['closed'],
    env : environment(r3v_native_cell_vars + {
      'R3V_NATIVE_MANIFEST_DIR' : meson.current_build_dir(),
    }),
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )

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
  # Depth control harness on the same shim fixture: the two-triangle
  # depth-test cell through the same recorder/queue/retention path,
  # armed by the cell's own digest in the open leg.
  r3v_native_zb_depth_control_harness = executable(
    'r3v_native_zb_depth_control_harness',
    files('tests/r3v_native_zb_depth_control_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : [r3v_native_deps, dep_dl],
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach cell_mode : ['closed', 'open']
    test(
      'r3v-native-zb-depth-control-@0@'.format(cell_mode),
      r3v_native_zb_depth_control_harness,
      args : [cell_mode],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach

  test(
    'r3v-native-queue-transport-status',
    r3v_native_queue_status_test,
    args : [r3v_native_direct_write_harness],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim, r3v_native_direct_write_harness],
    timeout : 180,
    suite : ['r3v'],
  )
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
  test(
    'r3v-xvfb-wrapper-signal-cleanup',
    prog_python,
    args : [files('tests/r3v_xvfb_wrapper_signal_cleanup.py'),
            r3v_xvfb_wrapper],
    suite : ['r3v'],
  )
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
    # Query-verdict calibration: each fixture replaces one loaded query
    # pointer and leaves every other query on the implementation path.
    # Direct query-error legs calibrate result checks, while the full-array
    # and short-array legs each make only their named count contract fail.
    foreach wsi_query : ['capabilities', 'capabilities-error',
                         'formats-full', 'formats-short', 'formats-error',
                         'modes-full', 'modes-short', 'modes-error']
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
      'VK_DRIVER_FILES' : _dev_icd.full_path(),
    })
    r3v_native_format_features_test = executable(
      'r3v_native_format_features_test',
      files('tests/r3v_native_format_features_test.c'),
      include_directories : [inc_include],
      dependencies : [dep_vulkan_loader],
      c_args : [no_override_init_args],
    )
    test(
      'r3v-native-format-features',
      r3v_native_format_features_test,
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      timeout : 120,
      suite : ['r3v'],
    )
    test(
      'r3v-native-format-features-known-bad-texel-buffer',
      r3v_native_format_features_test,
      args : ['--inject-uniform-texel-buffer'],
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      should_fail : true,
      timeout : 120,
      suite : ['r3v'],
    )
    test(
      'r3v-native-format-features-known-bad-query-disagreement',
      r3v_native_format_features_test,
      args : ['--inject-properties2-disagreement'],
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      should_fail : true,
      timeout : 120,
      suite : ['r3v'],
    )
    # Descriptor contract through the public surface: layout and pool
    # admission, pool capacity across free and reset, the descriptor
    # offset as the executed base, VK_WHOLE_SIZE clipped at the buffer
    # end, and poison-on-void-update refusing the dispatch recording.
    # Two calibrated mutations report an over-range write as admitted
    # and a past-capacity allocation as admitted.
    r3v_native_descriptor_test = executable(
      'r3v_native_descriptor_test',
      files('tests/r3v_native_descriptor_test.c'),
      include_directories : [inc_include, include_directories('.')],
      dependencies : [dep_vulkan_loader],
      c_args : [no_override_init_args],
    )
    test(
      'r3v-native-descriptor',
      r3v_native_descriptor_test,
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      timeout : 120,
      suite : ['r3v'],
    )
    test(
      'r3v-native-descriptor-known-bad-unpoisoned-oversize-write',
      r3v_native_descriptor_test,
      args : ['--inject-unpoisoned-oversize-write'],
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      should_fail : true,
      timeout : 120,
      suite : ['r3v'],
    )
    test(
      'r3v-native-descriptor-known-bad-pool-overflow-admits',
      r3v_native_descriptor_test,
      args : ['--inject-pool-overflow-admits'],
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      should_fail : true,
      timeout : 120,
      suite : ['r3v'],
    )
    # End-to-end dispatch through the native compute surface: the
    # gate-off leg proves the family withholds COMPUTE and pipeline
    # creation refuses; the gate-on leg drives the reference
    # identity-map module through descriptors, recording, and
    # submission to exact output bytes under the drm-shim transport.
    test(
      'r3v-native-compute-dispatch',
      executable(
        'r3v_native_compute_dispatch_test',
        files('tests/r3v_native_compute_dispatch_test.c'),
        include_directories : [inc_include, inc_src,
                               include_directories('.')],
        dependencies : [dep_vulkan_loader, idep_mesautil],
        c_args : [no_override_init_args],
      ),
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      timeout : 120,
      suite : ['r3v'],
    )

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
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      timeout : 120,
      suite : ['r3v'],
    )
  endif
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
      'VK_DRIVER_FILES' : _dev_icd.full_path(),
      'R3V_EXPECTED_ICD_DSO' : libvulkan_r3v.full_path(),
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
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd, r3v_native_loader_application,
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
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd, r3v_native_loader_application,
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
        depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                   _dev_icd, r3v_native_loader_application],
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

  # Recording calibration for the attended public GPU-producer cell on
  # the same fixture: the runner's own object construction and command
  # recording run to the recording boundary and stop, so a sequence
  # the driver's recording contract refuses fails here rather than on
  # an authorized hardware attempt.
  test(
    'r3v-native-public-gpu-producer-record',
    r3v_native_attended_public_gpu_producer,
    args : [meson.current_build_dir(), '--record-only'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The fetched cell shares the runner and the recording; its mode is the
  # composition the admission selects and the digest the run declares.
  test(
    'r3v-native-fetched-gpu-producer-record',
    r3v_native_attended_public_gpu_producer,
    args : [meson.current_build_dir(), '--record-only', '--fetched'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )

  # The timing runner's CPU-route authorization rests on the consumer
  # slice of the offline composition being the recorded consumer cell
  # verbatim; this host test pins that identity and the digest
  # separation between the two routes' declarations.
  r3v_native_route_timing_digest_test = executable(
    'r3v_native_route_timing_digest_test',
    files('tests/r3v_native_route_timing_digest_test.c',
          '../common/r300_r2vb_public_route.c'),
    include_directories : r3v_native_inc,
    dependencies : r3v_native_deps,
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-route-timing-digest',
    r3v_native_route_timing_digest_test,
    suite : ['r3v'],
  )
  # Calibration: one flipped consumer dword moves the CPU pin.
  test(
    'r3v-native-route-timing-digest-known-bad-consumer-drift',
    r3v_native_route_timing_digest_test,
    args : ['--inject-consumer-drift'],
    should_fail : true,
    suite : ['r3v'],
  )

  # Recording calibration for the timing runner on the fixture, one
  # leg per declared route, so a recording-contract deviation fails
  # here rather than on an authorized hardware attempt.
  test(
    'r3v-native-route-timing-record-cpu',
    r3v_native_attended_route_dispatch_timing,
    args : [meson.current_build_dir(), 'cpu', '--record-only'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  test(
    'r3v-native-route-timing-record-gpu',
    r3v_native_attended_route_dispatch_timing,
    args : [meson.current_build_dir(), 'gpu', '--record-only'],
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
      files('tests/r3v_native_public_surface_harness.c',
            '../common/r300_r2vb_public_route.c'),
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

  # Submit-order matrix: one public draw with a load-op clear per arm on a
  # fresh device; the armed arm executes and spends its token, every
  # pre-commit failure (closed gate, unwritable evidence directory, refused
  # authorization, failed vertex mapping) leaves target and carrier
  # untouched with the authorization unspent, and the two transport
  # failures (refused CS ioctl, failed completion wait) come after the one
  # execution.  Failures inject through the transport's ioctl table.
  r3v_native_submit_order_harness = executable(
    'r3v_native_submit_order_harness',
    files('tests/r3v_native_submit_order_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : r3v_native_deps,
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach submit_arm : ['armed', 'gate-closed', 'retention-unwritable',
                        'authorization-refused', 'map-failure',
                        'ioctl-refused', 'completion-failure',
                        'robust-oob-enabled', 'robust-oob-w0-refused',
                        'robust-oob-disabled', 'gpu-fetched-composed',
                        'gpu-fetched-compose-failure',
                        'gpu-fetched-wrong-digest',
                        'gpu-fetched-offset-misaligned',
                        'gpu-fetched-stride-misaligned',
                        'gpu-fetched-out-of-domain',
                        'gpu-fetched-out-of-bounds',
                        'gpu-fetched-aliased-source']
    test(
      'r3v-native-submit-order-@0@'.format(submit_arm),
      r3v_native_submit_order_harness,
      args : [submit_arm],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  # Calibration: a draw executed ahead of a refused submit is the
  # ordering the matrix refuses, so the untouched verdicts fail on it.
  test(
    'r3v-native-submit-order-known-bad-premature-draw',
    r3v_native_submit_order_harness,
    args : ['known-bad-premature-draw'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    should_fail : true,
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

  # Non-submitting depth-control arming runner: reports the cell's
  # digest and every arming factor, then stops at the authorization
  # boundary.  The check drives the refusal legs and the wrong-cell
  # negative against the triangle runner's digest.
  r3v_native_zb_depth_control_arming_runner = executable(
    'r3v_native_zb_depth_control_arming_runner',
    files('r3v_native_arming.c',
          'tests/r3v_native_zb_depth_control_arming_runner.c',
          '../common/r300_first_draw_state.c',
          '../common/r300_fragment_binary.c',
          '../common/r300_pm4_builder.c',
          '../common/r300_tcl_bypass_triangle.c',
          '../common/r300_zb_depth_control_cell.c',
          '../common/r300_zb_depth_state.c'),
    include_directories : [r3v_native_inc, include_directories('.')],
    dependencies : [idep_mesautil],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-zb-depth-control-arming-runner',
    prog_python,
    args : [
      files('tests/r3v_native_zb_depth_control_arming_runner_check.py'),
      r3v_native_zb_depth_control_arming_runner,
      r3v_native_arming_runner,
    ],
    suite : ['r3v'],
  )

  # Non-submitting producer-cell arming runner: reports the reference
  # producer pass's digest and every arming factor, then stops at the
  # authorization boundary.  The check drives the refusal legs and the
  # wrong-cell negative against the triangle runner's digest.
  r3v_native_producer_arming_runner = executable(
    'r3v_native_producer_arming_runner',
    files('r3v_native_arming.c',
          'tests/r3v_native_producer_arming_runner.c',
          '../common/r300_first_draw_state.c',
          '../common/r300_fragment_binary.c',
          '../common/r300_pm4_builder.c',
          '../common/r300_r2vb_carrier_delivery.c',
          '../common/r300_r2vb_producer_pass.c',
          '../common/r300_tcl_bypass_triangle.c'),
    include_directories : [r3v_native_inc, include_directories('.')],
    dependencies : [idep_mesautil],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-producer-arming-runner',
    prog_python,
    args : [
      files('tests/r3v_native_producer_arming_runner_check.py'),
      r3v_native_producer_arming_runner,
      r3v_native_arming_runner,
    ],
    suite : ['r3v'],
  )

  # Non-submitting public GPU-producer arming runner: composes the
  # route -- producer over the application records ahead of the
  # consumer cell -- reports its digest, its split, and both delivery
  # gates, then stops at the authorization boundary.  The check drives
  # the refusal legs and the producer-half digest as the wrong cell.
  r3v_native_public_gpu_producer_arming_runner = executable(
    'r3v_native_public_gpu_producer_arming_runner',
    files('r3v_native_arming.c',
          'tests/r3v_native_public_gpu_producer_arming_runner.c',
          '../common/r300_first_draw_state.c',
          '../common/r300_fragment_binary.c',
          '../common/r300_pm4_builder.c',
          '../common/r300_r2vb_carrier_delivery.c',
          '../common/r300_r2vb_producer_pass.c',
          '../common/r300_r2vb_public_route.c',
          '../common/r300_tcl_bypass_triangle.c'),
    include_directories : [r3v_native_inc, include_directories('.')],
    dependencies : [idep_mesautil],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-public-gpu-producer-arming-runner',
    prog_python,
    args : [
      files('tests/r3v_native_public_gpu_producer_arming_runner_check.py'),
      r3v_native_public_gpu_producer_arming_runner,
      r3v_native_producer_arming_runner,
    ],
    suite : ['r3v'],
  )

  # Non-submitting arming runner for the fetched GPU-producer route:
  # composes the fetched F32_4 reference route, reports its digest and
  # every arming factor including the third gate, then stops at the
  # authorization boundary.  The check drives the refusal legs and the
  # immediate route's digest as the wrong cell.
  r3v_native_fetched_gpu_producer_arming_runner = executable(
    'r3v_native_fetched_gpu_producer_arming_runner',
    files('r3v_native_arming.c',
          'tests/r3v_native_fetched_gpu_producer_arming_runner.c',
          '../common/r300_first_draw_state.c',
          '../common/r300_fragment_binary.c',
          '../common/r300_pm4_builder.c',
          '../common/r300_pm4_compose.c',
          '../common/r300_r2vb_carrier_delivery.c',
          '../common/r300_r2vb_fetch_pass.c',
          '../common/r300_r2vb_fetched_producer.c',
          '../common/r300_r2vb_producer_pass.c',
          '../common/r300_tcl_bypass_triangle.c'),
    include_directories : [r3v_native_inc, include_directories('.')],
    dependencies : [idep_mesautil],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-fetched-gpu-producer-arming-runner',
    prog_python,
    args : [
      files('tests/r3v_native_fetched_gpu_producer_arming_runner_check.py'),
      r3v_native_fetched_gpu_producer_arming_runner,
      r3v_native_public_gpu_producer_arming_runner,
    ],
    suite : ['r3v'],
  )

  r3v_native_reingest_arming_runner = executable(
    'r3v_native_reingest_arming_runner',
    files('r3v_native_arming.c',
          'tests/r3v_native_reingest_arming_runner.c',
          '../common/r300_first_draw_state.c',
          '../common/r300_fragment_binary.c',
          '../common/r300_pm4_builder.c',
          '../common/r300_r2vb_carrier_delivery.c',
          '../common/r300_r2vb_producer_pass.c',
          '../common/r300_r2vb_reingest_pass.c',
          '../common/r300_tcl_bypass_triangle.c'),
    include_directories : [r3v_native_inc, include_directories('.')],
    dependencies : [idep_mesautil],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-reingest-arming-runner',
    prog_python,
    args : [
      files('tests/r3v_native_reingest_arming_runner_check.py'),
      r3v_native_reingest_arming_runner,
      r3v_native_producer_arming_runner,
    ],
    suite : ['r3v'],
  )

  r3v_native_float2_tuple_arming_runner = executable(
    'r3v_native_float2_tuple_arming_runner',
    files('r3v_native_arming.c',
          'tests/r3v_native_float2_tuple_arming_runner.c',
          '../common/r300_first_draw_state.c',
          '../common/r300_fragment_binary.c',
          '../common/r300_pm4_builder.c',
          '../common/r300_r2vb_carrier_delivery.c',
          '../common/r300_r2vb_float2_tuple_pass.c',
          '../common/r300_r2vb_producer_pass.c',
          '../common/r300_tcl_bypass_triangle.c'),
    include_directories : [r3v_native_inc, include_directories('.')],
    dependencies : [idep_mesautil],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r3v-native-float2-tuple-arming-runner',
    prog_python,
    args : [
      files('tests/r3v_native_float2_tuple_arming_runner_check.py'),
      r3v_native_float2_tuple_arming_runner,
      r3v_native_producer_arming_runner,
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
