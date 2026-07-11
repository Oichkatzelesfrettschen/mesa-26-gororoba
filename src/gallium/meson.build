# Copyright © 2017 Dylan Baker
# Copyright © 2017-2018 Intel Corporation
# SPDX-License-Identifier: MIT

sym_config = configuration_data()

foreach d : [[with_gallium_r300 or with_gallium_radeonsi or with_gallium_r600, 'radeon_drm_winsys_create'],
             [with_gallium_radeonsi, 'amdgpu_winsys_create'],
             [with_gallium_nouveau, 'nouveau_drm_screen_create'],
             [with_gallium_freedreno, 'fd_drm_screen_create_renderonly'],
             [amd_with_llvm and with_gallium_radeonsi, 'ac_init_shared_llvm_once']]
  if d[0]
    sym_config.set(d[1], d[1] + ';')
  else
    sym_config.set(d[1], '')
  endif
endforeach

if with_gallium_va
  sym_config.set('va_driver_init', '__vaDriverInit_*_*;')
else
  sym_config.set('va_driver_init', '')
endif

inc_gallium_drivers = include_directories('drivers')
inc_gallium_winsys = include_directories('winsys')
inc_gallium_winsys_sw = include_directories('winsys/sw')

subdir('auxiliary')
subdir('auxiliary/pipe-loader')
subdir('winsys/sw/null')
if with_dri
  subdir('winsys/sw/dri')
else
  libswdri = []
endif
if with_gallium_drisw_kms
  subdir('winsys/sw/kms-dri')
else
  libswkmsdri = []
endif
if with_platform_windows
  subdir('winsys/sw/gdi')
else
  libwsgdi = null_dep
endif
subdir('winsys/sw/wrapper')
if with_platform_haiku
  subdir('winsys/sw/hgl')
endif
if with_gallium_softpipe
  subdir('drivers/softpipe')
else
  driver_softpipe = declare_dependency()
endif
if with_any_llvmpipe
  subdir('drivers/llvmpipe')
else
  driver_llvmpipe = declare_dependency()
endif
if with_any_llvmpipe and with_gallium_softpipe
  driver_swrast = declare_dependency(
    dependencies : [ driver_softpipe, driver_llvmpipe ],
  )
elif with_any_llvmpipe
  driver_swrast = driver_llvmpipe
elif with_gallium_softpipe
  driver_swrast = driver_softpipe
else
  driver_swrast = declare_dependency()
endif

