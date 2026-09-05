# R300 common contracts

`src/amd/r300/common` contains API-neutral R300 hardware vocabulary and plans.
The durable boundary puts Gallium adapters under `src/gallium/drivers/r300`,
NIR adapters under `src/amd/r300/compiler`, Vulkan objects and front ends
under `src/amd/r300/vulkan`, and neutral job executors under
`src/amd/r300/cpu`.  The direct SPIR-V admitters and the delivery route
policy live with the Vulkan product as `src/amd/r300/vulkan/r3v_*_spirv.*`
and `r3v_delivery_route.*`; only the neutral job IR they produce and the
hardware plans they select stay here.

A common component has one of two durable grounds:

- two or more landed production consumers use the same API-neutral contract;
- a standalone test, manifest writer, or kernel-parser replay demonstrates an
  R300 silicon, packet, register, numeric, or memory contract independently of
  an API runtime.

A planned consumer does not count.  A parity test does not turn an API front
end into common code.  The consumer-census checker covers every root `.c` and
`.h` file exactly once, verifies the named test registrations, and enforces the
decision rules below.

## Decision vocabulary

- `KEEP_SHARED`: at least two landed production consumer domains use the
  component.
- `KEEP_SILICON_CONTRACT`: the component has a registered standalone proof of
  an API-neutral R300 contract.  A production consumer is optional.

A component that interprets an API input or selects product behavior belongs
under `src/amd/r300/vulkan` with the front end or policy it serves; the census
refuses any other decision word.

Consumer tags name build domains: `r300g`, `compiler`, `native`, and `cpu`.
`none` means the component is an evidence producer rather than a production
object.  Test names are exact Meson registrations.

A consumer is measured from a domain's registered production source roots and
the C include graph.  Each implementation unit compiled once into
`libr300_common` inherits the domains that reach its public same-stem header;
linking the archive alone labels no source as consumed.  The checker refuses a
production domain that compiles a registered common implementation directly,
and include traversal stops at another domain's source boundary.  Manifest
writers are deliberately excluded from the archive because they are evidence
executables with `main()` rather than driver objects.

## Consumer census

