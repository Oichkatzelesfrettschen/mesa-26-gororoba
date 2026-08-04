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
