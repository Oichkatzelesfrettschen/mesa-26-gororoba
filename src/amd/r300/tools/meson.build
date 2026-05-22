# SPDX-License-Identifier: MIT

r300_raw_nop = executable(
  'r300_raw_nop',
  ['r300_raw_nop.c', sha1_h],
  include_directories : [inc_include, inc_src],
  dependencies : [dep_libdrm],
  install : false,
)

r300_raw_shader_triangle = executable(
  'r300_raw_shader_triangle',
  ['r300_raw_shader_triangle.c', sha1_h],
  include_directories : [inc_include, inc_src],
  dependencies : [dep_m],
  install : false,
)

r300_float24 = executable(
  'r300_float24',
  ['r300_float24.c'],
  dependencies : [dep_m],
  override_options : ['optimization=3'],
  install : false,
)
