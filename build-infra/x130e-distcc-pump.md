# x130e distcc and distcc-pump canonical split

## Why

The x130e is the target machine, but it only has two local CPU cores. For
Mesa/Terakan rebuilds, keep configuration and linking on x130e while offloading
C and C++ compilation to LAN distcc servers reached by mDNS hostnames.

## What

Current verified state:

- Source tree: `~/workspaces/mesa/mesa-26-gororoba`.
- Warm build tree:
  `~/workspaces/mesa/build/mesa-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache`.
- Pump build tree (historical; distcc-pump removed upstream):
  `~/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-pump` (archived).
- Active distcc SSH worker: `@x570-5600X3D.local/16,lzo`.
- Active Ubuntu WSL TCP worker: `ALIENWARE.local/32,lzo`.
- distcc host specs for this lane use mDNS `.local` names only; do not use raw DHCP addresses.
- Compiler: a coherent installed `clang` / `clang++` / `llvm-config`
  family selected by the Makefile-generated Meson native overlay.
- Warm profile: `build-infra/configs/alternates/5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache.meson`.
- Pump profile: removed upstream (distcc-pump is incompatible with ccache; use warm lane).
  Any distcc-pump details later in this document are historical-only; the pump
  lane is not an active workflow -- follow the warm (ccache-first) lane above.
- Default install prefix: `/opt/local/mesa-<profile>`.
  Pass `PREFIX=/opt/local/mesa-26-gororoba` only when intentionally
  installing into the shared active tree.

The no-Rusticl profile (5_) enables the daily Terakan/r600 release lane (its
option set, matching configs/alternates/5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache.meson):

- `gallium-drivers=[r600, zink, softpipe, llvmpipe]`
- `vulkan-drivers=[amd_terascale]`
- `gallium-rusticl=false`
- `amd-use-llvm=true`
- `gallium-extra-hud=true`
- `glx=dri`
- `buildtype=release`
- `b_ndebug=true`

Use the build-infra native file, not the legacy `/tmp/distcc-wrap` native files.
The canonical split is:

| Use case | C/C++ chain | Rust chain | Host options | Command |
| --- | --- | --- | --- | --- |
| Warm incremental | `ccache -> distcc -> clang`, no pump | `sccache -> rustc` | `lzo`, includes x570 + WSL + localhost + zeroconf | `make rebuild-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache` |
| Cold clean | distcc-pump removed upstream | `sccache -> rustc` | use warm lane instead | (no pump target) |

Warm/no-pump compiler wiring is generated at configure time:

```ini
[binaries]
c = ['ccache', '<selected-clang>']
cpp = ['ccache', '<selected-clang++>']
rust = ['sccache', 'rustc']
llvm-config = '<selected-llvm-config>'
```

Historical cold/pump compiler wiring was generated at configure time:

```ini
[binaries]
c   = ['distcc', '<selected-clang>']
cpp = ['distcc', '<selected-clang++>']
rust = ['sccache', 'rustc']
llvm-config = '<selected-llvm-config>'
```

The critical distcc and Bobcat flags are:

```sh
export DISTCC_HOSTS="--randomize @x570-5600X3D.local/16,lzo ALIENWARE.local/32,lzo localhost/2,lzo +zeroconf"
export CCACHE_PREFIX="${CCACHE_PREFIX:-distcc}"
export CCACHE_DIR="$HOME/.cache/ccache"
export SCCACHE_DIR="$HOME/.cache/sccache"
export CFLAGS="-march=btver1 -mtune=btver1 -pipe -fno-emulated-tls"
export CXXFLAGS="$CFLAGS"
export LDFLAGS=""
```

`-fno-emulated-tls` is required for the validated clang lane on this host;
without it, the Mesa link can fail when generated emulated-TLS symbols do not
match the ELF-TLS references used by Mesa objects.

