# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT


files_r300 = files(
  # Neutral R300 PM4 emitters shared with the direct Vulkan driver; the
  # dependency points from Gallium into src/amd/r300/common alone.
  '../../../amd/r300/common/r300_pm4_builder.c',
  '../../../amd/r300/common/r300_r2vb_fetch_pass.c',
  '../../../amd/r300/common/r300_r2vb_target_state.c',
  '../../../amd/r300/common/r300_carrier_policy.c',
  '../../../amd/r300/common/r300_numeric_domain.c',
  '../../../amd/r300/common/r300_shader_semantics.h',
  'r300_blit.c',
  'r300_cb.h',
  'r300_chipset.c',
  'r300_chipset.h',
  'r300_context.c',
  'r300_context.h',
  'r300_cs.h',
  'r300_hb_tcl.c',
  'r300_hb_tcl.h',
  'r300_hb_r400_us.c',
  'r300_hb_r400_us.h',
  'r300_video.c',
  'r300_video.h',
  'r300_debug.c',
  'r300_defines.h',
  'r300_emit.c',
  'r300_emit.h',
  'r300_flush.c',
  'r300_fs.c',
  'r300_fs.h',
  'r300_nir_ssa_cut.c',
  'r300_nir_ssa_cut.h',
  'r300_hyperz.c',
  'r300_public.h',
  'r300_icd.c',
  'r300_query.c',
  'r300_r2vb.c',
  'r300_r2vb_plan.c',
  'r300_r2vb_plan.h',
  'r300_r2vb_reingest_state.h',
  'r300_r2vb_telemetry.c',
  'r300_r2vb_telemetry.h',
  'r300_render.c',
  'r300_render_stencilref.c',
  'r300_render_translate.c',
  'r300_resource.c',
  'r300_screen_buffer.c',
  'r300_screen_buffer.h',
  'r300_screen.c',
  'r300_screen.h',
  'r300_state.c',
  'r300_state_derived.c',
  'r300_state_inlines.h',
  'r300_swtcl_gtt.h',
  'r300_texture.c',
  'r300_texture_desc.c',
  'r300_texture_desc.h',
  'r300_texture.h',
  'r300_compute_admission.c',
  'r300_transfer.c',
  'r300_transfer.h',
  'r300_vs.c',
  'r300_vs_draw.c',
  'r300_vs.h',
)

libr300 = static_library(
  'r300',
  files_r300,
  include_directories : [
    inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
    inc_amd_r300_common, inc_amd_r300_compiler,
  ],
  gnu_symbol_visibility : 'hidden',
  link_with : [libr300_compiler],
  dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
)

driver_r300 = declare_dependency(
  compile_args : '-DGALLIUM_R300',
  link_with : [libr300, libradeonwinsys],
)

