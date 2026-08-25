# R3V WSI denominator

## Scope

This document carries ICD-surface facts about the R3V native Vulkan
driver's WSI boundary, so it lives in `docs/hardware` beside
`r3v-implementation-boundaries.md` and the other r3v-native procedure and
contract documents `src/amd/r300/vulkan/README.md` links into.

`libvulkan_r3v.so` (`src/amd/r300/vulkan/`) links the Mesa common WSI layer
through `idep_vulkan_wsi` (`meson.build` line 120: `idep_vulkan_wsi`, `rg
--fixed-strings idep_vulkan_wsi src/amd/r300/vulkan/meson.build`) and
advertises `VK_KHR_surface` plus the X11 surface extensions, but the device
extension table carries no `VK_KHR_swapchain`. This document names the
denominator every WSI-dependent conformance case sits on top of: which
platform code links, which extensions the instance and device tables carry,
which driver callback the presentation path needs and what that callback
does today, and which dEQP groups a given host class can execute as a
result.

## Compile-time platform surface

`idep_vulkan_wsi` pulls in all four WSI platform backends unconditionally;
`nm libvulkan_r3v.so` finds their internal symbols regardless of which
surface extensions the instance table advertises (`nm
build/*/src/amd/r300/vulkan/libvulkan_r3v.so`, full symbol table, not the
dynamic-only `-D` table). The dynamic symbol table exports only the
`vk_icd*` loader entry points (`nm -D`: `vk_icdGetInstanceProcAddr`,
`vk_icdGetPhysicalDeviceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion`);
every `wsi_*` symbol is link-time internal to the DSO.

| WSI platform | internal symbol prefix (`nm`) | runtime library (`ldd`) | instance/device extension advertised |
|---|---|---|---|
| X11/DRI3 | `wsi_x11_*` (e.g. `wsi_x11_get_connection`) | `libxcb.so.1`, `libX11-xcb.so.1`, `libxcb-dri3.so.0`, `libxcb-present.so.0`, `libxcb-shm.so.0` | `KHR_xcb_surface`, `KHR_xlib_surface` |
| Wayland | `wsi_wl_*` (e.g. `wsi_wl_alloc_image_shm`) | `libwayland-client.so.0` | none |
| Display/KMS | `wsi_display_*` (e.g. `wsi_display_init_wsi`) | none beyond libdrm | none |
| Headless | `wsi_headless_*` (e.g. `wsi_headless_init_wsi`) | none | none (`EXT_headless_surface` absent: `rg EXT_headless_surface src/amd/r300/vulkan/` has no hit) |

Every backend links and runs its own compile-time and link-time checks; the
extension table is what makes a backend reachable from an application.
Wayland, display, and headless carry the code and the library dependency
with no matching `vkCreate*SurfaceKHR` entry point to reach them.

## Extension table gap

`r3v_instance_extensions_supported` (`r3v_instance.c` lines 46-61,
`rg --fixed-strings r3v_instance_extensions_supported
src/amd/r300/vulkan/r3v_instance.c`) carries
`KHR_get_physical_device_properties2`, `KHR_surface`, `KHR_xcb_surface`,
`KHR_xlib_surface`, and `KHR_external_memory_capabilities`.
`r3v_native_device_extensions_supported` (`r3v_physical_device.c` lines
397-402) carries `KHR_get_memory_requirements2`, `KHR_bind_memory2`, and
`KHR_dedicated_allocation`. Neither table carries `KHR_swapchain`; grepping
both source files for the string finds no occurrence. The advertised-surface
binding table (`tests/r3v_advertised_surface_deqp_binding.tsv`) rows every
supported instance and device extension to a dEQP group and carries no
`VK_KHR_swapchain` row, agreeing with the source tables it audits.

An application can construct a `VkSurfaceKHR` and query it (`KHR_surface`,
`KHR_xcb_surface`, `KHR_xlib_surface` are all `true`), but `vkCreateSwapchainKHR`
does not exist on this ICD: the extension that would expose it is not in the
device table, so the loader never resolves the entry point into the driver.

## Default present route

`r3v_init_wsi` (`r3v_physical_device.c` lines 436-459, `rg -n r3v_init_wsi
src/amd/r300/vulkan/r3v_physical_device.c`) reads `R3V_WSI_SW` and sets
`wsi_device_options.sw_device` to `true` only when the variable's first byte
is `'1'`. `wsi_sw` is otherwise `false`, so `wsi_device_init` receives
`device->render_node_fd`, not `-1`, and the common WSI layer takes the
DRM/DRI3 route by default; `R3V_WSI_SW=1` is the opt-in that forces the
`sw_device` (xcb-shm CPU) route. `wants_linear = true` is the only
`wsi_device` field the driver sets beyond what `wsi_device_init` fills in
(`struct wsi_device_options` at `src/vulkan/wsi/wsi_common.h` line 270 has
exactly `sw_device`, `extra_xwayland_image`, and `emulate_24as32`; r3v sets
none of the latter two).

Three comment sites carried the design as software-mode-by-default,
contradicting this control-flow reading:

