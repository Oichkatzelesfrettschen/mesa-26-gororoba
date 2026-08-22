# R300 common contracts

`src/amd/r300/common` contains API-neutral R300 hardware vocabulary and plans.
The durable boundary puts Gallium adapters under `src/gallium/drivers/r300`,
NIR adapters under `src/amd/r300/compiler`, Vulkan objects and front ends
under `src/amd/r300/vulkan`, and neutral job executors under
`src/amd/r300/cpu`.  The current Gallium-backed Vulkan exception is censused
below and expires during P2.

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
- `MOVE_TO_R3V_FRONTEND`: the component interprets an API input for the native
  Vulkan product and moves with that front end.
- `MOVE_TO_R3V_POLICY`: the component selects native Vulkan product behavior
  rather than describing hardware and moves with that policy.

Consumer tags name build domains: `r300g`, `compiler`, `native`, and `cpu`.
The temporary `legacy-r3v` tag names the Gallium-backed Vulkan experiment;
it is not r300g and it is not the native Vulkan product.  P2 deletes that
domain after its reusable compiler mechanisms move to r300g.  `none` means
the component is an evidence producer rather than a production object.  Test
names are exact Meson registrations.

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
| `r300_capabilities.h` | Chip capability record shared by screen and compiler decisions. | `r300g`, `compiler` | `r300-nir-vs-harness` | `KEEP_SHARED` |
| `r300_reg.h`<br>`r300_shader_semantics.h` | Register encodings and shader semantic vocabulary. | `r300g`, `compiler`, `native`, `legacy-r3v` | `r300-pm4-builder`, `r300-nir-to-rc` | `KEEP_SHARED` |
| `r300_numeric_domain.c`<br>`r300_numeric_domain.h`<br>`r300_us_source_read.h` | FP24 and source-read numeric domains. | `r300g`, `compiler`, `native`, `legacy-r3v` | `r300-fp16-limb-oracle`, `r300-classic-pair-value` | `KEEP_SHARED` |
| `r300_grid_fold.h` | FP24-exact invocation-index guards and bounded 1D/2D/3D folds onto the R300 sampleable raster. | `legacy-r3v` | `r300-grid-fold` | `KEEP_SILICON_CONTRACT` |
| `r300_vertex_format.h`<br>`r300_vertex_stream.h` | API-neutral vertex formats and byte-addressed stream records. | `r300g`, `native`, `cpu` | `r300-vertex-format-pipe`, `r300-cpu-vertex` | `KEEP_SHARED` |
| `r300_compute_job.h`<br>`r300_vertex_job.h` | Neutral compute and vertex job IR. | `compiler`, `native`, `cpu` | `r300-cpu-compute-job`, `r300-cpu-vertex-job`, `r300-vertex-front-end-parity` | `KEEP_SHARED` |
| `r300_compute_spirv.c`<br>`r300_compute_spirv.h`<br>`r300_vertex_spirv.c`<br>`r300_vertex_spirv.h` | Direct SPIR-V admission into neutral jobs.  SPIR-V is the Vulkan front-end input, not an R300 hardware contract. | `native` | `r3v-native-compute-frontend`, `r3v-native-pipeline-frontend`, `r300-vertex-front-end-parity` | `MOVE_TO_R3V_FRONTEND` |
| `r300_carrier_policy.c`<br>`r300_carrier_policy.h` | Numeric-domain, carrier-encoding, format, and stride policy for raster-backed operations. | `compiler` | `r300-carrier-policy`, `r300-carrier-format-pipe` | `KEEP_SILICON_CONTRACT` |
| `r300_pm4_builder.c`<br>`r300_pm4_builder.h` | Bounded PM4 word construction and error propagation. | `r300g`, `native` | `r300-pm4-builder` | `KEEP_SHARED` |
| `r300_pm4_compose.c`<br>`r300_pm4_compose.h` | Role-based PM4 fragment composition and relocation rebasing. | `none` | `r300-pm4-compose`, `r300-r2vb-reingest-compose-parity` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_source_contract.h` | R2VB source format, stride, swizzle, and fetch contract. | `r300g` | `r300-r2vb-source-contract` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_fetch_pass.c`<br>`r300_r2vb_fetch_pass.h`<br>`r300_r2vb_target_state.c`<br>`r300_r2vb_target_state.h` | R2VB fetch PM4 and target-state programming. | `r300g` | `r300-r2vb-fetch-pass`, `r300-r2vb-target-state` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_reingest_draw.c`<br>`r300_r2vb_reingest_draw.h` | Gallium-consumed R2VB reingest draw emission. | `r300g` | `r300-r2vb-reingest-draw` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_carrier_delivery.c`<br>`r300_r2vb_carrier_delivery.h` | Byte-defined R2VB carrier delivery model. | `native` | `r300-r2vb-carrier-delivery` | `KEEP_SILICON_CONTRACT` |
| `r300_first_draw_state.c`<br>`r300_first_draw_state.h` | Self-contained first-draw register contract and poison-state checker. | `native` | `r300-first-draw-state` | `KEEP_SILICON_CONTRACT` |
| `r300_fragment_binary.c`<br>`r300_fragment_binary.h` | Owned US/FG binary bytes, structural admission, and content identity.  The contract is hardware-owned; Vulkan-specific ownership wording is removed during the single-ICD cutover. | `native`, `legacy-r3v` | `r300-fragment-binary` | `KEEP_SILICON_CONTRACT` |
| `r300_tcl_bypass_triangle.c`<br>`r300_tcl_bypass_triangle.h`<br>`r300_tcl_bypass_triangle_fs_block.h` | Fixed TCL-bypass triangle hardware plan and generated fragment block. | `native` | `r300-tcl-bypass-triangle`, `r300-tcl-bypass-fs-block-regeneration` | `KEEP_SILICON_CONTRACT` |
| `r300_direct_write.c`<br>`r300_direct_write.h` | 2D direct-write control cell, relocation sites, and readback oracle. | `native` | `r300-direct-write`, `r300-direct-write-cs-track-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_producer_pass.c`<br>`r300_r2vb_producer_pass.h`<br>`r300_r2vb_producer_fs_block.h` | R2VB producer hardware plan, publication tail, and generated fragment block. | `native` | `r300-r2vb-producer-pass`, `r300-r2vb-producer-replay`, `r300-r2vb-producer-fs-block-regeneration` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_reingest_pass.c`<br>`r300_r2vb_reingest_pass.h` | Producer-plus-reingest hardware plan and relocation contract. | `native` | `r300-r2vb-reingest-pass`, `r300-r2vb-reingest-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_float2_tuple_pass.c`<br>`r300_r2vb_float2_tuple_pass.h` | Fetched FLOAT_4 plus FLOAT_2 producer plan and oracle. | `native` | `r300-r2vb-float2-tuple-pass`, `r300-r2vb-float2-tuple-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_public_route.c`<br>`r300_r2vb_public_route.h` | Producer and TCL-bypass consumer composed into one hardware plan. | `none` | `r300-r2vb-public-route`, `r300-r2vb-public-route-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_zb_depth_state.c`<br>`r300_zb_depth_state.h`<br>`r300_zb_depth_control_cell.c`<br>`r300_zb_depth_control_cell.h` | Z-buffer binding/test state and the dual-oracle depth control cell. | `native` | `r300-zb-depth-state`, `r300-zb-depth-control-cell`, `r300-zb-depth-control-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_delivery_route.c`<br>`r300_delivery_route.h` | Environment-gated CPU/R2VB route selection and measured default policy. | `native` | `r300-delivery-route` | `MOVE_TO_R3V_POLICY` |
| `r300_direct_write_manifest.c` | Test-only direct-write evidence writer. | `none` | `r300-direct-write-manifest-integration`, `r300-direct-write-cs-track-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_float2_tuple_manifest.c` | Test-only FLOAT_2 tuple evidence writer. | `none` | `r300-r2vb-float2-tuple-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_float2_tuple_burst_manifest.c` | Test-only tuple-burst evidence writer. | `none` | `r300-r2vb-float2-tuple-burst-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_producer_manifest.c` | Test-only producer evidence writer. | `none` | `r300-r2vb-producer-manifest-integration`, `r300-r2vb-producer-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_public_route_manifest.c` | Test-only composed-route evidence writer. | `none` | `r300-r2vb-public-route-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_r2vb_reingest_manifest.c` | Test-only reingest evidence writer. | `none` | `r300-r2vb-reingest-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_triangle_manifest.c` | Test-only triangle evidence writer. | `none` | `r300-triangle-manifest-integration`, `r300-cs-track-replay` | `KEEP_SILICON_CONTRACT` |
| `r300_zb_depth_control_manifest.c` | Test-only depth-control evidence writer. | `none` | `r300-zb-depth-control-replay` | `KEEP_SILICON_CONTRACT` |

## Migration consequences

The direct-SPIR-V readers move to the Vulkan front end because their only
production input is Vulkan SPIR-V.  The NIR-to-job path remains in the r300
compiler for r300g, and both front ends continue to meet at the neutral job IR
through the parity tests.  Until that P2 source move lands, the two readers and
the delivery-route selector are named in
`r300_common_pending_r3v_move_files`: one archive owns their object bytes, but
the build arrangement does not supersede their `MOVE_TO_R3V_*` dispositions.

The `legacy-r3v` rows expose the Gallium-backed Vulkan experiment's current
dependencies without making it a durable owner.  P2 moves its reusable NIR
compiler mechanisms to r300g and deletes that Vulkan adapter; no
`legacy-r3v` tag survives the single-ICD cutover.

The delivery-route selector moves to Vulkan because its environment gates and
measured default choose one R3V execution policy.  The R2VB passes and PM4
plans remain common because their tests establish packet, relocation, numeric,
and cache-publication contracts independently of Vulkan object lifetime.

The root-level manifest writers remain common silicon-contract tools.  They
compile no runtime object into either driver and publish exact PM4 and
relocation artifacts for offline kernel-parser replay.  Their row-by-row
decisions keep evidence tooling from being misreported as a second production
consumer.

The single-object cutover removed two source-list-only consumer labels.  r300g
previously compiled `r300_carrier_policy.c` and `r300_numeric_domain.c` as
members of `files_r300`, but no landed r300g production root reaches either
public contract.  Archive linkage does not restore that claim: the numeric
domain remains shared by compiler and native reachability, while carrier policy
stays common on its calibrated silicon-contract proofs.  A future r300g
carrier adapter earns the `r300g` label only when its production source reaches
the public header.