| Files | API-neutral contract | Production consumers | Standalone proof | Owner decision |
|---|---|---|---|---|
| `r300_chip_identity.c`<br>`r300_chip_identity.h` | PCI-keyed chip family and die-class table generated from `include/pci_ids/r300_pci_ids.h`; an identity outside the table refuses. | `native`, `r300g` | `r300-chip-identity` | `KEEP_SHARED` |
| `r300_chipset.c`<br>`r300_chipset.h` | Capability derivation per family over the identity table. | `r300g` | `r300-chip-identity` | `KEEP_SILICON_CONTRACT` |
| `r300_capabilities.h` | Chip capability record shared by screen and compiler decisions. | `r300g`, `compiler` | `r300-nir-vs-harness` | `KEEP_SHARED` |
| `r300_reg.h`<br>`r300_shader_semantics.h` | Register encodings and shader semantic vocabulary. | `r300g`, `compiler`, `native` | `r300-pm4-builder`, `r300-nir-to-rc` | `KEEP_SHARED` |
| `r300_numeric_domain.c`<br>`r300_numeric_domain.h`<br>`r300_us_source_read.h` | FP24 and source-read numeric traits without a family-wide evidence tier, plus the typed operation ID/name/domain registry. The catalog status and narratives remain an interim compatibility inventory pending complete external migration; `r300-operation-registry-export` emits only a structural join and is not an evidence validator. | `r300g`, `compiler`, `native` | `r300-fp16-limb-oracle`, `r300-classic-pair-value`, `r300-operation-registry-export` | `KEEP_SHARED` |
| `r300_grid_fold.h` | FP24-exact invocation-index guards and bounded 1D/2D/3D folds onto the R300 sampleable raster; the compute verb ledger binds each row's index class to it. | `native` | `r300-grid-fold` | `KEEP_SILICON_CONTRACT` |
| `r300_vertex_format.h`<br>`r300_vertex_stream.h` | API-neutral vertex formats and byte-addressed stream records. | `r300g`, `native`, `cpu` | `r300-vertex-format-pipe`, `r300-cpu-vertex` | `KEEP_SHARED` |
| `r300_compute_job.h`<br>`r300_vertex_job.h` | Neutral compute and vertex job IR. | `compiler`, `native`, `cpu` | `r300-cpu-compute-job`, `r300-cpu-vertex-job`, `r300-vertex-front-end-parity` | `KEEP_SHARED` |
| `r300_compute_identity_carrier.c`<br>`r300_compute_identity_carrier_contract.c`<br>`r300_compute_identity_carrier.h` | The identity verb's raster lowering and typed route certificate: the fetched producer pass over a storage-buffer input as F32_4 records into a storage-buffer output as one C4_32_FP slot row, with the FP24 host model of the delivery. | `native` | `r300-compute-identity-carrier`, `r300-compute-identity-carrier-cs-track-replay`, `r300-operation-ledger` | `KEEP_SILICON_CONTRACT` |
| `r300_compute_verb.c`<br>`r300_compute_verb.h` | The finite compute verb ledger: the fifteen kernel shapes the compute grammar admits, each bound to one catalog operation, the refusal classes, the failure clauses every compute route obeys, and the two queue predicates that project over the route ledger. | `native` | `r300-compute-verb-ledger`, `r300-operation-ledger` | `KEEP_SILICON_CONTRACT` |
| `r300_operation_route.c`<br>`r300_operation_route.h` | The operation route ledger: one row per route rather than per operation, carrying executor, maturity, execution unit, implementation, GPU route and admission contracts, index class, exactness bound, evidence strength beside its claim scope, and the route's own gate, with the fail-closed selector and the table checker. | `native` | `r300-operation-route`, `r300-operation-ledger` | `KEEP_SILICON_CONTRACT` |
| `r300_carrier_policy.c`<br>`r300_carrier_policy.h` | Operation-tagged numeric-domain, carrier-encoding, format, and stride inventory for candidate raster-backed operations; registration alone does not certify route liveness. | `compiler` | `r300-carrier-policy`, `r300-carrier-format-pipe`, `r300-operation-ledger` | `KEEP_SILICON_CONTRACT` |
| `r300_pm4_builder.c`<br>`r300_pm4_builder.h` | Bounded PM4 word construction and error propagation. | `r300g`, `native` | `r300-pm4-builder` | `KEEP_SHARED` |
| `r300_pm4_compose.c`<br>`r300_pm4_compose.h` | Role-based PM4 fragment composition and relocation rebasing. | `native` | `r300-pm4-compose`, `r300-r2vb-reingest-compose-parity` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_source_contract.h` | R2VB source format, stride, swizzle, and fetch contract. | `r300g` | `r300-r2vb-source-contract` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_fetch_pass.c`<br>`r300_r2vb_fetch_pass.h`<br>`r300_r2vb_target_state.c`<br>`r300_r2vb_target_state.h` | R2VB fetch PM4 and target-state programming. | `native`, `r300g` | `r300-r2vb-fetch-pass`, `r300-r2vb-target-state` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_reingest_draw.c`<br>`r300_r2vb_reingest_draw.h` | Gallium-consumed R2VB reingest draw emission. | `r300g` | `r300-r2vb-reingest-draw` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_carrier_delivery.c`<br>`r300_r2vb_carrier_delivery.h` | Byte-defined R2VB carrier delivery model. | `native` | `r300-r2vb-carrier-delivery` | `KEEP_SILICON_CONTRACT` |
| `r300_first_draw_state.c`<br>`r300_first_draw_state.h` | Self-contained first-draw register contract and poison-state checker. | `native` | `r300-first-draw-state` | `KEEP_SILICON_CONTRACT` |
| `r300_flat_color0_plan.c`<br>`r300_flat_color0_plan.h` | Direct GA Flat plan through color 0: canonical provoking-FIRST words, validator, contract application, per-draw stream check. | `native` | `r300-flat-color0-plan` | `KEEP_SILICON_CONTRACT` |
| `r300_rs_tex_adj_probe.c`<br>`r300_rs_tex_adj_probe.h` | Rasterizer interpolation discriminator: control and candidate plans (RS_INST_0.TEX_ADJ, GB_SELECT.W_SELECT), validator, per-draw stream check, unequal-W TEX0 carrier, registered interpolation models, expected-image writer, pixel census, and classification. | `native` | `r300-rs-tex-adj-probe`, `r300-rs-tex-adj-probe-kernel-replay`, `r300-rs-tex-adj-probe-cs-track-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_noperspective_reciprocal_plan.c`<br>`r300_noperspective_reciprocal_plan.h`<br>`r300_noperspective_reciprocal_fs_block.h` | NoPerspective reciprocal carrier: TC1 record plan (PSC, VTX_FMT, VSM_VTX_ASSM, RS_COUNT, RS_IP/RS_INST words), per-triangle normalized (a * w, w) packing with the FP24 admission envelope, carrier stream validator, host recovery model, and the baked RCP+MUL US block `r300_tcl_bypass_fs_tool --emit=noperspective-reciprocal` regenerates. | `native` | `r300-noperspective-reciprocal-fs-block-regeneration`, `r3v-post-vs-lowering` | `KEEP_SILICON_CONTRACT` |
| `r300_noperspective_q_lane_plan.c`<br>`r300_noperspective_q_lane_plan.h`<br>`r300_noperspective_q_lane_fs_block.h` | NoPerspective q-lane carrier: the varying cell's record and register words restated for a float, vec2, or vec3 varying, in-place packing of (a.xyz * c, 0 past the width, c = w / max(w)) under the shared FP24 envelope, stream and clipper-expanded validators, host recovery model, per-draw stream check, and the baked xyz * rcp(w) alpha-1 US block `r300_tcl_bypass_fs_tool --emit=noperspective-q-lane` regenerates. | `native` | `r300-noperspective-q-lane-fs-block-regeneration`, `r300-noperspective-q-lane-plan` | `KEEP_SILICON_CONTRACT` |
| `r300_noperspective_mixed_carrier_plan.c`<br>`r300_noperspective_mixed_carrier_plan.h`<br>`r300_noperspective_mixed_carrier_fs_block.h` | Mixed Smooth/NoPerspective carrier: the three-vector reciprocal register contract at VAP_VTX_SIZE 16 (TC0 Smooth verbatim, TC1 NoPerspective * c, TC2 (c, 0, 0, 1), c = w / max(w)), packing of twelve-dword two-location records into sixteen-dword records under the shared FP24 envelope, plan refusals (RS budget of four, carrier alias, premultiplied set, US budget), stream and clipper-expanded validators, host recovery model, per-draw stream check, and the baked (TC0.xy, (TC1 * rcp(TC2.x)).xy) US block `r300_tcl_bypass_fs_tool --emit=noperspective-mixed-carrier` regenerates with its ALU and temporary counts. | `native` | `r300-noperspective-mixed-carrier-fs-block-regeneration`, `r300-noperspective-mixed-carrier-plan`, `r300-noperspective-mixed-carrier-kernel-replay`, `r300-noperspective-mixed-carrier-cs-track-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_fragment_binary.c`<br>`r300_fragment_binary.h` | Owned US/FG binary bytes, structural admission, and content identity.  The contract is hardware-owned. | `native` | `r300-fragment-binary` | `KEEP_SILICON_CONTRACT` |
| `r300_tcl_bypass_triangle.c`<br>`r300_tcl_bypass_triangle.h`<br>`r300_tcl_bypass_triangle_fs_block.h`<br>`r300_tcl_bypass_sampled_fs_block.h` | Fixed TCL-bypass triangle hardware plan and generated fragment blocks, the sampled-texture US block included. | `native` | `r300-tcl-bypass-triangle`, `r300-tcl-bypass-fs-block-regeneration`, `r300-tcl-bypass-sampled-fs-block-regeneration` | `KEEP_SILICON_CONTRACT` |
| `r300_direct_write.c`<br>`r300_direct_write.h` | 2D direct-write control cell: the fixed 64x64 probe geometry, its relocation sites, and the readback oracle, emitted as one instance of the RB2D solid-fill plan. | `native` | `r300-direct-write`, `r300-direct-write-cs-track-replay` | `KEEP_SILICON_CONTRACT` |
| `radeon_legacy_2d_reg.h` | The legacy Radeon 2D/GUI register block: the twelve MMIO byte offsets the 2D engine's plans write, eleven of them on the `reg_srcs/r300` safe list and `DST_PITCH_OFFSET` the relocation-bearing `r100_reloc_pitch_offset` case, with the GUI master-control fields, the ARGB8888 destination datatype code, the walk direction, the destination-cache flush pattern, and the WAIT_UNTIL idle bits those words carry. | `native` | `r300-rb2d-fill-plan`, `r300-direct-write` | `KEEP_SILICON_CONTRACT` |
| `r300_rb2d_fill.c`<br>`r300_rb2d_fill.h` | RB2D solid-fill plan: a linear destination surface, its rectangles, the register sequence the 2D engine executes them through, and the admission rules DST_PITCH_OFFSET's 64-byte pitch grid and 1 KiB offset grid impose. | `native` | `r300-rb2d-fill-plan`, `r300-direct-write` | `KEEP_SILICON_CONTRACT` |
| `r300_rb2d_linear_span.c`<br>`r300_rb2d_linear_span.h` | Linear-span decomposition above the rectangle plan: a dword-aligned byte interval becomes the ordered fill plans that cover it exactly, on a 64-byte carrier row of sixteen words, with every field bound read from `r300_rb2d_fill.h` rather than restated. | `native` | `r300-rb2d-linear-span` | `KEEP_SILICON_CONTRACT` |
| `r300_rb2d_pitch_evidence.c`<br>`r300_rb2d_pitch_evidence.h` | Pitch-evidence registry for the RB2D carriers: one row per (pitch, format, usage) naming the highest evidence class that exercised it and the retained artifact, so execution admits SILICON_RECEIPT alone and a pitch is promoted by one data-row edit. | `native` | `r300-rb2d-legalize` | `KEEP_SILICON_CONTRACT` |
| `r300_rb2d_contract_evidence.c`<br>`r300_rb2d_contract_evidence.h` | Contract-evidence registry for the RB2D route contracts: one row per contract naming the highest class that exercised its stream shape, the window and relocation-site counts that class reached, and the retained artifact, so admission reads carrier evidence and contract evidence together and a receipted contract still refuses a wider stream than ran. | `native` | `r300-rb2d-contract-evidence`, `r300-rb2d-legalize` | `KEEP_SILICON_CONTRACT` |
| `r300_rb2d_legalize.c`<br>`r300_rb2d_legalize.h` | RB2D legalizer: lowers a byte interval into rebased surface windows on an evidence-admitted carrier, holds every window to the hardware grids and the kernel's footprint bound, proves exact byte coverage across windows, ranks carriers by cost, and emits DAG-ordered state with one typed relocation per window; V1 is byte-identical to the plan emitter. | `native` | `r300-rb2d-legalize`, `r300-rb2d-legalization-differential` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_producer_pass.c`<br>`r300_r2vb_producer_pass.h`<br>`r300_r2vb_producer_fs_block.h` | R2VB producer hardware plan, publication tail, and generated fragment block. | `native` | `r300-r2vb-producer-pass`, `r300-r2vb-producer-replay`, `r300-r2vb-producer-fs-block-regeneration` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_reingest_pass.c`<br>`r300_r2vb_reingest_pass.h` | Producer-plus-reingest hardware plan and relocation contract. | `native` | `r300-r2vb-reingest-pass`, `r300-r2vb-reingest-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_float2_tuple_pass.c`<br>`r300_r2vb_float2_tuple_pass.h` | Fetched FLOAT_4 plus FLOAT_2 producer plan and oracle. | `native` | `r300-r2vb-float2-tuple-pass`, `r300-r2vb-float2-tuple-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_fetched_producer.c`<br>`r300_r2vb_fetched_producer.h` | R2VB producer fetching the application vertex BO through the two-array fetched body, and its composed route over the four BO roles. | `native` | `r300-r2vb-fetched-producer`, `r300-r2vb-fetched-route-replay-f32_4`, `r300-r2vb-fetched-route-replay-f32_3`, `r300-r2vb-fetched-route-replay-f32_2` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_public_route.c`<br>`r300_r2vb_public_route.h` | Producer and TCL-bypass consumer composed into one hardware plan. | `none` | `r300-r2vb-public-route`, `r300-r2vb-public-route-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_zb_depth_state.c`<br>`r300_zb_depth_state.h`<br>`r300_zb_depth_control_cell.c`<br>`r300_zb_depth_control_cell.h` | Z-buffer binding/test state and the dual-oracle depth control cell. | `native` | `r300-zb-depth-state`, `r300-zb-depth-control-cell`, `r300-zb-depth-control-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_zb_hyperz_admission.c`<br>`r300_zb_hyperz_admission.h` | HyperZ ownership admission: the kernel's non-owner rows (ZB_BW_CNTL enables, ZMASK and HiZ offsets and pitches, GB_Z_PEQ_CONFIG, SC_HYPERZ, the two PACKET3 clears) as data, and a stream walker framed like radeon_cs_packet_parse that admits with ownership and refuses at the first gated site without it. | `native` | `r300-zb-hyperz-admission` | `KEEP_SILICON_CONTRACT` |
| `r300_direct_write_manifest.c` | Test-only direct-write evidence writer. | `none` | `r300-direct-write-manifest-integration`, `r300-direct-write-cs-track-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_compute_identity_carrier_manifest.c` | Test-only compute identity carrier evidence writer (ib.bin, manifest.json). | `none` | `r300-compute-identity-carrier-cs-track-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_fetched_route_manifest.c` | Test-only fetched-route evidence writer (ib.bin, bo_table.json, manifest.json per source width). | `none` | `r300-r2vb-fetched-route-replay-f32_4`, `r300-r2vb-fetched-route-replay-f32_3`, `r300-r2vb-fetched-route-replay-f32_2` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_float2_tuple_manifest.c` | Test-only FLOAT_2 tuple evidence writer. | `none` | `r300-r2vb-float2-tuple-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_tuple_width_burst_manifest.c` | Test-only tuple-width burst evidence writer for FLOAT_2 and FLOAT_4 model streams. | `none` | `r300-r2vb-tuple-width-burst-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_producer_manifest.c` | Test-only producer evidence writer. | `none` | `r300-r2vb-producer-manifest-integration`, `r300-r2vb-producer-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_public_route_manifest.c` | Test-only composed-route evidence writer. | `none` | `r300-r2vb-public-route-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_reingest_manifest.c` | Test-only reingest evidence writer. | `none` | `r300-r2vb-reingest-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_triangle_manifest.c` | Test-only triangle evidence writer. | `none` | `r300-triangle-manifest-integration`, `r300-cs-track-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_zb_depth_control_manifest.c` | Test-only depth-control evidence writer. | `none` | `r300-zb-depth-control-replay` | `KEEP_SILICON_CONTRACT` |

## Liveness census

The consumer census records an API-neutral contract's ownership and header
reachability.  This table records different facts for the carrier inventory:

- `BUILD_REGISTERED` means the provider source belongs to the named Meson
  archive target.
- `ARCHIVE_DEFINED` means the archive test finds the subject with `nm` after a
  build.
- `Contract reach` records production compilation paths that include the
  declaration header; object reference remains a separate fact.
- `Production references` records non-provider production source references.
- `Route selected` records an exact production selector adapter.  The checker
  rejects positive values until that adapter and its wrong-selector calibration
  land.
- `Hardware executed` records an exact typed silicon-evidence adapter.  The
  checker rejects positive values until that external binding lands.
- `Evidence-only references` records tests and manifest writers as provenance
  facts.

The checker derives the carrier subjects from the registry, verifies their
Meson membership and declaration-header reach, separates production from test
references, and separately verifies compiled definitions.  The archive leg
establishes archive membership and global-definition facts.  The reference,
route-selection, and hardware-execution columns keep call, route, and silicon
facts distinct for `libr300_common` and `r300_nir.c`.  Every current route and
hardware cell is `none`, so a future positive claim must extend the checker
with its exact adapter and a known-bad calibration.

| Subject | Build registration | Archive definition | Contract reach | Production references | Route selected | Hardware executed | Evidence-only references |
|---|---|---|---|---|---|---|---|
| `r300_carrier_identity` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c` |
| `r300_carrier_dp4_u7` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c`, `src/amd/r300/common/tests/r300_operation_ledger_test.c` |
| `r300_carrier_dp4_u8_boundary` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c` |
| `r300_carrier_blend_acc` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c` |
| `r300_carrier_zpass` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c` |
| `r300_carrier_ieee16_classify` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c` |
| `r300_carrier_ieee16_mul` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c` |
| `r300_carrier_ieee16_result` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c`, `src/amd/r300/common/tests/r300_operation_ledger_test.c` |
| `r300_carrier_ieee16_debug` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c` |
| `r300_carrier_policies` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c`, `src/amd/r300/common/tests/r300_operation_ledger_test.c` |
| `r300_carrier_dp4_select` | `libr300_common` | `libr300_common` | `compiler`, `r300g` | `none` | `none` | `none` | `src/amd/r300/common/tests/r300_carrier_policy_test.c` |
| `r300_nir_build_carrier_pack` | `libr300_compiler` | `libr300_compiler` | `compiler`, `r300g` | `none` | `none` | `none` | `none` |

## Migration consequences

The direct-SPIR-V readers (`src/amd/r300/vulkan/r3v_vertex_spirv.c`,
`r3v_compute_spirv.c`) live in the Vulkan front end because their only
production input is Vulkan SPIR-V.  The NIR-to-job path remains in the r300
compiler for r300g, and both front ends meet at the neutral job IR through
`r300-vertex-front-end-parity`.

The delivery-route selector (`src/amd/r300/vulkan/r3v_delivery_route.c`)
lives in Vulkan because its environment gates and measured default choose one
R3V execution policy.  The R2VB passes and PM4
plans remain common because their tests establish packet, relocation, numeric,
and cache-publication contracts independently of Vulkan object lifetime.

The root-level manifest writers remain common silicon-contract tools.  They
compile no runtime object into either driver and publish exact PM4 and
relocation artifacts for offline kernel-parser replay.  Their row-by-row
decisions keep evidence tooling from being misreported as a second production
consumer.

The single-object cutover removed two source-list-only consumer labels.  r300g
previously compiled `r300_carrier_policy.c` and `r300_numeric_domain.c` as
members of `files_r300`, but no landed r300g production source references
either registry's symbols.  Compiler headers carry the carrier declarations
into r300g compilation, as the liveness table records; declaration reach is a
separate fact from object reference and route selection.  Archive linkage does
not create either of those facts: the numeric domain remains shared by compiler
and native reachability, while carrier policy stays common on its calibrated
silicon-contract proofs.  A future r300g carrier adapter earns the `r300g`
consumer label when production code references the carrier registry or selects
one of its policies.
