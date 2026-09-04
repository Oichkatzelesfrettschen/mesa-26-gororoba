# RS482/r300 GPU debug stack

Four driver tiers plus the GPU instrumentation tool set, for r300/RS482 work on
the radeon KMD (RS482 is the **radeon** kernel driver, not amdgpu -- every tool
below targets radeon).  The tiers are numbered by RCA priority.

## The four driver tiers

| # | tier | package | profile | install | use |
|---|---|---|---|---|---|
| **1_** | **prime** | `mesa-gororoba-debug-asan` | `1_r300_full_debug_asan_o0` | **scoped /opt, via wrapper** | ASan+UBSan+extended sanitizers, -O0; the highest-information forensic RCA |
| **2_** | **debug-O0** | `mesa-gororoba-debug-o0` | `2_r300_full_debug_o0` | **scoped /opt, via wrapper** | -O0 -g asserts, no sanitizer; deepest non-ASan stepping |
| **3_** | **debug-optimized** | `mesa-gororoba-debug-optimized` | `3_r300_full_debug_optimized` | **system Mesa (/usr), the DEFAULT** | live asserts, -O2 -g3, frame pointers, gdb/RADEON_DEBUG RCA |
| **4_** | **release** | `mesa-gororoba` | `4_r300_full_release` | system Mesa (/usr), **reserved** | conformance + silicon baseline (no asserts, -O2, no sanitizer) |

## LOCKED safety invariant: the sanitizer/-O0 tiers are never the system Mesa

Only **debug-optimized (3_)** and **release (4_)** may own the `/usr` Mesa paths
(provides/conflicts/replaces, /usr symlinks).  The **prime (1_)** and **debug-O0
(2_)** tiers stage ONLY to `/opt/mesa-gororoba-debug-asan` and
`/opt/mesa-gororoba-debug-o0` and are reached only through their run-wrappers.
An ASan `libGL`/`libvulkan_r3v` as the default Mesa aborts every GL/VK client with
"ASan runtime does not come first in initial library list" and can black-screen the
login session -- on the Vostro that needs physical recovery (no remote reboot).  The
invariant is enforced mechanically: `make audit-werror` fails if a scoped PKGBUILD
declares system-Mesa activation, and the prime/-o0 `package()` functions fail the
build if anything stages a `/usr` loader object or an `ld.so.conf.d` drop-in.

## The prime instrumentation (what tier 1_ adds)

`1_r300_full_debug_asan_o0` = the full r300 surface + `b_sanitize=address,undefined`
at **-O0** (single-case forensic fidelity over speed), `b_ndebug=false` (asserts
live), `-g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls`, `b_lundef=false`
(the sanitizer runtimes need undefined symbols at link), `_FORTIFY_SOURCE` dropped
(conflicts with ASan), plus the extended sanitizer checks outside Clang's
`undefined` group: `pointer-compare,pointer-subtract` (invalid-pointer-pair),
`integer` (signed+unsigned overflow/shift), `implicit-conversion`, `local-bounds`,
`nullability`, `float-divide-by-zero`, ASan use-after-scope/use-after-return, and
`-fstack-protector-all`.  Ceiling is **address+undefined+those**: TSan is mutually
exclusive with ASan, MSan needs every linked lib instrumented (both are separate
harness lanes, not driver installs).

- **ASan** catches heap/stack buffer overflows, use-after-free/scope/return,
  double-free, and invalid pointer pairs in the CPU draw path.
- **UBSan + extended** catch the C undefined and suspicious behaviour the FP24
  integer/limb work is prone to: signed/unsigned overflow, shift-out-of-range,
  misaligned loads, bad enum/bool values, implicit narrowing, local OOB.

## Running the prime driver (the wrapper)

    mesa-gororoba-debug-asan-run vkcube
    mesa-gororoba-debug-asan-run gdb --args vulkaninfo --summary
    ASAN_OPTIONS=detect_leaks=1 mesa-gororoba-debug-asan-run <app>

The wrapper LD_PRELOADs the Clang ASan runtime (it must load first), points
`LIBGL_DRIVERS_PATH` + `VK_ICD_FILENAMES` + `LD_LIBRARY_PATH` at the /opt tree, and
sets maximal-detection sanitizer options (`halt_on_error=0` collects every finding;
`detect_stack_use_after_return=1`, `check_initialization_order=1`,
`detect_invalid_pointer_pairs=2`, `strict_string_checks=1`; `detect_leaks=0` by
default since Mesa's exit pools are noise -- override to 1 to hunt leaks; UBSan
prints a stack per finding).  The `-O0` deepest-stepping non-sanitizer sibling is
`mesa-gororoba-debug-o0-run`.  Always read `/proc/sys/kernel/random/boot_id`
before/after a GPU run: stable = pure userspace (no GPU reset); changed = a
reset/reboot happened.

## The GPU instrumentation tool set (radeon KMD)

Each paired with how it composes with the prime driver via the wrapper.

- **`RADEON_DEBUG`** (free, built-in, the single most useful): `fp` dumps the r300
  fragment program (alu_end = the 64-ALU-budget check), `cs` the command stream,
  `vm` the virtual-memory map.  `RADEON_DEBUG=fp,cs mesa-gororoba-debug-asan-run <app>`.
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
  silicon-falsified on RS485M (the real signal is mclk %).
- **`valgrind`**: complementary to ASan (run against the **debug-optimized** or
  **debug-O0** driver, NOT the prime -- don't stack valgrind on a sanitized binary);
  memcheck for the paths ASan instrumentation does not cover.
- **`gdb` / `pwndbg`**: break on draw, inspect r300 state; every debug tier keeps
  frame pointers + `-g3`, and the -O0 tiers (1_/2_) keep the 1:1 source mapping so
  backtraces land on the true fault site.
- **`NIR_DEBUG` / `MESA_DEBUG` / `MESA_VK_ABORT_ON_DEVICE_LOST`**: NIR pass dumps,
  Mesa API error checks, and aborting on Vulkan DEVICE_LOST to catch the faulting
  submit instead of a silent loss.
- **`perf` / `heaptrack` / `strace -e ioctl`**: CPU profiling, allocation
  profiling in the draw path, and DRM ioctl tracing.

## Updating the stack coherently ("if one updates, all update")

All four driver tiers build the same source tree, so they share `pkgver` (the mesa
version, 26.2.0) and `epoch` (2) and are rebuilt together:

    make rebuild-all-tiers    # rebuild all four r300 tier builddirs from the current tree

Then repack each (`for p in mesa-gororoba-debug-asan mesa-gororoba-debug-o0
mesa-gororoba-debug-optimized mesa-gororoba umr-gororoba mesa-gororoba-debug-tools;
do (cd packaging/$p && makepkg -f); done`).  Install the prime + -o0 to /opt and the
debug-optimized (or release) as the /usr system Mesa.  The meta-package
`mesa-gororoba-debug-tools` depends on the prime driver + umr + tools and
optdepends the other tiers, so installing it pulls the whole RCA stack alongside
whatever system driver is active.  Pacman has no atomic "update all," so the
rebuild target + the meta-package pin are the idiom.  `make audit` (which runs
`audit-werror`) gates that every profile keeps warnings-as-errors and the scoped
tiers never gain /usr activation.
