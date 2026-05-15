# x130e distcc and distcc-pump canonical split

## Why

The x130e is the target machine, but it only has two local CPU cores. For
Mesa/Terakan rebuilds, keep configuration and linking on x130e while offloading
C and C++ compilation to LAN distcc servers reached by mDNS hostnames.

## What

Current verified state:

- Source tree: `/home/eirikr/workspaces/mesa/mesa-26-gororoba`.
- Warm build tree:
  `/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump`.
- Pump build tree:
  `/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-pump`.
- Active distcc SSH worker: `@x570-5600X3D.local/16,lzo`.
- Active Ubuntu WSL TCP worker: `DESKTOP-CKP9KB6-2.local/32,lzo`.
- distcc host specs for this lane use mDNS `.local` names only; do not use raw DHCP addresses.
- Compiler: `clang-21` and `clang++-21`.
- Warm profile: `build-infra/configs/terakan-distcc-no-rusticl.meson`.
- Pump profile: `build-infra/configs/terakan-distcc-no-rusticl-pump.meson`.
- Active install prefix: `/usr/local/mesa-26-gororoba`.
  Older `/usr/local/mesa-debug` and `/usr/local/mesa-terakan-*`
  prefixes are historical unless a run explicitly overrides `PREFIX=...`.

The no-Rusticl profile enables the daily Terakan/r600 lane:

- `gallium-drivers=[r600]`
- `vulkan-drivers=[amd_terascale]`
- `gallium-rusticl=false`
- `amd-use-llvm=true`
- `gallium-extra-hud=true`
- `glx=auto`
- `buildtype=debug`
- `b_ndebug=false`

Use the build-infra native file, not the legacy `/tmp/distcc-wrap` native files.
The canonical split is:

| Use case | C/C++ chain | Rust chain | Host options | Command |
| --- | --- | --- | --- | --- |
| Warm incremental | `ccache -> distcc -> clang-21`, no pump | `sccache -> rustc` | `lzo`, includes x570 + DESKTOP + localhost + zeroconf | `make rebuild-terakan-distcc-no-rusticl-ccache-no-pump` |
| Cold clean | `distcc-pump -> distcc -> clang-21`, no ccache | `sccache -> rustc` | verified x570 mDNS worker with shell-derived `cpp,lzo` | `make rebuild-terakan-distcc-no-rusticl-pump` |

Warm/no-pump compiler wiring:

```ini
[binaries]
c   = [/usr/bin/ccache, /usr/bin/clang-21]
cpp = [/usr/bin/ccache, /usr/bin/clang++-21]
rust = [/home/eirikr/.local/bin/sccache, /home/eirikr/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc]
```

Cold/pump compiler wiring:

```ini
[binaries]
c   = [/usr/bin/distcc, /usr/bin/clang-21]
cpp = [/usr/bin/distcc, /usr/bin/clang++-21]
rust = [/home/eirikr/.local/bin/sccache, /home/eirikr/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc]
```

The critical distcc and Bobcat flags are:

```sh
export DISTCC_HOSTS="--randomize @x570-5600X3D.local/16,lzo DESKTOP-CKP9KB6-2.local/32,lzo localhost/2,lzo +zeroconf"
export CCACHE_PREFIX="/usr/bin/distcc"
export CCACHE_PATH="/usr/bin"
export CCACHE_DIR="$HOME/.cache/ccache"
export SCCACHE_DIR="$HOME/.cache/sccache"
export CFLAGS="-march=btver1 -mtune=btver1 -pipe -fno-emulated-tls"
export CXXFLAGS="$CFLAGS"
export LDFLAGS=""
```

`-fno-emulated-tls` is required for clang-21 on this host; without it, the Mesa
link can fail when generated emulated-TLS symbols do not match the ELF-TLS
references used by Mesa objects.

Do not put `ccache` or `sccache` in front of C/C++ distcc-pump.  Pump
needs distcc to see the original source and compiler command.  `sccache`
is still correct for Rust because it wraps rustc separately from the
C/C++ include-server path.

DESKTOP/WSL is intentionally excluded from the default pump lane after
live x130e builds showed Gallivm pump discrepancies and pump demotion.
It remains in the classic no-pump mesh.  Use
`TERAKAN_PUMP_ALLOW_DESKTOP=1` only for explicit parity probes.

## How

Sync the repo first:

```sh
cd /home/eirikr/workspaces/mesa/mesa-26-gororoba
git fetch --all --prune
git pull --ff-only origin main
```

