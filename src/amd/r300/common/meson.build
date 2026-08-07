# SPDX-License-Identifier: MIT

# R300 format and R2VB source-contract tests. These are host-only tests over
# the API-neutral R300 format vocabulary and its Gallium adapter; they perform
# no winsys setup and submit no command stream.
r300_format_contract_test_c_args = ['-Werror=unused-function']
r300_format_contract_test_includes = [
  inc_src,
  inc_include,
  inc_gallium,
  inc_gallium_aux,
  include_directories('../../../gallium/drivers/r300'),
]

r300_r2vb_source_contract_test = executable(
  'r300_r2vb_source_contract_test',
  files('../../../gallium/drivers/r300/compiler/tests/r300_r2vb_source_contract_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-source-contract',
  r300_r2vb_source_contract_test,
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
  r300_staging_manifest_test,
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

r300_vertex_format_pipe_test = executable(
  'r300_vertex_format_pipe_test',
  files('../../../gallium/drivers/r300/compiler/tests/r300_vertex_format_pipe_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-vertex-format-pipe',
  r300_vertex_format_pipe_test,
  suite : ['r300'],
)
