# x130e distcc and distcc-pump canonical split

## Why

The x130e is the target machine, but it only has two local CPU cores. For
Mesa/Terakan rebuilds, keep configuration and linking on x130e while offloading
C and C++ compilation to LAN distcc servers reached by mDNS hostnames.

## What

Current build policy:

- Source tree: the Mesa checkout containing this `build-infra/` directory.
- Warm build tree: `../../build/mesa-terakan-distcc-no-rusticl-ccache-no-pump`.
- Pump build tree: `../../build/mesa-terakan-distcc-no-rusticl-pump`.
- distcc host specs for this lane use mDNS `.local` names only; do not use raw DHCP addresses.
- Compiler: `clang-22` and `clang++-22`.
- Warm profile: `build-infra/configs/terakan-distcc-no-rusticl.meson`.
- Pump profile: `build-infra/configs/terakan-distcc-no-rusticl-pump.meson`.
- Target install prefix: set `PREFIX` explicitly for the host being tested.

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
| Warm incremental | `ccache -> distcc -> clang-22`, no pump | `sccache -> rustc` | no-pump mDNS host list | `make rebuild-terakan-distcc-no-rusticl-ccache-no-pump` |
| Cold clean | `distcc-pump -> distcc -> clang-22`, no ccache | `sccache -> rustc` | shell-derived `cpp,lzo` mDNS host list | `make rebuild-terakan-distcc-no-rusticl-pump` |

Warm/no-pump compiler wiring:

```ini
[binaries]
c   = ["ccache", "clang-22"]
cpp = ["ccache", "clang++-22"]
rust = ["sccache", "rustc"]
```

Cold/pump compiler wiring:

```ini
[binaries]
c   = ["distcc", "clang-22"]
cpp = ["distcc", "clang++-22"]
rust = ["sccache", "rustc"]
```

The critical distcc and Bobcat flags are:

```sh
export DISTCC_HOSTS="--randomize @build-worker.local/16,lzo localhost/2,lzo +zeroconf"
export CCACHE_PREFIX="/usr/bin/distcc"
export CCACHE_PATH="/usr/bin"
export CCACHE_DIR="$HOME/.cache/ccache"
export SCCACHE_DIR="$HOME/.cache/sccache"
export CFLAGS="-march=btver1 -mtune=btver1 -pipe -fno-emulated-tls"
export CXXFLAGS="$CFLAGS"
export LDFLAGS=""
```

`-fno-emulated-tls` is required for clang-22 on this host; without it, the Mesa
link can fail when generated emulated-TLS symbols do not match the ELF-TLS
references used by Mesa objects.

Do not put `ccache` or `sccache` in front of C/C++ distcc-pump.  Pump
needs distcc to see the original source and compiler command.  `sccache`
is still correct for Rust because it wraps rustc separately from the
C/C++ include-server path.

Workers that are known to demote pump mode stay out of the default pump
lane. Keep them in the classic no-pump mesh until object parity is proven;
opt in only for explicit parity probes.

## How

Sync the repo first:

```sh
cd "$(git rev-parse --show-toplevel)"
git fetch --all --prune
git pull --ff-only origin main
```

Use the warm lane for normal edit/build/probe loops:

```sh
cd "$(git rev-parse --show-toplevel)/build-infra"
make rebuild-terakan-distcc-no-rusticl-ccache-no-pump
```

Use the pump lane for cold clean builds where remote preprocessing is
worth more than cache hits:

```sh
cd "$(git rev-parse --show-toplevel)/build-infra"
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
sudo meson install --no-rebuild -C "$BUILDDIR"
```

Verify the installed artifacts:

```sh
find "$PREFIX" -maxdepth 5 -type f \
  \( -name "libvulkan_terascale.so" -o -name "r600_dri.so" -o -name "libgallium-*.so" -o -name "terascale_icd*.json" \) \
  -printf "%p %TY-%Tm-%Td %TH:%TM:%TS %s bytes\n" | sort
```

The Makefile sources `build-infra/env/*.env` for configure/build targets.
Use those env files as the canonical source of build lane policy rather
than ad hoc shell exports.

Retained host-specific run evidence belongs in the sibling evidence repository, not in this Mesa build policy file.
