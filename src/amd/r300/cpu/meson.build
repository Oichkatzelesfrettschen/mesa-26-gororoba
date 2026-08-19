# SPDX-License-Identifier: MIT

# Portable CPU vertex execution: the byte-defined baseline and the SSE2
# SIMD path behind one gather contract.  Host-only code; the native R3V
# implementation compiles this source into its own static implementation,
# while the CPU test links libr300_cpu.  The qualification oracle proves the
# implementations bit-identical against an independent reference model.
libr300_cpu = static_library(
  'r300_cpu',
  files('r300_cpu_vertex.c', 'r300_cpu_vertex_job.c'),
  include_directories : [inc_src, inc_include],
  gnu_symbol_visibility : 'hidden',
)

if with_tests
  # Silicon timing evidence comes from buildtype=release, so the bench
  # learns the Meson build type here -- compiler macros alone cannot
  # separate release from debugoptimized with b_ndebug -- and marks any
  # other build's row stream as smoke output.
  bench_release_args = get_option('buildtype') == 'release' \
    ? ['-DR300_CPU_VERTEX_BENCH_MESON_RELEASE=1'] : []

  # Timing bench, run by hand on the target host: baseline versus the
  # SSE2 and SSE3 candidates against the memcpy copy ceiling.  The
  # measurement is host-specific evidence, so no meson test wraps it.
  # This binary carries the build's own flags; a profile that pins
  # -march=x86-64 compiles the SSE3 lane out, and the lane reports
  # absent rather than substituting.
  executable(
    'r300_cpu_vertex_bench',
    [files('tests/r300_cpu_vertex_bench.c'), sha1_h],
    include_directories : [inc_src, inc_include,
                           include_directories('.')],
    link_with : [libr300_cpu],
    c_args : bench_release_args,
    dependencies : [dep_clock],
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
      [files('r300_cpu_vertex.c', 'tests/r300_cpu_vertex_bench.c'), sha1_h],
      include_directories : [inc_src, inc_include,
                             include_directories('.')],
      c_args : ['-march=k8-sse3', '-mtune=k8-sse3',
                '-DR300_CPU_VERTEX_BENCH_REQUIRE_K8=1'] + bench_release_args,
      dependencies : [dep_clock],
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

  test(
    'r300-cpu-vertex-job',
    executable(
      'r300_cpu_vertex_job_test',
      files('tests/r300_cpu_vertex_job_test.c'),
      include_directories : [inc_src, inc_include,
                             include_directories('.')],
      link_with : [libr300_cpu],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Differential equivalence of the SSE2 job kernel against the scalar
  # authority: randomized jobs over hostile bit patterns byte-identical,
  # numeric-policy witnesses reproduced, refusal errnos shared.  The
  # test self-skips where the build target lacks SSE2.
  test(
    'r300-cpu-vertex-job-sse2',
    executable(
      'r300_cpu_vertex_job_sse2_test',
      files('tests/r300_cpu_vertex_job_sse2_test.c'),
      include_directories : [inc_src, inc_include,
                             include_directories('.')],
      link_with : [libr300_cpu],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )
endif
