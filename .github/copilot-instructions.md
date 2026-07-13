# Copilot instructions for mesa-26-gororoba

## Build, test, and lint commands

Use the repository's `build-infra` entrypoint for day-to-day work.

| Task | Command |
|---|---|
| List supported profiles/host envs | `make -C build-infra list` |
| Host/toolchain audit before building | `make -C build-infra audit PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc` |
| Configure + build + install (profile lane) | `make -C build-infra clean configure build install PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc` |
| Incremental rebuild (warm lane) | `make -C build-infra rebuild-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc` |
| Run Meson/Ninja test target for current builddir | `make -C build-infra test PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc` |
| Run full Meson tests directly | `meson test -C <builddir> --print-errorlogs` |
| Run a single Meson test | `meson test -C <builddir> --list` then `meson test -C <builddir> <test_name> --print-errorlogs` |
| Run one r3v validation probe | `VK_ICD_FILENAMES=<r3v_icd.json> DEQP_VK=<deqp-vk> OUT=<output-parent> bash build-infra/conformance/r3v-vulkan-surface/run.sh --check` |
| Shell script lint pass used in CI | `./.gitlab-ci/run-shellcheck.sh` |
| CI Python checks (`pytest` + `flake8`) | `./.gitlab-ci/run-pytest.sh` |
| CI config lint checks | `python3 bin/toml_lint.py && ./.gitlab-ci/run-yamllint.sh` |

Fallback baseline (upstream Mesa style) remains valid when not using profile lanes:

```bash
meson setup build
ninja -C build
sudo ninja -C build install
```

## High-level architecture

This repository is a Mesa 26.x fork with custom driver and build lanes layered on top of upstream Mesa structure.

| Area | Role |
|---|---|
| `src/amd/terascale/vulkan/` | Terakan Vulkan driver lane for TeraScale-era hardware work. |
| `src/gallium/drivers/r300/` and `src/gallium/drivers/r600/` | Gallium driver backends touched by fork-specific bring-up and conformance work. |
| `src/gallium/auxiliary/vl/` | Shared video/decode infrastructure used by r300/r600 video paths (including g3dvl-related work). |
| `src/amd/r300/vulkan/r3v/` | R300 Vulkan research lane and probe tooling (`tests/` wrappers). |
| `build-infra/` | Canonical orchestration layer (Makefile + Meson native profiles + host env overlays). Meson owns configure/build internals; Make owns profile/host selection and orchestration targets. |
| `docs/submittingpatches.rst` | Upstream Mesa patch/testing expectations used as review baseline (component prefixes, tested commits, clean history). |

Important boundary: this repo can reference sibling `steinmarder` evidence, but normal Mesa build/install/test flows must stay standalone and must not depend on sibling paths at runtime.

## Key conventions in this codebase

1. `AGENTS.md` is the canonical AI/developer policy. `CLAUDE.md` and `GEMINI.md` are thin wrappers that import `AGENTS.md`; do not duplicate or diverge rules across wrappers.
2. Keep the Meson/Make split intact: Meson handles project configuration and Ninja generation; `build-infra/Makefile` handles profile/host orchestration and audits.
3. Use durable mechanism naming in branches/commit subjects/PR titles/comments (no phase/wave/session naming).
4. For source comments, document mechanism constraints (spec/kernel/hardware behavior), not workflow history. Avoid task IDs, private paths, or time-relative wording in source comments.
5. Preserve file language and standards per translation unit (C stays C, C++ stays C++; do not switch TU language).
6. Prefer repository-relative paths and hostname-based infra config; do not hardcode private absolute paths or raw IPs into tracked files.
7. Keep commits reviewable and scoped: component-prefixed titles, no formatting churn mixed with behavior changes, and explicit validation notes when tests are skipped/blocked.
8. For AI attribution in new commits, follow Mesa policy from `docs/submittingpatches.rst` and local policy in `AGENTS.md` (`Assisted-by:`/`Generated-by:` flow; do not use AI `Co-authored-by:`).
9. Prefer tool-backed analysis over hand analysis. Hand analysis is allowed only for lowest-token-budget cases with high confidence; otherwise, use source-map, structural, static, runtime, or RE tooling first.
10. Enforce a NIR-first compiler direction: do not introduce new TGSI paths. When TGSI is encountered, use tooling to analyze callsites and translation boundaries, then refactor and rewire the path to NIR.

### NIR-first migration rule (no new TGSI)

When work touches shader/frontend/compiler flow:

