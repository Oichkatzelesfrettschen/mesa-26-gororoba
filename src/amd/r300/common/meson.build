# SPDX-License-Identifier: MIT

# One object owner for the R300 contracts consumed by r300g and the pure
# Vulkan implementation.  Manifest writers stay test executables: their main
# functions publish evidence and are not driver objects.
r300_common_contract_files = files(
  'r300_carrier_policy.c',
  'r300_direct_write.c',
  'r300_first_draw_state.c',
  'r300_fragment_binary.c',
  'r300_numeric_domain.c',
  'r300_pm4_builder.c',
  'r300_pm4_compose.c',
  'r300_r2vb_carrier_delivery.c',
  'r300_r2vb_fetch_pass.c',
  'r300_r2vb_float2_tuple_pass.c',
  'r300_r2vb_producer_pass.c',
  'r300_r2vb_public_route.c',
  'r300_r2vb_reingest_draw.c',
  'r300_r2vb_reingest_pass.c',
  'r300_r2vb_target_state.c',
  'r300_tcl_bypass_triangle.c',
  'r300_zb_depth_control_cell.c',
  'r300_zb_depth_state.c',
)

# These implementation units still reside in common while the P2 ownership
# cutover is pending.  Keeping the exceptions named separately prevents the
# archive from laundering a Vulkan front end or product policy into the
# durable common contract: the consumer census continues to require their
# MOVE_TO_R3V_* dispositions.
r300_common_pending_r3v_move_files = files(
  'r300_compute_spirv.c',
  'r300_delivery_route.c',
  'r300_vertex_spirv.c',
)

r300_common_files = r300_common_contract_files + r300_common_pending_r3v_move_files

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

r300_grid_fold_test = executable(
  'r300_grid_fold_test',
  files('tests/r300_grid_fold_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : r300_format_contract_test_includes,
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-grid-fold',
  r300_grid_fold_test,
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

r300_delivery_route_test = executable(
  'r300_delivery_route_test',
  files('r300_delivery_route.c', 'tests/r300_delivery_route_test.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
test(
  'r300-delivery-route',
  r300_delivery_route_test,
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

r300_zb_depth_control_cell_test = executable(
  'r300_zb_depth_control_cell_test',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c',
        'r300_zb_depth_control_cell.c', 'r300_zb_depth_state.c',
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
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c',
        'r300_zb_depth_control_cell.c', 'r300_zb_depth_control_manifest.c',
        'r300_zb_depth_state.c'),
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
        'r300_r2vb_producer_pass.c', 'r300_tcl_bypass_triangle.c',
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
        'r300_tcl_bypass_triangle.c'),
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
        'r300_tcl_bypass_triangle.c',
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
        'r300_r2vb_reingest_pass.c', 'r300_tcl_bypass_triangle.c',
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

r300_r2vb_public_route_test = executable(
  'r300_r2vb_public_route_test',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_producer_pass.c', 'r300_r2vb_public_route.c',
        'r300_tcl_bypass_triangle.c',
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
        'r300_tcl_bypass_triangle.c'),
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

# Manifest writer for the re-ingest stream; the offline replay drives it.
r300_r2vb_reingest_manifest = executable(
  'r300_r2vb_reingest_manifest',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_producer_pass.c', 'r300_r2vb_reingest_manifest.c',
        'r300_r2vb_reingest_pass.c', 'r300_tcl_bypass_triangle.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# Offline kernel replay of the re-ingest stream, gated on
# R3V_CS_TRACK_REPLAY_TOOL; absent tool exits 77 (meson SKIP).
test(
  'r300-r2vb-reingest-replay',
  files('tests/run_r2vb_reingest_replay.sh')[0],
  args : [r300_r2vb_reingest_manifest],
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
        'r300_tcl_bypass_triangle.c',
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
        'r300_tcl_bypass_triangle.c'),
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
  args : [r300_r2vb_float2_tuple_manifest],
  suite : ['r300'],
)

# Manifest writer for the tuple burst pass; the offline replay drives
# it at each composition depth.
r300_r2vb_float2_tuple_burst_manifest = executable(
  'r300_r2vb_float2_tuple_burst_manifest',
  files('r300_first_draw_state.c', 'r300_fragment_binary.c',
        'r300_pm4_builder.c', 'r300_r2vb_carrier_delivery.c',
        'r300_r2vb_float2_tuple_burst_manifest.c',
        'r300_r2vb_float2_tuple_pass.c', 'r300_r2vb_producer_pass.c',
        'r300_tcl_bypass_triangle.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# Offline kernel replay of the tuple burst, gated on
# R3V_CS_TRACK_REPLAY_TOOL; R3V_KERNEL_REPLAY_TOOL adds the TCL-bypass
# width predicate over every member draw.
test(
  'r300-r2vb-float2-tuple-burst-replay',
  files('tests/run_r2vb_float2_tuple_burst_replay.sh')[0],
  args : [r300_r2vb_float2_tuple_burst_manifest],
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
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c',
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
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c',
        'r300_triangle_manifest.c'),
  c_args : r300_format_contract_test_c_args,
  include_directories : [r300_format_contract_test_includes,
                         include_directories('.')],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

# The writer's own artifacts, checked against each other: the unit test
# proves the helper, this proves the executable published what the helper
# computed.
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
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c',
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

# The 2D solid-fill direct-write control cell: the emitter, its oracle,
# the manifest writer, and the offline kernel-parser replay with the
# parser-decidable known-bad arms.
r300_direct_write_test = executable(
  'r300_direct_write_test',
  files('r300_direct_write.c', 'r300_fragment_binary.c',
        'r300_first_draw_state.c', 'r300_pm4_builder.c',
        'r300_tcl_bypass_triangle.c', 'tests/r300_direct_write_test.c'),
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
  files('r300_direct_write.c', 'r300_direct_write_manifest.c',
        'r300_fragment_binary.c', 'r300_first_draw_state.c',
        'r300_pm4_builder.c', 'r300_tcl_bypass_triangle.c'),
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
        'r300_tcl_bypass_triangle.c',
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
endif