# Render-to-texture verification of the H.264 4x4 inverse-transform fragment
# programs (vl_h264_idct.c) on a software screen.  Placed here, after the sw
# rasterizer is in scope: it creates a softpipe screen over the null winsys
# directly (no pipe loader, which would drag in the full driver matrix), links
# the video library for the shader builders, and libgallium for the
# CSO/draw/readback path.
if with_tests and with_gallium_softpipe
  test('vl-h264-idct',
    executable(
      'vl_h264_idct_test',
      'auxiliary/vl/tests/vl_h264_idct_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # Render-to-texture verification of the H.264 luma half-pel motion-compensation
  # fragment programs (vl_h264_mc.c): renders the horizontal and vertical six-tap
  # planes on a software screen and checks each against the integer spec.
  test('vl-h264-mc',
    executable(
      'vl_h264_mc_test',
      'auxiliary/vl/tests/vl_h264_mc_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # Render-to-texture verification of the H.264 chroma eighth-pel bilinear
  # motion-compensation fragment program (vl_h264_chroma.c): renders the
  # interpolated plane per fraction and checks each against the integer spec.
  test('vl-h264-chroma',
    executable(
      'vl_h264_chroma_test',
      'auxiliary/vl/tests/vl_h264_chroma_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # Render-to-texture verification of the H.264 normal luma deblock fragment
  # program (vl_h264_deblock.c): filters a strip of edge samples and checks the
  # four modified samples against the integer spec across the threshold grid.
  test('vl-h264-deblock',
    executable(
      'vl_h264_deblock_test',
      'auxiliary/vl/tests/vl_h264_deblock_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # Render-to-texture verification of the H.264 sample-reconstruction fragment
  # program (vl_h264_reconstruct.c): Clip1(prediction + residual) over both
  # planes, checked against the integer spec including the clip on both ends.
  test('vl-h264-reconstruct',
    executable(
      'vl_h264_reconstruct_test',
      'auxiliary/vl/tests/vl_h264_reconstruct_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # Render-to-texture verification of the whole-plane H.264 inverse-transform
  # fragment programs (vl_h264_idct_create_plane_row_fs / _plane_col_fs):
  # transforms a plane of tiled 4x4 blocks in two draws and checks every block
  # against the integer spec, for both the 16x16 luma and 8x8 chroma layouts.
  test('vl-h264-idct-plane',
    executable(
      'vl_h264_idct_plane_test',
      'auxiliary/vl/tests/vl_h264_idct_plane_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # End-to-end verification of the H.264 luma inter-reconstruction orchestrator
  # (vl_h264_emit.c): runs the scatter, per-macroblock motion compensation,
  # whole-frame inverse transform, and Clip1 reconstruction into a target plane
  # and checks every sample against the integer motion-compensation + transform
  # + reconstruction spec.
  test('vl-h264-emit',
    executable(
      'vl_h264_emit_test',
      'auxiliary/vl/tests/vl_h264_emit_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # End-to-end I+P luma decode on softpipe: entropy-decode the IDR and P slices,
  # reconstruct the reference frame (CPU intra), reconstruct the P frame (inter via
  # the back half, intra via CPU), and match ffmpeg.
  test('vl-h264-inter-e2e',
    executable(
      'vl_h264_inter_e2e_test',
      'auxiliary/vl/tests/vl_h264_inter_e2e_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    args : [join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_idr_slice.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_pframe1_slice.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_sps.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_pps.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_pframe1_ref.yuv')],
    suite: 'gallium',
  )

  # End-to-end I+P chroma decode on softpipe: the chroma companion to the luma
  # end-to-end test (bilinear motion compensation, so no FP24 gap).
  test('vl-h264-inter-chroma-e2e',
    executable(
      'vl_h264_inter_chroma_e2e_test',
      'auxiliary/vl/tests/vl_h264_inter_chroma_e2e_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    args : [join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_idr_slice.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_pframe1_slice.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_sps.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_pps.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_pframe1_ref.yuv')],
    suite: 'gallium',
  )

  # End-to-end verification of the H.264 decoder through its pipe_video_codec
  # vtable: drives begin_frame / decode_bitstream / end_frame with the replay
  # provider feeding a serialized inter-macroblock contract, reconstructs into a
  # real video buffer, and checks the target Y against the integer spec.
  test('vl-h264-decode',
    executable(
      'vl_h264_decode_test',
      'auxiliary/vl/tests/vl_h264_decode_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # The real decoder path for an IDR: drive begin_frame / decode_bitstream /
  # end_frame through the vtable with the Mesa CAVLC provider and no reference, so
  # end_frame reconstructs the whole intra frame on the CPU; match ffmpeg.
  test('vl-h264-decode-idr',
    executable(
      'vl_h264_decode_idr_test',
      'auxiliary/vl/tests/vl_h264_decode_idr_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    args : [join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_idr_slice.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_sps.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_pps.bin'),
            join_paths(meson.current_source_dir(), 'auxiliary/vl/tests',
                       'h264_cb_idr_ref.yuv')],
    suite: 'gallium',
  )

  # File-driven whole-frame H.264 inter reconstruction: reads a serialized slice
  # contract built from the libavcodec inter oracle and an MB-aligned reference
  # frame, runs the decode vtable through the replay provider, and writes the
  # reconstructed frame for an external diff against the ffmpeg decode.  Skips
  # (exit 77) unless its R300_H264_FRAME_* environment is set.
  test('vl-h264-decode-frame',
    executable(
      'vl_h264_decode_frame_test',
      'auxiliary/vl/tests/vl_h264_decode_frame_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # Verification of the H.264 chroma inter-reconstruction core
  # (vl_h264_emit_chroma_inter): scatter, eighth-pel bilinear motion
  # compensation, inverse transform, and Clip1 reconstruction over half-resolution
  # chroma planes, checked against the integer spec.
  test('vl-h264-emit-chroma',
    executable(
      'vl_h264_emit_chroma_test',
      'auxiliary/vl/tests/vl_h264_emit_chroma_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # Verification of the H.264 luma in-loop deblock orchestrator
  # (vl_h264_emit_deblock_luma): the three vertical then three horizontal internal
  # block edges of every macroblock, normal filter, boundary strength derived from
  # the contract, run as a six-pass ping-pong; checked sample-for-sample against an
  # independent integer reimplementation of the same internal-edge filter.
  test('vl-h264-emit-deblock',
    executable(
      'vl_h264_emit_deblock_test',
      'auxiliary/vl/tests/vl_h264_emit_deblock_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers'),
                             include_directories('auxiliary/vl')],
      link_with : [libgalliumvl, libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )

  # Dual-factory parity oracle for the direct-NIR draw vertex executor
  # (draw/draw_vs_nir.c): builds each test shader with nir_builder, runs it
  # through both draw_create_vs_exec (nir_to_tgsi + tgsi_exec) and
  # draw_create_vs_nir (the interpreter) on a headless softpipe screen, and
  # memcmp's the raw AOS output.  Calling the two factories directly avoids the
  # once-only DRAW_NIR_EXEC env gate.
  test('draw-vs-nir-raw-output',
    executable(
      'draw_vs_nir_raw_output_test',
      'auxiliary/draw/tests/draw_vs_nir_raw_output_test.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                             include_directories('winsys'),
                             include_directories('drivers')],
      link_with : [libgallium, libws_null],
      dependencies : [driver_softpipe, idep_nir, idep_mesautil, dep_thread],
    ),
    suite: 'gallium',
  )
endif

if with_gallium_asahi
  subdir('winsys/asahi/drm')
  subdir('drivers/asahi')
else
  driver_asahi = declare_dependency()
endif
if with_gallium_r300 or with_gallium_radeonsi or with_gallium_r600
  subdir('winsys/radeon/drm')
endif
if with_gallium_r300
  subdir('drivers/r300')
else
  driver_r300 = declare_dependency()
endif
if with_gallium_r600
  subdir('drivers/r600')
else
  driver_r600 = declare_dependency()
endif
if with_gallium_radeonsi
  subdir('winsys/amdgpu/drm')
  subdir('drivers/radeonsi')
else
  driver_radeonsi = declare_dependency()
endif
if with_gallium_nouveau
  subdir('winsys/nouveau/drm')
  subdir('drivers/nouveau')
else
  driver_nouveau = declare_dependency()
endif
if with_gallium_freedreno
  subdir('winsys/freedreno/drm')
  subdir('drivers/freedreno')
else
  driver_freedreno = declare_dependency()
endif
if with_gallium_vc4
  subdir('winsys/vc4/drm')
  subdir('drivers/vc4')
else
  driver_vc4 = declare_dependency()
endif
if with_gallium_panfrost
  subdir('winsys/panfrost/drm')
  subdir('drivers/panfrost')
else
  driver_panfrost = declare_dependency()
endif
if with_gallium_etnaviv
  subdir('winsys/etnaviv/drm')
  subdir('drivers/etnaviv')
else
  driver_etnaviv = declare_dependency()
endif
if with_gallium_kmsro
  subdir('winsys/kmsro/drm')
else
  driver_kmsro = declare_dependency()
endif
if with_gallium_v3d
  subdir('winsys/v3d/drm')
  subdir('drivers/v3d')
else
  driver_v3d = declare_dependency()
endif
if with_gallium_tegra
  subdir('winsys/tegra/drm')
  subdir('drivers/tegra')
else
  driver_tegra = declare_dependency()
endif
if with_gallium_crocus
  subdir('winsys/crocus/drm')
  subdir('drivers/crocus')
else
  driver_crocus = declare_dependency()
endif
if with_gallium_iris
  subdir('winsys/iris/drm')
  subdir('drivers/iris')
else
  driver_iris = declare_dependency()
endif
if with_gallium_i915
  subdir('winsys/i915/drm')
  subdir('drivers/i915')
else
  driver_i915 = declare_dependency()
endif
if with_gallium_svga
  if not with_platform_windows
     subdir('winsys/svga/drm')
  endif
  subdir('drivers/svga')
else
  driver_svga = declare_dependency()
endif
if with_gallium_virgl
  subdir('winsys/virgl/common')
  if not with_platform_windows
    subdir('winsys/virgl/drm')
    subdir('winsys/virgl/vtest')
  endif
  subdir('drivers/virgl')
else
  driver_virgl = declare_dependency()
endif
if with_gallium_lima
  subdir('winsys/lima/drm')
  subdir('drivers/lima')
else
  driver_lima = declare_dependency()
endif
if with_gallium_rocket
  subdir('winsys/rocket/drm')
  subdir('drivers/rocket')
else
  driver_rocket = declare_dependency()
endif
if with_gallium_ethosu
  subdir('winsys/ethosu/drm')
  subdir('drivers/ethosu')
else
  driver_ethosu = declare_dependency()
endif
if with_gallium_zink
  if not with_platform_windows
    subdir('winsys/zink/drm')
  endif
  subdir('drivers/zink')
else
  driver_zink = declare_dependency()
endif
if with_gallium_d3d12
  subdir('drivers/d3d12')
else
  driver_d3d12 = declare_dependency()
endif
if with_gallium_rusticl
  subdir('frontends/rusticl')
  subdir('targets/rusticl')
endif
if with_glx == 'xlib'
  subdir('winsys/sw/xlib')
  subdir('frontends/glx/xlib')
  subdir('targets/libgl-xlib')
endif
if with_gallium_xa
  subdir('frontends/xa')
  subdir('targets/xa')
endif
if with_gallium_va
  subdir('frontends/va')
  if not with_dri
    subdir('targets/va')
  endif
endif
if with_gallium_mediafoundation
  subdir('frontends/mediafoundation')
  subdir('targets/mediafoundation')
endif
if with_dri
  subdir('frontends/dri')
  subdir('targets/dri')
endif
if with_platform_haiku
  subdir('frontends/hgl')
endif
if with_gallium_st_d3d10umd
  subdir('frontends/d3d10umd')
  subdir('targets/d3d10umd')
endif
if with_platform_windows
  if with_opengl
    subdir('frontends/wgl')
    if with_gallium_d3d12
      subdir('winsys/d3d12/wgl')
    else
      winsys_d3d12_wgl = declare_dependency()
    endif
    subdir('targets/wgl')
    subdir('targets/libgl-gdi')
  endif
endif
if with_swrast_vk
  subdir('frontends/lavapipe')
  subdir('targets/lavapipe')
endif

if with_teflon
  subdir('frontends/teflon')
  subdir('targets/teflon')
endif
