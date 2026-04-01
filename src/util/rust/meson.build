# Copyright © 2024 Google
# SPDX-License-Identifier: MIT

dep_cfg_if = dependency('cfg-if',
  version: '>= 1.0.4',
  fallback: ['cfg-if-1-rs', 'dep_cfg_if'],
  required: true,
)

dep_thiserror = dependency('thiserror',
  version: '>= 2.0.18',
  fallback: ['thiserror-2-rs', 'dep_thiserror'],
  required: true,
)

dep_remain = dependency('remain',
  version: '>= 0.2.15',
  fallback: ['remain-0.2-rs', 'dep_remain'],
  required: true,
)

dep_zerocopy = dependency('zerocopy',
  version: '>= 0.8.48',
  fallback: ['zerocopy-0.8-rs', 'dep_zerocopy'],
  required: true,
)

dep_mesa3d_util = [dep_cfg_if, dep_thiserror, dep_remain, dep_zerocopy]

supported_systems = ['linux', 'windows', 'darwin', 'macos']
supported_host_machine = host_machine.system() in supported_systems

if supported_host_machine
  dep_rustix = dependency('rustix',
    version: '>= 1.1.4',
    fallback: ['rustix-1-rs', 'dep_rustix'],
    required: true,
  )

  dep_mesa3d_util += [dep_rustix]
endif

libmesa_rust_util = static_library(
  'mesa3d_util',
  'lib.rs',
  gnu_symbol_visibility : 'hidden',
  rust_abi : 'rust',
  rust_args : rust_global_args,
  dependencies: dep_mesa3d_util,
)
