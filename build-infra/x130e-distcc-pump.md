# x130e distcc-pump clean build

## Why

The x130e is the target machine, but it only has two local CPU cores. For
Mesa/Terakan rebuilds, keep configuration and linking on x130e while offloading
C and C++ compilation to LAN distcc servers reached by mDNS hostnames.

## What

Current verified state:

- Source tree: `/home/eirikr/workspaces/mesa/mesa-26-gororoba`.
- Remote distcc server: `DESKTOP-CKP9KB6.local`.
- Optional Mac distcc server: `Eirikrs-MacBook-Air.local`.
- distcc server port: `3632`, verified reachable from x130e before each run.
- distcc host specs for this lane use `.local` names only.
- Compiler: `clang-22` and `clang++-22` through ccache, with ccache misses sent through distcc.
- Build profile: `build-infra/configs/terakan-distcc.meson`.
- Target install prefix for the mesa-debug lane: `/usr/local/mesa-debug`.
- Clean build directory: `/home/eirikr/workspaces/build/mesa-terakan-distcc-pump`.

The profile enables the daily Terakan/r600 lane:

- `gallium-drivers=[r600]`
- `vulkan-drivers=[amd_terascale]`
- `gallium-rusticl=true`
- `gallium-rusticl-enable-drivers=[r600]`
- `amd-use-llvm=true`
- `gallium-extra-hud=true`
- `glx=auto`
- `buildtype=debug`
- `b_ndebug=false`

Use the build-infra native file, not the legacy `/tmp/distcc-wrap` native files.
The current canonical compiler wiring is:

```ini
[binaries]
c   = [/usr/bin/ccache, /usr/bin/clang-22]
cpp = [/usr/bin/ccache, /usr/bin/clang++-22]
rust = [/home/eirikr/.local/bin/sccache, /home/eirikr/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc]
```

The critical distcc and Bobcat flags are:

```sh
export DISTCC_HOSTS="DESKTOP-CKP9KB6.local/32,cpp,lzo Eirikrs-MacBook-Air.local/8,lzo"
export CCACHE_PREFIX="distcc"
export CCACHE_DIR="$HOME/.cache/ccache"
export SCCACHE_DIR="$HOME/.cache/sccache"
export CFLAGS="-march=btver1 -mtune=btver1 -pipe -fno-emulated-tls"
export CXXFLAGS="$CFLAGS"
export LDFLAGS=""
```

`-fno-emulated-tls` is required for clang-22 on this host; without it, the Mesa
link can fail when generated emulated-TLS symbols do not match the ELF-TLS
references used by Mesa objects.

## How

Sync the repo first:

```sh
cd /home/eirikr/workspaces/mesa/mesa-26-gororoba
git fetch --all --prune
git pull --ff-only origin main
```

Start from a new clean build directory. This preserves older source-tree build
directories such as `build-terakan-firstsync` for evidence and avoids deleting
old logs.

```sh
cd /home/eirikr/workspaces/mesa/mesa-26-gororoba
export BUILDDIR=/home/eirikr/workspaces/build/mesa-terakan-distcc-pump
export PREFIX=/usr/local/mesa-debug

mkdir -p "$(dirname "$BUILDDIR")"

export DISTCC_HOSTS="DESKTOP-CKP9KB6.local/32,cpp,lzo Eirikrs-MacBook-Air.local/8,lzo"
export CCACHE_PREFIX="distcc"
export CCACHE_DIR="$HOME/.cache/ccache"
export SCCACHE_DIR="$HOME/.cache/sccache"
export CFLAGS="-march=btver1 -mtune=btver1 -pipe -fno-emulated-tls"
export CXXFLAGS="$CFLAGS"
export LDFLAGS=""

meson setup \
  --native-file="$PWD/build-infra/configs/terakan-distcc.meson" \
  --prefix="$PREFIX" \
  "$BUILDDIR" "$PWD"

distcc-pump ninja -C "$BUILDDIR" -j34
```