Do not revive the historical C/C++ pump lane by putting `ccache` or `sccache`
in front of distcc-pump.  If the archived workflow is studied, remember that
pump needed distcc to see the original source and compiler command.  `sccache`
remained correct for Rust because it wrapped rustc separately from the C/C++
include-server path.

The Ubuntu WSL worker participated in the pump lane by default.  Operators
opted out per-build by exporting `TERAKAN_PUMP_ALLOW_WSL=0` before
sourcing the env; absent or `=1` kept the WSL worker in the pump lane.
distcc-pump's three-step include-fingerprint safety ladder handled
the residual class of translation units whose preprocessed hash is
not stable across the LAN crossing:

  1. pump preprocess on the client + remote compile
  2. classic distcc preprocess on the client + remote compile
  3. fully local compile on the client

Step 3 was the safety net.  Translation units that fell back to step 3 built
locally without breaking the rest of the build.

## How

Sync the repo first:

```sh
cd ~/workspaces/mesa/mesa-26-gororoba
git fetch --all --prune
git pull --ff-only origin main
```

Use the warm lane for normal edit/build/probe loops:

```sh
cd ~/workspaces/mesa/mesa-26-gororoba/build-infra
make rebuild-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache
```

Or the Rusticl-enabled variant when Rusticl is buildable:

```sh
make rebuild-3_terakan_full_release_x86_64v1-clang22-distcc-cache
```

Install with root privileges only after the user-owned build converges.
Do not commit or build the repo as root:

```sh
make install-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache
```

Verify the installed artifacts:

```sh
make artifact-check PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache PREFIX=/opt/local/mesa-26-gororoba
```

The Makefile sources `build-infra/env/*.env` for configure/build targets.
Use those env files as the canonical source of build lane policy rather
than ad hoc shell exports.

## 2026-05-13 canonical split check

The first full pump build with the Ubuntu WSL worker in pump mode completed, but
reported 15 pump demotions.  After adding the generated-output preflight,
the warning dropped to 3 Gallivm discrepancies, all on
`ALIENWARE.local`.  That makes the WSL worker non-canonical for
default pump mode.

The default pump env was then tightened to the verified x570 mDNS worker:

```text
--randomize @x570-5600X3D.local/16,cpp,lzo localhost/2,lzo
```

Validation:

```text
tmux: terakan_build_pump_x570_only_check_20260513Tcanonical
log:  /tmp/terakan_build_pump_x570_only_check_20260513Tcanonical.log
rc:   0
work: 100 incremental Ninja steps
note: no distcc discrepancy or pump-demotion warning
```

## 2026-04-26 historical run result

Source revision:

```text
a512f2bf07e terakan/image: SLICE_MAX = depth-1 for VK_IMAGE_TYPE_3D (FIX-3D-SLICE-MAX)
```

The rusticl-enabled `terakan-distcc` profile configured and built to
`1409/1415`, then failed in Rust with bindgen symbol drift:

```text
pipe_image_view__bindgen_ty_1__bindgen_ty_1 missing
pipe_image_view__bindgen_ty_1__bindgen_ty_2 missing
pipe_sampler_view__bindgen_ty_1__bindgen_ty_2 missing
nir_spirv_execution_environment missing
```

That is the exact failure mode described by the no-Rusticl profile, so the
successful installed build used:

```sh
export BUILDDIR=~/workspaces/mesa/build/mesa-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache
export PREFIX=/usr/local/mesa-debug
export DISTCC_HOSTS="--randomize @x570-5600X3D.local/16,lzo ALIENWARE.local/32,lzo localhost/2,lzo +zeroconf"
export CCACHE_PREFIX="${CCACHE_PREFIX:-distcc}"
export CCACHE_DIR="$HOME/.cache/ccache"
export SCCACHE_DIR="$HOME/.cache/sccache"
export CFLAGS="-march=btver1 -mtune=btver1 -pipe -fno-emulated-tls"
export CXXFLAGS="$CFLAGS"
export LDFLAGS=""

meson setup \
  --native-file="$PWD/build-infra/configs/alternates/5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache.meson" \
  --prefix="$PREFIX" \
  "$BUILDDIR" "$PWD"

ninja -C "$BUILDDIR" -j6
```

