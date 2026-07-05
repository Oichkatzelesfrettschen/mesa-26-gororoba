# r3v Vulkan extension-surface conformance harness

Regression gate for the promoted KHR extensions r3v advertises additively at
apiVersion 1.0, and for the direct-VK draw-replay robustness fixes.

## Why

The extensions (`VK_KHR_bind_memory2`, `get_memory_requirements2`,
`dedicated_allocation`, `driver_properties`, `format_feature_flags2`,
`uniform_buffer_standard_layout`, `relaxed_block_layout`,
`storage_buffer_storage_class`, `sampler_mirror_clamp_to_edge`) are enumeration
and metadata plus two requirements getters. Nothing in the GL/zink path
exercises them, so a withdrawn extension bit, a dropped `VkFormatProperties3`
fill, or a null-pipeline replay crash would regress silently. This harness pins
the validated state and fails any `Pass -> Fail`.

## What it checks

1. `smoke.device_extension.<name>` -- each of the nine extensions is present in
   `vulkaninfo`'s device extension list.
2. `smoke.vkcube.no_crash` -- `vkcube` (a direct-VK xcb app) runs without
   SIGSEGV/abort (the null-bound-pipeline replay guard). Recorded
   `NotSupported` when no display or `vkcube` is available.
3. `dEQP-VK.api.info.*` (see `caselist.txt`) -- per-case status from the
   `deqp-vk` headless build, diffed for `Pass -> Fail` regressions.

All three feed one `status.tsv` that is diffed against `baseline.tsv`, exactly
like the deqp-GLES2 `r300-point-line-sprite` harness.

## How

```sh
# record the baseline (after a validated driver change)
DEQP_VK=/path/to/deqp-vk run.sh --record

# gate a build (exit 1 on any Pass->Fail regression)
DEQP_VK=/path/to/deqp-vk run.sh --check

# test a scoped build instead of the system driver
VK_ICD_FILENAMES=/tmp/r3v_test/r300_test_icd.json run.sh --check
```

`DEQP_VK` defaults to the vostro headless build
(`deqp-vk/build-vostro-r300vk-headless/.../deqp-vk`). The driver under test is
whatever the Vulkan loader resolves; export `VK_ICD_FILENAMES` for a scoped
build. `vkcube` needs `DISPLAY` + `XAUTHORITY` (xcb); without them that one check
is recorded `NotSupported`.

## Baseline provenance

`baseline.tsv` was recorded on the RS482 (ATI RS480/RS482) against the r3v
build carrying the nine extensions and the null-pipeline replay guard. The
pre-existing deqp-vk Fails it pins (e.g. `format_properties.r8g8b8a8_unorm`,
`get_physical_device_properties2` limit-validation cases) are r3v format and
1.x-limit gaps unrelated to the extension surface; they are recorded so the gate
fires only on a true `Pass -> Fail`.
