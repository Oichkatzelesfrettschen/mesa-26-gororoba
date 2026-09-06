# SPDX-License-Identifier: MIT

# One object owner for the R300 contracts consumed by r300g and the pure
# Vulkan implementation.  Manifest writers stay test executables: their main
# functions publish evidence and are not driver objects.
r300_common_contract_files = files(
  'r300_carrier_policy.c',
  'r300_chip_identity.c',
  'r300_chipset.c',
  'r300_compute_identity_carrier.c',
  'r300_compute_identity_carrier_contract.c',
  'r300_compute_verb.c',
  'r300_operation_route.c',
  'r300_direct_write.c',
  'r300_first_draw_state.c',
  'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
  'r300_fragment_binary.c',
  'r300_numeric_domain.c',
  'r300_pm4_builder.c',
  'r300_rb2d_fill.c',
  'r300_rb2d_linear_span.c',
  'r300_rb2d_pitch_evidence.c',
  'r300_rb2d_contract_evidence.c',
  'r300_rb2d_legalize.c',
  'r300_pm4_compose.c',
  'r300_r2vb_carrier_delivery.c',
  'r300_r2vb_fetch_pass.c',
  'r300_r2vb_fetched_producer.c',
  'r300_r2vb_float2_tuple_pass.c',
  'r300_r2vb_producer_pass.c',
  'r300_r2vb_public_route.c',
  'r300_r2vb_reingest_draw.c',
  'r300_r2vb_reingest_pass.c',
  'r300_r2vb_target_state.c',
  'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
  'r300_zb_depth_control_cell.c',
  'r300_zb_depth_layout.c',
  'r300_zb_depth_state.c',
  'r300_zb_depth_surface.c',
  'r300_zb_hyperz_admission.c',
  'r300_zmask_clear_plan.c',
  'r300_zmask_layout.c',
)

r300_common_files = r300_common_contract_files