1. Do not add new TGSI entrypoints, intermediates, or lowering dependencies.
2. Mine existing Mesa NIR patterns first (same subsystem/adjacent drivers) and reuse those patterns before introducing custom abstractions.
3. For any encountered TGSI surface, perform tool-backed mapping (`rg`, `clangd`/xref, structural sweep), identify ownership and call boundaries, and migrate/rewire toward pure NIR.
4. Keep migration patches mechanism-scoped and reviewable (one boundary/class of TGSI dependency per patch where practical).

## Deduplication rules

| Canonical name | Folded names / rule |
|---|---|
| `ast-grep` | Includes `sg` only when `sg` is the ast-grep CLI alias. |
| `ctags` | Means Universal Ctags unless stated otherwise. |
| `global` | Includes `gtags` and GNU Global query commands. |
| `coccinelle` | Includes the executable `spatch`. |
| `radare2` | Includes `r2`, `radiff2`, and r2 scripting surfaces. |
| `systemtap` | Includes the executable `stap`. |
| `afl++` | Includes the `afl-fuzz` workflow. |
| `perfetto` | Includes `trace_processor` when that provider is installed. |
| `lizard` | Means the complexity analyzer; on Arch/CachyOS prefer package `python-lizard`. |
| `crucible` | Means Mesa's Vulkan test harness, not the unrelated AUR launcher. |
| `flashrom` | Baseline ROM utility; `flashprog` is tracked as an additional candidate/fork. |

## Workflow blocks

| Block | Purpose | Primary tools |
|---|---|---|
| Source map | Find symbols, callers, definitions, history, and file slices. | `git`, `gh`, `rg`, `fd`, `clangd`, `rust-analyzer`, `ctags`, `global`, `cscope`, `tree-sitter-cli`, `cflow` |
| Structural sweep | Find code shapes and mechanically rewrite proven patterns. | `ast-grep`, `weggli`, `comby`, `semgrep`, `coccinelle` |
| Static flow | Check cross-function, path-sensitive, kernel, and CPG claims. | `sparse`, `smatch`, `scan-build`, `clang-tidy`, `cppcheck`, `infer`, `codeql`, `joern` |
| Runtime evidence | Debug crashes, trace scheduling, measure costs, and verify live behavior. | `gdb`, `lldb`, `rr`, `valgrind`, `strace`, `ltrace`, `perf`, `bpftrace`, `trace-cmd`, `lttng`, `systemtap` |
| Binary and firmware RE | Disassemble, decompile, diff, unpack, and inspect ROM/firmware blobs. | `ghidra`, `rizin`, `radare2`, `retdec`, `lief`, `binwalk`, `yara`, `chipsec`, `uefitool`, `atomdis-git` |
| GPU and graphics validation | Probe radeon registers, replay graphics workloads, and run GL/VK tests. | `umr-gororoba`, `radeon_bar2_read`, `r600-recovery-tools`, `piglit-distrobox`, `waffle-distrobox`, `gfxreconstruct`, `apitrace`, `renderdoc`, `vkmark`, `vkpeak` |
| Fuzzing | Mutate command streams, parsers, firmware inputs, and validator harnesses. | `afl++`, `honggfuzz`, `radamsa` |
| Data and reports | Analyze probe output, TSV/CSV/JSON, traces, and provenance databases. | `python3`, `jq`, `sqlite3`, `gnuplot`, `numpy`, `pandas`, `scipy` |
| Build and package | Reproduce builds, package tools, stage artifacts, and verify payloads. | `make`, `cmake`, `meson`, `ninja-build`, `pkg-config`, `makepkg`, `pacman`, `paru`, `apt-cache`, `dpkg-query`, `distrobox` |
| Remote lab | Keep sessions, forward GUIs, and share temporary collaborator access. | `mosh`, `tmux`, `zellij`, `tailscale`, `tmate`, `syncthing`, `openvscode-server`, `waypipe`, `xpra` |

## Source map and indexing

| Program | Description |
|---|---|
| `git` | Version control, history search, blame, `git grep`, and source provenance. |
| `gh` | GitHub CLI for issues, PRs, repositories, and automation glue. |
| `rg` | Fast literal and regex search; fallback when AST tools do not apply. |
| `fd` | Fast file discovery for source-tree slicing and script input lists. |
| `clangd` | C/C++ LSP for definitions, references, call hierarchy, and type information. |
| `clangd-index` | Offline clangd index surface when available. |
| `rust-analyzer` | Rust LSP for Rusticl, Naga, and auxiliary Rust tooling. |
| `ctags` | Universal Ctags symbol database for macros, prototypes, and editor tags. |
| `readtags` | Query helper for Universal Ctags tag files. |
| `global` | GNU Global source cross-reference database; durable `global -x` and `global -r` queries. |
| `cscope` | C caller/callee and symbol cross-reference database. |
| `tree-sitter-cli` | AST query substrate for C, C++, Rust, and custom structural tooling. |
| `cflow` | Static C call graph generator. |
| `ag` | Silver Searcher text search; keep as legacy helper, prefer `rg`. |