- `r3v_physical_device.c`, directly above `r3v_init_wsi`: "Mesa common WSI
  in software mode, the lavapipe pattern: sw_device makes the swapchain
  allocate CPU-reachable images and present through the xcb-shm path".
- `r3v_physical_device.h`, on the `wsi_device` field: the same "software
  mode (the lavapipe pattern)" description.
- `r3v_instance.c`, on `KHR_surface`: "backs presentation through Mesa's
  common WSI in software mode ... X11 surfaces only; presentation runs the
  xcb-shm CPU path, no DRM modifiers or dma-buf involved".

The correct description already existed inline inside `r3v_init_wsi` itself:
"GPU-resident present by default: the render-node fd lets the common WSI
take the DRM/DRI3 path ... `R3V_WSI_SW=1` falls back to the xcb-shm CPU
copy". The three comments now state the same mechanism the inline comment
and the code carry: a value of `R3V_WSI_SW` beginning with `'1'` sets
`sw_device` and passes `-1` for the fd; every other value routes through
the render-node fd, the DRM/DRI3 path.

## Driver callback denominator

No `vkCreateSwapchainKHR` call ever reaches the driver today, so the
question below the extension gap is what the presentation callbacks the
common WSI layer would call find when a swapchain-shaped image reaches the
driver's own entry points.

| callback / path | location | status | evidence |
|---|---|---|---|
| `vkGetMemoryFdKHR` / PRIME export | `r3v_native_memory.c` | absent | `rg -i "GetMemoryFd|PRIME|dma.buf" src/amd/r300/vulkan/r3v_native_memory.c` has no hit |
| `vkCreateImage` on a color-attachment image outside 64x64 | `r3v_native_image.c` `r3v_CreateImage`, lines 73-78 | refused (`R3V_NATIVE_REFUSAL_RESULT`) | `R3V_NATIVE_TARGET_WIDTH`/`R3V_NATIVE_TARGET_HEIGHT` are `64` (`r3v_native.h` lines 787-788) |
| `vkCreateImage` on a transfer image outside 2048 per axis | `r3v_native_image.c` lines 81-85 | refused | `R3V_NATIVE_TRANSFER_DIMENSION_MAX` is `2048` (`r3v_native.h` line 815) |
| `vkCreateImage` with mixed color-attachment and transfer usage | `r3v_native_image.c` lines 73-90 | refused | color-attachment branch requires `usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` exactly; the transfer branch requires `usage & ~(TRANSFER_SRC\|TRANSFER_DST) == 0`; every other usage combination falls to the `else` refusal at line 90 |
| `vkCreateImage` non-2D, non-`LINEAR`, non-exclusive, or with an unrecognized `flags` bit | `r3v_native_image.c` lines 59-67 | refused | same function, first guard clause; `VK_IMAGE_CREATE_ALIAS_BIT` is the one admitted flag bit, and only the transfer branch keeps an aliased image (the color-attachment branch re-refuses any nonzero `flags` at line 74) |
| sync export (external semaphore/fence) | `r3v_physical_device.h` lines 57-61 (`sync_types[3]`) | absent | comment: "r3v uses a CPU-side binary sync; the radeon DRM driver does not support DRM_CAP_SYNCOBJ"; two slots only, no external sync type |

A swapchain-shaped presentable image (any extent an application picks, a
transfer-capable usage set for the present blit, potentially non-`LINEAR`
tiling for scanout) is refused by `r3v_CreateImage`'s own admission gate
before any WSI-specific callback executes. The refusal is the same gate that
protects the qualified render-target and transfer families from every other
image shape; it carries no separate WSI-aware exception.

## Per-host-class dEQP applicability

`tests/r3v_conformance_partition.tsv` assigns two orders to WSI groups, both
requiring `display` hazard clearance and `silicon` evidence:

| order | slice | dEQP groups | requires |
|---|---|---|---|
| 11 | `wsi` | `dEQP-VK.wsi.xcb.surface`, `dEQP-VK.wsi.xlib.surface` | `KHR_surface` + `KHR_xcb_surface`/`KHR_xlib_surface` (both advertised) |
| 18 | `wsi-presentation` | every `dEQP-VK.wsi.*` group beyond surface query, including `.swapchain`, `.headless`, `.wayland`, `.display*` | `KHR_swapchain` (absent) and, per sub-group, `EXT_headless_surface`/Wayland/display extensions (all absent) |

`tests/r3v_native_wsi_surface_contract.c` states its own boundary in its file
comment (lines 4-5): "surface construction and surface queries at instance
scope, with no presentation capability at device scope." Its registration in
`meson.build` (lines 1467, 1503-1512) is gated on `dep_xcb.found() and
prog_xvfb.found()`; a host missing either `libxcb` development headers or
`Xvfb` registers zero test binaries from this file, not a skip result.

