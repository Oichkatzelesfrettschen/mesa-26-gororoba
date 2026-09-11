# R3V Vulkan 1.0 core review

`r3v-vulkan-1-0-core-sheet.json` is the machine readable projection of
`VK_VERSION_1_0` from the final Vulkan 1.0 core registry release. Khronos
published `v1.0.69-core` on 2018-02-19. The annotated tag resolves to
`ab08f0951ef1ad9b84db93f971e113c1d9d55609`; the official
`src/spec/vk.xml` payload hashes to
`820d7e3f6fb54d955b0bbd89a0c96be3e66f9a7cc60a7eb28d820ad57c6c2b4e`.
The registry landing page explains that the former Vulkan 1.0 reference-page
compilations moved to the Vulkan-Web-Registry history after Vulkan 1.1.

The sheet preserves the 30 direct requirement blocks, 137 commands, 16 types,
and 9 enumerants from that registry feature. JSON is the canonical format
because each requirement block has ordered command, type, and enumerant arrays
and each tracker row has several evidence locators and a falsifier. A TSV
would require escaped lists and lose the structural checks that the JSON audit
performs. The existing R3V dEQP partition remains TSV because its rows contain
one flat test-slice record each.

The registry projection excludes normative specification prose, valid-usage
rules, synchronization semantics, format tables, and CTS expectations. Those
obligations require separate claims and tests. The sheet therefore identifies
the API denominator, and the tracker records the evidence needed to decide
whether the driver satisfies a category.

## Reproducing the projection

The following commands retrieve the exact official source outside the tree and
validate the checked-in projection against it:

```sh
curl --fail --location --silent --show-error \
  https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/ab08f0951ef1ad9b84db93f971e113c1d9d55609/src/spec/vk.xml \
  -o /tmp/vk-1.0.69-core.xml
sha256sum /tmp/vk-1.0.69-core.xml
"$PYTHON" src/amd/r300/vulkan/tests/r3v_vulkan_1_0_core_tracker_audit.py \
  --sheet docs/hardware/r3v-vulkan-1-0-core-sheet.json \
  --tracker docs/hardware/r3v-vulkan-1-0-core-implementation-tracker.json \
  --source-root . \
  --registry-xml /tmp/vk-1.0.69-core.xml
```

Set `PYTHON` to the interpreter resolved by the active Meson configuration.
The expected XML digest appears in the sheet and in the audit. The registered
offline test also compares the complete ordered requirement-block projection
with the SHA-256 digest derived from that XML. The audit rejects a different
XML payload, a changed direct-requirement projection, duplicate tracker
coverage within or across rows, an unknown requirement block, a missing
requirement block, a non-object JSON input, an evidence locator whose literal
term is absent from the source blob at the tracker's reviewed Mesa commit, or
a status that claims conformance. The audit resolves those blobs through the
Git object database, so a later checkout cannot silently change the source
review result.

## Source review at Mesa `0b66d14e758c80808e7cc661c008b2a834d12fba`

The native R3V ICD owns a Gallium-free Radeon DRM transport. The separation
audit examines native source and binary links for Gallium identifiers and
symbols. `r3v_native_entrypoint_audit.py` classifies core entry points and
holds the native dispatch closure to its declared set. The compiled
direct-table sweep calls `vkEnumerateInstanceVersion` and checks the Vulkan
1.0 major and minor version. The checks establish a source-level boundary;
they supply neither a loader observation nor a Vulkan conformance result.

The implementation tracker records eleven mechanism groups:

| Group | Source review result | Next decisive evidence |
| --- | --- | --- |
| API definition and version | bounded | direct version and constant checks against the reported API |
| instance and discovery | bounded | loader query against the exact ICD |
| device, queue, and submission | bounded behind arming and retention | retained target submission with completion and output evidence |
| memory and binding | bounded one-BO model | memory CTS slice and target lifetime receipt |
| sparse resources | refused | complete sparse-memory design, execution route, and CTS plan |
| synchronization and queries | bounded CPU-side objects | ordered queue and query CTS evidence |
| buffers, images, and views | bounded admitted families | transfer and image CTS evidence |
| shaders, pipelines, and descriptors | bounded admitted subsets | shader and descriptor CTS evidence |
| render passes and command lifetime | bounded recorded cell | public command-buffer CTS evidence |
| command recording | refused outside qualified subsets | per-command execution route and source plus runtime tests |
| auxiliary core types | unassessed | type-specific command contract and focused test |

The current source therefore supplies bounded mechanisms and explicit refusal
surfaces. The current source does not support a Vulkan 1.0 conformance claim.
The 137-command denominator contains a wider semantic contract than a static
entrypoint closure, and the tracker contains neither a complete CTS pass nor a
single retained result that covers every core requirement.

The existing conformance partition reinforces that boundary. Its 21 slices
cover 3,251,483 pinned dEQP-VK cases, while the status authority records
silicon-delivered observations for selected host-model and shut-gate slices.
The remaining slices and every source-only tracker row stay outside a complete
API-version verdict. A build, an offline parser replay, or a gated command
submission cannot replace that verdict.

## Evidence and residuals

The review uses the official Vulkan-Docs tag and XML as primary API authority,
R3V native source as implementation authority, and the existing entrypoint,
advertised-surface, memory, queue, object-lifetime, pipeline, descriptor, and
public-surface tests as static or host-model checks. This change runs neither a
loader probe, dEQP, nor a hardware submission because the requested conversion
and review change documentation and source audits only. Hardware and
conformance evidence remain in `steinmarder-r300` under their existing
retention and safety contracts.