Install with root privileges after the build succeeds:

```sh
ssh x130e-root ninja -C /home/eirikr/workspaces/build/mesa-terakan-distcc-pump install
```

Verify the installed artifacts:

```sh
find /usr/local/mesa-debug -maxdepth 5 -type f \
  \( -name "libvulkan_terascale.so" -o -name "r600_dri.so" -o -name "libgallium-*.so" -o -name "terascale_icd*.json" \) \
  -printf "%p %TY-%Tm-%Td %TH:%TM:%TS %s bytes\n" | sort
```

If a build must use the Makefile instead of raw Meson/Ninja, still export the
distcc and ccache environment above for the build step. `make configure` sources
`build-infra/env/btver1.env`, but `make build` invokes Ninja directly and needs
`CCACHE_PREFIX` and `DISTCC_HOSTS` in the live shell environment.

## 2026-04-26 run result

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
export BUILDDIR=/home/eirikr/workspaces/build/mesa-terakan-distcc-pump-no-rusticl
export PREFIX=/usr/local/mesa-debug
export DISTCC_HOSTS="DESKTOP-CKP9KB6.local/32,cpp,lzo Eirikrs-MacBook-Air.local/8,lzo"
export CCACHE_PREFIX="distcc"
export CCACHE_DIR="$HOME/.cache/ccache"
export SCCACHE_DIR="$HOME/.cache/sccache"
export CFLAGS="-march=btver1 -mtune=btver1 -pipe -fno-emulated-tls"
export CXXFLAGS="$CFLAGS"
export LDFLAGS=""

meson setup \
  --native-file="$PWD/build-infra/configs/terakan-distcc-no-rusticl.meson" \
  --prefix="$PREFIX" \
  "$BUILDDIR" "$PWD"

distcc-pump ninja -C "$BUILDDIR" -j34
```

Install must avoid root-side rebuilds. If `ninja install` dirties build targets
(for example after `src/git_sha1.h` changes), reconverge as user first with the
same distcc environment, then install as root with:

```sh
meson install --no-rebuild -C /home/eirikr/workspaces/build/mesa-terakan-distcc-pump-no-rusticl
```

Installed files were copied to `/usr/local/mesa-debug` at 2026-04-26T01:23:48Z.
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

- `distcc-pump` used `DESKTOP-CKP9KB6.local`, but pump demoted to plain mode for WSI
  files after generated-header consistency warnings. Those files retried locally
  and the final no-rusticl build succeeded.

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

The Mac worker was then verified from x130e with:

```sh
DISTCC_HOSTS="Eirikrs-MacBook-Air.local/1,lzo" \
  distcc /usr/bin/clang-22 -march=btver1 -mtune=btver1 \
    -fno-emulated-tls -c /tmp/distcc_mac_probe.c \
    -o /tmp/distcc_mac_probe.o
file /tmp/distcc_mac_probe.o
```

The output object was an x86-64 ELF relocatable, so the Mac worker can compile
Bobcat-targeted Terakan objects when reached through mDNS. The active no-pump
host file was reduced to the reachable Mac worker while the desktop worker's
distcc port timed out:

```text
Eirikrs-MacBook-Air.local/8,lzo
```

Do not re-add `DESKTOP-CKP9KB6.local` to `~/.distcc/hosts` until
`nc -zvw4 DESKTOP-CKP9KB6.local 3632` succeeds from x130e.
- The failed rusticl-enabled build log is
  `/home/eirikr/logs/mesa_gororoba_pump_build_20260426T003804Z.log`.
- The successful no-rusticl build log is
  `/home/eirikr/logs/mesa_gororoba_no_rusticl_build_20260426T010131Z.log`.
- The successful install log is
  `/home/eirikr/logs/mesa_gororoba_no_rusticl_install_norebuild_20260426T012346Z.log`.
