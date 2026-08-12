# r300 full profile option boundary

The active full r300 profiles are:

- `4_r300_full_release_x86_64v1-clang22-distcc-cache`
- `3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache`

Both profiles build the same core API surface:

- Gallium driver: `r300`
- Vulkan driver: `ati_r300`
- OpenGL: desktop GL, GLES1, GLES2
- Window systems and loader surfaces: GLVND, GLX DRI, EGL, GBM
- Platforms: X11, Wayland, surfaceless, DRM, XCB
- Video and presentation: r300 Gallium-VA MPEG-1/MPEG-2 decode and the restored
  Gallium-XA 2D/Xv frontend
- Tooling: r300 tools, shader cache, Gallium HUD/lm-sensors

Both profiles keep software rasterizers and non-r300 hardware drivers out:

- `llvm = 'disabled'`
- `draw-use-llvm = false`
- no `swrast`, `llvmpipe`, `softpipe`, r600, radeonsi, or terakan
- no Rusticl, D3D10 UMD, MediaFoundation, or unrelated video codecs

Zink is a deliberate debug-profile exception, not a shared exclusion:

- debug config: `gallium-drivers = ['r300', 'zink']` so zink can ride the
  `ati_r300` ICD as the zink-upon-r3v GL-on-Vulkan experiment lane
- release config: `gallium-drivers = ['r300']` with no zink

The release profile uses `buildtype = 'release'`, `b_ndebug = 'true'`, `-O2`,
and `build-tests = true` for the Vostro runtime lane.  The canonical
`rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache` target configures and
builds this profile, then runs the registered in-tree unit tests through the
locked `make test` target.  A separate `make test` invocation reruns the tests
without rebuilding.  Those results are build/test evidence and do not establish
hardware behavior or CTS/Piglit/dEQP conformance.

The debug profile uses `buildtype = 'debugoptimized'`, `b_ndebug = 'false'`, and
the same x86-64-v1 ISA/linker policy with frame pointers for RCA.  It also sets
`build-tests = true`; debug-only instrumentation and tooling include Valgrind
annotations, libunwind, Perfetto CPU-side timeline hooks, standalone compiler
tools, and the mesa overlay/device-select Vulkan layers.  The release profile
keeps those debug-only surfaces out of the artifact.

The release and debug profiles intentionally keep `llvm = 'disabled'` and
`draw-use-llvm = false` until the no-LLVM RS482 SW-TCL baseline has a retained
measurement.  A Vostro experiment branch named
`r300/rs482-swtcl-draw-jit-release` demonstrated the alternate profile shape
(`llvm = 'enabled'`, `draw-use-llvm = true`) for Gallium draw JIT throughput, but
that branch is an after-baseline performance lane.  It is not the canonical
release profile because draw LLVM changes the SW-TCL execution substrate being
measured.

`egl-native-platform = 'surfaceless'` makes `EGL_DEFAULT_DISPLAY` use the
surfaceless platform, the renderD128 headless path the Piglit/dEQP lane runs
on; X11 and Wayland callers select their platform explicitly. Upstream Mesa
commit b0050c4e754 ("meson: drop misleading `-D egl-native-platform` values")
removed `'drm'`, `'wayland'`, `'windows'`, and `'macos'` from this option's
choices, keeping only the values EGL_DEFAULT_DISPLAY can actually honor.

## The two profiles are the whole r300 lane

The debug profile
`3_r300_full_debug_optimized_*`
(`build-infra/configs/`)
and the release profile
`4_r300_full_release_*`
(`build-infra/configs/alternates/`)
are the canonical r300 pair.
`build-infra/configs/alternates/` also carries the asan `1_` and -O0 `2_`
debug *variants* of the same r300 surface; they are not the primary pair.
The primary pair remains release `4_` and assertions-live debug `3_`.

Every prior r300/vostro variant is removed and subsumed: the GL-only
`r300-canonical-vostro-*`, the Vulkan-ICD-only `r3v-vostro-*` (`opengl=false`),
the `*-vostro-k8-*` Turion-native pair, and the
`r300-{trace,egl-gbm-trace}-vostro-k8` capture variants.  One artifact now
carries the full GL/GLES surface and the ati_r300 ICD, so r300 conformance,
desktop, r3v RCA, and silicon evidence all build from the same standardized
pair: release `4_`, assertions-live debug `3_`.  The x86-64-v1 psABI baseline is
a safe subset of the Turion 64 X2, so the removed k8-native lane added no reach
the generic baseline lacks on this hardware.

## Build placement and artifact hygiene

Builds land out-of-tree.  The `build-infra/Makefile` resolves
`BUILDDIR ?= $(CURDIR)/../../build/mesa-<profile>`, i.e. a sibling
`build/mesa-<profile>/` directory outside the repository working tree, so a
build never appears in `git status`.  Each profile maps to one install
prefix: release builds to `/opt/local/mesa-26-gororoba`, debug builds to
`/opt/local/mesa-gororoba-debug-optimized`; neither prefix is inside the repo and
system Mesa at `/usr/lib` is left untouched by these `/opt/local` profiles.

The package-managed release PKGBUILD uses `/opt/mesa-gororoba` as the FHS
canonical add-on prefix and ships compatibility aliases for older local scripts:
`/opt/local/mesa-26-gororoba` and `/opt/share/mesa-26-gororoba` both point at
that prefix, while `/usr/share/mesa-26-gororoba` points at the same canonical
prefix for older script compatibility.  The debug package follows the same
pattern at `/opt/mesa-gororoba-debug-optimized`.  Use
`mesa-gororoba-run <probe>` or `mesa-gororoba-debug-optimized-run <probe>` to select
the side-by-side driver for one command without replacing stock Mesa.

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

Older Mesa branches exposed additional Gallium frontends.  `gallium-xa` has
since been restored in this fork for the r300 lane; the remaining rows are still
branch-archeology items, not missing toggles in the current profile.

| option | present in old release option files | current status |
|---|---|---|
| `gallium-nine` | Mesa 22.3, 23.3, 24.3 | absent by Mesa 25.3 and current Mesa 26 |
| `gallium-xa` | Mesa 22.3, 23.3, 24.3 | restored in this fork for r300 2D/Xv |
| `gallium-vdpau` | Mesa 22.3, 23.3, 24.3 | absent by Mesa 25.3 and current Mesa 26 |
| `gallium-omx` | Mesa 22.3, 23.3 | absent by Mesa 24.3, 25.3, and current Mesa 26 |

Restoring any remaining frontend means carrying old state trackers forward as
source code, not adding Meson options to this build profile.

`osmesa` still exists in older releases, but this r300 profile does not enable
it because the lane is intentionally no-swrast/no-llvmpipe.  If a future
hardware-backed offscreen OSMesa experiment is needed, it should be a separate
profile and evidence lane.
