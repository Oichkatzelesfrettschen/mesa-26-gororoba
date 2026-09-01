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
            'r3v_vertex_spirv.c',
            '../cpu/r300_cpu_vertex.c', '../cpu/r300_cpu_vertex_job.c'),
      include_directories : [inc_include, inc_src,
                             include_directories('.')],
      dependencies : [dep_m],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )

  # Shader-interface record: the fixtures' qualifiers read back per
  # location and built-in, the two-stage link matches and refuses by
  # name, and every record's canonical serialization pins to a BLAKE3
  # digest so a dropped qualifier is a digest movement.
  test(
    'r3v-shader-interface',
    executable(
      'r3v_shader_interface_test',
      files('tests/r3v_shader_interface_test.c', 'r3v_shader_interface.c'),
      include_directories : [inc_include, inc_src,
                             include_directories('.')],
      dependencies : [idep_mesautil],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )

  # Post-vertex lowering: a Flat varying replicates the provoking
  # vertex's value across its triangle, Smooth passes untouched, the
  # per-location mask selects inside a multi-varying record, indexed
  # permutations follow the dereferenced list order, and the wrong
  # provoking vertex and a dropped qualifier each move the oracle.
  test(
    'r3v-post-vs-lowering',
    executable(
      'r3v_post_vs_lowering_test',
      files('tests/r3v_post_vs_lowering_test.c', 'r3v_post_vs_lowering.c',
            'r3v_shader_interface.c',
            '../common/r300_noperspective_reciprocal_plan.c', '../common/r300_noperspective_q_lane_plan.c', '../common/r300_noperspective_mixed_carrier_plan.c'),
      include_directories : [inc_include, inc_src,
                             include_directories('.')],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )

  # Interpolation route selection: the direct GA Flat route opens on
  # the full conjunction alone; each predicate flipped, the partially
  # clipped primitive included, lands on replication.
  test(
    'r3v-interpolation-lowering',
    executable(
      'r3v_interpolation_lowering_test',
      files('tests/r3v_interpolation_lowering_test.c',
            'r3v_interpolation_lowering.c', 'r3v_shader_interface.c',
            '../common/r300_noperspective_reciprocal_plan.c',
            '../common/r300_noperspective_mixed_carrier_plan.c'),
      include_directories : [inc_include, inc_src,
                             include_directories('.'),
                             include_directories('../common')],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )

  # Delivery route policy: the R2VB gate opens on the exact value alone,
  # the format set is closed, and every refused input lands on the CPU
  # route with a naming reason.
  test(
    'r3v-delivery-route',
    executable(
      'r3v_delivery_route_test',
      files('r3v_delivery_route.c', 'tests/r3v_delivery_route_test.c'),
      include_directories : [inc_include, inc_src,
                             include_directories('.')],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )

  # The SPIR-V admitters name every refused construct through a string
  # literal; the ledger enumerates that rejection surface so a new or
  # removed reason reports as a ledger movement.
  test(
    'r3v-spirv-refusal-reasons',
    prog_python,
    args : [files('tests/r3v_spirv_refusal_reasons.py'),
            files('tests/r3v_spirv_refusal_reasons.txt'),
            files('r3v_vertex_spirv.c'), files('r3v_compute_spirv.c'),
            files('r3v_shader_interface.c')],
    suite : ['r3v'],
  )
  test(
    'r3v-spirv-refusal-reasons-selftest',
    prog_python,
    args : [files('tests/r3v_spirv_refusal_reasons.py'), '--selftest',
            files('tests/r3v_spirv_refusal_reasons.txt'),
            files('r3v_vertex_spirv.c'), files('r3v_compute_spirv.c'),
            files('r3v_shader_interface.c')],
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
            'r3v_compute_spirv.c',
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
  'r3v_compute_spirv.c',
  'r3v_delivery_route.c',
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
  'r3v_native_plan.c',
  'r3v_native_plan_capture.c',
  'r3v_native_plan_replay.c',
  'r3v_native_queue.c',
  'r3v_native_recording.c',
  'r3v_native_transfer.c',
  'r3v_interpolation_lowering.c',
  'r3v_post_vs_lowering.c',
  'r3v_shader_interface.c',
  'r3v_vertex_spirv.c',
)

# The ICD compiles with no Gallium include root.  r3v_format.h owns the
# PIPE_FORMAT_* provenance and idep_mesautil supplies its util/format include
# graph; the separation-audit tests prove the sources, compile commands, and
# linked library stay free of Gallium includes and symbols.
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
  [r3v_loader_base_files, r3v_native_files, r3v_entrypoints, sha1_h],
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
      'r3v_native_plan.h',
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
  # Every integer in a blocking non-pass ledger row carries a source
  # query, a receipt, or a prose authority, and a source query's value
  # equals the ledger's number, so the frontier never cites a hand-typed
  # limit.
  test(
    'r3v-conformance-ledger-audit-selftest',
    prog_python,
    args : [files('tests/r3v_conformance_ledger_audit.py'), '--selftest'],
    suite : ['r3v'],
  )
  test(
    'r3v-conformance-ledger-audit',
    prog_python,
    args : [files('tests/r3v_conformance_ledger_audit.py'),
            '--ledger', files('tests/r3v_conformance_nonpass_ledger.tsv'),
            '--source-root', meson.project_source_root()],
    suite : ['r3v'],
  )
  # docs/hardware/r3v-wsi-denominator.md "Retained checks that pin the
  # denominator": the device extension table, the advertised-surface
  # binding ledger, and the built ICD's own symbol table must agree
  # that KHR_swapchain is absent, so a partial edit to any one of the
  # three fails closed rather than silently promising a route the
  # other two do not carry.
  test(
    'r3v-native-swapchain-absence-selftest',
    prog_python,
    args : [files('tests/r3v_native_swapchain_absence_audit.py'),
            '--selftest'],
    suite : ['r3v'],
  )
  swapchain_absence_args = [
    files('tests/r3v_native_swapchain_absence_audit.py'),
    '--device-extensions', files('r3v_physical_device.c'),
    '--binding-tsv', files('tests/r3v_advertised_surface_deqp_binding.tsv'),
  ]
  if prog_nm_r3v.found()
    test(
      'r3v-native-swapchain-absence',
      prog_python,
      args : swapchain_absence_args + [
        '--library', libvulkan_r3v.full_path(),
        '--nm-binary', prog_nm_r3v.full_path(),
      ],
      depends : libvulkan_r3v,
      suite : ['r3v'],
    )
  else
    test(
      'r3v-native-swapchain-absence',
      prog_python,
      args : swapchain_absence_args,
      suite : ['r3v'],
    )
  endif
  # docs/hardware/r3v-wsi-denominator.md "Default present route": the
  # three comment sites that once stated software-mode-by-default must
  # each name R3V_WSI_SW wherever they still say "software mode" or
  # "the lavapipe pattern", so the mismatch the document corrected
  # cannot silently reappear.
  test(
    'r3v-wsi-init-comment-parity-selftest',
    prog_python,
    args : [files('tests/r3v_wsi_init_comment_parity_audit.py'),
            '--selftest'],
    suite : ['r3v'],
  )
  test(
    'r3v-wsi-init-comment-parity',
    prog_python,
    args : [files('tests/r3v_wsi_init_comment_parity_audit.py'),
            '--physical-device', files('r3v_physical_device.c'),
            '--header', files('r3v_physical_device.h'),
            '--instance', files('r3v_instance.c')],
    suite : ['r3v'],
  )
  # docs/hardware/r3v-wsi-denominator.md "Per-host-class dEQP
  # applicability": the WSI surface-contract registrations must sit
  # inside the dep_xcb.found() and prog_xvfb.found() guard, so a host
  # missing either dependency registers zero test binaries rather than
  # a run-time skip; parsed from meson.build text, so it needs no
  # configured build to check either guard direction.
  test(
    'r3v-wsi-registration-gate-selftest',
    prog_python,
    args : [files('tests/r3v_wsi_registration_gate_audit.py'),
            '--selftest'],
    suite : ['r3v'],
  )
  test(
    'r3v-wsi-registration-gate',
    prog_python,
    args : [files('tests/r3v_wsi_registration_gate_audit.py'),
            '--meson-build', files('meson.build')],
    suite : ['r3v'],
  )
  # The Vulkan 1.0 requirement inventory: feature and limit member
  # identity from the registry, the advertised extension surface with
  # its dependency closure evaluated at core 1.0 (unknown, disabled,
  # unsatisfied-depends each reject by class; orphan-alias screens
  # registry integrity), the queue-family shape, and the apiVersion 1.0 pin.  The
  # checked-in TSV must equal the regeneration; the core 1.0 command
  # closure stays with r3v_native_entrypoint_audit.py.
  test(
    'r3v-vulkan10-requirement-inventory-selftest',
    prog_python,
    args : [files('tests/r3v_vulkan10_requirement_inventory.py'),
            '--selftest', '--registry', vk_api_xml],
    suite : ['r3v'],
  )
  test(
    'r3v-vulkan10-requirement-inventory',
    prog_python,
    args : [files('tests/r3v_vulkan10_requirement_inventory.py'),
            '--registry', vk_api_xml,
            '--physical-device', files('r3v_physical_device.c'),
            '--instance', files('r3v_instance.c'),
            '--private-header', files('r3v_private.h'),
            '--inventory',
            files('tests/r3v_vulkan10_requirement_inventory.tsv')],
    suite : ['r3v'],
  )
  # The advertised-bit binding ledger: every advertised extension,
  # granted feature bit, receipt-bound limit, and format-feature row
  # names the registered test that exercises its executing route and
  # the exact dEQP-VK group whose mustpass cases judge it.  A derived
  # item with no row, a row nothing advertises, or an unregistered test
  # fails; with R3V_DEQP_MUSTPASS_DIR naming a CTS mustpass directory a
  # group matching no case fails, and without it that clause reports
  # itself as not run while the others still decide.
  test(
    'r3v-advertised-surface-deqp-binding-selftest',
    prog_python,
    args : [files('tests/r3v_advertised_surface_deqp_binding.py'),
            '--selftest'],
    suite : ['r3v'],
  )
  test(
    'r3v-advertised-surface-deqp-binding',
    prog_python,
    args : [files('tests/r3v_advertised_surface_deqp_binding.py'),
            '--ledger',
            files('tests/r3v_advertised_surface_deqp_binding.tsv'),
            '--physical-device', files('r3v_physical_device.c'),
            '--instance', files('r3v_instance.c'),
            '--private-header', files('r3v_private.h'),
            '--intro-tests',
            meson.global_build_root() / 'meson-info' / 'intro-tests.json'],
    suite : ['r3v'],
  )
  # The conformance runner seals an identity receipt around a dEQP-VK
  # caselist run (source SHA against the declared SHA and cleanliness,
  # Meson options, ICD DSO digest, dEQP digest and the log's own release
  # name, kernel and module identity, PCI identity and render node,
  # environment, per-case status, dmesg delta, the deadline, artifact
  # digests) with a finite verdict: NotSupported never counts as a pass,
  # a process the deadline kills never passes, and every non-pass
  # classifies against the ledger most-specific-row-first.  The selftest
  # drives fake-dEQP fixtures and a replay of a real dEQP log through
  # the real runner; the ledger check classifies each row's witness
  # through the real path so every row is reachable and unshadowed.
  test(
    'r3v-conformance-runner-selftest',
    prog_python,
    args : [files('tests/r3v_conformance_runner.py'), 'selftest',
            '--fixture-qpa',
            files('tests/r3v_conformance_runner_fixture_info.qpa')],
    suite : ['r3v'],
    timeout : 120,
  )
  test(
    'r3v-conformance-ledgers',
    prog_python,
    args : [files('tests/r3v_conformance_runner.py'), 'check-ledgers',
            '--nonpass-ledger',
            files('tests/r3v_conformance_nonpass_ledger.tsv'),
            '--slices', files('tests/r3v_conformance_slices.tsv')],
    suite : ['r3v'],
  )
  # The exact-cover partition assigns every pinned mustpass case to one
  # slice and one slice alone, generates each slice's caselist with its
  # count and digest, and marks an unknown-hazard slice blocked so the
  # runner refuses it.  The selftest calibrates every refusal on a
  # synthetic corpus; the corpus checks prove the exhaustive table's
  # exact cover and the pilot table's disjointness against the pinned
  # mustpass directory when R3V_DEQP_MUSTPASS_DIR names it, against the
  # checked-in corpus pin; unnamed, they skip (exit 77).
  # The conformance tooling is Python the runner executes on the target;
  # pyflakes holds it free of undefined names and duplicate keys.
  prog_pyflakes = find_program('pyflakes', required : false)
  if prog_pyflakes.found()
    test(
      'r3v-conformance-tooling-pyflakes',
      prog_pyflakes,
      args : [files('tests/r3v_conformance_runner.py',
                    'tests/r3v_conformance_partition.py',
                    'tests/r3v_deqp_provision.py',
                    'tests/r3v_native_plan_compose_check.py',
                    'tests/r3v_native_plan_compose_shard.py')],
      suite : ['r3v'],
    )
  endif
  test(
    'r3v-conformance-partition-selftest',
    prog_python,
    args : [files('tests/r3v_conformance_partition.py'), 'selftest'],
    suite : ['r3v'],
  )
  test(
    'r3v-conformance-partition-exhaustive',
    prog_python,
    args : [files('tests/r3v_conformance_partition.py'), 'check',
            '--kind', 'exhaustive', '--partition',
            files('tests/r3v_conformance_partition.tsv'),
            '--corpus-pin', files('tests/r3v_conformance_corpus.pin')],
    suite : ['r3v'],
    timeout : 300,
  )
  test(
    'r3v-conformance-partition-pilot',
    prog_python,
    args : [files('tests/r3v_conformance_partition.py'), 'check',
            '--kind', 'pilot', '--partition',
            files('tests/r3v_conformance_slices.tsv'),
            '--corpus-pin', files('tests/r3v_conformance_corpus.pin')],
    suite : ['r3v'],
    timeout : 300,
  )
  # A dEQP-VK binary reaches the target as a bundle whose provenance
  # (source commit and cleanliness, CMake source/build pins, ELF and
  # dynamic symbol versions, XED ISA-set census, complete GNU property
  # note, checked-in corpus pin, data and mustpass tree digests)
  # verifies itself before a run; the selftest calibrates the refusals.
  test(
    'r3v-deqp-provision-selftest',
    prog_python,
    args : [files('tests/r3v_deqp_provision.py'), 'selftest'],
    suite : ['r3v'],
    timeout : 120,
  )
  # Source layer: the ICD's sources carry no Gallium include and no
  # Gallium runtime identifier on any line the compile keeps.
  test(
    'r3v-native-gallium-separation-sources',
    prog_python,
    args : [files('tests/r3v_native_separation_audit.py'), '--sources',
            '--native-root', meson.current_source_dir() / '..']
           + r3v_loader_base_files + r3v_native_files + r300_cpu_files
           + files(
      'r3v_cpu_sync.h',
      'r3v_format.h',
      'r3v_instance.h',
      'r3v_memory_properties_contract.h',
      'r3v_native.h',
      'r3v_native_arming.h',
      'r3v_native_identity.h',
      'r3v_native_plan.h',
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
  # Dropping the target of the BindBufferMemory bridge produces one known
  # open slot.  This passing leg proves the baseline comparator observes that
  # exact slot before the extra-drop leg adds a second, unexpected slot.
  test(
    'r3v-native-entrypoint-audit-baseline-bind-buffer-memory2-drop',
    prog_python,
    args : [r3v_native_entrypoint_audit_args, '--baseline',
            'BindBufferMemory', '--drop', 'BindBufferMemory2'],
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
  # --require-tests refuses a required test the build root does not
  # register while admitting host absences (replay tools, deployment
  # identity), so a meson.build arm renamed away from the inventory's
  # required list refuses in the tree rather than first in the package
  # recipe's check().
  test(
    'r3v-qualification-inventory-require-tests',
    prog_python,
    args : [files('tests/r3v_qualification_inventory.py'),
            meson.project_build_root(), '--require-tests'],
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
    # The ratchet walks every scan root of the source tree; the RS482 host
    # (K8) needs past the default 30 s for that walk.
    timeout : 600,
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

# Attended render-shape cell: performs a live DRM_RADEON_CS on RS482
# silicon under the authorization and procedure in
# docs/hardware/r3v-native-attended-render-shape-procedure.md.  It takes
# the shape on its command line in the arming runner's --shape form, so
# the digest the runner reports names the cell this program submits.
r3v_native_attended_render_shape = executable(
  'r3v_native_attended_render_shape',
  files('tests/r3v_native_attended_render_shape.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended sampled cell: drives the public sampling surface (varying
# vertex module, sampled fragment module, set-0 combined image sampler
# over a one-color texture) to a live DRM_RADEON_CS on RS482 silicon
# under the render-shape attended procedure; the uniform texture makes
# the interior equal one texel whatever the interpolated coordinates.
r3v_native_attended_sampled = executable(
  'r3v_native_attended_sampled',
  files('tests/r3v_native_attended_sampled.c',
        'tests/r3v_native_recovery_waiver.c',
        'tests/r3v_native_watchdog_client.c',
        'tests/r3v_native_watchdog_guard.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended composed render-then-sample cell: records the composed cell
# through r3v_native_record_composed_render_sample and drives it to a
# live DRM_RADEON_CS on RS482 silicon, where the render half's target is
# the sample half's texture and both targets take one coverage verdict.
r3v_native_attended_multi_pass = executable(
  'r3v_native_attended_multi_pass',
  files('tests/r3v_native_attended_multi_pass.c',
        'tests/r3v_native_recovery_waiver.c',
        'tests/r3v_native_watchdog_client.c',
        'tests/r3v_native_watchdog_guard.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended public two-draw cell: two render passes with a draw each
# through the public surface, one pipeline over each admitted fragment
# module, driven to a live DRM_RADEON_CS after the runner proves the
# recorded stream's digest is the emitter's.
r3v_native_attended_public_two_draw = executable(
  'r3v_native_attended_public_two_draw',
  files('tests/r3v_native_attended_public_two_draw.c',
        'tests/r3v_native_recovery_waiver.c',
        'tests/r3v_native_watchdog_client.c',
        'tests/r3v_native_watchdog_guard.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# The attended public Flat two-draw cell: the same two-render-pass
# recording under the Flat module pair, each pass in its own vertex
# order, driven to a live DRM_RADEON_CS after the runner proves the
# recorded stream's digest is the varying two-pass emitter's.
r3v_native_attended_public_flat_two_draw = executable(
  'r3v_native_attended_public_flat_two_draw',
  files('tests/r3v_native_attended_public_flat_two_draw.c',
        'tests/r3v_native_recovery_waiver.c',
        'tests/r3v_native_watchdog_client.c',
        'tests/r3v_native_watchdog_guard.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# The attended public direct GA Flat two-draw cell: the same public
# surface under the RGBA Flat module pair with the pipeline on the
# direct route, judged by byte equality with the CPU replication
# oracle's expected targets; the recorded stream's digest is the
# color-0 two-pass emitter's.
r3v_native_attended_public_flat_color0_two_draw = executable(
  'r3v_native_attended_public_flat_color0_two_draw',
  files('tests/r3v_native_attended_public_flat_color0_two_draw.c',
        'tests/r3v_native_recovery_waiver.c',
        'tests/r3v_native_watchdog_client.c',
        'tests/r3v_native_watchdog_guard.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended rasterizer interpolation probe: two render passes over one
# unequal-W carrier through the smooth and the NoPerspective fragment
# interfaces, the second pass alone carrying the gated candidate control
# word, judged by a census of the candidate target against the
# registered interpolation models.
r3v_native_attended_rs_tex_adj_probe = executable(
  'r3v_native_attended_rs_tex_adj_probe',
  files('tests/r3v_native_attended_rs_tex_adj_probe.c',
        'tests/r3v_native_recovery_waiver.c',
        'tests/r3v_native_watchdog_client.c',
        'tests/r3v_native_watchdog_guard.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

r3v_native_attended_composed = executable(
  'r3v_native_attended_composed',
  files('tests/r3v_native_attended_composed.c',
        'tests/r3v_native_recovery_waiver.c',
        'tests/r3v_native_watchdog_client.c',
        'tests/r3v_native_watchdog_guard.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended multisample resolve cell: records the resolve cell through
# r3v_native_record_msaa_resolve and drives it to a live DRM_RADEON_CS
# on RS482 silicon, where four passes of the subsample-set oracle over
# one denominator classify what AARESOLVE_MODE_RESOLVE sends to the
# resolve destination.
r3v_native_attended_msaa_resolve = executable(
  'r3v_native_attended_msaa_resolve',
  files('tests/r3v_native_attended_msaa_resolve.c',
        'tests/r3v_native_recovery_waiver.c',
        'tests/r3v_native_watchdog_client.c',
        'tests/r3v_native_watchdog_guard.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Recovery-waiver calibration: the waiver admits one attended run, so
# every binding it carries -- boot, attempt, cell, runner image, age --
# is exercised against a document that differs by that field alone.
r3v_native_recovery_waiver_test = executable(
  'r3v_native_recovery_waiver_test',
  files('tests/r3v_native_recovery_waiver_test.c',
        'tests/r3v_native_recovery_waiver.c'),
  include_directories : include_directories('tests'),
  build_by_default : true,
  install : false,
)
test(
  'r3v-native-recovery-waiver',
  r3v_native_recovery_waiver_test,
  timeout : 60,
  suite : ['r3v'],
)

# SB600 TCO watchdog bracket co-process: holds the privilege the
# attended submit gate's watchdog needs, so the runner arms across
# DRM_IOCTL_RADEON_CS through fence completion while the ICD and the
# evidence writes stay unprivileged.
r3v_native_watchdog_bracket = executable(
  'r3v_native_watchdog_bracket',
  files('tests/r3v_native_watchdog_bracket.c'),
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

# Attended computed-varying triangle cell: the varying vertex and
# pass-through fragment modules through the public surface on the CPU
# route, so one vertex-computed varying crosses the RS interpolator into
# the color target on silicon.  Built and never registered as a test,
# because running it is a hardware authorization rather than a
# build-graph result.
r3v_native_attended_varying_triangle = executable(
  'r3v_native_attended_varying_triangle',
  files('tests/r3v_native_attended_varying_triangle.c'),
  include_directories : [r3v_native_inc, include_directories('.')],
  dependencies : r3v_native_deps,
  link_whole : [libr3v_native_impl],
  link_with : [libradeon_drm_vk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : false,
)

# Attended compute identity carrier cell: the public compute surface on
# the identity verb's GPU route -- two storage buffers, the reference
# identity-map kernel, one dispatch, submit -- so RS482 silicon fetches the
# input as F32_4 records and writes the output as one C4_32_FP slot row,
# the driver's read-back judging it against the CPU bit copy.  Built and
# never registered as a test, because running it is a hardware
# authorization rather than a build-graph result.
r3v_native_attended_compute_identity = executable(
  'r3v_native_attended_compute_identity',
  files('tests/r3v_native_attended_compute_identity.c'),
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
# around the synchronous vkQueueSubmit, so the CPU, immediate GPU
# producer, and fetched GPU producer delivery routes earn comparable
# timing rows on verified bytes.  Built and never registered as a
# test, because running it is a hardware authorization rather than a
# build-graph result.
r3v_native_attended_route_dispatch_timing = executable(
  'r3v_native_attended_route_dispatch_timing',
  files('tests/r3v_native_attended_route_dispatch_timing.c',
        '../common/r300_r2vb_fetched_producer.c',
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
  # The ordered-submission plan validates without the device: seal,
  # schema, identity binding, whole-entry matching, and the monotonic
  # replay session each refuse on their single-defect fixtures.
  test(
    'r3v-native-plan',
    executable(
      'r3v_native_plan_test',
      files('r3v_native_plan.c', 'tests/r3v_native_plan_test.c'),
      include_directories : [inc_include, inc_src,
                             include_directories('.')],
      dependencies : [idep_mesautil],
      link_with : [libr300_common],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r3v'],
  )
  # The plan tool composes a sealed plan from a captured transcript and
  # the declared run identities; the capture harness below runs two
  # distinct draws under the shim with the capture path set and proves
  # the transcript carries both entries, then composes and binds it.
  r3v_native_plan_tool = executable(
    'r3v_native_plan_tool',
    files('r3v_native_plan.c', 'tests/r3v_native_plan_tool.c'),
    include_directories : [inc_include, inc_src, include_directories('.')],
    dependencies : [idep_mesautil],
    link_with : [libr300_common],
    gnu_symbol_visibility : 'hidden',
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
          '../common/r300_compute_identity_carrier.c',
          '../common/r300_compute_identity_carrier_contract.c',
          '../common/r300_first_draw_state.c',
          '../common/r300_fragment_binary.c',
          '../common/r300_pm4_builder.c',
          '../common/r300_pm4_compose.c',
          '../common/r300_r2vb_carrier_delivery.c',
          '../common/r300_r2vb_fetch_pass.c',
          '../common/r300_r2vb_fetched_producer.c',
          '../common/r300_r2vb_producer_pass.c',
          '../common/r300_tcl_bypass_triangle.c',
          '../common/r300_flat_color0_plan.c', '../common/r300_rs_tex_adj_probe.c', '../common/r300_noperspective_reciprocal_plan.c', '../common/r300_noperspective_q_lane_plan.c', '../common/r300_noperspective_mixed_carrier_plan.c'),
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

  # Non-submitting producer-cell arming runner: reports the reference
  # producer pass's digest and every arming factor, then stops at the
  # authorization boundary.  The check drives the refusal and deterministic
  # ARMED calibration legs without requiring the optional drm-shim harness.
  r3v_native_producer_arming_runner = executable(
    'r3v_native_producer_arming_runner',
    files('r3v_native_arming.c',
          'tests/r3v_native_producer_arming_runner.c',
          '../common/r300_first_draw_state.c',
          '../common/r300_fragment_binary.c',
          '../common/r300_pm4_builder.c',
          '../common/r300_r2vb_carrier_delivery.c',
          '../common/r300_r2vb_producer_pass.c',
          '../common/r300_tcl_bypass_triangle.c',
          '../common/r300_flat_color0_plan.c', '../common/r300_rs_tex_adj_probe.c', '../common/r300_noperspective_reciprocal_plan.c', '../common/r300_noperspective_q_lane_plan.c', '../common/r300_noperspective_mixed_carrier_plan.c'),
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

  # The varying cell's two authorities -- the arming runner's --varying
  # emission and the manifest writer's --varying ib.bin -- are one byte
  # stream, so the digest the operator declares from either names the
  # recorded cell.
  test(
    'r3v-native-arming-runner-varying-ib-identity',
    find_program('sh'),
    args : [
      '-c',
      'set -e; d=$(mktemp -d); trap \'rm -rf "$d"\' EXIT; ' +
      '"$1" --varying --emit-ib "$d/runner.bin"; ' +
      '"$2" "$d" --varying >/dev/null; cmp "$d/runner.bin" "$d/ib.bin"',
      'varying-ib-identity',
      r3v_native_arming_runner.full_path(),
      r300_triangle_manifest.full_path(),
    ],
    depends : [r3v_native_arming_runner, r300_triangle_manifest],
    suite : ['r3v'],
  )
  # The compute identity carrier cell: the arming runner's
  # --compute-identity emission and the carrier manifest writer's ib.bin
  # are one byte stream, so the digest the operator declares from either
  # names the pass the identity verb's GPU route installs.
  test(
    'r3v-native-arming-runner-compute-identity-ib-identity',
    find_program('sh'),
    args : [
      '-c',
      'set -e; d=$(mktemp -d); trap \'rm -rf "$d"\' EXIT; ' +
      '"$1" --compute-identity --emit-ib "$d/runner.bin"; ' +
      '"$2" "$d" >/dev/null; cmp "$d/runner.bin" "$d/ib.bin"',
      'compute-identity-ib-identity',
      r3v_native_arming_runner.full_path(),
      r300_compute_identity_carrier_manifest.full_path(),
    ],
    depends : [r3v_native_arming_runner,
               r300_compute_identity_carrier_manifest],
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
  # The compute recording calibration runs under the compute gate: the
  # surface the runner records through stands behind it.
  r3v_native_compute_record_env = environment(
    r3v_native_cell_vars + {'R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL' : '1'})
  foreach cell_mode : ['closed', 'open', 'shape', 'shape-offset']
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
    ['query-refusal', 'query-refusal'],
    ['event-refusal', 'event-refusal'],
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
  # Composed render-then-sample recorder on the same shim fixture: the
  # record-time relocation contract, where five slots reach four buffer
  # objects and the payloads name the merged indices.
  r3v_native_composed_cell_harness = executable(
    'r3v_native_composed_cell_harness',
    files('tests/r3v_native_composed_cell_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : [r3v_native_deps, dep_dl],
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach composed_mode : ['bound', 'unbound']
    test(
      'r3v-native-composed-cell-reloc-@0@'.format(composed_mode),
      r3v_native_composed_cell_harness,
      args : [composed_mode],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  # Two-pass recorder on the same fixture: the first pass installs, the
  # second appends through the public route's primitive, the offline
  # emitter reproduces the stream, and the mutation modes arm digests
  # of streams the recorder never installs.
  r3v_native_multi_pass_cell_harness = executable(
    'r3v_native_multi_pass_cell_harness',
    files('tests/r3v_native_multi_pass_cell_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : [r3v_native_deps, dep_dl],
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach multi_pass_mode : ['bound', 'shared', 'alias', 'mutated-flush',
                             'mutated-second-state']
    test(
      'r3v-native-multi-pass-cell-@0@'.format(multi_pass_mode),
      r3v_native_multi_pass_cell_harness,
      args : [multi_pass_mode],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  # Multisample resolve recorder on the same shim fixture: five slots
  # reach four buffer objects, the multisample surface is the
  # recording's own device-local allocation carrying a write alone, and
  # the payloads name the merged indices.
  r3v_native_msaa_cell_harness = executable(
    'r3v_native_msaa_cell_harness',
    files('tests/r3v_native_msaa_cell_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : [r3v_native_deps, dep_dl],
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach msaa_mode : ['bound', 'unbound', 'bound-clear']
    test(
      'r3v-native-msaa-cell-reloc-@0@'.format(msaa_mode),
      r3v_native_msaa_cell_harness,
      args : [msaa_mode],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
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
      args : ['--inject-storage-texel-buffer'],
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
    test(
      'r3v-native-descriptor-known-bad-typed-pool-overflow-admits',
      r3v_native_descriptor_test,
      args : ['--inject-typed-pool-overflow-admits'],
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      should_fail : true,
      timeout : 120,
      suite : ['r3v'],
    )
    # Transfer-op contract through the public surface: dword-pattern
    # fills with whole-size truncation, inline updates from
    # record-time-captured bytes, unit-scale blits lowered onto the
    # image copy executor, and the overlap, scale, flip, alignment,
    # and unsupported-layout refusals.  Two calibrated mutations
    # report a scaling blit and an overlapping copy as admitted.
    r3v_native_transfer_ops_test = executable(
      'r3v_native_transfer_ops_test',
      files('tests/r3v_native_transfer_ops_test.c'),
      include_directories : [inc_include, include_directories('.')],
      dependencies : [dep_vulkan_loader],
      c_args : [no_override_init_args],
    )
    test(
      'r3v-native-transfer-ops',
      r3v_native_transfer_ops_test,
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      timeout : 120,
      suite : ['r3v'],
    )
    test(
      'r3v-native-transfer-ops-known-bad-scaled-linear-blit-admits',
      r3v_native_transfer_ops_test,
      args : ['--inject-scaled-linear-blit-admits'],
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      should_fail : true,
      timeout : 120,
      suite : ['r3v'],
    )
    test(
      'r3v-native-transfer-ops-known-bad-overlap-copy-admits',
      r3v_native_transfer_ops_test,
      args : ['--inject-overlap-copy-admits'],
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      should_fail : true,
      timeout : 120,
      suite : ['r3v'],
    )
    # Object-lifetime contract through the public surface: sampler
    # creation as recorded state with anisotropy refused and use
    # poisoned at the descriptor write, buffer-view refusal under the
    # advertised format table, and the once-only host-visible binding
    # admission for buffers and images.  Two calibrated mutations
    # report a rebind and a wrong-type bind as admitted.
    r3v_native_object_lifetime_test = executable(
      'r3v_native_object_lifetime_test',
      files('tests/r3v_native_object_lifetime_test.c'),
      include_directories : [inc_include, include_directories('.')],
      dependencies : [dep_vulkan_loader],
      c_args : [no_override_init_args],
    )
    test(
      'r3v-native-object-lifetime',
      r3v_native_object_lifetime_test,
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      timeout : 120,
      suite : ['r3v'],
    )
    test(
      'r3v-native-object-lifetime-known-bad-rebind-admits',
      r3v_native_object_lifetime_test,
      args : ['--inject-rebind-admits'],
      env : r3v_native_loader_env,
      depends : [libradeon_noop_drm_shim, libvulkan_r3v,
                 _dev_icd],
      should_fail : true,
      timeout : 120,
      suite : ['r3v'],
    )
    test(
      'r3v-native-object-lifetime-known-bad-wrong-type-bind-admits',
      r3v_native_object_lifetime_test,
      args : ['--inject-wrong-type-bind-admits'],
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
      include_directories : [inc_include, inc_src, include_directories('.')],
      dependencies : [dep_vulkan_loader],
      c_args : [no_override_init_args],
    )
    r3v_native_loader_app_vars = r3v_native_cell_vars + {
      'VK_DRIVER_FILES' : _dev_icd.full_path(),
      'R3V_EXPECTED_ICD_DSO' : libvulkan_r3v.full_path(),
    }
    # The queue-claim report reaches the ICD through the loader under
    # VK_DRIVER_FILES and names the queue flags it advertises, the claim
    # mode behind the compute bit, and the verb ledger digest; the
    # conformance runner records it in every receipt so a gate-assisted
    # run reads as its mode.  Both gate states run under the shim and the
    # report's own consistency check judges them.
    r3v_native_queue_claim_report = executable(
      'r3v_native_queue_claim_report',
      files('tests/r3v_native_queue_claim_report.c'),
      include_directories : [inc_include, inc_src, inc_amd_common],
      dependencies : [dep_vulkan_loader, idep_mesautil],
      link_with : [libr300_common],
      gnu_symbol_visibility : 'hidden',
    )
    test(
      'r3v-native-queue-claim-report-default',
      prog_python,
      args : [files('tests/r3v_native_queue_claim_report_check.py'),
              r3v_native_queue_claim_report, 'default_graphics_only'],
      env : environment(r3v_native_loader_app_vars),
      depends : [libradeon_noop_drm_shim, libvulkan_r3v, _dev_icd],
      suite : ['r3v'],
    )
    test(
      'r3v-native-queue-claim-report-gated',
      prog_python,
      args : [files('tests/r3v_native_queue_claim_report_check.py'),
              r3v_native_queue_claim_report, 'experimental_compute_subset'],
      env : environment(r3v_native_loader_app_vars +
                        {'R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL' : '1'}),
      depends : [libradeon_noop_drm_shim, libvulkan_r3v, _dev_icd],
      suite : ['r3v'],
    )
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
  # The varying triangle cell's recording on the same fixture: the
  # varying pipeline admits and the draw records the varying cell.
  test(
    'r3v-native-varying-triangle-record',
    r3v_native_attended_varying_triangle,
    args : [meson.current_build_dir(), '--record-only'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The composed render-then-sample runner's recording boundary on the
  # fixture: five relocation slots reach four buffer objects, the render
  # target filling both the render color slot and the sample texture
  # slot, so a binding that names the wrong buffer fails here rather
  # than on the one attended submission the cell gets.
  test(
    'r3v-native-composed-record',
    r3v_native_attended_composed,
    args : [meson.current_build_dir(), '--record-only'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  test(
    'r3v-native-multi-pass-record',
    r3v_native_attended_multi_pass,
    args : [meson.current_build_dir(), '--record-only'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The public two-draw runner's recording boundary on the fixture: the
  # recorded stream's digest equals the emitter's and the kind is the
  # frozen multi-pass kind, so a public-route drift fails here rather
  # than on the one attended submission.
  test(
    'r3v-native-public-two-draw-record',
    r3v_native_attended_public_two_draw,
    args : [meson.current_build_dir(), '--record-only'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The Flat two-draw runner's recording boundary on the fixture: the
  # Flat pipeline's interface record, both deferred draws' lowering over
  # their own vertex order, and the recorded stream's digest against
  # the varying two-pass emitter's.
  test(
    'r3v-native-public-flat-two-draw-record',
    r3v_native_attended_public_flat_two_draw,
    args : [meson.current_build_dir(), '--record-only'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  test(
    'r3v-native-public-flat-color0-two-draw-record',
    r3v_native_attended_public_flat_color0_two_draw,
    args : [meson.current_build_dir(), '--record-only'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The probe runner's recording boundary on the shim fixture, one arm
  # per candidate: the operator gate the runner requires, the two
  # pipelines' route selection, the one-dword separation of the two
  # emitted streams, the recorded digest, the per-pass state checks, and
  # the census calibrated against its own predictions.
  test(
    'r3v-native-rs-tex-adj-probe-record',
    r3v_native_attended_rs_tex_adj_probe,
    args : [meson.current_build_dir(), '--record-only',
            '--candidate', 'tex-adj'],
    env : environment(
      r3v_native_cell_vars + {'R3V_NATIVE_RS_TEX_ADJ_PROBE' : '1'}),
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  test(
    'r3v-native-rs-w-select-probe-record',
    r3v_native_attended_rs_tex_adj_probe,
    args : [meson.current_build_dir(), '--record-only',
            '--candidate', 'w-select'],
    env : environment(
      r3v_native_cell_vars + {'R3V_NATIVE_RS_W_SELECT_PROBE' : '1'}),
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The production NoPerspective route's recording boundary: no probe
  # gate, the NoPerspective pipeline on the direct GB W_SELECT route,
  # and the recorded stream the gated W_SELECT candidate's byte for byte.
  test(
    'r3v-native-noperspective-production-route-record',
    r3v_native_attended_rs_tex_adj_probe,
    args : [meson.current_build_dir(), '--record-only',
            '--candidate', 'w-select', '--production'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The forced reciprocal-carrier rung's recording boundary: under the
  # exact force gate the NoPerspective pipeline selects the carrier route
  # and pass 1 records the TC1 carrier cell.
  test(
    'r3v-native-noperspective-carrier-route-record',
    r3v_native_attended_rs_tex_adj_probe,
    args : [meson.current_build_dir(), '--record-only',
            '--candidate', 'reciprocal-carrier'],
    env : environment(r3v_native_cell_vars +
                      {'R3V_NATIVE_NOPERSPECTIVE_CARRIER_FORCE' : '1'}),
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The public partial-clip fallback's recording boundary: no gate, the
  # NoPerspective pipeline created on the adaptive route, the selection
  # after the CPU vertex execution resolving the TC1 carrier cell for
  # the partially clipped triangle, and the recorded stream byte-equal to
  # the carrier cell the forced rung armed.
  test(
    'r3v-native-noperspective-public-partial-clip-route-record',
    r3v_native_attended_rs_tex_adj_probe,
    args : [meson.current_build_dir(), '--record-only',
            '--candidate', 'reciprocal-carrier-partial', '--production'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The partial-clip rung of the same runner: vertex 0 past x = -w, the
  # carrier fan validated and the census judged away from the border.
  test(
    'r3v-native-noperspective-carrier-partial-route-record',
    r3v_native_attended_rs_tex_adj_probe,
    args : [meson.current_build_dir(), '--record-only',
            '--candidate', 'reciprocal-carrier-partial'],
    env : environment(r3v_native_cell_vars +
                      {'R3V_NATIVE_NOPERSPECTIVE_CARRIER_FORCE' : '1'}),
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The q-lane rung's recording boundary: no gate, the vec3 NoPerspective
  # pipeline on the q-lane route, pass 1 the q-lane cell, the per-channel
  # oracle calibrated over the predictions.
  test(
    'r3v-native-noperspective-q-lane-route-record',
    r3v_native_attended_rs_tex_adj_probe,
    args : [meson.current_build_dir(), '--record-only',
            '--candidate', 'reciprocal-q-lane'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The mixed carrier rung's recording boundary: no gate, the Smooth
  # plus NoPerspective pipeline on the mixed route, pass 1 the
  # sixteen-dword three-vector cell, the per-channel oracle calibrated
  # over the predictions.
  test(
    'r3v-native-noperspective-mixed-carrier-route-record',
    r3v_native_attended_rs_tex_adj_probe,
    args : [meson.current_build_dir(), '--record-only',
            '--candidate', 'mixed-reciprocal-carrier'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # Flat beside NoPerspective on the mixed cell: no gate, location 0
  # Flat replicated on the host ahead of the packing, the same
  # sixteen-dword cell, the flat-mixed oracle calibrated against its
  # own prediction and against rung D's interpolated image.
  test(
    'r3v-native-noperspective-flat-mixed-carrier-route-record',
    r3v_native_attended_rs_tex_adj_probe,
    args : [meson.current_build_dir(), '--record-only',
            '--candidate', 'flat-mixed-reciprocal-carrier'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The multisample resolve runner's recording boundary on the fixture:
  # the device-local surface allocation, both vertex layouts, and the
  # merged binding, at each subsample set, so an argument or seeding
  # defect fails here rather than on the one attended submission.
  foreach msaa_samples : ['2', '4']
    test(
      'r3v-native-msaa-resolve-record-@0@x'.format(msaa_samples),
      r3v_native_attended_msaa_resolve,
      args : [meson.current_build_dir(), '--samples', msaa_samples,
              '--record-only'],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  # The cleared cell's recording boundary: the clear half leads the
  # stream and its sites bind through the same merged indices.
  foreach msaa_samples : ['2', '4']
    test(
      'r3v-native-msaa-resolve-record-cleared-@0@x'.format(msaa_samples),
      r3v_native_attended_msaa_resolve,
      args : [meson.current_build_dir(), '--samples', msaa_samples,
              '--clear', '--record-only'],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  # The compute identity carrier runner's recording boundary on the
  # fixture: the compute surface under its gate, the descriptor set, the
  # reference kernel, and one dispatch record.
  test(
    'r3v-native-compute-identity-record',
    r3v_native_attended_compute_identity,
    args : [meson.current_build_dir(), '--record-only'],
    env : r3v_native_compute_record_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    suite : ['r3v'],
  )
  # The fetched cell shares the runner and the recording; its mode is the
  # composition the admission selects and the digest the run declares,
  # and each source width records its own stride and attribute format.
  foreach fetched_width : ['f32_4', 'f32_3', 'f32_2']
    test(
      'r3v-native-fetched-gpu-producer-record-@0@'.format(fetched_width),
      r3v_native_attended_public_gpu_producer,
      args : [meson.current_build_dir(), '--record-only',
              '--fetched=@0@'.format(fetched_width)],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach

  # The timing runner's CPU-route authorization rests on the consumer
  # slice of the offline composition being the recorded consumer cell
  # verbatim; this host test pins that identity and the digest
  # separation between the two routes' declarations.
  r3v_native_route_timing_digest_test = executable(
    'r3v_native_route_timing_digest_test',
    files('tests/r3v_native_route_timing_digest_test.c',
          '../common/r300_r2vb_fetched_producer.c',
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
  test(
    'r3v-native-route-timing-record-fetched',
    r3v_native_attended_route_dispatch_timing,
    args : [meson.current_build_dir(), 'fetched', '--record-only'],
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
  # pre-commit failure (closed gate, injected retention failure, refused
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
  foreach submit_arm : ['armed', 'gate-closed', 'retention-failure',
                        'authorization-refused', 'map-failure',
                        'ioctl-refused', 'completion-failure',
                        'robust-oob-enabled', 'robust-oob-w0-degenerate',
                        'robust-oob-disabled', 'gpu-fetched-composed',
                        'gpu-fetched-composed-f32_3',
                        'gpu-fetched-composed-f32_2',
                        'gpu-fetched-compose-failure',
                        'gpu-fetched-wrong-digest',
                        'gpu-fetched-offset-misaligned',
                        'gpu-fetched-stride-misaligned',
                        'gpu-fetched-out-of-domain',
                        'gpu-fetched-out-of-bounds',
                        'gpu-fetched-aliased-source', 'varying-armed',
                        'varying-fragment-mismatch', 'varying-missing',
                        'sampled-armed', 'sampled-layer-armed',
                        'sampled-array-view-refused',
                        'multi-attribute-armed',
                        'multi-attribute-interleaved-armed',
                        'multi-attribute-overlap-refused',
                        'multi-attribute-unbound-refused',
                        'multi-attribute-alias-target-refused',
                        'multi-attribute-fetched-refused',
                        'indexed-armed', 'indexed-permuted-armed',
                        'indexed-robust-restart-armed',
                        'indexed-restart-enabled-refused',
                        'indexed-zero-record-degenerate',
                        'indexed-range-refused', 'indexed-unbound-refused',
                        'indexed-uint8-refused',
                        'indexed-alias-target-refused',
                        'indexed-fetched-refused', 'instanced-armed',
                        'instanced-first-instance-armed',
                        'instanced-index-armed', 'vertex-index-armed',
                        'instanced-robust-armed', 'instanced-zero-refused',
                        'instanced-out-of-bounds-refused',
                        'instanced-fetched-refused',
                        'multi-triangle-armed',
                        'non-triangle-count-refused',
                        'cull-back-kept-armed',
                        'cull-back-dropped-armed',
                        'sample-mask-zero-armed',
                        'write-mask-zero-armed']
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
  # The memory-model matrix: transport-injected allocation and map
  # refusals unwind to the exact Vulkan error and succeed once lifted,
  # mapped-range validation admits partial and empty ranges and refuses
  # out-of-range and wrap-shaped sizes, bind admission proves the
  # once-per-buffer aligned type-0 contract and admits aliasing windows,
  # and the budget arm holds the reported table to the platform-ceiling
  # contract, the zero commitment, the 64-byte noncoherent atom, and the
  # never-dedicated requirement.  The image-alias arm binds two
  # identically-created VK_IMAGE_CREATE_ALIAS_BIT transfer images over one
  # linear footprint and a third over a disjoint window of the same
  # allocation.  One known-bad arm asserts an admitted out-of-range flush,
  # so the range validation must fail it; the other asserts the flag
  # admitted on the render family, which binds at offset zero alone, so
  # the family-scoped admission must fail it.  That arm opens with a
  # plain render-family create and destroy, so its abort attributes to
  # the flag rather than to the shape.
  r3v_native_memory_model_harness = executable(
    'r3v_native_memory_model_harness',
    files('tests/r3v_native_memory_model_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : r3v_native_deps,
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach memory_arm : ['alloc-refused', 'map-refused', 'mapped-ranges',
                        'bind-admission', 'budget-contract',
                        'image-alias-binding']
    test(
      'r3v-native-memory-model-@0@'.format(memory_arm),
      r3v_native_memory_model_harness,
      args : [memory_arm],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  test(
    'r3v-native-memory-model-known-bad-range-admits',
    r3v_native_memory_model_harness,
    args : ['known-bad-range-admits'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    should_fail : true,
    suite : ['r3v'],
  )
  test(
    'r3v-native-memory-model-known-bad-render-family-alias-admits',
    r3v_native_memory_model_harness,
    args : ['known-bad-render-family-alias-admits'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    should_fail : true,
    suite : ['r3v'],
  )
  # The kernel memory-contract pin: each kernel-side fact the memory
  # model consumes is bound to a SHA-256 of its exact linux-radeon
  # policy-ledger row.  With R3V_KERNEL_MEMORY_CONTRACT_ROOT naming the
  # kernel checkout a moved row fails the check (a mismatch blocks, it
  # does not skip); with the checkout unavailable the check exits 77.
  # The selftest calibrates the pass and three failure classes on
  # synthetic fixtures.
  test(
    'r3v-kernel-memory-contract-pin-selftest',
    prog_python,
    args : [files('tests/r3v_kernel_memory_contract_pin.py'), '--selftest'],
    suite : ['r3v'],
  )
  test(
    'r3v-kernel-memory-contract-pin',
    prog_python,
    args : [files('tests/r3v_kernel_memory_contract_pin.py')],
    suite : ['r3v'],
  )
  # The kernel deployment reconciliation: before a plan-bound silicon
  # run, the pins, the kernel head, the deployed package's source
  # checkpoint, and the running module srcversion meet in one artifact
  # whose every source difference is classified from the checked-in
  # table (semantic_required, safety_required, optimization_only,
  # unrelated).  The selftest calibrates each verdict and each refusal
  # on a synthetic repository; the real reconciliation runs by hand
  # against the kernel checkout and the target's modinfo.
  test(
    'r3v-kernel-deployment-reconcile-selftest',
    prog_python,
    args : [files('tests/r3v_kernel_deployment_reconcile.py'), '--selftest'],
    suite : ['r3v'],
  )
  # The memory-concurrency matrix: independent memory cycles across
  # threads keep the atomic cache-sync arithmetic closed (one sync at map
  # establishment, one at the unmap or free publication), empty
  # submissions interleave with those cycles, concurrent frees of
  # live-mapped objects publish exactly once each, and a lost queue
  # refuses submission from the runtime without reaching the transport
  # while allocation, map, and free stay serviceable for teardown.  The
  # known-bad arm asserts a lost queue admitting a submission, so the
  # loss classification must fail it.
  r3v_native_memory_concurrency_harness = executable(
    'r3v_native_memory_concurrency_harness',
    files('tests/r3v_native_memory_concurrency_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : r3v_native_deps,
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach concurrency_arm : ['concurrent-map-free',
                             'concurrent-submit-teardown',
                             'teardown-live-frees',
                             'device-loss-classification']
    test(
      'r3v-native-memory-concurrency-@0@'.format(concurrency_arm),
      r3v_native_memory_concurrency_harness,
      args : [concurrency_arm],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  test(
    'r3v-native-memory-concurrency-known-bad-lost-queue-submits',
    r3v_native_memory_concurrency_harness,
    args : ['known-bad-lost-queue-submits'],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 60,
    should_fail : true,
    suite : ['r3v'],
  )
  # The identity verb's GPU route under the compute gate and the verb's
  # gate: the composed carrier pass against its pin, the shim's empty
  # delivery quarantining on the oracle divergence, and the named
  # refusals (FP24 domain, one buffer object for both ranges, a record
  # count past the single-row ceiling) each before any write; with the verb gate
  # closed the dispatch keeps the CPU route.  The gate-table arm proves
  # every ledger gate's exact-value read and that a non-identity gate
  # selects nothing; the pre-seeded arm calibrates the comparator's
  # positive side; the quarantined arm proves the refusal after a
  # divergence.
  r3v_native_compute_gpu_route_harness = executable(
    'r3v_native_compute_gpu_route_harness',
    files('tests/r3v_native_compute_gpu_route_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : r3v_native_deps,
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach compute_arm : ['gate-off-cpu', 'composed', 'out-of-domain-refused',
                         'alias-refused', 'ceiling-refused', 'gate-table',
                         'preseeded-agreement', 'quarantined-refused']
    test(
      'r3v-native-compute-gpu-route-@0@'.format(compute_arm),
      r3v_native_compute_gpu_route_harness,
      args : [compute_arm],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  # Plan replay under the shim: a captured two-draw transcript composes
  # into a plan bound to this process's identity; the replay arm consumes
  # it to exhaustion with the IB store and hash chain retained, and each
  # defect arm refuses by name and latches.
  r3v_native_plan_replay_harness = executable(
    'r3v_native_plan_replay_harness',
    [files('tests/r3v_native_plan_replay_harness.c'), sha1_h],
    include_directories : r3v_native_inc,
    dependencies : r3v_native_deps,
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach replay_arm : ['replay', 'reordered', 'duplicated', 'extra',
                        'missing-terminal', 'missing-terminal-entry',
                        'mutated-entry', 'two-buffers', 'deadline',
                        'ioctl-failure-then-continue', 'wrong-nonce',
                        'wrong-kernel', 'wrong-dso', 'wrong-pci',
                        'wrong-source', 'used-evidence-dir',
                        'gate-contamination', 'truncated-plan',
                        'hazard-gate-open', 'capture-and-plan']
    test(
      'r3v-native-plan-replay-@0@'.format(replay_arm),
      r3v_native_plan_replay_harness,
      args : [replay_arm],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  # Plan capture under the shim: two distinct draws in separate submits
  # land two ordered transcript entries with the retained digests and
  # the whole relocation shape; a two-buffer submit refuses; an open
  # hazard gate or an unreachable transcript refuses the device; a
  # second device in the process lands its draw at the first's ordinal-1
  # path while the first, submitting nothing, leaves the base path
  # absent; a stale ordinal-1 path refuses the device that would claim
  # it.
  r3v_native_plan_capture_harness = executable(
    'r3v_native_plan_capture_harness',
    files('tests/r3v_native_plan_capture_harness.c'),
    include_directories : r3v_native_inc,
    dependencies : r3v_native_deps,
    link_whole : [libr3v_native_impl],
    link_with : [libradeon_drm_vk],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
  )
  foreach capture_arm : ['capture', 'slot-roles', 'gate-open-refused',
                         'manifest-dir-refused', 'second-device-ordinal',
                         'derived-path-exists-refused',
                         'missing-directory-refused',
                         'relocation-count-diagnostic',
                         'write-failure-status']
    test(
      'r3v-native-plan-capture-@0@'.format(capture_arm),
      r3v_native_plan_capture_harness,
      args : [capture_arm],
      env : r3v_native_cell_env,
      depends : [libradeon_noop_drm_shim],
      timeout : 60,
      suite : ['r3v'],
    )
  endforeach
  # The composed plan: the harness transcript composes with declared
  # identities into a plan the tool checks; a missing identity refuses.
  test(
    'r3v-native-plan-compose',
    prog_python,
    args : [files('tests/r3v_native_plan_compose_check.py'),
            r3v_native_plan_capture_harness, r3v_native_plan_tool],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 120,
    suite : ['r3v'],
  )
  # A shard captured one process apiece leaves one transcript per case;
  # the shard composer gives each its own nonce and evidence directory,
  # since a replay session binds one nonce and creates session.state
  # with O_EXCL under one empty directory.  The selftest composes the
  # harness transcript under three case names and calibrates the
  # damaged-transcript and short-nonce refusals.
  test(
    'r3v-native-plan-compose-shard-selftest',
    prog_python,
    args : [files('tests/r3v_native_plan_compose_shard.py'), 'selftest',
            '--harness', r3v_native_plan_capture_harness,
            '--tool', r3v_native_plan_tool],
    env : r3v_native_cell_env,
    depends : [libradeon_noop_drm_shim],
    timeout : 120,
    suite : ['r3v'],
  )

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

  # The independent witness that a run entered the kernel with no
  # command submission, calibrated over its parser, every refusal its
  # verdict carries, and the shim arms that bound its scope; the
  # README's conformance ladder carries the mechanism.  The selftest
  # skips itself when strace is absent or cannot attach.
  test(
    'r3v-cs-ioctl-trace-selftest',
    prog_python,
    args : [
      files('tests/r3v_cs_ioctl_trace.py'),
      'selftest',
      '--harness', r3v_native_submit_order_harness.full_path(),
      '--shim', libradeon_noop_drm_shim.full_path(),
    ],
    depends : [r3v_native_submit_order_harness, libradeon_noop_drm_shim],
    timeout : 120,
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
          '../common/r300_tcl_bypass_triangle.c',
          '../common/r300_flat_color0_plan.c', '../common/r300_rs_tex_adj_probe.c', '../common/r300_noperspective_reciprocal_plan.c', '../common/r300_noperspective_q_lane_plan.c', '../common/r300_noperspective_mixed_carrier_plan.c'),
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
            '../common/r300_flat_color0_plan.c', '../common/r300_rs_tex_adj_probe.c', '../common/r300_noperspective_reciprocal_plan.c', '../common/r300_noperspective_q_lane_plan.c', '../common/r300_noperspective_mixed_carrier_plan.c',
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
          '../common/r300_tcl_bypass_triangle.c',
          '../common/r300_flat_color0_plan.c', '../common/r300_rs_tex_adj_probe.c', '../common/r300_noperspective_reciprocal_plan.c', '../common/r300_noperspective_q_lane_plan.c', '../common/r300_noperspective_mixed_carrier_plan.c'),
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
          '../common/r300_tcl_bypass_triangle.c',
          '../common/r300_flat_color0_plan.c', '../common/r300_rs_tex_adj_probe.c', '../common/r300_noperspective_reciprocal_plan.c', '../common/r300_noperspective_q_lane_plan.c', '../common/r300_noperspective_mixed_carrier_plan.c'),
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
          '../common/r300_tcl_bypass_triangle.c',
          '../common/r300_flat_color0_plan.c', '../common/r300_rs_tex_adj_probe.c', '../common/r300_noperspective_reciprocal_plan.c', '../common/r300_noperspective_q_lane_plan.c', '../common/r300_noperspective_mixed_carrier_plan.c'),
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
          '../common/r300_tcl_bypass_triangle.c',
          '../common/r300_flat_color0_plan.c', '../common/r300_rs_tex_adj_probe.c', '../common/r300_noperspective_reciprocal_plan.c', '../common/r300_noperspective_q_lane_plan.c', '../common/r300_noperspective_mixed_carrier_plan.c'),
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
            '../common/r300_tcl_bypass_triangle.c',
          '../common/r300_flat_color0_plan.c', '../common/r300_rs_tex_adj_probe.c', '../common/r300_noperspective_reciprocal_plan.c', '../common/r300_noperspective_q_lane_plan.c', '../common/r300_noperspective_mixed_carrier_plan.c'),
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
  # The producer cell's exact-submit replay binds the 64-byte carrier and
  # four-byte completion BO from submit_manifest.json to the full CS-track
  # parser, then rejects malformed stream, geometry, and truncation controls.
  # Runtime-gated on R3V_CS_TRACK_REPLAY_TOOL (skip 77 when unset).
  test(
    'r3v-native-producer-submit-object-replay',
    files('tests/run_producer_submit_object_replay.sh')[0],
    args : [r3v_native_producer_cell_harness],
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
