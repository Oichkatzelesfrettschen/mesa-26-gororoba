# RS485M/r300 GPU debug stack

The r300/RS485M debug profiles target the radeon kernel driver. Qualification
uses separate build roots for each optimization and instrumentation profile.

| Profile | System package | Execution boundary |
|---|---|---|
| ASan/UBSan O0 | Build experiment | `BUILD_ROOT/prefix`, scoped runtime launcher |
| Ordinary O0 | `mesa-gororoba-debug-o0` | Mutually exclusive `/usr` replacement or build staging |
| Debugoptimized | `mesa-gororoba-debug-optimized` | Mutually exclusive `/usr` replacement or build staging |
| Release | `mesa-gororoba` | Mutually exclusive `/usr` replacement or build staging |

System packages own real files at stock paths. The shared package gate rejects
sanitized configurations because uninstrumented clients require the sanitizer
runtime before the driver. The ASan launcher preloads that runtime and selects
only the explicit experimental prefix. Instrumentation tools install independently
of any driver variant. Release runtime observations retain their distinct
conformance/silicon evidence class; debug builds change assertion and timing behavior.

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

    build-infra/packaging/mesa-gororoba-debug-asan/mesa-gororoba-debug-asan-run "$MESA_BUILD_ROOT/prefix" vkcube
    build-infra/packaging/mesa-gororoba-debug-asan/mesa-gororoba-debug-asan-run "$MESA_BUILD_ROOT/prefix" gdb --args vulkaninfo --summary
    ASAN_OPTIONS=detect_leaks=1 mesa-gororoba-debug-asan-run "$MESA_BUILD_ROOT/prefix" <app>

The wrapper LD_PRELOADs the Clang ASan runtime (it must load first), points
`LIBGL_DRIVERS_PATH` + `VK_ICD_FILENAMES` + `LD_LIBRARY_PATH` at the selected build-owned staging tree, and
sets maximal-detection sanitizer options (`halt_on_error=0` collects every finding;
`detect_stack_use_after_return=1`, `check_initialization_order=1`,
`detect_invalid_pointer_pairs=2`, `strict_string_checks=1`; `detect_leaks=0` by
default since Mesa's exit pools are noise -- override to 1 to hunt leaks; UBSan
prints a stack per finding).  The `-O0` deepest-stepping non-sanitizer sibling is
`mesa-gororoba-debug-o0-run`.  Always read `/proc/sys/kernel/random/boot_id`
before/after a GPU run to identify a reboot. Kernel DRM logs supply GPU-reset
evidence independently of the boot identifier.

## The GPU instrumentation tool set (radeon KMD)

Each paired with how it composes with the prime driver via the wrapper.

- **`RADEON_DEBUG`** (free, built-in, the single most useful): `fp` dumps the r300
  fragment program (alu_end = the 64-ALU-budget check), `cs` the command stream,
  `vm` the virtual-memory map.  `RADEON_DEBUG=fp,cs mesa-gororoba-debug-asan-run "$MESA_BUILD_ROOT/prefix" <app>`.
- **`umr` (umr-gororoba)**: register / ring / IP-block inspection over radeon.
  The fork carries the RS485M ip_discovery-absent skip so it drives RS485M without
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

All four qualification profiles build one source revision in separate build roots.
`make rebuild-all-tiers` requires the detached source/control selectors documented
in `build-infra/README.md`. Packaging selects one unsanitized native profile with
the shared stock overlay; install only the selected system variant through pacman.
The debug-tools package depends on UMR and instrumentation tools. Sanitizer
experiments remain in the build root and run through the source-tree launcher.

The stock package gate checks source identity, registered tests, R3V advertised
surface, loader objects, implicit layer pairs, pkg-config paths, and complete
payload hashes. The configured profile preserves codec and runtime hazard gates.
