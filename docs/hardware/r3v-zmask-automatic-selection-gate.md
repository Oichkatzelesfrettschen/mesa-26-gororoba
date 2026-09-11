# ZMASK automatic selection gate

Automatic ZMASK selection is the standing decision to substitute the ZMASK
fast clear for the ordinary combined depth and stencil clear with no
operator gate in the path. `r3v_native_zmask_admission.{c,h}` declares that
decision as two tables, and the device holds it closed:
`device->zmask_automatic_qualified` is the conjunction of a pure admission
predicate over the candidate in front of the driver and a retained
promotion record that `r3v_native_zmask_promotion_retained` answers `NULL`
for. The conjunction is therefore false by construction, and the enable
becomes reachable exactly when the record opens.

## Evidence class

Every value this gate pins comes from an offline model.
`analysis/r3v-zmask-public-lifecycle-qualification/prediction/prediction.json`
in `steinmarder-r300` carries `evidence_class` `offline model; silicon
observation pending`, and `r3v-zmask-clear-protocol.md` states that no
silicon run exists for any stage of the ZMASK ladder. The gate accordingly
records a predicted configuration and eight predicted results. All eight
requirements stand outstanding, and no clause below is a silicon claim.

## The logical-image invariant

Every clause serves one invariant: a ZMASK representation is storage, so
the logical image an application reads is the image it wrote, for every
aspect the specification requires to survive.

Vulkan 1.0 grants the storage freedom and fixes the boundary in three
places. Resource Creation, "Images": `VK_IMAGE_TILING_OPTIMAL` lays texels
out in an implementation-dependent arrangement. Resource Creation, "Image
Layouts": images are stored in implementation-dependent opaque layouts,
and each layout bounds the operations its subresources support.
Synchronization and Cache Control, "Image Layout Transitions": a
transition happens inside a memory dependency, and a transition whose old
layout matches the subresource's current layout preserves that range's
contents, with `VK_IMAGE_LAYOUT_UNDEFINED` the only old layout that
discards it. The "Image Memory Barriers" note states the equal-layout case
directly: with old and new layout equal, data is preserved whatever the
values say.

Metadata that stands in for depth memory is therefore admissible storage.
A candidate whose clauses all hold is one where the substitution stays
invisible to the logical image.

## The admission predicate

`r3v_native_zmask_automatic_admission` walks the clause table and returns
the first row that refuses, so a refusal names one mechanism rather than a
conjunction. Each row carries a durable name, its own verdict, and the
predicate that reads the candidate.

| Clause | Verdict | What the clause reads |
|---|---|---|
| `compressed-writes-withheld` | `COMPRESSION_UNQUALIFIED` | The candidate asks for no compressed depth writes |
| `rs485m-resolved-platform` | `PLATFORM_UNQUALIFIED` | The resolved board is `R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M` and its PCI pair agrees |
| `d24-unorm-s8-uint-format` | `FORMAT_UNQUALIFIED` | `VK_FORMAT_D24_UNORM_S8_UINT` |
| `microtiled-and-macrotiled` | `TILING_UNQUALIFIED` | Optimal tiling resolved as microtiled and macrotiled |
| `single-sample` | `SAMPLE_COUNT_UNQUALIFIED` | One sample per pixel |
| `qualified-geometry-pitch-and-base` | `ENVELOPE_UNQUALIFIED` | 64x64 at a 64-pixel row pitch, based 2048 bytes into the allocation |
| `full-surface-combined-clear` | `CLEAR_SCOPE_UNQUALIFIED` | One clear covering depth and stencil together over every pixel |
| `hyperz-ownership-acquired` | `OWNERSHIP_UNQUALIFIED` | `RADEON_INFO_WANT_HYPERZ` granted this file descriptor |
| `zmask-metadata-interval-available` | `METADATA_INTERVAL_UNQUALIFIED` | A ZMASK metadata interval is available |
| `no-conflicting-metadata-owner` | `METADATA_OWNER_CONFLICT` | No other live image holds the ZMASK RAM |
| `ordinary-fallback-and-materializer` | `FALLBACK_UNQUALIFIED` | The ordinary clear and the materializer scratch both stand |
| `supported-aspect-operations` | `ASPECT_OPERATION_UNQUALIFIED` | Every aspect operation in the active transition is one the representation answers |

Compression stands first because its refusal is unconditional. A candidate
asking the depth pipe to write compressed tiles raises `ZB_BW_CNTL`
`WR_COMP_ENABLE`, which changes what depth memory holds, while the fast
clear leaves every stored value alone. Placing the row first means a
compressed request names compression whatever else about the candidate
disagrees, so genuine compression cannot ride the fast-clear promotion.

