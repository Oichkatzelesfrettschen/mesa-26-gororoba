# SPDX-License-Identifier: MIT

# K8-safe CPU vertex execution: the scalar reference and the SSE2
# specialization behind one gather contract.  Host-only code; the
# native R3V layer links it, and the qualification oracle proves the
# implementations bit-identical against an independent reference model.
libr300_cpu = static_library(
  'r300_cpu',
  files('r300_cpu_vertex.c'),
  include_directories : [inc_src, inc_include],
  gnu_symbol_visibility : 'hidden',
)

if with_tests
  test(
    'r300-cpu-vertex',
    executable(
      'r300_cpu_vertex_test',
      files('tests/r300_cpu_vertex_test.c'),
      include_directories : [inc_src, inc_include,
                             include_directories('.')],
      link_with : [libr300_cpu],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )
endif