## Structural search and rewrite

| Program | Description |
|---|---|
| `ast-grep` | Tree-sitter-backed structural search and rewrite for source-shape queries. |
| `weggli` | C/C++ structural expression search with metavariable capture. |
| `comby` | Language-aware structural search and mechanical rewrite tool. |
| `semgrep` | Rule-based source pattern analysis with metavariables and fixture calibration. |
| `coccinelle` | Semantic patching for kernel-style and large C-tree transformations. |

## Static analysis and code quality

| Program | Description |
|---|---|
| `clang` | Compiler frontend for syntax, diagnostics, and build reproduction. |
| `clang-tidy` | C/C++ lint and bug-prone-pattern checks. |
| `scan-build` | Clang static analyzer driver for path-sensitive checks. |
| `cppcheck` | Broad C/C++ static analyzer. |
| `cppcheck-htmlreport` | HTML report generator for `cppcheck` output. |
| `infer` | Interprocedural static analyzer; useful when compile commands are reliable. |
| `sparse` | Kernel-oriented C type checker for address spaces, endian tags, and annotations. |
| `smatch` | Kernel static analyzer for cross-function flow, locking, and integer defects. |
| `codeql` | Code property graph and data-flow query engine. |
| `joern` | Code property graph platform for AST/CFG/DDG reachability queries. |
| `rats` | Lightweight C/C++ security-pattern scanner. |
| `lizard` | Complexity, function length, parameter count, and nesting metrics. |
| `cloc` | Source, comment, and blank line accounting. |
| `scc` | Fast source tree code-count and language inventory. |
| `ruff` | Fast Python linter and formatter. |
| `pylint` | Deeper Python static analysis and style checking. |
| `bear` | Build wrapper that emits `compile_commands.json`. |

## Runtime debugging and tracing

| Program | Description |
|---|---|
| `gdb` | GNU debugger for native processes and core files. |
| `lldb` | LLVM debugger. |
| `pwndbg` | GDB enhancement package for crash and binary triage. |
| `gef` | GDB enhancement package for exploit-style and low-level debugging. |
| `gdbgui` | Browser frontend for GDB over a remote lab link. |
| `rr` | Record/replay debugger for deterministic userspace debugging. |
| `strace` | Syscall tracer for userspace process behavior. |
| `ltrace` | Library-call tracer for dynamic-linker and userspace ABI inspection. |
| `valgrind` | Dynamic memory checking and instrumentation suite. |
| `callgrind_annotate` | Callgrind profile annotation utility. |
| `gprof2dot` | Converts profiling output into Graphviz call graphs. |
| `addr2line` | Maps instruction addresses to source file and line locations. |
| `perf` | Linux performance counters, profiling, and trace capture. |
| `bpftrace` | eBPF one-liners for kernel/runtime tracing. |
| `bcc-tools` | BPF Compiler Collection tracing helpers. |
| `trace-cmd` | ftrace recording and inspection tool. |
| `lttng` | Low-overhead Linux tracing framework. |
| `lttng-tools-generic-kernel` | REkit-preferred tools-only LTTng package lane for Arch-derived hosts. |
| `systemtap` | Dynamic instrumentation framework; executable `stap`. |
| `sysprof` | System profiler and trace viewer. |
| `perfetto` | Trace capture, conversion, and visualization stack. |
| `kernelshark` | GUI trace viewer for ftrace/trace-cmd data. |
| `gpuvis-distrobox` | REkit-staged GPU/ftrace timeline viewer built in the Debian container lane. |
| `heaptrack` | Allocation profiler for C/C++ processes. |
| `hotspot` | GUI flamegraph and perf-data viewer. |
| `frida` | Dynamic instrumentation framework for live userspace probing. |
| `python-frida` | Python bindings for Frida instrumentation. |
| `python-frida-tools` | Frida command-line helpers and Python tooling. |
| `python-ptrace` | Scriptable ptrace library for process tracing and crash triage. |

## Binary, firmware, and low-level RE