libr300_common = static_library(
  'r300_common',
  r300_common_files,
  include_directories : [inc_src, inc_include, inc_amd_common,
                         include_directories('.')],
  dependencies : [dep_m, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

if with_tests
# Host-only tests over the API-neutral R300 hardware vocabulary.  Everything
# here builds against inc_src, inc_include, inc_amd_common, and this
# directory alone, which keeps the common layer's independence from the
# Gallium tree enforceable at the build level; the Gallium adapter tests
# live under src/gallium/drivers/r300.  These tests perform no winsys setup
# and submit no command stream.
r300_format_contract_test_c_args = ['-Werror=unused-function']
r300_format_contract_test_includes = [
  inc_src,
  inc_include,
  inc_amd_common,
  include_directories('.'),
]

# The consumer census records API-neutral ownership.  The liveness census
# separately proves build registration, declaration reach, non-owner symbol
# references, and evidence-only uses for the candidate carrier inventory.  Its
# route-selection and hardware-execution cells stay fail-closed until exact
# adapters establish those stronger facts.
r300_common_liveness_census = files('tests/r300_common_liveness_census.py')
test(
  'r300-common-liveness-census-selftest',
  prog_python,
  args : [r300_common_liveness_census, '--selftest'],
  suite : ['r300'],
)
test(
  'r300-common-liveness-census',
  prog_python,
  args : [r300_common_liveness_census, meson.project_source_root()],
  suite : ['r300'],
  # The census walks the source tree; the Vostro 1000's K8 needs about
  # three times the workstation's nine seconds, past meson's 30 s default.
  timeout : 180,
)

r300_grid_fold_test = executable(
  'r300_grid_fold_test',
  files('tests/r300_grid_fold_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  gnu_symbol_visibility : 'hidden',
)
# The compute verb ledger: rows, checker calibration, precommitment.
r300_compute_verb_test = executable(
  'r300_compute_verb_test',
  files('r300_compute_verb.c', 'r300_operation_route.c', 'r300_numeric_domain.c',
        'tests/r300_compute_verb_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-compute-verb-ledger',
  r300_compute_verb_test,
  suite : ['r300'],
)

# Cross-registry typed joins: operation catalog, verb authority, executing
# identity route certificate, and candidate carrier-policy inventory.
r300_operation_ledger_test = executable(
  'r300_operation_ledger_test',
  files('tests/r300_operation_ledger_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [dep_m, idep_mesautil],
  link_with : libr300_common,
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-operation-ledger',
  r300_operation_ledger_test,
  suite : ['r300'],
)

# Test-only structural export for external evidence ledgers.  The executable is
# not part of libr300_common and emits no evidence or route-state authority.
r300_operation_registry_export = executable(
  'r300_operation_registry_export',
  files('tests/r300_operation_registry_export.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [dep_m, idep_mesautil],
  link_with : libr300_common,
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-operation-registry-export',
  r300_operation_registry_export,
  suite : ['r300'],
)

# Test-only route sheet: renders the compute-verb ledger with route status
# ahead of evidence, so a reader meets what executes before what was retained.
r300_compute_verb_route_sheet = executable(
  'r300_compute_verb_route_sheet',
  files('tests/r300_compute_verb_route_sheet.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [dep_m, idep_mesautil],
  link_with : libr300_common,
  gnu_symbol_visibility : 'hidden',
)
r300_operation_route_test = executable(
  'r300_operation_route_test',
  files('tests/r300_operation_route_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [dep_m, idep_mesautil],
  link_with : libr300_common,
  gnu_symbol_visibility : 'hidden',
)
r300_rb2d_fill_test = executable(
  'r300_rb2d_fill_test',
  files('tests/r300_rb2d_fill_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [dep_m, idep_mesautil],
  link_with : libr300_common,
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-rb2d-fill-plan',
  r300_rb2d_fill_test,
  suite : ['r300'],
)

r300_rb2d_linear_span_test = executable(
  'r300_rb2d_linear_span_test',
  files('tests/r300_rb2d_linear_span_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [dep_m, idep_mesautil],
  link_with : libr300_common,
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-rb2d-linear-span',
  r300_rb2d_linear_span_test,
  suite : ['r300'],
)

r300_rb2d_contract_evidence_test = executable(
  'r300_rb2d_contract_evidence_test',
  files('tests/r300_rb2d_contract_evidence_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [dep_m, idep_mesautil],
  link_with : libr300_common,
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-rb2d-contract-evidence',
  r300_rb2d_contract_evidence_test,
  suite : ['r300'],
)

r300_rb2d_legalize_test = executable(
  'r300_rb2d_legalize_test',
  files('tests/r300_rb2d_legalize_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [dep_m, idep_mesautil],
  link_with : libr300_common,
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-rb2d-legalize',
  r300_rb2d_legalize_test,
  suite : ['r300'],
)

# Emits legalized RB2D streams and their relocation bundles for the
# kernel-replay differential; the differential script assembles the raw
# rejecting streams itself.
r300_rb2d_legalize_tool = executable(
  'r300_rb2d_legalize_tool',
  files('tests/r300_rb2d_legalize_tool.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [dep_m, idep_mesautil],
  link_with : libr300_common,
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-rb2d-legalization-differential',
  prog_python,
  args : [files('tests/r300_rb2d_legalization_differential.py'),
          r300_rb2d_legalize_tool],
  depends : [r300_rb2d_legalize_tool],
  suite : ['r300'],
)

test(
  'r300-operation-route',
  r300_operation_route_test,
  suite : ['r300'],
)

test(
  'r300-compute-verb-route-sheet',
  r300_compute_verb_route_sheet,
  suite : ['r300'],
)
test(
  'r300-grid-fold',
  r300_grid_fold_test,
  suite : ['r300'],
)

r300_chip_identity_test = executable(
  'r300_chip_identity_test',
  files('tests/r300_chip_identity_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  link_with : libr300_common,
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-chip-identity',
  r300_chip_identity_test,
  suite : ['r300'],
)

# The source audit proves the namespace and include boundary.  This archive
# audit proves the separately compiled common object set cannot hide an
# undefined Gallium, NIR, VTN, or TGSI dependency behind an inline or generated
# source path.
prog_nm_r300_common = find_program('nm', required : false)
if prog_nm_r300_common.found()
  test(
    'r300-common-boundary-audit-common-objects',
    prog_python,
    args : [
      files('tests/r300_common_boundary_audit.py'),
      '--objects',
      prog_nm_r300_common.full_path(),
      libr300_common.full_path(),
    ],
    depends : libr300_common,
    suite : ['r300'],
  )
endif

r300_carrier_policy_test = executable(
  'r300_carrier_policy_test',
  files('r300_carrier_policy.c', 'tests/r300_carrier_policy_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-carrier-policy',
  r300_carrier_policy_test,
  suite : ['r300'],
)

r300_r2vb_fetch_pass_test = executable(
  'r300_r2vb_fetch_pass_test',
  files('r300_pm4_builder.c', 'r300_r2vb_fetch_pass.c',
        'tests/r300_r2vb_fetch_pass_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-fetch-pass',
  r300_r2vb_fetch_pass_test,
  suite : ['r300'],
)

r300_r2vb_reingest_draw_test = executable(
  'r300_r2vb_reingest_draw_test',
  files('r300_pm4_builder.c', 'r300_r2vb_reingest_draw.c',
        'tests/r300_r2vb_reingest_draw_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-reingest-draw',
  r300_r2vb_reingest_draw_test,
  suite : ['r300'],
)

r300_pm4_compose_test = executable(
  'r300_pm4_compose_test',
  files('r300_pm4_builder.c', 'r300_pm4_compose.c',
        'tests/r300_pm4_compose_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-pm4-compose',
  r300_pm4_compose_test,
  suite : ['r300'],
)

r300_r2vb_target_state_test = executable(
  'r300_r2vb_target_state_test',
  files('r300_pm4_builder.c', 'r300_r2vb_fetch_pass.c',
        'r300_r2vb_target_state.c',
        'tests/r300_r2vb_target_state_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-target-state',
  r300_r2vb_target_state_test,
  suite : ['r300'],
)

r300_zb_depth_state_test = executable(
  'r300_zb_depth_state_test',
  files('r300_pm4_builder.c', 'r300_zb_depth_state.c',
        'tests/r300_zb_depth_state_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-zb-depth-state',
  r300_zb_depth_state_test,
  suite : ['r300'],
)

r300_zb_hyperz_admission_test = executable(
  'r300_zb_hyperz_admission_test',
  files('tests/r300_zb_hyperz_admission_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  dependencies : [dep_m, idep_mesautil],
  link_with : libr300_common,
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-zb-hyperz-admission',
  r300_zb_hyperz_admission_test,
  suite : ['r300'],
)

r300_zb_depth_surface_test = executable(
  'r300_zb_depth_surface_test',
  files('r300_zb_depth_surface.c',
        'tests/r300_zb_depth_surface_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-zb-depth-surface',
  r300_zb_depth_surface_test,
  suite : ['r300'],
)

r300_zb_depth_layout_test = executable(
  'r300_zb_depth_layout_test',
  files('r300_zb_depth_layout.c', 'r300_zb_depth_surface.c',
        'tests/r300_zb_depth_layout_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-zb-depth-layout',
  r300_zb_depth_layout_test,
  suite : ['r300'],
)

r300_zb_depth_control_cell_test = executable(
  'r300_zb_depth_control_cell_test',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'r300_zb_depth_control_cell.c', 'r300_zb_depth_state.c',
        'r300_zb_depth_surface.c',
        'tests/r300_zb_depth_control_cell_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-zb-depth-control-cell',
  r300_zb_depth_control_cell_test,
  suite : ['r300'],
)

# Manifest writer: the no-submit evidence generator for the depth control
# cell; the offline replay drives it.
r300_zb_depth_control_manifest = executable(
  'r300_zb_depth_control_manifest',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'r300_zb_depth_control_cell.c', 'r300_zb_depth_control_manifest.c',
        'r300_zb_depth_state.c', 'r300_zb_depth_surface.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# Offline kernel replay of the depth control cell, gated on
# R3V_CS_TRACK_REPLAY_TOOL.  ZB_DEPTHOFFSET's relocation and the
# depth-buffer size check reach the parser here rather than on a boot.
test(
  'r300-zb-depth-control-replay',
  files('tests/run_zb_depth_control_replay.sh')[0],
  args : [r300_zb_depth_control_manifest],
  suite : ['r300'],
)

r300_r2vb_producer_pass_test = executable(
  'r300_r2vb_producer_pass_test',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_producer_pass.c', 'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'tests/r300_r2vb_producer_pass_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-producer-pass',
  r300_r2vb_producer_pass_test,
  suite : ['r300'],
)

# Manifest writer: the no-submit evidence generator for the producer
# pass; the offline replay drives it.
r300_r2vb_producer_manifest = executable(
  'r300_r2vb_producer_manifest',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_producer_manifest.c', 'r300_r2vb_producer_pass.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# Offline kernel replay of the producer pass, gated on
# R3V_CS_TRACK_REPLAY_TOOL; R3V_KERNEL_REPLAY_TOOL adds the TCL-bypass
# width predicate's declared-scope decline.  The script exits 77 (meson
# SKIP) when the tool is absent, so the default graph never depends on a
# sibling checkout.
test(
  'r300-r2vb-producer-replay',
  files('tests/run_r2vb_producer_replay.sh')[0],
  args : [r300_r2vb_producer_manifest],
  suite : ['r300'],
)

r300_r2vb_reingest_pass_test = executable(
  'r300_r2vb_reingest_pass_test',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_producer_pass.c', 'r300_r2vb_reingest_pass.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'tests/r300_r2vb_reingest_pass_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-reingest-pass',
  r300_r2vb_reingest_pass_test,
  suite : ['r300'],
)

r300_r2vb_reingest_compose_parity_test = executable(
  'r300_r2vb_reingest_compose_parity_test',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_pm4_compose.c',
        'r300_r2vb_carrier_delivery.c', 'r300_r2vb_producer_pass.c',
        'r300_r2vb_reingest_pass.c', 'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'tests/r300_r2vb_reingest_compose_parity_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-reingest-compose-parity',
  r300_r2vb_reingest_compose_parity_test,
  suite : ['r300'],
)

r300_r2vb_fetched_producer_test = executable(
  'r300_r2vb_fetched_producer_test',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_pm4_compose.c',
        'r300_r2vb_carrier_delivery.c', 'r300_r2vb_fetch_pass.c',
        'r300_r2vb_fetched_producer.c', 'r300_r2vb_producer_pass.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'tests/r300_r2vb_fetched_producer_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-fetched-producer',
  r300_r2vb_fetched_producer_test,
  suite : ['r300'],
)

r300_r2vb_public_route_test = executable(
  'r300_r2vb_public_route_test',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_producer_pass.c', 'r300_r2vb_public_route.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'tests/r300_r2vb_public_route_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-public-route',
  r300_r2vb_public_route_test,
  suite : ['r300'],
)

# Manifest writer for the composed public GPU-producer route; the
# offline replay drives it, and the attended run's authorization
# declares the ib_blake3 it publishes.
r300_r2vb_public_route_manifest = executable(
  'r300_r2vb_public_route_manifest',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_producer_pass.c', 'r300_r2vb_public_route.c',
        'r300_r2vb_public_route_manifest.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# Offline kernel replay of the composed route, gated on
# R3V_CS_TRACK_REPLAY_TOOL; absent tool exits 77 (meson SKIP).
test(
  'r300-r2vb-public-route-replay',
  files('tests/run_r2vb_public_route_replay.sh')[0],
  args : [r300_r2vb_public_route_manifest],
  suite : ['r300'],
)

# Manifest writer for the composed fetched route; the offline replay
# drives it, once per admitted source width.
r300_r2vb_fetched_route_manifest = executable(
  'r300_r2vb_fetched_route_manifest',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_pm4_compose.c',
        'r300_r2vb_carrier_delivery.c', 'r300_r2vb_fetch_pass.c',
        'r300_r2vb_fetched_producer.c', 'r300_r2vb_fetched_route_manifest.c',
        'r300_r2vb_producer_pass.c', 'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# The compute identity carrier pass: structure, pinned reference digest,
# refusals, and the host model against the CPU route's bit copy.
r300_compute_identity_carrier_sources = files(
  'r300_compute_identity_carrier.c',
  'r300_compute_identity_carrier_contract.c', 'r300_first_draw_state.c',
  'r300_fragment_binary.c', 'r300_pm4_builder.c', 'r300_pm4_compose.c',
  'r300_r2vb_carrier_delivery.c', 'r300_r2vb_fetch_pass.c',
  'r300_r2vb_fetched_producer.c', 'r300_r2vb_producer_pass.c',
  'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c')
r300_compute_identity_carrier_test = executable(
  'r300_compute_identity_carrier_test',
  [r300_compute_identity_carrier_sources,
   files('tests/r300_compute_identity_carrier_test.c')],
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-compute-identity-carrier',
  r300_compute_identity_carrier_test,
  suite : ['r300'],
)
r300_compute_identity_carrier_manifest = executable(
  'r300_compute_identity_carrier_manifest',
  [r300_compute_identity_carrier_sources,
   files('r300_compute_identity_carrier_manifest.c')],
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
# Offline kernel replay of the reference compute identity carrier pass:
# one draw over three relocation entries, gated on R3V_CS_TRACK_REPLAY_TOOL.
test(
  'r300-compute-identity-carrier-cs-track-replay',
  files('tests/run_compute_identity_carrier_replay.sh')[0],
  args : [r300_compute_identity_carrier_manifest],
  suite : ['r300'],
)

# Offline kernel replay of the composed fetched route per width, gated
# on R3V_CS_TRACK_REPLAY_TOOL; absent tool exits 77 (meson SKIP).
test(
  'r300-r2vb-fetched-route-replay-f32_4',
  files('tests/run_r2vb_fetched_route_replay.sh')[0],
  args : [r300_r2vb_fetched_route_manifest, 'f32_4'],
  suite : ['r300'],
)
test(
  'r300-r2vb-fetched-route-replay-f32_3',
  files('tests/run_r2vb_fetched_route_replay.sh')[0],
  args : [r300_r2vb_fetched_route_manifest, 'f32_3'],
  suite : ['r300'],
)
test(
  'r300-r2vb-fetched-route-replay-f32_2',
  files('tests/run_r2vb_fetched_route_replay.sh')[0],
  args : [r300_r2vb_fetched_route_manifest, 'f32_2'],
  suite : ['r300'],
)

# Manifest writer for the re-ingest stream; the offline replay drives it.
r300_r2vb_reingest_manifest = executable(
  'r300_r2vb_reingest_manifest',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_producer_pass.c', 'r300_r2vb_reingest_manifest.c',
        'r300_r2vb_reingest_pass.c', 'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# The re-ingest writer's artifacts are checked against each other and against
# the fixed stream contract.  Calibration mutants keep malformed retained
# metadata from becoming a green replay-only result.
test(
  'r300-r2vb-reingest-manifest-integration',
  prog_python,
  args : [files('tests/r300_r2vb_reingest_manifest_check.py'),
          r300_r2vb_reingest_manifest.full_path()],
  depends : [r300_r2vb_reingest_manifest],
  suite : ['r300'],
)

# Offline kernel replay of the re-ingest stream, gated on
# R3V_CS_TRACK_REPLAY_TOOL; absent tool exits 77 (meson SKIP).
test(
  'r300-r2vb-reingest-replay',
  files('tests/run_r2vb_reingest_replay.sh')[0],
  args : [r300_r2vb_reingest_manifest,
          files('tests/r300_r2vb_reingest_manifest_check.py')[0]],
  depends : [r300_r2vb_reingest_manifest],
  suite : ['r300'],
)

# The FP24 boundary-sweep stream replays through the same script: the
# layout, relocation, and color-buffer bound are unchanged, so the
# known-good and known-bad arms and their verdicts carry over.
test(
  'r300-r2vb-producer-fp24-sweep-replay',
  files('tests/run_r2vb_producer_replay.sh')[0],
  args : [r300_r2vb_producer_manifest, 'fp24-sweep'],
  suite : ['r300'],
)

test(
  'r300-r2vb-producer-fp24-bisect-replay',
  files('tests/run_r2vb_producer_replay.sh')[0],
  args : [r300_r2vb_producer_manifest, 'fp24-bisect'],
  suite : ['r300'],
)

r300_r2vb_float2_tuple_pass_test = executable(
  'r300_r2vb_float2_tuple_pass_test',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_float2_tuple_pass.c', 'r300_r2vb_producer_pass.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'tests/r300_r2vb_float2_tuple_pass_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-r2vb-float2-tuple-pass',
  r300_r2vb_float2_tuple_pass_test,
  suite : ['r300'],
)

# Manifest writer for the FLOAT_4 + FLOAT_2 tuple pass; the offline
# replay drives it.
r300_r2vb_float2_tuple_manifest = executable(
  'r300_r2vb_float2_tuple_manifest',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_float2_tuple_manifest.c',
        'r300_r2vb_float2_tuple_pass.c', 'r300_r2vb_producer_pass.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# Offline kernel replay of the tuple pass, gated on
# R3V_CS_TRACK_REPLAY_TOOL; R3V_KERNEL_REPLAY_TOOL adds the TCL-bypass
# width predicate, which must PASS the fetched FLOAT_2 + XY01 +
# vtx_size 6 tuple -- the userspace/kernel validator agreement point.
test(
  'r300-r2vb-float2-tuple-replay',
  files('tests/run_r2vb_float2_tuple_replay.sh')[0],
  args : [r300_r2vb_float2_tuple_manifest,
          files('tests/r300_r2vb_float2_tuple_manifest_check.py')[0]],
  depends : [r300_r2vb_float2_tuple_manifest],
  suite : ['r300'],
)

# The tuple writer's artifacts are checked against each other and against the
# emitted VAP tuple before the replay accepts the stream.  Calibration mutants
# keep a malformed retained vertex stream, metadata set, or command state from
# becoming a green replay-only result.
test(
  'r300-r2vb-float2-tuple-manifest-integration',
  prog_python,
  args : [files('tests/r300_r2vb_float2_tuple_manifest_check.py'),
          r300_r2vb_float2_tuple_manifest.full_path()],
  depends : [r300_r2vb_float2_tuple_manifest],
  suite : ['r300'],
)

# Manifest writer for the tuple-width burst pass; the offline replay drives
# both model widths at each composition depth.
r300_r2vb_tuple_width_burst_manifest = executable(
  'r300_r2vb_tuple_width_burst_manifest',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_tuple_width_burst_manifest.c',
        'r300_r2vb_float2_tuple_pass.c', 'r300_r2vb_producer_pass.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# Offline kernel replay of the tuple-width burst, gated on
# R3V_CS_TRACK_REPLAY_TOOL; R3V_KERNEL_REPLAY_TOOL adds the TCL-bypass
# width predicate over every member draw.
test(
  'r300-r2vb-tuple-width-burst-replay',
  files('tests/run_r2vb_tuple_width_burst_replay.sh')[0],
  args : [r300_r2vb_tuple_width_burst_manifest],
  suite : ['r300'],
)

# The producer writer's artifacts are checked against each other and against
# the emitted VAP tuple.  Calibration mutants keep a malformed retained
# manifest from becoming a green replay-only result.
test(
  'r300-r2vb-producer-manifest-integration',
  prog_python,
  args : [files('tests/r300_r2vb_producer_manifest_check.py'),
          r300_r2vb_producer_manifest.full_path()],
  depends : [r300_r2vb_producer_manifest],
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
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
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
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'r300_triangle_manifest.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

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
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'tests/r300_staging_manifest_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-staging-manifest',
  prog_python,
  args : [files('tests/r300_staging_manifest_runner.py'),
          r300_staging_manifest_test,
          files('tests/r300_triangle_manifest_check.py'),
          r300_triangle_manifest.full_path()],
  depends : [r300_staging_manifest_test, r300_triangle_manifest],
  suite : ['r300'],
)
test(
  'r300-staging-manifest-runner-selftest',
  prog_python,
  args : [files('tests/r300_staging_manifest_runner.py'), '--selftest'],
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
# The varying cell through the same parser and tracker: the two-element
# PSC list, VAP_VTX_SIZE 8, the TEX0 output declaration, and the 32-byte
# vertex-array record all inside the vertex-output check's proven scope.
test(
  'r300-varying-cell-cs-track-replay',
  files('tests/run_cs_track_replay.sh')[0],
  args : [r300_triangle_manifest, '--varying'],
  suite : ['r300'],
)
# The two-triangle family member -- the host expansion of a two-instance
# draw -- through the same parser and tracker: six records inside the
# one-page vertex array bound, VAP_VF_MAX_VTX_INDX 5, NUM_VERTICES 6.
test(
  'r300-instanced-cell-cs-track-replay',
  files('tests/run_cs_track_replay.sh')[0],
  args : [r300_triangle_manifest, '--triangles', '2'],
  suite : ['r300'],
)
# The two-pass stream through the same parser and tracker: two
# contract-prefixed cells, the second's relocations naming indices 2 and
# 3 of a four-entry list, so every reloc the second cell consumes
# resolves inside the chunk.
test(
  'r300-multi-pass-cs-track-replay',
  files('tests/run_cs_track_replay.sh')[0],
  args : [r300_triangle_manifest, '--multi-pass'],
  suite : ['r300'],
)

# The direct GA Flat two-pass cell (--flat-color0), pinned against its
# host-replicated control (--flat-replicate).  The kernel TCL-bypass
# vertex check declines every draw of the direct stream -- its
# VAP_OUTPUT_VTX_FMT_0 COLOR_0_PRESENT record widens the true PSC layout
# past what the check's width derivation covers -- while ordinary CS
# tracking accepts both streams; R3V_CS_TRACK_CONTROLS isolates the one
# CS-track predicate (VAP_VTX_SIZE below the output width) that the
# direct stream alone fails to hold.
test(
  'r300-flat-color0-kernel-replay',
  files('tests/run_flat_color0_kernel_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)
test(
  'r300-flat-color0-cs-track-replay',
  files('tests/run_flat_color0_cs_track_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)
# The rasterizer probe two-pass cells through the same two replays:
# both the control and the candidate stream PASS the width check and
# ACCEPT under CS tracking, holding every negative control.
test(
  'r300-rs-tex-adj-probe-kernel-replay',
  files('tests/run_rs_tex_adj_probe_kernel_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)
test(
  'r300-rs-tex-adj-probe-cs-track-replay',
  files('tests/run_rs_tex_adj_probe_cs_track_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)
# The NoPerspective reciprocal-carrier two-pass cell through both
# replays: the TC1 carrier draw PASSes the width check at VAP_VTX_SIZE 12
# and REJECTs at 8, and ACCEPTs under CS tracking with every control.
test(
  'r300-noperspective-carrier-kernel-replay',
  files('tests/run_noperspective_carrier_kernel_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)
test(
  'r300-noperspective-carrier-cs-track-replay',
  files('tests/run_noperspective_carrier_cs_track_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)
# The NoPerspective q-lane two-pass cell through both replays: the
# eight-dword record PASSes the width check at VAP_VTX_SIZE 8 on both
# draws, REJECTs at 4, and ACCEPTs under CS tracking with every control.
test(
  'r300-noperspective-q-lane-kernel-replay',
  files('tests/run_noperspective_q_lane_kernel_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)
test(
  'r300-noperspective-q-lane-cs-track-replay',
  files('tests/run_noperspective_q_lane_cs_track_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)
# The mixed Smooth/NoPerspective carrier two-pass cell through both
# replays: the control PASSes at VAP_VTX_SIZE 8 and the sixteen-dword cell
# at 16, VTX_SIZE 12 REJECTs the mixed draw, and CS tracking ACCEPTs with
# every control.
test(
  'r300-noperspective-mixed-carrier-kernel-replay',
  files('tests/run_noperspective_mixed_carrier_kernel_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)
test(
  'r300-noperspective-mixed-carrier-cs-track-replay',
  files('tests/run_noperspective_mixed_carrier_cs_track_replay.sh')[0],
  args : [r300_triangle_manifest],
  suite : ['r300'],
)

# The 2D solid-fill direct-write control cell: the emitter, its oracle,
# the manifest writer, and the offline kernel-parser replay with the
# parser-decidable known-bad arms.
r300_direct_write_test = executable(
  'r300_direct_write_test',
  files('r300_direct_write.c', 'r300_rb2d_fill.c', 'r300_fragment_binary.c',
        'r300_first_draw_state.c', 'r300_pm4_builder.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c', 'tests/r300_direct_write_test.c'),
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
  files('r300_direct_write.c', 'r300_rb2d_fill.c', 'r300_direct_write_manifest.c',
        'r300_fragment_binary.c', 'r300_first_draw_state.c',
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c'),
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

# The writer's own artifacts, checked against each other: the unit test
# proves the emitter and oracle, this proves the executable published
# what the emitter computed.
test(
  'r300-direct-write-manifest-integration',
  prog_python,
  args : [files('tests/r300_direct_write_manifest_check.py'),
          r300_direct_write_manifest.full_path()],
  depends : [r300_direct_write_manifest],
  suite : ['r300'],
)

r300_first_draw_state_test = executable(
  'r300_first_draw_state_test',
  files('r300_fragment_binary.c', 'r300_pm4_builder.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c', 'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
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

# Direct GA Flat plan calibration: canonical words, every mutation's
# refusal and localized byte deviation, the per-draw stream check, and
# the expected-target generator.
r300_flat_color0_plan_test = executable(
  'r300_flat_color0_plan_test',
  files('r300_fragment_binary.c', 'r300_pm4_builder.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c',
        'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c', 'r300_first_draw_state.c',
        'tests/r300_flat_color0_plan_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-flat-color0-plan',
  r300_flat_color0_plan_test,
  suite : ['r300'],
)

# Rasterizer probe calibration: control bytes equal the varying cell's,
# each candidate differs in its one word, every mutation refuses and
# localizes, the per-draw stream check, the model oracle, and the
# census classification against synthetic images.
r300_rs_tex_adj_probe_test = executable(
  'r300_rs_tex_adj_probe_test',
  files('r300_fragment_binary.c', 'r300_pm4_builder.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c',
        'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c', 'r300_first_draw_state.c',
        'tests/r300_rs_tex_adj_probe_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-rs-tex-adj-probe',
  r300_rs_tex_adj_probe_test,
  suite : ['r300'],
)
# The NoPerspective reciprocal carrier plan: register words, packing
# identity and envelope, the recovery model, and the per-draw stream
# check over the emitted carrier cell with each word mutated.
r300_noperspective_reciprocal_plan_test = executable(
  'r300_noperspective_reciprocal_plan_test',
  files('r300_fragment_binary.c', 'r300_pm4_builder.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c',
        'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c', 'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c',
        'r300_first_draw_state.c',
        'tests/r300_noperspective_reciprocal_plan_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-noperspective-reciprocal-plan',
  r300_noperspective_reciprocal_plan_test,
  suite : ['r300'],
)
# The NoPerspective q-lane plan: the varying cell's register words, the
# packing identity and zero-fill per width, the envelope refusals, the
# stream and expanded validators, the recovery model, and the stream
# check over the emitted q-lane cell with each word mutated in turn.
r300_noperspective_q_lane_plan_test = executable(
  'r300_noperspective_q_lane_plan_test',
  files('r300_fragment_binary.c', 'r300_pm4_builder.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c',
        'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c',
        'r300_noperspective_q_lane_plan.c', 'r300_noperspective_mixed_carrier_plan.c', 'r300_first_draw_state.c',
        'tests/r300_noperspective_q_lane_plan_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-noperspective-q-lane-plan',
  r300_noperspective_q_lane_plan_test,
  suite : ['r300'],
)
# The mixed Smooth/NoPerspective carrier plan: the three-vector register
# words, the selective premultiplication, the plan refusals (RS budget,
# carrier alias, premultiplied set, US budget), the validators, the
# recovery model, and the stream check over the emitted cell with each
# word mutated in turn.
r300_noperspective_mixed_carrier_plan_test = executable(
  'r300_noperspective_mixed_carrier_plan_test',
  files('r300_fragment_binary.c', 'r300_pm4_builder.c',
        'r300_tcl_bypass_triangle.c', 'r300_flat_color0_plan.c',
        'r300_rs_tex_adj_probe.c', 'r300_noperspective_reciprocal_plan.c',
        'r300_noperspective_q_lane_plan.c',
        'r300_noperspective_mixed_carrier_plan.c', 'r300_first_draw_state.c',
        'tests/r300_noperspective_mixed_carrier_plan_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-noperspective-mixed-carrier-plan',
  r300_noperspective_mixed_carrier_plan_test,
  suite : ['r300'],
)
# The ZMASK layout: the stride and dword derivation cross-checked point by
# point against a transcription of r300_setup_hyperz_properties, plus the
# per-family ZMASK RAM table and the refusals.
r300_zmask_layout_test = executable(
  'r300_zmask_layout_test',
  files('r300_zmask_layout.c', 'tests/r300_zmask_layout_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-zmask-layout',
  r300_zmask_layout_test,
  suite : ['r300'],
)
# The ZMASK clear ladder: the words each of the four stages appends, the
# clear payload's dword count, and the HyperZ admission verdict of each
# stage's stream with and without ownership.
r300_zmask_clear_plan_test = executable(
  'r300_zmask_clear_plan_test',
  files('r300_pm4_builder.c', 'r300_zb_hyperz_admission.c',
        'r300_zmask_layout.c', 'r300_zmask_clear_plan.c',
        'tests/r300_zmask_clear_plan_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-zmask-clear-plan',
  r300_zmask_clear_plan_test,
  suite : ['r300'],
)
endif
