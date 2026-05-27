# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT

files_r300 = files(
  'r300_blit.c',
  'r300_cb.h',
  'r300_chipset.c',
  'r300_chipset.h',
  'r300_context.c',
  'r300_context.h',
  'r300_cs.h',
  'r300_debug.c',
  'r300_defines.h',
  'r300_emit.c',
  'r300_emit.h',
  'r300_flush.c',
  'r300_fs.c',
  'r300_fs.h',
  'r300_hyperz.c',
  'r300_public.h',
  'r300_icd.c',
  'r300_query.c',
  'r300_reg.h',
  'r300_render.c',
  'r300_render_stencilref.c',
  'r300_render_translate.c',
  'r300_resource.c',
  'r300_screen_buffer.c',
  'r300_screen_buffer.h',
  'r300_screen.c',
  'r300_screen.h',
  'r300_shader_semantics.h',
  'r300_state.c',
  'r300_state_derived.c',
  'r300_state_inlines.h',
  'r300_texture.c',
  'r300_texture_desc.c',
  'r300_texture_desc.h',
  'r300_texture.h',
  'r300_nir_to_rc_direct.c',
  'r300_nir_to_rc_direct.h',
  'r300_tgsi_to_rc.c',
  'r300_tgsi_to_rc.h',
  'r300_transfer.c',
  'r300_transfer.h',
  'r300_vs.c',
  'r300_vs_draw.c',
  'r300_vs.h',
  'compiler/memory_pool.c',
  'compiler/memory_pool.h',
  'compiler/nir_to_rc.c',
  'compiler/nir_to_rc.h',
  'compiler/r300_fragprog.c',
  'compiler/r300_fragprog_emit.c',
  'compiler/r300_fragprog.h',
  'compiler/r300_fragprog_swizzle.c',
  'compiler/r300_fragprog_swizzle.h',
  'compiler/r3xx_fragprog.c',
  'compiler/r3xx_vertprog.c',
  'compiler/r3xx_vertprog_dump.c',
  'compiler/r500_fragprog.c',
  'compiler/r500_fragprog_emit.c',
  'compiler/r500_fragprog.h',
  'compiler/r300_nir.c',
  'compiler/r300_nir.h',
  'compiler/r300_nir_lower_vs_system_values.c',
  'compiler/r500_nir_lower_fcsel.c',
  'compiler/radeon_code.c',
  'compiler/radeon_code.h',
  'compiler/radeon_compiler.c',
  'compiler/radeon_compiler.h',
  'compiler/radeon_compiler_util.c',
  'compiler/radeon_compiler_util.h',
  'compiler/radeon_dataflow.c',
  'compiler/radeon_dataflow_deadcode.c',
  'compiler/radeon_dataflow.h',
  'compiler/radeon_dataflow_swizzles.c',
  'compiler/radeon_inline_literals.c',
  'compiler/radeon_list.c',
  'compiler/radeon_list.h',
  'compiler/radeon_opcodes.c',
  'compiler/radeon_opcodes.h',
  'compiler/radeon_optimize.c',
  'compiler/radeon_pair_dead_sources.c',
  'compiler/radeon_pair_regalloc.c',
  'compiler/radeon_pair_schedule.c',
  'compiler/radeon_pair_translate.c',
  'compiler/radeon_program_alu.c',
  'compiler/radeon_program_alu.h',
  'compiler/radeon_program.c',
  'compiler/radeon_program_constants.h',
  'compiler/radeon_program.h',
  'compiler/radeon_program_pair.c',
  'compiler/radeon_program_pair.h',
  'compiler/radeon_program_print.c',
  'compiler/radeon_program_tex.c',
  'compiler/radeon_program_tex.h',
  'compiler/radeon_regalloc.c',
  'compiler/radeon_regalloc.h',
  'compiler/radeon_remove_constants.c',
  'compiler/radeon_remove_constants.h',
  'compiler/radeon_swizzle.h',
  'compiler/radeon_variable.c',
  'compiler/radeon_variable.h',
  'compiler/radeon_vert_fc.c',
)

r300_nir_algebraic_c = custom_target(
  'r300_nir_algebraic.c',
  input : 'compiler/r300_nir_algebraic.py',
  output : 'r300_nir_algebraic.c',
  command : [
    prog_python, '@INPUT@', '-p', dir_compiler_nir, '@OUTPUT@',
  ],
  depend_files : nir_algebraic_depends,
)

libr300 = static_library(
  'r300',
  files_r300, r300_nir_algebraic_c,
  include_directories : [
    inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  gnu_symbol_visibility : 'hidden',
  dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
)

driver_r300 = declare_dependency(
  compile_args : '-DGALLIUM_R300',
  link_with : [libr300, libradeonwinsys],
)

if with_tests
  # Hand-built rc_program correctness corpus.  It links libr300 directly (the
  # static archive resolves the hidden-visibility RC symbols at link time) and
  # needs no winsys, so it stays a pure compiler-pass unit test.
  test(
    'r300-compiler-corpus',
    executable(
      'r300_compiler_corpus_test',
      files('compiler/tests/rc_compiler_corpus_test.c'),
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        include_directories('compiler'),
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # NIR-input admission harness for r300_nir_to_rc_direct: proves distinct
  # vertex attributes do not alias and that an unsupported VS system value
  # fails compilation deterministically instead of aborting.
  test(
    'r300-nir-vs-harness',
    executable(
      'r300_nir_vs_harness_test',
      files('compiler/tests/r300_nir_vs_harness_test.c'),
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        include_directories('compiler'),
      ],
      # libgallium supplies the ureg_*/tgsi_* symbols nir_to_rc.c references
      # (r300_nir_lower_for_rc shares that translation unit).
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Fragment-stage admission harness for r300_nir_to_rc_direct: proves the
  # multiply-add family compiles.  nir_op_fmad must emit RC_OPCODE_MAD, and
  # nir_op_flrp (fog / mix) must lower to a fmad chain before the emitter
  # rather than tripping its defensive raw-flrp guard.
  test(
    'r300-nir-fs-harness',
    executable(
      'r300_nir_fs_harness_test',
      files('compiler/tests/r300_nir_fs_harness_test.c'),
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        include_directories('compiler'),
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )
endif
