# Port plan: restore the g3dvl shader video decoder for r300 gallium-va (mesa 26)

mesa 26 removed only the shader-based g3dvl DECODE kernels (`vl_idct`, `vl_mc`,
`vl_zscan`, `vl_matrix_filter`, `vl_mpeg12_decoder`, `vl_decoder`).  The
PRESENTATION half survives: `vl_compositor{,_cs,_gfx}.c` (gfx and compute paths),
`vl_csc.c`, `vl_deint_filter{,_cs}.c`, `vl_codec.c` (a new support-dispatch),
`vl_video_buffer.c`, `vl_mpeg12_bitstream.c`, `vl_vertex_buffers.c`.  RS482/R300
has no UVD/VCE, so shader decode is the only HW-accelerated video path; this
branch restores the removed decode kernels and re-wires r300's gallium-va backend.
It is a kept PORT BRANCH, NOT merged to main until it builds and passes the kernel
compile-verify.  Decomposition and admissibility proofs:
`docs/.../rs482-mpeg2-decode-verb-decomposition.md`,
`rs482-h264-mpeg4-decode-verb-factoring.md`, the formal monograph.

## Mined kernels (restored into src/gallium/auxiliary/vl/)

`vl_idct.{c,h}` (868L), `vl_mc.{c,h}` (658L), `vl_zscan.{c,h}` (615L),
`vl_matrix_filter.{c,h}` (307L), `vl_mpeg12_decoder.{c,h}` (1239L) -- from
mesa-21.3.3 (the last pre-removal tag).

`vl_decoder.{c,h}` was also restored but then DELETED: its `vl_create_decoder`
dispatch + `vl_profile_supported`/`vl_level_supported` are superseded by mesa-26's
`vl_codec.c` (`vl_codec_supported`, gated on the `VIDEO_CODEC_*` build defines) and
by the driver instantiating the codec directly through its `create_video_codec`
vtable hook + `vl_create_mpeg12_decoder()`.  `vl_create_decoder` has zero callers
in mesa 26.

## Drift assessment (tractable)

Present in mesa 26 (no port needed): `tgsi_ureg.h` (the shader-gen API the kernels
use), `pipe/p_video_codec.h`, `pipe/p_video_enums.h`, and the shared vl headers
`vl_defines.h`, `vl_types.h`, `vl_video_buffer.h`.

## Empirical drift inventory (clang-22 against mesa 26, build oracle)

Wiring done: meson lists the 5 decode kernels in `files_libgalliumvl` (an
unconditional `files()` list -- compiled whenever libgalliumvl builds, i.e. when a
video frontend is enabled); `_va_drivers` includes `with_gallium_r300`; the build
profiles default `video-codecs = ['mpeg12dec']` + `gallium-va = 'enabled'`.  libva
1.23.0 is found.  The removed `pipe/p_compiler.h` include in `vl_zscan.h` is
replaced by `util/compiler.h`.

Remaining drift building `libgalliumvl.a` (~86 errors, 2 files truncated at
`-ferror-limit`, so the true count is higher).  Classes, fix in this order:

1. Mechanical enum renames (safe, high volume): `PIPE_SHADER_{VERTEX,FRAGMENT}` ->
   `MESA_SHADER_*`; `PIPE_PRIM_*` -> `MESA_PRIM_*`; plus a few fully-removed
   identifiers with no compiler suggestion (TGSI/PIPE_CAP leftovers).
2. Struct-field refactors: `pipe_surface` lost `width`/`height` (derive via
   `u_minify(tex->width0/height0, level)` or the new accessor); `pipe_surface` is
   now passed by pointer at several call sites (the "dereference with *" errors);
   `pipe_sampler_state.normalized_coords` removed (mesa-26 sampler-state shape).
3. Vtable arity changes (DANGEROUS -- a clean compile here can be wrong at
   runtime): `set_sampler_views`, `set_constant_buffer`, `draw_vbo`, and similar
   `pipe_context` hooks changed argument counts/shapes.  MUST be matched to the new
   API contract by reading a live caller (`vl_compositor_gfx.c`, radeonsi) -- NOT
   by adding arguments to silence clang.  The only runtime check is hazard-gated on
   vostro, so a wrong arity sits undetected.

## Milestones (a clean libgalliumvl.a is milestone 1 of 3)

1. Decode kernels compile: `libgalliumvl.a` builds clean (the drift above).
2. r300 backend wiring: add `r300_get_video_param` + `screen.get_video_param` +
   `screen.is_video_format_supported = vl_video_buffer_is_format_supported` +
   `context.create_video_codec` (-> `vl_create_mpeg12_decoder`) to the mesa-26
   r300 screen/context vtables (21.3.3 `r300_screen.c:437/732/734` as reference,
   updated to the new vtable shapes).
