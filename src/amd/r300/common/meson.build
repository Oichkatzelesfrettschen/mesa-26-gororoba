# SPDX-License-Identifier: MIT

# Host-only tests over the API-neutral R300 hardware vocabulary.  Everything
# here builds against inc_src, inc_include, inc_amd_common, and this
# directory alone, which keeps the common layer's independence from the
# Gallium tree enforceable at the build level; the Gallium adapter tests
# live under src/gallium/drivers/r300.  These tests perform no winsys setup
# and submit no command stream.
r300_format_contract_test_c_args = ['-Werror=unused-function']
r300_format_contract_test_includes = [
  inc_src,
  inc_include,
  inc_amd_common,
  include_directories('.'),
]


r300_r2vb_carrier_delivery_test = executable(
  'r300_r2vb_carrier_delivery_test',
  files('r300_r2vb_carrier_delivery.c', '../cpu/r300_cpu_vertex.c',
        'tests/r300_r2vb_carrier_delivery_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-carrier-delivery',
  r300_r2vb_carrier_delivery_test,
  suite : ['r300'],
)

r300_delivery_route_test = executable(
  'r300_delivery_route_test',
  files('r300_delivery_route.c', 'tests/r300_delivery_route_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-delivery-route',
  r300_delivery_route_test,
  suite : ['r300'],
)

r300_r2vb_producer_pass_test = executable(
  'r300_r2vb_producer_pass_test',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_producer_pass.c', 'r300_tcl_bypass_triangle.c',
        'tests/r300_r2vb_producer_pass_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-producer-pass',
  r300_r2vb_producer_pass_test,
  suite : ['r300'],
)

# Manifest writer: the no-submit evidence generator for the producer
# pass; the offline replay drives it.
r300_r2vb_producer_manifest = executable(
  'r300_r2vb_producer_manifest',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_producer_manifest.c', 'r300_r2vb_producer_pass.c',
        'r300_tcl_bypass_triangle.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# Offline kernel replay of the producer pass, gated on
# R3V_CS_TRACK_REPLAY_TOOL; R3V_KERNEL_REPLAY_TOOL adds the TCL-bypass
# width predicate's declared-scope decline.  The script exits 77 (meson
# SKIP) when the tool is absent, so the default graph never depends on a
# sibling checkout.
test(
  'r300-r2vb-producer-replay',
  files('tests/run_r2vb_producer_replay.sh')[0],
  args : [r300_r2vb_producer_manifest],
  suite : ['r300'],
)

# The producer writer's artifacts are checked against each other and against
# the emitted VAP tuple.  Calibration mutants keep a malformed retained
# manifest from becoming a green replay-only result.
test(
  'r300-r2vb-producer-manifest-integration',
  prog_python,
  args : [files('tests/r300_r2vb_producer_manifest_check.py'),
          r300_r2vb_producer_manifest.full_path()],
  depends : [r300_r2vb_producer_manifest],
  suite : ['r300'],
)

r300_fragment_binary_test = executable(
  'r300_fragment_binary_test',
  files('r300_fragment_binary.c', 'tests/r300_fragment_binary_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-fragment-binary',
  r300_fragment_binary_test,
  suite : ['r300'],
)

r300_pm4_builder_test = executable(
  'r300_pm4_builder_test',
  files('r300_pm4_builder.c', 'tests/r300_pm4_builder_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-pm4-builder',
  r300_pm4_builder_test,
  suite : ['r300'],
)

r300_tcl_bypass_triangle_test = executable(
  'r300_tcl_bypass_triangle_test',
  files('r300_fragment_binary.c', 'r300_first_draw_state.c',
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c',
        'tests/r300_tcl_bypass_triangle_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-tcl-bypass-triangle',
  r300_tcl_bypass_triangle_test,
  suite : ['r300'],
)

# Manifest writer: the no-submit evidence generator for the fixed cell.  The
# replay test drives it and the attended-cell staging invokes it directly.
r300_triangle_manifest = executable(
  'r300_triangle_manifest',
  files('r300_fragment_binary.c', 'r300_first_draw_state.c',
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c',
        'r300_triangle_manifest.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# The writer's own artifacts, checked against each other: the unit test
# proves the helper, this proves the executable published what the helper
# computed.
test(
  'r300-triangle-manifest-integration',
  prog_python,
  args : [files('tests/r300_triangle_manifest_check.py'),
          r300_triangle_manifest.full_path()],
  depends : [r300_triangle_manifest],
  suite : ['r300'],
)

r300_staging_manifest_test = executable(
  'r300_staging_manifest_test',
  files('r300_fragment_binary.c', 'r300_first_draw_state.c',
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c',
        'tests/r300_staging_manifest_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-staging-manifest',
  prog_python,
  args : [files('tests/r300_staging_manifest_runner.py'),
          r300_staging_manifest_test,
          files('tests/r300_triangle_manifest_check.py'),
          r300_triangle_manifest.full_path()],
  depends : [r300_staging_manifest_test, r300_triangle_manifest],
  suite : ['r300'],
)
test(
  'r300-staging-manifest-runner-selftest',
  prog_python,
  args : [files('tests/r300_staging_manifest_runner.py'), '--selftest'],
  suite : ['r300'],
)

# Offline kernel-parser replay, gated on R3V_KERNEL_REPLAY_TOOL pointing at
# the replay binary built in the Linux radeon source tree.  The script exits
# 77 (meson SKIP) when the tool is absent, so the default graph never
# depends on a sibling checkout.
test(
  'r300-tcl-bypass-offline-replay',
  files('tests/run_offline_ib_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)

# Full CS parser and tracker replay, gated on R3V_CS_TRACK_REPLAY_TOOL
# pointing at replay_r300_cs_track built in the Linux radeon source tree.
# The width replay above decides one predicate; this one walks packet
# framing, register admission, relocation consumption, and every buffer
# bound r100_cs_track_check applies.  R3V_CS_TRACK_CONTROLS adds the
# parser-invalid and semantically blank control classes.
test(
  'r300-cs-track-replay',
  files('tests/run_cs_track_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)

# The 2D solid-fill direct-write control cell: the emitter, its oracle,
# the manifest writer, and the offline kernel-parser replay with the
# parser-decidable known-bad arms.
r300_direct_write_test = executable(
  'r300_direct_write_test',
  files('r300_direct_write.c', 'r300_fragment_binary.c',
        'r300_first_draw_state.c', 'r300_pm4_builder.c',
        'r300_tcl_bypass_triangle.c', 'tests/r300_direct_write_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-direct-write',
  r300_direct_write_test,
  suite : ['r300'],
)

r300_direct_write_manifest = executable(
  'r300_direct_write_manifest',
  files('r300_direct_write.c', 'r300_direct_write_manifest.c',
        'r300_fragment_binary.c', 'r300_first_draw_state.c',
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-direct-write-cs-track-replay',
  files('tests/run_direct_write_cs_track_replay.sh')[0],
  args : [r300_direct_write_manifest],
  suite : ['r300'],
)

# The writer's own artifacts, checked against each other: the unit test
# proves the emitter and oracle, this proves the executable published
# what the emitter computed.
test(
  'r300-direct-write-manifest-integration',
  prog_python,
  args : [files('tests/r300_direct_write_manifest_check.py'),
          r300_direct_write_manifest.full_path()],
  depends : [r300_direct_write_manifest],
  suite : ['r300'],
)

r300_first_draw_state_test = executable(
  'r300_first_draw_state_test',
  files('r300_fragment_binary.c', 'r300_pm4_builder.c',
        'r300_tcl_bypass_triangle.c',
        'r300_first_draw_state.c', 'tests/r300_first_draw_state_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-first-draw-state',
  r300_first_draw_state_test,
  suite : ['r300'],
)

