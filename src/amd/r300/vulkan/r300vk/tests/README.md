# r300vk Validation Probes

This directory holds standalone r300vk probes that validate API contracts
against an installed or build-tree ICD.  The probes are not a replacement for
CTS.  They provide narrow, inspectable checks for driver mechanisms that CTS
will later cover in broader shards.

## 4096 Image Floor

`r300vk_4096_image_probe.c` verifies the RS482/R300 4096 image-floor path:

- `vkGetPhysicalDeviceImageFormatProperties` reports a 4096 2D extent with one
  mip level and one array layer.
- `vkCreateImage` accepts a `4096x4096` `B8G8R8A8_UNORM` color image.
- a render-pass load clear covers the tiled image.
- `vkCmdCopyImageToBuffer` reads small regions at the origin, across the
  2560-span seam, and at the bottom-right edge.

Run it through the POSIX wrapper:

```sh
VK_ICD_FILENAMES=/usr/local/mesa-26-gororoba/share/vulkan/icd.d/r300_icd.x86_64.json \
  src/amd/r300/vulkan/r300vk/tests/run_r300vk_4096_validation.sh \
  /tmp/r300vk-4096-validation
```

The wrapper writes `probe.jsonl`, `compile.log`, `summary.txt`, and a filtered
`dmesg` tail into the output directory.