if with_tests
  # Test translation units carry their own registration duty: a static
  # test function defined but never wired into main() is a defect, so the
  # driver-wide -Wno-unused-function relaxation is reversed for tests.
  r300_test_c_args = ['-Werror=unused-function']
  # Hand-built rc_program correctness corpus.  It links libr300 directly (the
  # static archive resolves the hidden-visibility RC symbols at link time) and
  # needs no winsys, so it stays a pure compiler-pass unit test.
  test(
    'r300-compiler-corpus',
    executable(
      'r300_compiler_corpus_test',
      files('../../../amd/r300/compiler/tests/rc_compiler_corpus_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # NIR-input admission harness for nir_to_rc: proves distinct vertex
  # attributes do not alias and that an unsupported VS system value fails
  # compilation deterministically instead of aborting.
  test(
    'r300-nir-vs-harness',
    executable(
      'r300_nir_vs_harness_test',
      files('../../../amd/r300/compiler/tests/r300_nir_vs_harness_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      # libgallium supplies the ureg_*/tgsi_* symbols referenced from the
      # production NIR-to-RC translation unit.
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  test(
    'r300-vs-ushr-int-lowering',
    executable(
      'r300_vs_ushr_int_lowering_test',
      files('../../../amd/r300/compiler/tests/r300_vs_ushr_int_lowering_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # transform_DP2 lowering guard: RC_OPCODE_DP2 -> RC_OPCODE_DP3 with the Z/W
  # swizzles of both sources forced to ZERO, so the three-term dot reproduces the
  # true two-term dot.  Nothing else pins this rc-level lowering (the pair value
  # oracle never sees DP2; the parity test only evaluates opcode semantics).
  test(
    'r300-dp2-lowering',
    executable(
      'r300_dp2_lowering_test',
      files('../../../amd/r300/compiler/tests/r300_dp2_lowering_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Vertex sibling of r300-dp2-lowering: the r300..r500 vertex engine's only dot
  # is DP4, so transform_r300_vertex_DP2/DP3 widen DP2/DP3 to DP4 with the surplus
  # terms' swizzles forced to ZERO (Z+W for DP2, W for DP3).  Nothing pinned these
  # widenings -- there was no vertex-ALU-transform test at all.
  test(
    'r300-vertex-dot-lowering',
    executable(
      'r300_vertex_dot_lowering_test',
      files('../../../amd/r300/compiler/tests/r300_vertex_dot_lowering_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Fragment-stage admission harness for nir_to_rc: proves the
  # multiply-add family compiles.  nir_op_fmad must emit RC_OPCODE_MAD, and
  # nir_op_flrp (fog / mix) must lower to a fmad chain before the emitter
  # rather than tripping its defensive raw-flrp guard.
  test(
    'r300-nir-fs-harness',
    executable(
      'r300_nir_fs_harness_test',
      files('../../../amd/r300/compiler/tests/r300_nir_fs_harness_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Classify-only compute-admission harness: a compute kernel that only loads,
  # does FP24-range arithmetic, and writes its buffer output is admissible;
  # shared memory, a barrier, a general atomic, a scatter, or FP64 reject.
  test(
    'r300-nir-compute-admission',
    executable(
      'r300_nir_compute_admission_test',
      files('tests/r300_nir_compute_admission_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Pure state-machine guard for the SW-TCL GTT drain budget. The streak
  # survives allocations until usage falls, the hard cap rejects unbounded
  # growth, and the API draw boundary clears only per-draw state.
  test(
    'r300-swtcl-gtt-backpressure',
    executable(
      'r300_swtcl_gtt_backpressure_test',
      files('tests/r300_swtcl_gtt_backpressure_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
      ],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Compile-budget gate for the H.264 back-half fragment programs: builds the
  # vl_h264_idct and vl_h264_mc kernels as raw NIR and runs them through
  # r300_optimize_nir + nir_to_rc against an is_r500=false screen, proving every
  # op lowers for R300 and the emitted instruction counts fit the fragment
  # limits.  Links libgalliumvl for the kernel builders.
  test(
    'r300-h264-fs-budget',
    executable(
      'r300_h264_fs_budget_test',
      files('tests/r300_h264_fs_budget_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300, libgalliumvl, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Byte-identity pin between the legacy pipe-format PSC construction and the
  # neutral vertex-format identity the producer interface routes through.
  test(
    'r300-r2vb-psc-byte-identity',
    executable(
      'r300_r2vb_psc_byte_identity_test',
      files('tests/r300_r2vb_psc_byte_identity_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        include_directories('.'),
      ],
      dependencies : [idep_mesautil],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Six-dword FLOAT_4+FLOAT_2 producer tuple through the gated pure chain,
  # with the structural standing exclusion (ungated entries decline) and the
  # exact-value gate parser calibration.
  test(
    'r300-r2vb-float2-tuple',
    executable(
      'r300_r2vb_float2_tuple_test',
      files('tests/r300_r2vb_float2_tuple_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # R2VB carry-BO producer split decision: an over-budget position-pass FS cut
  # at a single-vec4 SSA frontier into two FP32 halves that both compile under
  # the 64-slot ceiling; wide and unproven integer carries decline, while
  # range-proven signed and unsigned carries use matching typed conversions.
  # Pure NIR + the production nir_to_rc oracle, no winsys.
  test(
    'r300-r2vb-producer-split',
    executable(
      'r300_r2vb_producer_split_test',
      files('tests/r300_r2vb_producer_split_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Route-chain oracle for the R2VB producer plan, unit tier: hand-built
  # application-VS NIR through the production shape validation, restage,
  # plan, cut walk, and admission compile.  Typed over-budget producers plan
  # SPLIT with {f,b}/{f,i}/{f,u} carries; range, signedness, structural, and
  # under-budget-typed rows reject with their class as the primary reason.
  test(
    'r300-r2vb-plan-oracle',
    executable(
      'r300_r2vb_plan_oracle_test',
      files('tests/r300_r2vb_plan_oracle_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Admission-versus-CSO identity: the plan's throwaway measurement and the
  # actual fragment-shader CSO precompile run the same program under the same
  # flat-producer contract, on the plan's canonical baseline and on both
  # rebuilt split halves, and the emitted backend resource vector must agree.
  test(
    'r300-r2vb-admission-cso-identity',
    executable(
      'r300_r2vb_admission_cso_identity_test',
      files('tests/r300_r2vb_admission_cso_identity_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      # The real state vtable reaches r300_video.c, whose codec constructors
      # resolve from the video auxiliary library.
      link_with : [libr300, libgalliumvl, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Standing-route telemetry calibration: counters count per plan class, the
  # retention gate fails closed, an over-budget producer retains exactly one
  # content-hash file, a recurring shape deduplicates, and a structural
  # reject never retains.
  test(
    'r300-r2vb-telemetry',
    executable(
      'r300_r2vb_telemetry_test',
      files('tests/r300_r2vb_telemetry_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      # The context object carries the teardown summary hook, and its video
      # codec constructors resolve from the video auxiliary library.
      link_with : [libr300, libgallium, libgalliumvl],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Retained-corpus census loader: replans every telemetry-retained producer
  # blob (r2vb-vs-<blake3>.nir in the directory named by
  # R300_R2VB_RETAINED_CORPUS) through r300_r2vb_plan_producer under both
  # computed-varying keys and both position spaces, one census row per
  # (specimen, cv, space).  Each file hash-verifies against its content-hash
  # filename and decodes in a forked child; for a nonempty corpus, the
  # producer SHA in its manifest must match this build (git_sha1.h).  Without
  # a corpus directory or matching entries, the run exits 77 so meson reports
  # SKIP.
  test(
    'r300-r2vb-retained-corpus-census',
    executable(
      'r300_r2vb_retained_corpus_census',
      [files('tests/r300_r2vb_retained_corpus_census.c'), sha1_h],
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      # r300_screen_init_nir_options pulls r300_screen.c.o, whose screen
      # vtable reaches the g3dvl constructors, so the video auxiliary
      # library joins the link exactly as in the telemetry test.
      link_with : [libr300, libgallium, libgalliumvl],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    # A retained corpus runs four planner keys per specimen and may contain
    # many serialized shaders; reserve a corpus-sized window instead of the
    # 30-second Meson default.
    timeout : 300,
    suite : ['r300'],
  )

  # AUTO_SINGLE canary calibration: the exact-value gate and strict-decimal
  # floor parsers fail closed on every malformed input, and the pure
  # admission policy composes route-support shape, both delivery cells
  # (READY SINGLE untyped one-input for cv=0 clip and cv=0 window), and the
  # vertex floor -- synthetic plans drive each decline arm, live planner
  # output calibrates the execute row.
  test(
    'r300-r2vb-auto-single',
    executable(
      'r300_r2vb_auto_single_test',
      files('tests/r300_r2vb_auto_single_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      # The policy lives in r300_r2vb.c, whose context object reaches the
      # video codec constructors in the video auxiliary library.
      link_with : [libr300, libgallium, libgalliumvl],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Diagnostic typed-split route calibration: the exact-value gate parser
  # admits only "1", and the route contract admits only the position cell of
  # a READY SPLIT plan with a typed source, flat-vertex semantics, a one-vec4
  # carry, and an owned candidate; every decline arm calibrates on synthetic
  # plans plus the two live decline classes.
  test(
    'r300-r2vb-typed-route',
    executable(
      'r300_r2vb_typed_route_test',
      files('tests/r300_r2vb_typed_route_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      # The contract lives in r300_r2vb.c, whose context object reaches the
      # video codec constructors in the video auxiliary library.
      link_with : [libr300, libgallium, libgalliumvl],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # MVP route classifier: UBO block or deref binding zero is the only matrix
  # source mirrored by the executor, and literal offset chains are folded on a
  # throwaway clone.
  test(
    'r300-r2vb-mvp-match',
    executable(
      'r300_r2vb_mvp_match_test',
      files('tests/r300_r2vb_mvp_match_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Fragment input-semantics helper: the interpolated contract nudges every
  # f2i/f2u source by 1 + 2^-15 and the flat R2VB-producer contract omits it,
  # with an observable one-integer boundary difference.  Pure NIR helper +
  # nir_lower_int_to_float fold, no winsys.
  test(
    'r300-fs-input-semantics',
    executable(
      'r300_fs_input_semantics_test',
      files('../../../amd/r300/compiler/tests/r300_fs_input_semantics_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, dep_m, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  test(
    'r300-nir-to-rc',
    executable(
      'r300_nir_to_rc_test',
      files('../../../amd/r300/compiler/tests/r300_nir_to_rc_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Classic fragment-compiler target descriptor: diffs every budget field
  # against the radeon_code.h authority and the r300_fs.c selection logic.
  test(
    'r300-classic-target',
    executable(
      'r300_classic_target_test',
      files('../../../amd/r300/compiler/tests/r300_classic_target_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Classic fragment-compiler golden parity: both front ends into rc_programs,
  # executed by a CPU evaluator on shared inputs; outputs must agree.
  test(
    'r300-classic-parity',
    executable(
      'r300_classic_parity_test',
      files('../../../amd/r300/compiler/tests/r300_classic_parity_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Oracle (b), schedule legality: legality on the emitted rc_pair_instruction
  # stream, checked against the register-documented RGB/Alpha cross-pipe
  # coupling and presub-NOP hazard, independent of the CPU-evaluator parity
  # check above.  Corpus half compiles through the unmodified legacy
  # pipeline; synthetic half isolates each check against a hand-built pair.
  test(
    'r300-classic-validate-schedule',
    executable(
      'r300_classic_validate_schedule_test',
      files('../../../amd/r300/compiler/tests/r300_classic_validate_schedule_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Oracle (c), reference-path determinism: compiling the same shader shape
  # twice through nir_to_rc and the full backend pass chain must produce
  # byte-identical R300 hardware code. This proves the harness itself before
  # a byte-diff gates any new backend work.
  test(
    'r300-classic-byte-exact-selfdiff',
    executable(
      'r300_classic_byte_exact_selfdiff_test',
      files('../../../amd/r300/compiler/tests/r300_classic_byte_exact_selfdiff_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # New R300 pair scheduler (R300_CLASSIC_NEW_SCHED path): the subset ALU
  # corpus is translated then scheduled by r300_classic_schedule, and gated by
  # the schedule-legality oracle plus a pure-reordering / topological-order
  # equivalence check; a legacy control and a texture-deferral case bound it.
  test(
    'r300-classic-new-sched',
    executable(
      'r300_classic_new_sched_test',
      files('../../../amd/r300/compiler/tests/r300_classic_new_sched_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Oracle (a) on the scheduled-and-merged pair stream: a value-level CPU
  # evaluator over RC_INSTRUCTION_PAIR itself, exercised on hand-built
  # merge/reorder/refusal corpora with directly computable expected values
  # and on a real-front-end shader chained to the existing nir_to_rc
  # reference oracle.  Gates the cross-instruction RGB+Alpha merge and the
  # critical-path reorder tie-break r300_classic_schedule now performs.
  test(
    'r300-classic-pair-value',
    executable(
      'r300_classic_pair_value_test',
      files('../../../amd/r300/compiler/tests/r300_classic_pair_value_test.c',
            '../../../amd/r300/compiler/tests/r300_pair_eval.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Classic fragment-compiler emission: the whole classic ladder feeding the
  # real backend pass chain through hardware code generation.
  test(
    'r300-classic-emit',
    executable(
      'r300_classic_emit_test',
      files('../../../amd/r300/compiler/tests/r300_classic_emit_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Compiled US blocks for the native cells: --check proves a checked-in
  # golden header regenerates from the classic ladder and the production
  # cb_code baker; --emit rewrites it.
  r300_tcl_bypass_fs_tool = executable(
    'r300_tcl_bypass_fs_tool',
    files('../../../amd/r300/compiler/tests/r300_tcl_bypass_fs_tool.c'),
    c_args : r300_test_c_args,
    include_directories : [
      inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
      inc_amd_r300_common,
      inc_amd_r300_compiler,
    ],
    link_with : [libr300, libgallium],
    dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
    gnu_symbol_visibility : 'hidden',
  )
  test(
    'r300-tcl-bypass-fs-block-regeneration',
    r300_tcl_bypass_fs_tool,
    args : ['--check=triangle'],
    suite : ['r300'],
  )
  test(
    'r300-r2vb-producer-fs-block-regeneration',
    r300_tcl_bypass_fs_tool,
    args : ['--check=r2vb-producer'],
    suite : ['r300'],
  )

  # Classic fragment-compiler register allocation: chain recycling and the
  # register-file boundary, fit and reject sides.
  test(
    'r300-classic-regalloc',
    executable(
      'r300_classic_regalloc_test',
      files('../../../amd/r300/compiler/tests/r300_classic_regalloc_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Classic fragment-compiler NIR selection: the supported instruction
  # subset selects, and everything outside it rejects with a named reason.
  test(
    'r300-classic-select',
    executable(
      'r300_classic_select_test',
      files('../../../amd/r300/compiler/tests/r300_classic_select_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Classic fragment-compiler IR: SSA def-before-use validation and the
  # text dump, positive and negative cases.
  test(
    'r300-classic-ir',
    executable(
      'r300_classic_ir_test',
      files('../../../amd/r300/compiler/tests/r300_classic_ir_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # CPU oracle for the virtual IEEE FP16 machine (R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL).
  # Tests domain catalog registration, all 65536 FP16 classifications, and
  # representative multiply pairs (2-limb base-64 vs direct reference).
  # Hardware not run; RS482 silicon probing is the next gate.
  test(
    'r300-fp16-limb-oracle',
    executable(
      'r300_fp16_limb_oracle',
      files('tests/r300_fp16_limb_oracle.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        inc_amd_r300_compiler,
      ],
      link_with : [libr300],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )
  # Host corpus for the R2VB Draw-parity clip classifier (r300_r2vb_clip.h):
  # pins the draw_cliptest_tmp.h bit order, the NaN-outside rule, the half-Z
  # near plane, the 0.5 guard-band coefficient, and FALLBACK-dominates for
  # undividable w, ahead of the FP24 clip-BO oracle and the route action.
  test(
    'r300-r2vb-clip',
    executable(
      'r300_r2vb_clip_test',
      files('tests/r300_r2vb_clip_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        include_directories('.'),
      ],
      dependencies : [dep_m, dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Host corpus for the R2VB per-output re-ingest stream layout
  # (r300_r2vb_reingest_stream_layout): one vertex element per VS output, so
  # two outputs sourced from one input still fetch two elements and the
  # delivery's VAP_VTX_SIZE matches the VAP_OUTPUT_VTX_FMT tuple.
  test(
    'r300-r2vb-reingest-layout',
    executable(
      'r300_r2vb_reingest_layout_test',
      files('tests/r300_r2vb_reingest_layout_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        include_directories('.'),
      ],
      link_with : [libr300, libgallium],
      dependencies : [dep_llvm, idep_mesautil, idep_nir, idep_xmlconfig],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Pure host contract for the R2VB diagnostic transport gate.  Pins exact
  # capture and submit values, raw-submit consent, one-shot flush admission,
  # and active-query isolation before the diagnostic can touch a command stream.
  test(
    'r300-r2vb-capture-gate',
    executable(
      'r300_r2vb_capture_gate_test',
      files('tests/r300_r2vb_capture_gate_test.c', 'r300_chipset.c'),
      c_args : r300_test_c_args,
      include_directories : [inc_src, inc_include, inc_gallium, inc_gallium_aux,
                             inc_amd_r300_common, include_directories('.')],
      dependencies : [idep_mesautilc11],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Host calibration for the C1b VTE restore and the position-only C1c VAP
  # state packet.  The matrix covers closed gates, known-good state, and
  # inherited point-size or texcoord declarations that must be rejected.
  test(
    'r300-r2vb-reingest-state',
    executable(
      'r300_r2vb_reingest_state_test',
      files('tests/r300_r2vb_reingest_state_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
        inc_amd_r300_common,
        include_directories('.'),
      ],
      link_with : [libr300, libgallium],
      dependencies : [idep_mesautil],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Producer-submit state calibration: an available state emits one
  # candidate, a submitted state consumes repeated qualifying draws, and an
  # unknown state fails closed without another emission.
  test(
    'r300-r2vb-submit3-state',
    executable(
      'r300_r2vb_submit3_state_test',
      files('tests/r300_r2vb_submit3_state_test.c'),
      c_args : r300_test_c_args,
      include_directories : [inc_src, inc_include, inc_gallium, inc_gallium_aux,
                             inc_amd_r300_common, include_directories('.')],
      dependencies : [idep_mesautilc11],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  # Host calibration for the blend-factor target remapper.  RGB and alpha
  # SRC_ALPHA_SATURATE factors have different contracts, and intensity
  # targets retain their replicated destination alpha instead of using the
  # no-alpha constants.
  test(
    'r300-blend-factor-target',
    executable(
      'r300_blend_factor_target_test',
      files('tests/r300_blend_factor_target_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux,
        inc_gallium_drivers, inc_amd_r300_common, include_directories('.'),
      ],
      dependencies : [idep_mesautil],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )
  # Gallium adapter contracts over the API-neutral common vocabulary: the
  # pipe_format vertex-format adapter and the R2VB source contract.  Both
  # exercise Gallium-side headers, so they build here rather than under
  # src/amd/r300/common.
  test(
    'r300-vertex-format-pipe',
    executable(
      'r300_vertex_format_pipe_test',
      files('tests/r300_vertex_format_pipe_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux,
        inc_amd_r300_common, include_directories('.'),
      ],
      dependencies : [idep_mesautil],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )

  test(
    'r300-r2vb-source-contract',
    executable(
      'r300_r2vb_source_contract_test',
      files('tests/r300_r2vb_source_contract_test.c'),
      c_args : r300_test_c_args,
      include_directories : [
        inc_src, inc_include, inc_gallium, inc_gallium_aux,
        inc_amd_r300_common, include_directories('.'),
      ],
      dependencies : [idep_mesautil],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['r300'],
  )
endif
