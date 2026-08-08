# SPDX-License-Identifier: MIT

# Portable CPU vertex execution: the byte-defined baseline and the SSE2
# tuned path behind one gather contract.  Host-only code; the
# native R3V layer links it, and the qualification oracle proves the
# implementations bit-identical against an independent reference model.
libr300_cpu = static_library(
  'r300_cpu',
  files('r300_cpu_vertex.c'),
  include_directories : [inc_src, inc_include],
  gnu_symbol_visibility : 'hidden',
)

if with_tests
  # Timing bench, run by hand on the target host: baseline versus the
  # SSE2 and SSE3 candidates against the memcpy copy ceiling.  The
  # measurement is host-specific evidence, so no meson test wraps it.
  # This binary carries the build's own flags; a profile that pins
  # -march=x86-64 compiles the SSE3 lane out, and the lane reports
  # absent rather than substituting.
  executable(
    'r300_cpu_vertex_bench',
    files('tests/r300_cpu_vertex_bench.c'),
    include_directories : [inc_src, inc_include,
                           include_directories('.')],
    link_with : [libr300_cpu],
    gnu_symbol_visibility : 'hidden',
  )

  # The K8-target bench: the same bench with the gather compiled at the
  # TL-66 CPUID ceiling, so all three lanes exist under the exact
  # target codegen whatever the enclosing profile's -march is.  The
  # tune override rides along because the enclosing profiles set
  # -mtune=generic, and -mtune selects the scheduling model, so the
  # measured gather assembly matches the K8 pipeline only with both
  # flags pinned.  The gather compiles into this binary directly
  # because the lanes live in r300_cpu_vertex.c, and the library copy
  # keeps the profile's flags.
  if host_machine.cpu_family() == 'x86_64' and \
     cc.has_argument('-march=k8-sse3') and \
     cc.has_argument('-mtune=k8-sse3')
    executable(
      'r300_cpu_vertex_bench_k8',
      files('r300_cpu_vertex.c', 'tests/r300_cpu_vertex_bench.c'),
      include_directories : [inc_src, inc_include,
                             include_directories('.')],
      c_args : ['-march=k8-sse3', '-mtune=k8-sse3'],
      gnu_symbol_visibility : 'hidden',
    )
  endif

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
