# RS482/r300 GPU debug stack

Three driver tiers plus the GPU instrumentation tool set, for r300/RS482 work on
the radeon KMD (RS482 is the **radeon** kernel driver, not amdgpu -- every tool
below targets radeon).

## The three driver tiers

| tier | package | profile | install | use |
|---|---|---|---|---|
| **release** | `mesa-gororoba` | `1_r300_full_release` | system Mesa (/usr) | conformance + silicon evidence baseline (no asserts, -O2) |
| **standard debug** | `mesa-gororoba-debug` | `2_r300_full_debug` | system Mesa (/usr) | default install: live asserts, -O2 -g3, frame pointers, gdb/RADEON_DEBUG RCA |
| **maximal (ASan+UBSan)** | `mesa-gororoba-asan` | `7_r300_full_asanubsan_max_debug` | **scoped /opt, via wrapper** | memory + UB hunting; max info at the expense of speed |

Release and debug **become** the system Mesa (provides/conflicts/replaces, /usr
symlinks).  The ASan tier is the inverse: it stages only to
`/opt/mesa-26-gororoba-asan` and is **never** the system driver -- an ASan
`libGL`/`libvulkan_r300` as the default Mesa aborts every GL/VK client with "ASan
runtime does not come first in initial library list" and bricks the session.  It
co-installs alongside the others and is reached only through its wrapper.

## The maximal instrumentation (what ASan+UBSan adds)

`7_r300_full_asanubsan_max_debug` = `2_r300_full_debug` + `b_sanitize=address,undefined`,
`-O1` (ASan/UBSan target; -O0 is slower to build AND run on the 2-core K8 for no
extra coverage), `b_ndebug=false` (asserts live), `-g3 -fno-omit-frame-pointer`,
`b_lundef=false` (the sanitizer runtimes need undefined symbols at link),
`_FORTIFY_SOURCE` dropped (conflicts with ASan).  Ceiling is **address+undefined**:
TSan is mutually exclusive with ASan, MSan needs every linked lib instrumented.

- **ASan** catches heap/stack buffer overflows, use-after-free, double-free in the
  CPU draw path (it confirmed the PR#795 draw-module vertex-overrun fix is clean).
- **UBSan** catches the C undefined behaviour that the FP24 integer/limb work is
  prone to: signed overflow, shift-out-of-range, misaligned loads, bad enum/bool
  values -- the class the r300 NIR integer lowering and the multi-limb MAC touch.

## Running the ASan driver (the wrapper)

    mesa-26-gororoba-asan-run vkcube
    mesa-26-gororoba-asan-run glxgears
    ASAN_OPTIONS=detect_leaks=1 mesa-26-gororoba-asan-run <app>

The wrapper LD_PRELOADs the Clang ASan runtime (it must load first), points
`LIBGL_DRIVERS_PATH` + `VK_ICD_FILENAMES` + `LD_LIBRARY_PATH` at the /opt tree, and
sets maximal-info sanitizer options (`halt_on_error=0` collects every finding;
`detect_leaks=0` by default since Mesa's exit pools are noise -- override to 1 to
hunt leaks; UBSan prints a stack per finding).  Always read
`/proc/sys/kernel/random/boot_id` before/after a GPU run: stable = pure userspace
(no GPU reset); changed = a reset/reboot happened.

## The GPU instrumentation tool set (radeon KMD)

Each paired with how it composes with the ASan driver via the wrapper.

- **`RADEON_DEBUG`** (free, built-in, the single most useful): `fp` dumps the r300
  fragment program (alu_end = the 64-ALU-budget check), `cs` the command stream,
  `vm` the virtual-memory map.  `RADEON_DEBUG=fp,cs mesa-26-gororoba-asan-run <app>`.
- **`umr` (umr-gororoba)**: register / ring / IP-block inspection over radeon.
  The fork carries the RS482 ip_discovery-absent skip so it drives RS482 without
  the navi discovery path.  `sudo umr -O bits -r rs480.rs480.<reg>` etc.
- **`sudo dmesg`** (mandatory for GPU work, per AGENTS.md): the radeon CS validator
  rejects (`radeon: ... CS ...`) only show under sudo; discriminate a driver bug
  from a silicon limit here before symbolizing a crash.
- **`bpftrace`** on the radeon KMD ioctls (NOT amdgpu): trace DRM_IOCTL_RADEON_*,
  CS submits, BO maps live without recompiling, e.g.
  `sudo bpftrace -e 'tracepoint:gpu_scheduler:* { printf("%s\n", comm); }'`.
- **`radeontop`**: live GPU utilization / clock / memory; the clock-gap reads were
  silicon-falsified on RS482 (the real signal is mclk %).
- **`valgrind`**: complementary to ASan (run against the **debug** driver, not the
  ASan one -- don't stack valgrind on a sanitized binary); memcheck for the paths
  ASan instrumentation does not cover.
- **`gdb` / `pwndbg`**: break on draw, inspect r300 state; the debug + ASan tiers
  keep frame pointers + `-g3` so backtraces land on the true fault site.
- **`NIR_DEBUG` / `MESA_DEBUG` / `MESA_VK_ABORT_ON_DEVICE_LOST`**: NIR pass dumps,
  Mesa API error checks, and aborting on Vulkan DEVICE_LOST to catch the faulting
  submit instead of a silent loss.
- **`perf` / `heaptrack` / `strace -e ioctl`**: CPU profiling, allocation
  profiling in the draw path, and DRM ioctl tracing.

## Updating the stack coherently ("if one updates, all update")

All three driver profiles repack from the same source tree, so they share `pkgver`
(the mesa version, 26.2.0) and are rebuilt+bumped together:

    make rebuild-all     # rebuild release + debug + asan-max builddirs from current main
    make install-all     # repack + install all three (asan to /opt, others to system)

The meta-package `mesa-gororoba-gpu-debug-stack` pins the component versions
(`mesa-gororoba-asan`, `umr-gororoba`, and the tools) so installing it pulls the
whole stack; bumping the drivers + re-installing the meta-package re-pins them in
lockstep.  Pacman has no atomic "update all," so the all-target + the meta-package
pin are the idiom.