Use the warm lane for normal edit/build/probe loops:

```sh
cd /home/eirikr/workspaces/mesa/mesa-26-gororoba/build-infra
make rebuild-terakan-distcc-no-rusticl-ccache-no-pump
```

Use the pump lane for cold clean builds where remote preprocessing is
worth more than cache hits:

```sh
cd /home/eirikr/workspaces/mesa/mesa-26-gororoba/build-infra
make rebuild-terakan-distcc-no-rusticl-pump
```

The pump target intentionally runs in three phases:

1. Configure normally.
2. Prebuild generated Meson/Ninja targets outside the include-server lifetime.
3. Run the heavy compile under `distcc-pump`.

That split prevents generated headers such as `u_format_gen.h` from
changing while pump's include server is snapshotting dependencies.

Install with root privileges only after the user-owned build converges.
Do not commit or build the repo as root:

```sh
sudo meson install --no-rebuild -C /home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump
```

Verify the installed artifacts:

```sh
find /usr/local/mesa-26-gororoba -maxdepth 5 -type f \
  \( -name "libvulkan_terascale.so" -o -name "r600_dri.so" -o -name "libgallium-*.so" -o -name "terascale_icd*.json" \) \
  -printf "%p %TY-%Tm-%Td %TH:%TM:%TS %s bytes\n" | sort
```

The Makefile sources `build-infra/env/*.env` for configure/build targets.
Use those env files as the canonical source of build lane policy rather
than ad hoc shell exports.

## 2026-05-13 canonical split check

The first full pump build with DESKTOP/WSL in pump mode completed, but
reported 15 pump demotions.  After adding the generated-output preflight,
the warning dropped to 3 Gallivm discrepancies, all on
`DESKTOP-CKP9KB6-2.local`.  That makes DESKTOP/WSL non-canonical for
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

That is the exact failure mode described by the `terakan-distcc-no-rusticl`
profile, so the successful installed build used:

```sh
export BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-pump-no-rusticl
export PREFIX=/usr/local/mesa-debug
export DISTCC_HOSTS="--randomize @x570-5600X3D.local/16,lzo DESKTOP-CKP9KB6-2.local/32,lzo localhost/2,lzo +zeroconf"
export CCACHE_PREFIX="/usr/bin/distcc"
export CCACHE_PATH="/usr/bin"
export CCACHE_DIR="$HOME/.cache/ccache"
export SCCACHE_DIR="$HOME/.cache/sccache"
export CFLAGS="-march=btver1 -mtune=btver1 -pipe -fno-emulated-tls"
export CXXFLAGS="$CFLAGS"
export LDFLAGS=""

meson setup \
  --native-file="$PWD/build-infra/configs/terakan-distcc-no-rusticl.meson" \
  --prefix="$PREFIX" \
  "$BUILDDIR" "$PWD"

ninja -C "$BUILDDIR" -j36
```

Install must avoid root-side rebuilds. If `ninja install` dirties build targets
(for example after `src/git_sha1.h` changes), reconverge as user first with the
same distcc environment, then install as root with:

```sh
meson install --no-rebuild -C /home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-pump-no-rusticl
```

Installed files were copied to `/usr/local/mesa-debug` at 2026-04-26T01:23:48Z.
That prefix is historical.  The active prefix is now
`/usr/local/mesa-26-gororoba`.
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
  distcc /usr/bin/clang-21 -march=btver1 -mtune=btver1 \
    -fno-emulated-tls -c /tmp/distcc_mac_probe.c \
    -o /tmp/distcc_mac_probe.o
file /tmp/distcc_mac_probe.o
```

The output object was an x86-64 ELF relocatable, so the Mac worker can compile
Bobcat-targeted Terakan objects when reached through mDNS. On 2026-05-12 this
was superseded by SSH-mode distcc over mDNS. The active host file is now:

```text
--randomize @x570-5600X3D.local/16,lzo DESKTOP-CKP9KB6-2.local/32,lzo localhost/2,lzo +zeroconf
```

`DESKTOP-CKP9KB6-2.local/32,lzo` is active only for the clang-21 lane.
- The failed rusticl-enabled build log is
  `/home/eirikr/logs/mesa_gororoba_pump_build_20260426T003804Z.log`.
- The successful no-rusticl build log is
  `/home/eirikr/logs/mesa_gororoba_no_rusticl_build_20260426T010131Z.log`.
- The successful install log is
  `/home/eirikr/logs/mesa_gororoba_no_rusticl_install_norebuild_20260426T012346Z.log`.
