# Copyright (c) 2026 Terascale Functionalists
# SPDX-License-Identifier: MIT

r300vk_entrypoints = custom_target(
  'r300vk_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['r300vk_entrypoints.h', 'r300vk_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@',
    '--xml', '@INPUT1@',
    '--proto', '--weak',
    '--out-h', '@OUTPUT0@',
    '--out-c', '@OUTPUT1@',
    '--prefix', 'r300vk',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

libr300vk_files = files(
  'r300vk_instance.c',
  'r300vk_physical_device.c',
)

libr300vk_deps = [
  dep_libdrm,
  idep_mesautil,
  idep_vulkan_runtime,
  idep_vulkan_util,
]

libr300vk_inc = [
  inc_include,
  inc_src,
]

libr300vk_link_with = []

# Gallium-backed device and submit path.  Gated behind r300vk-gallium-backend
# so the loader-only R0 build (no Gallium dependency) continues to work.
# with_gallium_r300 is set by the parent meson.build when gallium-drivers
# includes r300; guard both options so the build fails clearly if only one
# is set.
# Processed from src/meson.build AFTER subdir('gallium') so that driver_r300
# and libgallium are in scope.
if get_option('r300vk-gallium-backend') and with_gallium_r300
  libr300vk_files += files(
    'r300vk_device.c',
    'r300vk_queue.c',
    'r300vk_memory.c',
    'r300vk_buffer.c',
    'r300vk_image.c',
    'r300vk_shader_module.c',
    'r300vk_render_pass.c',
    'r300vk_framebuffer.c',
    'r300vk_cmd_buffer.c',
    'r300vk_pipeline.c',
  )
  libr300vk_deps += [
    idep_nir,
    idep_vulkan_wsi,
    driver_r300,
  ]
  libr300vk_inc += [
    inc_gallium,
    inc_gallium_aux,
    include_directories('../../../../gallium/drivers'),
  ]
  # libgallium provides Gallium auxiliary symbols (u_upload_alloc_ref,
  # u_default_texture_subdata, etc.) that libr300.a references but does
  # not define.  Must be in link_with (not dependencies) because meson
  # tracks it as a static_library object, not a declare_dependency.
  libr300vk_link_with += [libgallium]
endif

libvulkan_r300 = shared_library(
  'vulkan_r300',
  [libr300vk_files, r300vk_entrypoints],
  vs_module_defs : vulkan_api_def,
  include_directories : libr300vk_inc,
  dependencies : libr300vk_deps,
  link_with : libr300vk_link_with,
  c_args : [no_override_init_args],
  link_args : [
    ld_args_build_id,
    ld_args_bsymbolic,
    ld_args_gc_sections,
    vulkan_icd_link_args,
  ],
  link_depends : vulkan_icd_link_depends,
  gnu_symbol_visibility : 'hidden',
  install : true,
)

# ICD JSON manifest.  The loader discovers ICDs by scanning
# /usr/share/vulkan/icd.d/ for *.json files and following the
# library_path entry.  vk_icd_gen emits the canonical form so the
# version negotiation matches what r300vk_instance.c implements.
icd_lib_path = join_paths(get_option('prefix'), get_option('libdir'))
icd_file_name = 'libvulkan_r300.so'

r300_icd = custom_target(
  'r300_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'r300_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.0',
    '--xml', '@INPUT1@',
    '--icd-lib-path', icd_lib_path,
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)

# Devenv ICD: when running meson devenv, point the loader at the build
# directory copy of the library so `VK_ICD_FILENAMES=<build>/r300_icd*.json`
# picks up the in-tree build.
_dev_icdname = 'r300_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  'r300_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.0',
    '--xml', '@INPUT1@',
    '--icd-lib-path', meson.current_build_dir(),
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install : false,
)

devenv.append('VK_ICD_FILENAMES', _dev_icd.full_path())