| Program | Description |
|---|---|
| `objdump` | Binary disassembly and object inspection from binutils. |
| `nm` | Symbol table inspection from binutils. |
| `readelf` | ELF header, section, relocation, and dynamic-symbol inspection. |
| `ghidra` | Full reverse-engineering platform with disassembly, decompilation, and scripting. |
| `rizin` | Scriptable reverse-engineering framework. |
| `rz-cutter` | GUI front-end for rizin; executable may be `cutter`. |
| `radare2` | Reverse-engineering framework; includes `r2` and `radiff2` surfaces. |
| `r2ghidra` | Ghidra decompiler integration for radare2. |
| `retdec` | Decompiler and binary-analysis toolkit. |
| `ida-free` | Free IDA disassembler/decompiler tier for host-side inspection. |
| `binaryninja-free` | Optional Binary Ninja free-tier GUI disassembler. |
| `iaito` | Qt GUI front-end for radare2. |
| `angr` | Python binary-analysis and symbolic-execution framework. |
| `klee` | Symbolic execution engine for LLVM bitcode. |
| `lief` | Library for parsing and modifying ELF/PE/Mach-O binaries. |
| `python-lief` | Python bindings for LIEF. |
| `binwalk` | Firmware and blob extraction utility. |
| `firmware-mod-kit` | Firmware extraction and modification helper suite. |
| `yara` | Byte-pattern and signature matching engine. |
| `chipsec` | Platform firmware and chipset security analysis framework. |
| `uefitool` | UEFI firmware parser and extractor. |
| `uefitool-cli` | UEFIExtract, UEFIReplace, and UEFIPatch command-line tools. |
| `uefi_firmware` | Python UEFI firmware parser package. |
| `firmwalker` | Shell-based sweep for keys, certificates, URLs, and firmware secrets. |
| `atomdis-git` | AMD/ATI AtomBIOS bytecode disassembler. |
| `yaabe-git` | AMD/ATI AtomBIOS editor/parser; use read-only unless a write policy exists. |
| `pcileech` | PCIe-side memory/MMIO access and forensic tool. |
| `flashrom` | Firmware flash read/write utility; preferred over unsafe vendor flashers. |
| `flashprog` | Candidate flashrom fork/package for read-only ROM dump workflows. |
| `radeontool` | Legacy low-level Radeon register and BIOS-table poking utility. |
| `rhd_conntest` | Legacy xf86-video-radeonhd utility used in ATI VBIOS and connector workflows. |
| `rhd_dump` | Companion utility from the RadeonHD tool set. |

## GPU, graphics, and shader validation

| Program | Description |
|---|---|
| `umr-gororoba` | REkit UMR fork with radeon BAR access and Palm/Wrestler register probing. |
| `umr-git` | Upstream/AUR UMR candidate; superseded by `umr-gororoba` for Palm. |
| `radeon_bar2_read` | Small standalone BAR2 register dumper and SQ counter sanity probe. |
| `r600-recovery-tools` | Palm recovery, watchdog, health canary, and induced-lockup probe bundle. |
| `radeon-palm-gate-dkms` | DKMS package for Palm-gated radeon kernel-module patches. |
| `gfxreconstruct` | Vulkan capture and replay tool. |
| `apitrace` | OpenGL trace capture and replay tool. |
| `renderdoc` | Graphics debugger and capture tool. |
| `waffle-distrobox` | REkit-staged Waffle/wflinfo build for GL/EGL/GBM capability checks. |
| `piglit-distrobox` | REkit-staged Piglit OpenGL conformance and regression test suite. |
| `crucible` | Mesa Vulkan test harness; use Mesa's source project. |
| `mesa-shader-db` | Mesa shader corpus and shader-compiler regression harness. |
| `mesa-amber` | Legacy Mesa driver package for fallback GL comparisons. |
| `vkpeak` | Vulkan device peak-capacity and bandwidth profiler. |
| `vkmark` | Vulkan benchmark/canary workload. |
| `vulkan-tools` | Vulkan command-line utilities such as `vulkaninfo`. |
| `vulkan-validation-layers` | Vulkan validation layer package. |
| `spirv-tools` | SPIR-V assembler, disassembler, validator, and optimizer. |
| `spirv-cross` | SPIR-V reflection and cross-compiler tool. |
| `glslang` | GLSL/HLSL to SPIR-V compiler front-end. |
| `glslang-tools` | Debian command-line package for glslang utilities. |
| `shaderc` | Shader compiler library and command-line utilities. |
| `mangohud` | Runtime overlay for GPU metrics and benchmark annotation. |
| `drm-info` | DRM/KMS capability and connector/state inspection tool. |
| `gpu-fpu-stress` | Proposed small workload to hold shader ALUs busy for counter experiments. |
| `intel-gpu-tools` | Mostly Intel-specific DRM/GPU utility collection; mine only generic DRM helpers if needed. |
| `bochs` | Full-system x86 emulator useful for pre-hardware sanity checks. |

