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

r300_tcl_bypass_triangle_test = executable(
  'r300_tcl_bypass_triangle_test',
  files('r300_fragment_binary.c', 'r300_first_draw_state.c',
        'r300_tcl_bypass_triangle.c',
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
        'r300_tcl_bypass_triangle.c', 'r300_triangle_manifest.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
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

r300_first_draw_state_test = executable(
  'r300_first_draw_state_test',
  files('r300_fragment_binary.c', 'r300_tcl_bypass_triangle.c',
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