| host class | slice 11 (`wsi`) | slice 18 (`wsi-presentation`) | surface-contract test registration |
|---|---|---|---|
| X11/KMS host with `libxcb` + `Xvfb` | reachable (extensions present) | zero cases execute (`KHR_swapchain` absent) | registered |
| X11/KMS host missing `libxcb` or `Xvfb` | not registered | not registered | not registered |
| headless host (no X11) | not applicable (no `xcb`/`xlib` surface) | zero cases execute; `EXT_headless_surface` is not advertised, so `dEQP-VK.wsi.headless` is also unreachable | not registered |

Every host class executes zero `wsi-presentation` cases today. The
denominator is the extension gap, not the host: adding `EXT_headless_surface`
alone would not open slice 18, because the group's non-headless cases still
need `KHR_swapchain`, which is a device-level extension with no code path in
this ICD.

## Retained checks that pin the denominator

Each item names its mechanism, the retained check that landed for it, and
the falsifier that check runs against itself. None of these change the
presentation surface; each pins the denominator itself so a future
extension can be measured against a known baseline instead of an assumed
one. Every check registers in `suite : ['r3v']` beside the audits this
document's earlier sections cite.

### Swapchain-extension-absence audit

`tests/r3v_native_swapchain_absence_audit.py` reads the device extension
table (`r3v_native_device_extensions_supported`), the advertised-surface
binding ledger (`tests/r3v_advertised_surface_deqp_binding.tsv`), and the
built ICD's own symbol table (`nm libvulkan_r3v.so`) together, and fails the
moment any one of the three carries a `KHR_swapchain` signal -- an
advertised extension bit, a binding row, or a defined `r3v_CreateSwapchainKHR`
route -- the other two do not corroborate, since this document's
driver-callback table names no route for the extension at all.

Falsifier, exercised by `--selftest` and confirmed against the real tree: the
audit passes on the current tree (no `KHR_swapchain` signal anywhere) and
fails when a synthetic device-extension-table edit adds `.KHR_swapchain =
true` with no matching binding row or defined `r3v_CreateSwapchainKHR`
symbol in `nm`.

### WSI-init comment/code parity

`tests/r3v_wsi_init_comment_parity_audit.py` locates the comment
immediately above `r3v_init_wsi`, the `wsi_device` field declaration, and
the `KHR_surface` extension entry, and fails when any of the three names
"software mode" or "lavapipe pattern" in a file that nowhere names
`R3V_WSI_SW`, so a future edit cannot reintroduce the
default-value/comment mismatch named in Default present route above.

Falsifier, exercised by `--selftest` and confirmed against the real tree:
the check fails against a synthetic reintroduction of the original wording
at any of the three sites (with the `R3V_WSI_SW` guard removed from that
site's file), and passes on the corrected tree.

### Swapchain-image-shape refusal pins

`tests/r3v_native_transfer_ops_test.c`'s `check_optimal_tiling` now
constructs a `VkImageCreateInfo` shaped like a typical swapchain image
(`TRANSFER_DST | COLOR_ATTACHMENT` usage, `VK_IMAGE_TILING_OPTIMAL`, a
128x128 extent above the render family's 64x64 ceiling) and asserts
`R3V_NATIVE_REFUSAL_RESULT`, pinning the refusal path this document's
driver-callback table documents so it survives future image-admission
refactors.

Falsifier, confirmed on drm-shim silicon-shape hardware evidence: widening
`r3v_CreateImage`'s admission gate to accept the color-attachment usage bit
inside the transfer branch's usage mask makes the new case return
`VK_SUCCESS` and the test fail; reverting the gate restores the pass. Until
a future change makes an explicit WSI decision, the case passes and
documents the boundary.

### WSI-test registration-gate audit

`tests/r3v_wsi_registration_gate_audit.py` parses `meson.build` as text --
no configured build required -- tracks its `if`/`foreach` nesting, and
asserts that the `r3v_native_wsi_surface_contract` target and its
associated `r3v-xvfb-wrapper-passthrough-*`/`-infrastructure-refusal` tests
sit inside the `if dep_xcb.found() and prog_xvfb.found()` block, while
`r3v-xvfb-wrapper-signal-cleanup` (which exercises the wrapper script's own
signal-cleanup logic against plain `true`/`false` commands, needing neither
`libxcb` nor a real `Xvfb` display) sits outside it, closing the "registers
nothing" gap this document's per-host-class table names as distinct from a
run-time skip.

Falsifier, exercised by `--selftest` and confirmed against the real tree: a
mutated `meson.build` that drops the `if dep_xcb.found() and
prog_xvfb.found()`/`endif` pair while keeping the body makes the audit fail,
because none of the tracked `if`-blocks names both guard tokens any more.
The audit catches a dropped guard and a misplaced unconditional test by the
same line-range mechanism, and it passes again once the guard is restored.

## Dropped claims

None. Every claim in this document was checked against the tree at the
commit this revision was written against
(`git log -1 --format=%H -- docs/hardware/r3v-wsi-denominator.md`): the
extension tables, the `nm`/`ldd` symbol evidence, the `r3v_CreateImage`
refusal gate and its exact guard lines, the partition and
advertised-surface TSV rows, the surface-contract test's own file comment,
and its `meson.build` registration gate all hold as stated.
