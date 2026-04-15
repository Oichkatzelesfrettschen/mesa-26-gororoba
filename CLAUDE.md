# Mesa 26.1-devel / Terakan+r600 Driver -- Claude Project Context

## Overview

This repo is a fork of Mesa 26.1-devel tracking upstream closely.
It adds the Terakan Vulkan driver (`src/amd/terascale/vulkan/`) and associated
r600 SFN improvements for Radeon HD 6310 (CHIP_SUMO, TeraScale-2 VLIW5).

Hardware target: x130e (Radeon HD 6310, Evergreen/Cayman family)
Upstream remote: https://gitlab.freedesktop.org/mesa/mesa.git (branch: main)
GitHub mirror: git@github.com:Oichkatzelesfrettschen/mesa-26-debug-gororoba.git

## Accessing Files

**Primary -- NFS mount (read/edit with Claude's dedicated tools):**

  Mac:      /Volumes/x130e/workspaces/mesa/mesa-26-debug/
  CachyOS:  /mnt/x130e/workspaces/mesa/mesa-26-debug/
  WSL2:     ~/mnt/x130e/workspaces/mesa/mesa-26-debug/

Never SSH just to read or grep a file. Use Read/Grep/Edit tools on the NFS path.

**Interactive sessions on x130e (fallback order):**

1. Mosh + tmux (preferred -- survives network hiccups, IP changes, laptop sleep):
     mosh x130e -- tmux attach -t steinmarder
   Send a build command without holding the connection open:
     ssh x130e 'tmux send-keys -t steinmarder "cd ~/workspaces/mesa/mesa-26-debug/build-debug && ninja -j36" Enter'
   Read output:
     ssh x130e 'tmux capture-pane -t steinmarder -p | tail -30'

2. SSH one-shot (for short non-interactive commands: git, quick checks):
     ssh x130e 'git log --oneline -5'

3. SCP/rsync (file transfer fallback only -- NFS is faster on LAN):
     rsync -av x130e:path .

## Build System

**Build directory:** `build-debug/` (Meson, debug build, clang-19)

Build always uses distcc via /tmp/distcc-wrap wrappers. Check before building:

  ls /tmp/distcc-wrap/     # should show cc and c++

If missing (lost on reboot), reconstruct:

  ssh x130e 'mkdir -p /tmp/distcc-wrap && \
    printf "#!/bin/sh\nexec distcc /usr/bin/clang-19 \"\$@\"\n" > /tmp/distcc-wrap/cc && \
    printf "#!/bin/sh\nexec distcc /usr/bin/clang++-19 \"\$@\"\n" > /tmp/distcc-wrap/c++ && \
    chmod +x /tmp/distcc-wrap/cc /tmp/distcc-wrap/c++'

**Build command (send to steinmarder tmux, or run directly):**

  cd ~/workspaces/mesa/mesa-26-debug/build-debug && ninja -j36

**distcc cluster (~72 total slots, any offline host skipped automatically):**

  localhost/4        -- x130e local cores (fallback only, always present)
  DESKTOP-CKP9KB6/48 -- primary build server (Linux, 48 distcc slots)
  X570-5600X3D/12    -- secondary (Linux, 12 slots)
  Eirikrs-MacBook-Air.local/8 -- tertiary (Mac, 8 cpp-mode slots)

Hosts file: ~/.distcc/hosts on x130e (--randomize for load balancing)
"INCLUDE_SERVER_PORT not set" warnings are harmless (pump mode not running).
Use -j36 normally; raise to -j48 if all hosts are confirmed up.

## Install

  ssh x130e 'sudo cp ~/workspaces/mesa/mesa-26-debug/build-debug/src/gallium/targets/dri/libgallium-26.1.0-devel.so \
    /usr/local/mesa-debug/lib/x86_64-linux-gnu/libgallium-26.1.0-devel.so'

## Canonical Test Commands

  # GL baseline (expect >=148 FPS warm)
  ssh x130e 'LIBGL_DRIVERS_PATH=/usr/local/mesa-debug/lib/x86_64-linux-gnu/dri \
    vblank_mode=0 glmark2 -s 400x300 -b shading:phong 2>&1 | tail -3'

  # VK baseline (expect >=1449 FPS warm)
  ssh x130e 'VK_ICD_FILENAMES=/usr/local/mesa-debug/share/vulkan/icd.d/terascale_icd.x86_64.json \
    vkmark --winsys headless 2>&1 | tail -5'

  # OpenCL / RAT readback
  ssh x130e 'LIBGL_DRIVERS_PATH=/usr/local/mesa-debug/lib/x86_64-linux-gnu/dri \
    RUSTICL_ENABLE=r600 timeout 30 /tmp/rat_test_fixed 2>/tmp/err.txt; echo exit=$?; cat /tmp/err.txt'

Regressions: GL <148 FPS, VK <1449 FPS warm, or RAT wrong output -- bisect immediately.

## Debug Variables

  R600_DEBUG=compute      -- dump compute shader ISA
  R600_DEBUG=vs,ps        -- dump vertex/pixel shader ISA
  GALLIUM_HUD=stdout,fps  -- pipeline counters

## Upstream Sync

Do NOT cherry-pick individual upstream commits. Instead squash-merge:

  git fetch upstream
  git merge --squash upstream/main
  # resolve conflicts (RCA each one), then commit

Conflict resolution priority:
- r600/terascale files: resolve manually, document RCA in commit message
- Non-terascale driver files (radv, intel, nouveau, etc.): git checkout --theirs
- AMD common (ac_gpu_info, etc.): git checkout --theirs (we don't modify these)

## Key Source Locations

  src/amd/terascale/vulkan/          -- Terakan Vulkan driver
  src/amd/terascale/vulkan/meta/     -- meta operations (clear, copy, blit, query)
  src/amd/terascale/vulkan/nir/      -- NIR lowering passes
  src/gallium/drivers/r600/sfn/      -- SFN shader compiler backend
  src/gallium/drivers/r600/          -- r600 Gallium driver (GL/Rusticl)

## Standards

- Treat all compiler warnings as errors (-Wall -Wextra -Werror in dev builds)
- Document WHY before WHAT and HOW in all comments and commit messages
- No cherry-picks from upstream; squash-merge only to maintain clean fork history
- All new features must have a verified test path before the feature flag is enabled