Install must avoid root-side rebuilds. If `ninja install` dirties build targets
(for example after `src/git_sha1.h` changes), reconverge as user first with the
same distcc environment, then install as root with:

```sh
meson install --no-rebuild -C ~/workspaces/mesa/build/mesa-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache
```

Installed files were copied to `/usr/local/mesa-debug` at 2026-04-26T01:23:48Z.
That prefix is historical.  The default active lane prefix is
profile-derived unless a run explicitly passes `PREFIX=...`.
Key artifacts:

```text
/usr/local/mesa-debug/lib/x86_64-linux-gnu/libvulkan_terascale.so
/usr/local/mesa-debug/lib/x86_64-linux-gnu/libgallium-26.1.0-devel.so
/usr/local/mesa-debug/lib/x86_64-linux-gnu/dri/libdril_dri.so
/usr/local/mesa-debug/lib/x86_64-linux-gnu/dri/r600_dri.so -> libdril_dri.so
/usr/local/mesa-debug/share/vulkan/icd.d/terascale_icd.x86_64.json
```

Because the successful profile disables Rusticl, stale Rusticl/OpenCL files from
older installs were moved aside to:

```text
/usr/local/mesa-debug/stale-rusticl-20260426T012454Z/
```

Verification:

```sh
env LD_LIBRARY_PATH=/usr/local/mesa-debug/lib/x86_64-linux-gnu \
  VK_ICD_FILENAMES=/usr/local/mesa-debug/share/vulkan/icd.d/terascale_icd.x86_64.json \
  vulkaninfo --summary
```

The installed ICD reports:

```text
deviceName = AMD R8xx Palm (Terakan)
apiVersion = 1.0.348
vendorID   = 0x1002
deviceID   = 0x9802
```

Notes:

- Older pump runs demoted some files to plain mode after generated-header
  consistency warnings. The canonical pump target now prebuilds generated
  outputs before starting the include server.

## 2026-05-03 hostname and Mac worker audit

The workspace policy is hostname-only: do not put raw DHCP IPv4 addresses in
`/etc/hosts`, `~/.distcc/hosts`, build scripts, or prescriptive docs.

On 2026-05-03, x130e had stale raw-IP `/etc/hosts` pins for both the Mac and
desktop worker. Because `nsswitch.conf` checks `files` before mDNS, those pins
overrode the current mDNS answers and made the Mac worker appear unreachable.
The stale pins were removed and backed up on x130e at:

```text
/etc/hosts.codex-backup-20260503T1330Z
```

The historical Mac worker was then verified from x130e with:

```sh
DISTCC_HOSTS="Eirikrs-MacBook-Air.local/1,lzo" \
  distcc clang -march=btver1 -mtune=btver1 \
    -fno-emulated-tls -c /tmp/distcc_mac_probe.c \
    -o /tmp/distcc_mac_probe.o
file /tmp/distcc_mac_probe.o
```

The output object was an x86-64 ELF relocatable, so the Mac worker can compile
Bobcat-targeted Terakan objects when reached through mDNS. On 2026-05-12 this
was superseded by SSH-mode distcc over mDNS. The active host file is now:

```text
--randomize @x570-5600X3D.local/16,lzo ALIENWARE.local/32,lzo localhost/2,lzo +zeroconf
```

`ALIENWARE.local/32,lzo` is active only for the no-pump clang lane.
- The failed rusticl-enabled build log is
  `~/logs/mesa_gororoba_pump_build_20260426T003804Z.log`.
- The successful no-rusticl build log is
  `~/logs/mesa_gororoba_no_rusticl_build_20260426T010131Z.log`.
- The successful install log is
  `~/logs/mesa_gororoba_no_rusticl_install_norebuild_20260426T012346Z.log`.
