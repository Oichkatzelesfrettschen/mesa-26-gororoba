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
   the selected r3v device's `vulkaninfo` extension list.
2. `smoke.vkcube.no_crash` -- `vkcube` (a direct-VK xcb app) runs without
   SIGSEGV/abort (the null-bound-pipeline replay guard). Recorded
   `NotSupported` when `DISPLAY`, `vkcube`, or `timeout` is missing, or when
   the display cannot be opened (stale Xauthority / SSH without X). Other
   nonzero exits remain `Fail`.
3. `dEQP-VK.api.info.*` (see `caselist.txt`) -- per-case status from the
   `deqp-vk` headless build, diffed for `Pass -> Fail` regressions.

All three feed one `status.tsv` that is diffed against `baseline.tsv`, exactly
like the deqp-GLES2 `r300-point-line-sprite` harness.

## How

```sh
# record the baseline after a validated driver change
VK_ICD_FILENAMES=/path/to/r3v_icd.json \
DEQP_VK=/path/to/deqp-vk OUT=/var/tmp/r3v-vulkan-surface \
run.sh --record

# gate a build (exit 1 on any Pass->Fail regression)
VK_ICD_FILENAMES=/path/to/r3v_icd.json \
DEQP_VK=/path/to/deqp-vk OUT=/var/tmp/r3v-vulkan-surface \
run.sh --check

# deqp-vk can come from PATH when DEQP_VK is unset
VK_ICD_FILENAMES=/path/to/r3v_icd.json OUT=/var/tmp/r3v-vulkan-surface \
run.sh --check
```

The harness requires one `VK_ICD_FILENAMES` JSON file and rejects a list. It
clears `VK_DRIVER_FILES`, requires `vulkaninfo` to report `driverName = r3v`,
and runs all tools through that loader selection. `DEQP_VK` selects an executable
directly; otherwise `deqp-vk` must be on `PATH`.

`OUT` is a disposable **run parent** (not a shared root such as `/var/tmp`).
Each invocation creates and retains one fresh child under that parent, so prior
artifacts stay intact. The harness rejects shared temporary roots and the
source directory; pass a dedicated directory such as
`/var/tmp/r3v-vulkan-surface`. `vkcube` needs `DISPLAY`, `timeout`, and an
executable `vkcube`; `--record` also requires its successful completion because
the committed baseline records that smoke case as `Pass`.

Run `bash test-run.sh` for the hermetic fake-tool integrity fixtures. They cover
missing dEQP during recording, selected-driver verification, vkcube failures,
unknown dEQP verdicts, failed baseline copies, and retained output sentinels.

## Baseline provenance

`baseline.tsv` was recorded on the RS485M (renderer `ATI RS480`) against the r3v
build carrying the nine extensions and the null-pipeline replay guard. The
pre-existing deqp-vk Fails it pins (e.g. `format_properties.r8g8b8a8_unorm`,
`get_physical_device_properties2` limit-validation cases) are r3v format and
1.x-limit gaps unrelated to the extension surface; they are recorded so the gate
fires only on a true `Pass -> Fail`.