### Clause authorities

The clauses cite Vulkan 1.0 core as bounded by
`r3v-vulkan-1-0-core-sheet.json`, and the silicon rules underneath them
come from the R300-class sources the protocol document names.

- Format: Vulkan 1.0, Formats, "Depth/Stencil Formats". `D24_UNORM_S8_UINT`
  carries 24 unsigned-normalized depth bits beside 8 stencil bits in 32,
  the width `r300_zmask_layout_compute` admits.
- Tiling: Vulkan 1.0, Resource Creation, "Images"
  (`VK_IMAGE_TILING_OPTIMAL`) and "Image Layouts". ZMASK covers a
  microtiled level, and the 8x8 compression block adds macrotiling.
- Samples: Vulkan 1.0, Resource Creation, "Images"
  (`VkImageCreateInfo::samples`). ZMASK resolves one sample per pixel.
- Clear scope: Vulkan 1.0, Clear Commands, "Clearing Images Outside a
  Render Pass Instance". The aspect mask selects which aspects
  `vkCmdClearDepthStencilImage` writes, and the substitution replaces one
  clear covering both.
- Aspect operations: Vulkan 1.0, Synchronization and Cache Control, "Image
  Layout Transitions". A transition out of a matching old layout preserves
  the range, so an operation the representation cannot answer would break
  that preservation.
- Ownership: `r300_packet0_check` and `r300_packet3_check` in
  `drivers/gpu/drm/radeon/r300.c` reject a non-owner's `ZB_ZMASK_PITCH`
  write and `3D_CLEAR_ZMASK` packet;
  `RADEON_INFO_WANT_HYPERZ` in `radeon_kms.c` records the owner.

### The frozen envelope

The qualification froze one image rather than a range, so the envelope
clause compares for equality: width 64, height 64, row pitch 64 pixels,
base 2048 bytes. A geometry, pitch, or base outside those values is a
configuration the qualification says nothing about. The ZMASK RAM fit
follows from the same values -- `r300_zmask_layout_compute` resolves this
level inside the 5120-dword RS480 budget -- so the envelope clause pins
the layout without a separate fit clause.

`r3v_native_zmask_qualification_candidate` fills exactly that candidate
with a caller's resolved board in place of the qualification's own, so a
caller holding a board and nothing else asks one question: does this
device stand where the qualification stood.

## The promotion requirements

`r3v_native_zmask_promotion_requirements` is the second table: the eight
results the promotion consumes, each keyed by a durable identifier that a
retained bundle, a finding, and this table all spell the same way.
`r3v_native_zmask_promotion_missing` evaluates a supplied record and
reports how many stand outstanding and which one comes first.

| Result identifier | What the retained result shows |
|---|---|
| `zmask-fast-clear-substitution` | The fast clear produces the image the ordinary combined clear produces |
| `zmask-partial-update-through-materialization` | A partial depth update through the materializer holds untouched pixels at the clear value and updated pixels at their written depth and stencil |
| `zmask-materialized-exact-readback` | Materialized depth memory reads back byte for byte |
| `zmask-aba-metadata-switch` | Metadata follows image A to image B and back to A without carrying B's state into A |
| `zmask-allocation-boundary-preservation` | The prefix guard, storage padding, and tail guard around the surface envelope survive the substitution |
| `zmask-cross-command-buffer-ordering` | Metadata state recorded in one command buffer is the state the next submission observes |
| `zmask-default-loader-public-path` | The result reproduces through the default Vulkan loader on the public entry points |
| `zmask-kernel-log-clean` | The run leaves no DRM CS rejection, lockup, or reset in the kernel log |

Automatic selection stays disabled until every one of the eight is
retained. `r3v_native_zmask_promotion_retained` answers `NULL` while any
of them stands outstanding, and the device's conjunction reads that
answer, so the driver's behavior follows the table rather than a comment.

## Validation

`r3v-native-zmask-admission` walks the clause table with the admitting
candidate and one known-bad mutation per clause, asserting that the
predicate names exactly the mutated clause and that the mutation count
equals the clause count. It drives the requirement evaluator against an
empty, a partial, and a complete record, and it asserts that the retained
record is absent and that the device's conjunction is therefore false.
`r3v_native_zmask_admission_tables_self_check` holds both tables to
distinct names, distinct verdicts, and distinct record flags.
