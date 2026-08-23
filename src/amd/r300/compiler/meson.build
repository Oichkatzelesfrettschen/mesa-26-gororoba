# SPDX-License-Identifier: MIT

# The R300 shader compiler as a single build owner.  The sources include
# NIR and util headers only, so the library carries no Gallium, TGSI, LLVM,
# or xmlconfig dependency; the Gallium r300 driver and the compiler test
# executables link this archive instead of compiling the sources into
# themselves.
inc_amd_r300_compiler = include_directories('.')

# The parent meson.build enters this directory for either front end, so the
# library builds under the same condition and a Vulkan-only configuration
# still defines libr300_compiler.
r300_nir_algebraic_c = custom_target(
  'r300_nir_algebraic.c',
  input : 'r300_nir_algebraic.py',
  output : 'r300_nir_algebraic.c',
  command : [
    prog_python, '@INPUT@', '-p', dir_compiler_nir, '@OUTPUT@',
  ],
  depend_files : nir_algebraic_depends,
)

files_r300_compiler = files(
  'classic/r300_classic_emit.c',
  'classic/r300_classic_emit.h',
  'classic/r300_classic_ir.c',
  'classic/r300_classic_ir.h',
  'classic/r300_classic_regalloc.c',
  'classic/r300_classic_regalloc.h',
  'classic/r300_classic_schedule.c',
  'classic/r300_classic_schedule.h',
  'classic/r300_classic_select.c',
  'classic/r300_classic_select.h',
  'classic/r300_classic_target.c',
  'classic/r300_classic_target.h',
  'classic/r300_classic_validate_schedule.c',
  'classic/r300_classic_validate_schedule.h',
  'memory_pool.c',
  'memory_pool.h',
  'nir_to_rc.c',
  'nir_to_rc.h',
  'r300_fragprog.c',
  'r300_fragprog_emit.c',
  'r300_fragprog.h',
  'r300_fragprog_swizzle.c',
  'r300_fragprog_swizzle.h',
  'r3xx_fragprog.c',
  'r3xx_vertprog.c',
  'r3xx_vertprog_dump.c',
  'r500_fragprog.c',
  'r500_fragprog_emit.c',
  'r500_fragprog.h',
  'r300_nir.c',
  'r300_r2vb_nir.c',
  'r300_r2vb_nir.h',
  'r300_nir_fold_periodic_loops.c',
  'r300_nir.h',
  'r300_nir_lower_bitwise_to_arith.c',
  'r300_nir_lower_f2i_epsilon.c',
  'r300_nir_lower_ieee16.c',
  'r300_nir_lower_vs_system_values.c',
  'r300_shader_code.h',
  'r300_vertex_job_nir.c',
  'r300_vertex_job_nir.h',
  'r500_nir_lower_fcsel.c',
  'radeon_code.c',
  'radeon_code.h',
  'radeon_compiler.c',
  'radeon_compiler.h',
  'radeon_compiler_util.c',
  'radeon_compiler_util.h',
  'radeon_dataflow.c',
  'radeon_dataflow_deadcode.c',
  'radeon_dataflow.h',
  'radeon_dataflow_swizzles.c',
  'radeon_inline_literals.c',
  'radeon_list.c',
  'radeon_list.h',
  'radeon_opcodes.c',
  'radeon_opcodes.h',
  'radeon_optimize.c',
  'radeon_pair_dead_sources.c',
  'radeon_pair_regalloc.c',
  'radeon_pair_schedule.c',
  'radeon_pair_translate.c',
  'radeon_program_alu.c',
  'radeon_program_alu.h',
  'radeon_program.c',
  'radeon_program_constants.h',
  'radeon_program.h',
  'radeon_program_pair.c',
  'radeon_program_pair.h',
  'radeon_program_print.c',
  'radeon_regalloc.c',
  'radeon_regalloc.h',
  'radeon_remove_constants.c',
  'radeon_remove_constants.h',
  'radeon_swizzle.h',
  'radeon_variable.c',
  'radeon_variable.h',
  'radeon_vert_fc.c',
)

libr300_compiler = static_library(
  'r300_compiler',
  files_r300_compiler, r300_nir_algebraic_c, spirv_info_h,
  include_directories : [
    inc_src, inc_include, inc_amd_r300_common, inc_amd_r300_compiler,
  ],
  gnu_symbol_visibility : 'hidden',
  dependencies : [idep_mesautil, idep_nir],
)

# Source registration and declaration reach run from common.  This artifact
# leg independently proves that the named common and compiler symbols are
# defined by the archives built for the same configuration.
prog_nm_r300_liveness = find_program('nm', required : false)
if with_tests and prog_nm_r300_liveness.found()
  test(
    'r300-common-liveness-census-archives',
    prog_python,
    args : [files('../common/tests/r300_common_liveness_census.py'),
            meson.project_source_root(),
            '--archives',
            '--nm', prog_nm_r300_liveness.full_path(),
            '--common-archive', libr300_common.full_path(),
            '--compiler-archive', libr300_compiler.full_path()],
    depends : [libr300_common, libr300_compiler],
    suite : ['r300'],
  )
endif