## Fuzzing and mutation

| Program | Description |
|---|---|
| `afl++` | Coverage-guided fuzzing suite; includes the `afl-fuzz` workflow. |
| `honggfuzz` | Feedback-driven fuzzing engine. |
| `radamsa` | General-purpose mutation fuzzer and seed generator. |
| `sandsifter` | CPU instruction-decoder fuzzer; belongs to CPU/Bobcat lanes, not GPU REkit. |

## Data and probe-result analysis

| Program | Description |
|---|---|
| `python3` | Primary scripting and analysis runtime. |
| `jq` | JSON query and transformation tool. |
| `sqlite3` | SQLite shell for provenance and probe-result inspection. |
| `gnuplot` | Quick plotting for CSV and time-series data. |
| `numpy` | Python numerical array library. |
| `pandas` | Python tabular data and CSV/TSV analysis library. |
| `scipy` | Python scientific-computing library. |
| `python3-mako` | Python templating dependency used by graphics test/build tools. |
| `python3-six` | Python 2/3 compatibility dependency for legacy tooling. |
| `python3-capstone` | Python bindings for the Capstone disassembly engine. |
| `python3-ropgadget` | Python ROP gadget finder package. |

## Build and packaging support

| Program | Description |
|---|---|
| `make` | Conventional build orchestration and local targets. |
| `cmake` | CMake configure/build system for local source packages. |
| `meson` | Meson configure/build system for Mesa-adjacent projects. |
| `ninja-build` | Ninja backend used by Meson and many CMake builds. |
| `pkg-config` | Compiler and linker flag discovery for installed libraries. |
| `build-essential` | Debian compiler and base build-tool metapackage. |
| `ca-certificates` | Certificate bundle needed for HTTPS package/source fetches. |
| `wget` | Simple source/archive fetch utility. |
| `binutils` | ELF/binutils suite; includes object and symbol utilities. |
| `file` | File-type and ELF identification utility. |
| `makepkg` | Arch/AUR package build tool used by REkit AUR staging. |
| `pacman` | Arch/CachyOS package manager and read-only catalog query surface. |
| `paru` | AUR helper used for package discovery and host installs. |
| `yay` | Alternate AUR helper visible in discovery surfaces. |
| `apt-cache` | Debian package metadata query tool. |
| `dpkg-query` | Debian installed-package query tool. |
| `distrobox` | Container wrapper for Debian/trixie build lanes. |
| `distcc` | Distributed C/C++ compilation helper. |
| `ccache` | Compiler result cache. |
| `sccache` | Shared compiler cache for C/C++/Rust build acceleration. |

## Remote lab and collaboration

| Program | Description |
|---|---|
| `mosh` | Resilient SSH-like remote shell for flaky links. |
| `tmux` | Terminal multiplexer for persistent sessions. |
| `zellij` | Modern terminal multiplexer. |
| `tailscale` | WireGuard mesh overlay for remote lab access. |
| `tmate` | Temporary shared terminal sessions. |
| `syncthing` | Peer-to-peer evidence and workspace synchronization. |
| `openvscode-server` | Browser IDE for remote collaborator workflows. |
| `waypipe` | Wayland application forwarding. |
| `xpra` | X11/GUI application forwarding. |

## MCP and skills setup

For this repository, configure and prioritize Semgrep-centric MCP workflows for security/static-analysis passes.

1. MCP server priority for security/code-quality sweeps:
   - `mcp-semgrep-workflow` (primary for rule-driven security/static checks).
   - `mcp-ripgrep-workflow` + `mcp-filesystem-workflow` for fast triage and deterministic file operations.
   - `mcp-git-workflow` for history/diff/branch inspection.
2. Skill invocation rule:
   - When a task matches an available MCP skill, invoke the skill first instead of manual ad-hoc shell analysis.
3. Suggested skill set for this repo:
   - `mcp-semgrep-workflow`
   - `mcp-ripgrep-workflow`
   - `mcp-filesystem-workflow`
   - `mcp-git-workflow`
   - `mcp-bash-workflow`
   - `mcp-context7-workflow` (API/library lookup)
