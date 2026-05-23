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

libvulkan_r300 = shared_library(
  'vulkan_r300',
  [libr300vk_files, r300vk_entrypoints],
  vs_module_defs : vulkan_api_def,
  include_directories : [
    inc_include,
    inc_src,
  ],
  dependencies : libr300vk_deps,
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