3. Hazard-gated runtime: `vainfo` shows the MPEG-2 profile, then end-to-end decode
   of a clip on vostro (tasks 14/28), NIR->RC budget compile-verify (task 27).

Only after milestone 1 builds clean does this branch become a merge candidate; the
backend (milestone 2) makes va advertise a profile, and milestone 3 is "it plays."

## Remaining-class API contracts (mesa 26, verified against source)

Done so far on this branch: superseded `vl_decoder.c` deleted; `p_compiler.h` ->
`util/compiler.h`; `PIPE_SHADER_{VERTEX,FRAGMENT}` -> `MESA_SHADER_*`, `PIPE_PRIM_*`
-> `MESA_PRIM_*`.  Drift 86 -> 56.  Remaining classes, with the exact mesa-26 idiom:

- `pipe_surface` was rebuilt as a lightweight VALUE (p_state.h:411): fields are
  `format:16`, `nr_samples:16`, `first_layer:16`, `last_layer:16`, `level`,
  `texture` -- no `width`, `height`, or `.u` union.  Framebuffer state now embeds
  `struct pipe_surface cbufs[]` BY VALUE, not by pointer.
  * `surf->width` / `surf->height` -> `pipe_surface_size(surf, &w, &h)`
    (`util/u_inlines.h:403`; signature `(const struct pipe_surface *, unsigned *w,
    unsigned *h)`), or `u_minify(surf->texture->width0/height0, surf->level)`.
  * `surf->u.tex.level/first_layer/last_layer` -> `surf->level/first_layer/
    last_layer` (union flattened).  MUST be edited per site, NOT sed'd: a blind
    `.u.tex.` rewrite would corrupt `pipe_sampler_view`, which KEEPS its `.u.tex`.
  * `pipe_context.create_surface` is gone (the "no member create_surface" +
    "assigning to pipe_surface from pipe_surface*; dereference with *" errors): the
    kernels' render-target setup must move to the value-embedded framebuffer model
    that `vl_compositor_gfx.c` uses (build `pipe_surface` inline, assign into
    `fb_state.cbufs[i]` by value).  Deepest sub-fix; do per kernel.
- `pipe_sampler_state.normalized_coords` -> `unnormalized_coords` (p_state.h:468,
  INVERTED sense): `normalized_coords = 1` -> drop (default 0); `= 0` ->
  `unnormalized_coords = 1`.  Check each site's sense.
- `pipe_screen.get_param`/`get_shader_param` -> the `pipe_caps`/`shader_caps`
  structs (~mesa 23): `screen->get_param(s, PIPE_CAP_MAX_RENDER_TARGETS)` ->
  `screen->caps.max_render_targets` (p_defines.h:1075); `PIPE_SHADER_CAP_MAX_
  INSTRUCTIONS` -> `screen->shader_caps[MESA_SHADER_*].max_instructions`;
  `PIPE_CAP_TGSI_FS_POSITION_IS_SYSVAL` was REMOVED -- FS position is always a
  sysval now, so take that branch unconditionally and delete the query.
- Arity class (DANGEROUS -- compiles wrong, runtime-only check, hazard-gated on
  vostro): `pipe_context` hooks `set_sampler_views`, `set_constant_buffer`,
  `draw_vbo` changed shape.  Match each to a LIVE caller (`vl_compositor_gfx.c`,
  radeonsi `si_*`), not by counting args.  Do LAST, one site at a time, each
  against its reference caller.

## Build / iterate / verify order

1. Wire meson; build vl/ kernels (video enabled) under the canonical clang-22
   ccache+distcc path; fix the surfaced drift errors iteratively (expect items
   2-3 plus a few ureg/enum renames the compiler flags).
2. Re-add the r300 backend (item 4); build the r300 driver with va.
3. Compile-verify the IDCT/MC kernels reach the r300 RC within the
   64-ALU/32-TEX budget (task 27; the monograph's L2.1/B2/B4 bounds).
4. Merge to main only once it builds clean.
5. Pull on cachyos-vostro1000; rebuild via build-infra
   `1_r300_full_release_x86_64v1-clang22-distcc-cache` (and/or the -debug profile
   to /opt/user); install via PKGBUILD; probe MPEG-2 decode hazard-gated
   (tasks 14/28) against the real system dirs.

## Scope

MPEG-2 main first (I-frame-only is the minimal viable path, then P/B); then
MPEG-4 ASP (reuses the kernels + quarter-pel); H.264 needs the additional 6-tap
luma + integer transform + intra-wavefront work (separate, larger). "Even
minimal, it plays."
