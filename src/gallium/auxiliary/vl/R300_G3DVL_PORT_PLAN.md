# Port plan: restore the g3dvl shader video decoder for r300 gallium-va (mesa 26)

The shader-based g3dvl decode kernels were removed from mesa 26 (only
`vl_mpeg12_bitstream` and `vl_vertex_buffers` remain).  RS482/R300-class has no
UVD/VCE, so shader decode is the only HW-accelerated video path; this branch
restores the removed kernels and re-wires r300's gallium-va backend.  It is a kept
PORT BRANCH and is NOT merged to main until it builds and passes the kernel
compile-verify.  Decomposition and admissibility proofs:
`docs/.../rs482-mpeg2-decode-verb-decomposition.md`,
`rs482-h264-mpeg4-decode-verb-factoring.md`, the formal monograph.

## Mined kernels (restored into src/gallium/auxiliary/vl/)

`vl_idct.{c,h}` (868L), `vl_mc.{c,h}` (658L), `vl_zscan.{c,h}` (615L),
`vl_matrix_filter.{c,h}` (307L), `vl_mpeg12_decoder.{c,h}` (1239L),
`vl_decoder.{c,h}` (94L) -- from mesa-21.3.3 (the last pre-removal tag).

## Drift assessment (tractable)

Present in mesa 26 (no port needed): `tgsi_ureg.h` (the shader-gen API the kernels
use), `pipe/p_video_codec.h`, `pipe/p_video_enums.h`, and the shared vl headers
`vl_defines.h`, `vl_types.h`, `vl_video_buffer.h`.

Known drift to fix:
1. meson wiring -- re-add the decode sources to `src/gallium/auxiliary/meson.build`
   under the video-frontend conditional (in 21.3.3 they sat with vl_decoder.c at
   the lines that also list vl_idct/vl_mc/vl_zscan/vl_matrix_filter/
   vl_mpeg12_decoder).
2. `vl_mc.c:89` uses `screen->get_param(screen, PIPE_CAP_TGSI_FS_POSITION_IS_SYSVAL)`
   -- that PIPE_CAP was removed in mesa 26 (FS position is sysval via NIR now).
   Fix: drop the conditional and take the sysval path unconditionally (or the
   mesa-26 caps equivalent), since modern r300 NIR emits position as a sysval.
3. `screen->get_param`/`get_video_param` signatures: mesa migrated PIPE_CAP to the
   `pipe_caps` struct (~mesa 23).  The kernels query few caps; update those call
   sites to the caps-struct accessors the build flags.
4. r300 backend -- re-add to `src/gallium/drivers/r300/r300_screen.c` (from 21.3.3
   r300_screen.c:437 r300_get_video_param + :732 `screen.get_video_param =
   r300_get_video_param` + :734 `screen.is_video_format_supported =
   vl_video_buffer_is_format_supported`), updated to the mesa 26 screen vtable.
5. enable `gallium-va` (and/or `vdpau`) + `video-codecs=mpeg2` in the build
   profile; the va target needs libva headers.

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
