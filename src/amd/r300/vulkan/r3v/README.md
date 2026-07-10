# r3v: experimental Vulkan on Radeon R300-class hardware

r3v is Mesa's experimental Vulkan ICD for the R300/R400/RS4xx family. It shares
the r300 Gallium driver and Radeon DRM winsys rather than implementing a second
hardware driver. The primary hardware target in this tree is the RS482 IGP in
the Dell Vostro 1000 (PCI 1002:5974).

This is an executable research driver, not a conformant Vulkan implementation.
Its status must be read from the code paths named below, not from historical
“loader skeleton” descriptions.

## What is implemented

The ICD currently provides Vulkan 1.0 instance/device enumeration, memory and
resource objects, descriptors, command recording, graphics-pipeline creation,
Gallium-mediated draw replay, transfers, render passes, dynamic state, queries,
and a deliberately restricted hybrid compute experiment.

RS480-family parts have no hardware vertex processor in r300g's chipset model.
The vertex stage therefore runs through Gallium Draw/software TCL; fragment
programs and fixed-function raster/ROP/ZB work still execute on the GPU.

The implemented submission backend is the Gallium-mediated path:

1. Vulkan objects and commands are recorded by r3v.
2. NIR shaders are passed into r300g, which owns the NIR-to-Radeon-compiler
   translation and state emission.
3. `r3v_queue.c` replays graphics or admitted compute-as-raster work through a
   `pipe_context`.
4. the Radeon winsys submits the resulting command stream to the kernel driver.

`R3V_CS_DIRECT_BACKEND_HAZARD_ACCEPTED=1` only selects an experimental direct-CS
request. A standalone native emitter is not implemented; r3v reports that gap
and falls back to the Gallium replay path.

## Conformance status

r3v advertises API version 1.0 with a zero conformance version. Both operating
modes are intentionally nonconformant:

| Mode | Queue flags | Status |
| --- | --- | --- |
| default | graphics + transfer | `experimental_nonconformant_graphics_without_compute` |
| `R3V_HYBRID_COMPUTE_EXPERIMENTAL=1` | graphics + transfer + compute on the same queue | `experimental_nonconformant_hybrid_compute_queue` |

The hybrid gate does not create a native compute engine and does not imply
general Vulkan compute support. It admits a bounded subset of NIR shapes that
can be lowered onto graphics-era hardware. Workgroup shared memory, arbitrary
shader atomics, arbitrary control flow, and general SSBO semantics are not
thereby implemented. Unsupported or unknown shapes produce a diagnostic and a
defined no-op dispatch rather than being silently miscompiled.

## Compute-as-raster architecture

The experiment decomposes selected compute operations across hardware and
orchestration mechanisms that already exist on RS482:

- fragment ALU and texture sampling for elementwise maps and gathered inputs;
- render-target export and host-staged copies for output materialization;
- blend/ROP paths for selected reductions and bit-exact logic;
- depth/stencil and ZPASS/query machinery for admitted counter/reduction shapes;
- multipass raster orchestration for scans, wide integer arithmetic, and
  explicitly recognized algebraic kernels.

The executable contract has four stages:

1. **Admission and classification** in
   `src/gallium/drivers/r300/r300_compute_admission.[ch]`.
2. **Route membership** in `r3v_pipeline_matched_raster_verb()`.
3. **Shader/state synthesis** in `r3v_synthesize_compute_shaders()`.
4. **Dispatch replay** in `r3v_replay_dispatch()` and the helpers declared by
   `r3v_identity_map.h`.

Those route sets must remain identical. The
`r3v-compute-verb-reachability` Meson test parses all three functions and fails
when a verb is added to one stage but omitted from another.

The current routes include families of constant fills, identity/unary/binary
maps, native transcendental maps, exact ROP logic, constant and per-element
logical shifts, exact multi-limb integer multiply, selected CAS/pooling/scan and
reduction shapes, gathered/predicated stores, IEEE-16 helper operations, and
recognized quaternion/octonion transforms. This paragraph is descriptive, not
the canonical inventory; the three executable functions above are authoritative.

## Numeric domains

Do not collapse all arithmetic into a single “FP24 compute” claim.

- R300 fragment arithmetic uses the hardware FP24 domain. FP16 texture/RT
  carriers can be represented within that domain, but an FP24 intermediate does
  not guarantee bit-identical IEEE-754 FP16 operation for every expression.
- Exact u32 multiplication is implemented by an explicit multi-limb route whose
  intermediate bounds fit the FP24 exact-integer envelope.
- Bitwise logic is implemented through the ROP carrier and does not inherit FP24
  arithmetic rounding.
- Stencil/ZPASS/blend behavior is route-specific and only supports kernels whose
  classifier and replay prove the needed addressing, ordering, and value domain.
- Native FP32 storage does not imply native FP32 fragment arithmetic.

Claims about PALM/Wrestler, R6xx/Evergreen, Terakan KCACHE, INT24, or soft-fp64
belong to their own generation lane and do not validate RS482 behavior.

## Build

A working r3v build needs both the ICD and the r300 Gallium backend:

```sh
meson setup builddir-r3v \
  -Dvulkan-drivers=ati_r300 \
  -Dgallium-drivers=r300 \
  -Dr3v-gallium-backend=true \
  -Dbuild-tests=true
meson compile -C builddir-r3v
```

Use the generated ICD manifest from the build directory or install it through
the normal Mesa install path. For the opt-in compute experiment:

```sh
export R3V_HYBRID_COMPUTE_EXPERIMENTAL=1
```

Exact string comparison is intentional; other values leave the gate disabled.

## Validation

Validation is layered because a successful build is not a silicon verdict:

1. Meson/unit tests, including admission and route-reachability checks.
2. Loader/device probes and focused API tests.
3. Render/dispatch probes with output oracles and retained kernel logs.
4. RS482 hardware runs under the `steinmarder-r300` hazard policy.
5. CTS only for behavior actually advertised and implemented; no conformance
   claim is made by this README.

The cross-repository ownership and evidence hierarchy is documented in
`docs/hardware/rs482-source-authority.md`. Mesa owns executable userspace code;
`radeon-custom` owns the out-of-tree kernel package; `steinmarder-r300` owns
retained probes, result bundles, and hardware verdicts.
