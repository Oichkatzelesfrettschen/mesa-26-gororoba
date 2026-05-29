# r300 full profile option boundary

The active full r300 profiles are:

- `1_r300_full_release_x86_64v1-clang22-distcc-cache`
- `2_r300_full_debug_x86_64v1-clang22-distcc-cache`

Both profiles build the same API surface:

- Gallium driver: `r300`
- Vulkan driver: `amd_r300`
- OpenGL: desktop GL, GLES1, GLES2
- Window systems and loader surfaces: GLVND, GLX DRI, EGL, GBM
- Platforms: X11, Wayland, surfaceless, DRM, XCB
- Tooling: r300 tools, shader cache, Gallium HUD/lm-sensors

Both profiles deliberately exclude software and unrelated frontends:

- `llvm = 'disabled'`
- `draw-use-llvm = false`
- no `swrast`, `llvmpipe`, `softpipe`, zink, r600, radeonsi, or terakan
- no Rusticl, D3D10 UMD, VA, MediaFoundation, video codecs, Vulkan layers,
  perfetto, libunwind, or valgrind

The release profile uses `buildtype = 'release'`, `b_ndebug = 'true'`, and
`-Os` for the space-constrained Vostro runtime lane.  The debug profile uses
`buildtype = 'debugoptimized'`, `b_ndebug = 'false'`, and the same x86-64-v1
ISA/linker policy without release-only `-Os` in the native-file arguments.

`egl-native-platform = 'drm'` makes `EGL_DEFAULT_DISPLAY` use the DRM native
platform, the renderD128 headless path the Piglit/deqp lane runs on; X11 and
Wayland callers select their platform explicitly.

## The two profiles are the whole r300 lane

`1_`/`2_` are the only r300 build profiles.  Every prior r300/vostro variant is
removed and subsumed: the GL-only `r300-canonical-vostro-*`, the Vulkan-ICD-only
`r300vk-vostro-*` (`opengl=false`), the `*-vostro-k8-*` Turion-native pair, and
the `r300-{trace,egl-gbm-trace}-vostro-k8` capture variants.  One artifact now
carries the full GL/GLES surface and the amd_r300 ICD, so r300 conformance,
desktop, r300vk RCA, and silicon evidence all build from the same standardized
pair: release `1_`, assertions-live debug `2_`.  The x86-64-v1 psABI baseline is
a safe subset of the Turion 64 X2, so the removed k8-native lane added no reach
the generic baseline lacks on this hardware.

## Build placement and artifact hygiene

Builds land out-of-tree.  The `build-infra/Makefile` resolves
`BUILDDIR ?= $(CURDIR)/../../build/mesa-<profile>`, i.e.
`~/workspaces/mesa/build/mesa-<profile>/` -- outside the repository working
tree -- so a build can never appear in `git status`.  Each profile maps to one
install prefix: release builds to `/opt/local/mesa-26-gororoba`, debug builds to
`/opt/local/mesa-26-gororoba-debug`; neither prefix is inside the repo and
system Mesa at `/usr/lib` is left untouched by these `/opt/local` profiles.

Concurrent builds serialize through a lock: the Makefile wraps `ninja` in
`flock -x -w 7200 $(HOME)/.cache/mesa-26-gororoba/mesa-build.lock`, so a second
`make build` waits (up to two hours) for an in-flight build instead of
colliding on the cache.  Check whether a build is active with
`flock -n -x ~/.cache/mesa-26-gororoba/mesa-build.lock true && echo free || echo busy`.

`.gitignore` guards the in-tree artifacts a hand-run build can drop: `/build`
and the documented `/builddir`/`/builddir-*` Meson directories, the clangd
`compile_commands.json` database, the GNU-global/clangd index files
(`GTAGS`/`tags`/etc.), and `build-infra/shadercache.bin`.

## Historical options

Older Mesa branches exposed additional Gallium frontends that are not available
in the current Mesa 26 tree:

| option | present in old release option files | current status |
|---|---|---|
| `gallium-nine` | Mesa 22.3, 23.3, 24.3 | absent by Mesa 25.3 and current Mesa 26 |
| `gallium-xa` | Mesa 22.3, 23.3, 24.3 | absent by Mesa 25.3 and current Mesa 26 |
| `gallium-vdpau` | Mesa 22.3, 23.3, 24.3 | absent by Mesa 25.3 and current Mesa 26 |
| `gallium-omx` | Mesa 22.3, 23.3 | absent by Mesa 24.3, 25.3, and current Mesa 26 |

Those are branch-archeology items, not missing toggles in the current profile.
Restoring them would mean carrying old state trackers forward as source code,
not adding Meson options to this build profile.

`osmesa` still exists in older releases, but this r300 profile does not enable
it because the lane is intentionally no-swrast/no-llvmpipe.  If a future
hardware-backed offscreen OSMesa experiment is needed, it should be a separate
profile and evidence lane.
