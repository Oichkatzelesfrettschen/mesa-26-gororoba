# A stable hybrid vertex TCL for RS482, built up from SW-TCL

The RS482 IGP has no hardware transform-and-lighting: `r300_chipset.c` leaves
`num_vert_fpus = 0` for the RS480/RS482/RS485 family, so `has_tcl` is false and
the programmable vertex shader (PVS / SE_TCL) block behind the register window
`0x2200-0x2504` is architecturally absent, not merely clock-gated. Any attempt
to drive it wedges the part with no software recovery. This document decomposes
the vertex pipeline so the transform runs where it is safe -- SW plus the one
programmable ALU the chip does have -- and the hardware vertex frontend only
ever receives vertices it can pass straight through.

## Evidence tiers (morphemic convention)

This document reserves **proven** for claims formally verified in Rocq in the
`open_gororoba` repository (`proofs/theories/*.v`, checked, zero admits) -- the
mathematics: Cayley-Dickson algebra, quaternion and octonion norms, quaternion
matrix-rotation equivalence, rotation norm preservation, and the FP24 DP4
exact-integer bound. Everything about the RS482 silicon -- that a draw executes,
that the wedge is in the VAP/GA, that a register shape submits hang-free, that
MRT export is byte-exact, that the transform runs at a measured rate -- is
**demonstrated on silicon** or **measured**, never "proven." A finite hardware
run demonstrates; it does not prove.

## Proof-carrying algebra kernels

The exceptional-algebra transforms this design runs on the fragment ALU
(quaternion rotation, the octonion and Cayley-Dickson kernels, the MVP as
DP4s) become *proven* -- not merely demonstrated -- by construction, along a
concrete path already scaffolded in `open_gororoba`:

1. **Derive from first principles in Rocq.** State the kernel over an abstract
   `FLOAT_OPS` signature and prove its algebraic laws (Hurwitz norm
   multiplicativity, quaternion rotation as conjugation, Cayley-Dickson
   doubling) against the existing theories (`Quaternion.v`, `OctonionNorm.v`,
   `HurwitzTheorem.v`, `SchaferDivAlg16.v`), machine-checked with zero admits.
   For the driver lane, `proofs/theories/QuatRotationLaws.v` proves both
   `quat_rotate_eq_matrix_rotate` (the 3x3 matrix form equals conjugation by a
   unit quaternion) and `quat_rotate_preserves_norm` (the rotation preserves
   `vec3_norm_sq`). `Print Assumptions` reaches only the standard Reals
   axiomatization. The same facts were already present in the claim ledger at
   `verified/C876_QuaternionRotation.v` and the corresponding C911 norm law;
   the driver-lane file makes the dependency first-class instead of relying on a
   ledger re-export.
2. **Compile through verified extraction lanes.** The main proof switch remains
   the working Rocq 9.1.1 / OCaml 5.4 environment, but the verified compiler
   lanes live in dedicated switches so they do not perturb that baseline:
   `rocq-verified-extraction` supplies verified MetaRocq erasure to OCaml in an
   OCaml 5.3 switch, and `rocq-certirocq` (CertiRocq 0.9.1+9.1) supplies the
   Rocq-9.1 Gallina-to-CompCert-Clight/C lane in an OCaml 4.14 switch. The
   OCaml-4 switch is required because the CertiRocq opam package depends on
   CompCert (`coq-compcert` 3.17), and CompCert remains an OCaml-4.x package.
   The result is not future-blocked: verified Rocq-to-C/Clight is available on
   the current Rocq line, just not inside the OCaml-5.4 proof switch.
3. **Two named boundaries remain explicit.** (a) The
   `FLOAT_OPS`-to-concrete-float mapping: for vertex math held in the FP24
   exact-integer window -- Qm.f (fixed-point with m integer bits and f fractional bits) with `m + f <= 17` -- it reduces to integer
   arithmetic whose exactness is *proven* (`IDCT8DP4ExactBound.v`:
   `8*1448^2 < 2^24`, `2^17 < 2^24`), closing this boundary in that window;
   outside it, it is a bounded worst-case-ulp boundary. (b) The generated-code
   boundary is collapsed only for kernels actually accepted by CertiRocq and
   recorded with their generated C/Clight artifact, runtime/ABI assumptions, and
   extraction command. Until a kernel takes that lane, a hand C translation
   remains only demonstrated by differential test.

So the honest standing per kernel is tiered. The *algebra* is proven in Rocq;
verified OCaml erasure is available through `rocq-verified-extraction`; verified
C/Clight generation is available through CertiRocq in the dedicated OCaml-4.14
switch; and *silicon execution* remains demonstrated. For a transform kernel
kept in the FP24 exact-integer window and emitted by CertiRocq, the algebra
proof plus the FP24 exactness proof plus the verified extraction/compilation
path compose into a proven generated C/Clight kernel. The driver integration,
runtime ABI, and RS482 execution stay separately demonstrated or measured.

## What actually wedges (RS482 vertex frontend)

A `glClear` of a color framebuffer (piglit `fbo-clearmipmap`) hangs the GPU.
Live instrumentation localized it precisely:

- `strace` on the DRM ioctls: `DRM_IOCTL_RADEON_CS` returns `0` (the kernel
  accepts the submission, fence seqno emitted); the hang is downstream in
  `DRM_IOCTL_RADEON_GEM_WAIT_IDLE` -> `EBUSY` -> `ERESTARTSYS`. The GPU never
  finishes executing the command stream and the fence never signals.
- `RBBM_STATUS` sampled during the wedge: `VAP_BUSY = 100%`, `GA_BUSY = 100%`,
  and every other block (`RB3D`, `RE`, `TAM`, `PB`, `E2`) at `0%`. The stall is
  in the vertex assembly processor and geometry assembly -- the vertex
  frontend -- not the pixel path.
- `RADEON_DEBUG=nocbzb` still hangs and emits zero CBZB clears: the color-clear
  ZB-aliasing path is refuted.

This is not the "hardware-TCL first draw hangs the ring" hazard: that requires
forcing `R300_HB_TCL=1` (an experimental harness) which flips `has_tcl = true`
and routes draws through `r300_emit_vs_state`. On the default path `has_tcl` is
false, `r300_create_rs_state` unconditionally sets `VAP_CNTL_STATUS.TCL_BYPASS`
(bit 8, `0x2140`) and `VAP_CLIP_CNTL = CLIP_DISABLE` (bit 16, `0x221C`, written
never read), and `r300_emit_vs_state` / `r300_emit_vs_constants` are never
called -- so SW-TCL issues no MMIO into the `0x2200-0x2504` wedge window by
construction. The clear-quad draw and an ordinary r3v triangle draw take the
identical `r300_swtcl_draw_vbo` path through the identical emit-atom array; r3v
itself calls the same `r300_clear`. The divergence is therefore a value in the
bypass-mode VAP/GA vertex assembly of the fullscreen clear quad, not a
different code path and not a hardware-TCL engagement. The VAP is stuck
assembling or passing a vertex stream whose descriptor does not match what the
frontend expects, so it waits forever (`VAP_BUSY = 100%`).

## The demonstrated-safe bypass shape

One bypass draw is known pixel-exact and hang-free on this silicon: the R2VB
direct-VAP handoff. Its register shape is the convergence target for any draw
on RS482:

- `VAP_CNTL_STATUS.TCL_BYPASS` set: the VAP passes pre-transformed vertices
  straight to setup without engaging T&L.
- `VAP_CLIP_CNTL = CLIP_DISABLE`: clip is owned by the CPU/Draw side.
- `VAP_CNTL` (`0x2080`) written once, statically, as `0x0014025a`
  (`PVS_NUM_SLOTS(10) | PVS_NUM_CNTLRS(5) | PVS_NUM_FPUS(2) | VF_MAX_VTX_NUM(5)`).
  `VF_MAX_VTX_NUM = 5` is the fingerprint of the static bypass write; the
  hardware-TCL emit path instead writes `VF_MAX_VTX_NUM(12)`.
- `VAP_VTE_CNTL` (`0x20b0`) `VTX_XY_FMT` / `VTX_Z_FMT` / `VTX_W0_FMT` select
  pre-divided window-space versus full clip-space fetch -- the bit the producer
  (window-space) and re-ingest (clip-space) stages flip between.
- Vertices arrive via `3D_LOAD_VBPNTR` from an already-transformed buffer; the
  per-stream `VAP_PROG_STREAM_CNTL` (`0x2150+`) carries the layout and is
  writable and reusable, never read back.
- Validator gates that made the submission hang-free: `ZB_CNTL.Z_ENABLE = 0`
  (skips the z-buffer check with no z/s surface) and `SC_SCISSORS_BR` sized to
  exactly fit the target.

The immediate `fbo-clearmipmap` fix is to make the blitter clear-quad draw
reproduce this shape: the value that diverges is one of `VAP_VTE_CNTL` (vertex
coordinate space), `ZB_CNTL.Z_ENABLE`, the `SC_SCISSORS` / `SC_CLIPRECT` value
after the non-r500 `+1440` scissor offset (`r300_emit.c`), `VAP_VF_CNTL`
`NUM_VERTICES`, or the `PSC` / `VAP_VTX_SIZE` stride. A register-level decode of
the retained 672-dword IB (via the no-submit PM4 decode workflow, not another
live wedge) names the exact one.

## VAP register table

The concrete registers the bypass shape programs, from `r300_reg.h`. The r3xx
family register file governs the RS482 VAP; the `R500_*` rows exist in the same
header but are gated off on this silicon (`is_r500 = false`) and are listed so
the missing-fallback hazards are explicit. Offsets are byte addresses; a `*`
in the R2VB column marks a register the direct-VAP route (`r300_r2vb.c`) emits.

| Register | Offset | Load-bearing field(s) | Role in the TCL_BYPASS / R2VB route | R2VB |
| --- | --- | --- | --- | --- |
| `VAP_PORT_IDX0` | `0x2040` | immediate vertex dword | Immediate-mode vertex data port; the bypass fetches from a VBO instead, but the port is the alternative inline write surface | |
| `VAP_CNTL` | `0x2080` | `PVS_NUM_SLOTS`/`CNTLRS`/`FPUS`, `VF_MAX_VTX_NUM` | Static `0x0014025a`; `VF_MAX_VTX_NUM=5` is the bypass fingerprint (HW-TCL writes `12`) | |
| `VAP_VF_CNTL` | `0x2084` | `PRIM_TYPE[3:0]`, `PRIM_WALK[5:4]`, `NUM_VERTICES[31:16]` | Kicks the draw; `NUM_VERTICES` is a 16-bit field (the underflow lever, below) | * |
| `R500_VAP_ALT_NUM_VERTICES` | `0x2088` | 32-bit count | r5xx-only wide count; **absent on RS482**, so the byte-budget clamp is the sole guard | |
| `R500_VAP_INDEX_OFFSET` | `0x208c` | signed index bias | r5xx-only; RS482 applies index bias on the CPU side | |
| `VAP_OUTPUT_VTX_FMT_0` | `0x2090` | `POS_PRESENT` b0, `COLOR_[0..3]_PRESENT` b1-4, `PT_SIZE_PRESENT` b16 | Declares which post-transform attributes the VAP emits to setup | * |
| `VAP_OUTPUT_VTX_FMT_1` | `0x2094` | `TEX_[0..7]_COMP_CNT` (3 bits each) | Per-texcoord component count of the emitted vertex | * |
| `VAP_VTE_CNTL` | `0x20b0` | `VPORT_[XYZ]_SCALE`/`OFFSET_ENA` b0-5, `VTX_XY_FMT` b8, `VTX_Z_FMT` b9, `VTX_W0_FMT` | Selects pre-divided window-space vs full clip-space fetch -- the bit the producer (window) and re-ingest (clip) stages flip. `VAP_VTE_CNTL` is the *enable/format* control only; the six registers below hold the transform its enable bits gate | * |
| `SE_VPORT_[XYZ]_SCALE`/`_OFFSET` | `0x1d98-0x1dac` | six FP32 scale + bias | The affine `NDC * scale + bias -> window` transform itself (stage 4 of the decomposition, the coordinate-contract "window" row). `r300_emit_viewport_state` emits all six when the viewport atom is dirty (`OUT_CS_REG_SEQ(R300_SE_VPORT_XSCALE, 6)`); consecutive draws with an unchanged viewport do not re-emit; the NDC-buffer bypass shape (`VPORT_*_ENA` on) reads exactly these | * |
| `VAP_VPORT_[XYZ]_SCALE`/`_OFFSET` | `0x2098-0x20ac` | same six scale/offset | VAP-window alias of the viewport block; the driver writes the `SE_` (`0x1d98`) alias instead, so this alias stays idle. Named so the dual mapping is explicit | |
| `VAP_VTX_SIZE` | `0x20b4` | per-vertex dword stride | Fetch stride; part of the `PSC`/stride tuple a diverging clear-quad can miss | * |
| `VAP_VF_MAX_VTX_INDX` | `0x2134` | max index | Upper index clamp for the fetch window | * |
| `VAP_VF_MIN_VTX_INDX` | `0x2138` | min index | Lower index clamp | |
| `VAP_CNTL_STATUS` | `0x2140` | `TCL_BYPASS` b8, `PVS_BUSY` b11, `VS_BUSY` b24, swap `[1:0]` | Sets bypass (VAP forwards pre-transformed vertices, T&L idle). Directly readable (observed responding, `= 0x00000100` with bit 8 set). Programmed by the RS/state path (`r300_create_rs_state` / flush), not by `r300_r2vb.c` itself | |
| `VAP_PROG_STREAM_CNTL_0..7` | `0x2150-0x216c` | per-stream data-type + `DST_VEC_LOC` | Attribute stream layout; writable and reusable, never read back; idle `_1..7` are headroom for synthesized channels | * |
| `VAP_VTX_STATE_CNTL` | `0x2180` | vertex state select | Vertex state routing for the bypass fetch | * |
| `VAP_VSM_VTX_ASSM` | `0x2184` | vertex input-assembly select | Input-assembly latch feeding the VAP; `always_emitted` by `r300_emit_rs_block_state`, the sibling of `VTX_STATE_CNTL` | * |
| `VAP_PVS_VECTOR_INDX_REG` | `0x2200` | PVS upload index | PVS program-upload window; **write-only, dead on RS48x** (no PVS) | |
| `VAP_PSC_SGN_NORM_CNTL` | `0x21dc` | per-component sign/normalize | Fetch sign-extend / normalize control | |
| `VAP_PROG_STREAM_CNTL_EXT_0..7` | `0x21e0-0x21fc` | swizzle + write-mask | Extended per-stream layout (swizzle, write-enable) | * |
| `VAP_CLIP_CNTL` | `0x221c` | `CLIP_DISABLE` b16, `UCP_ENABLE_[0..5]` b0-5, `PS_UCP_MODE` b14-15 | Bypass sets `CLIP_DISABLE`; clip is owned by the CPU/Draw side | * |
| `VAP_GB_VERT_CLIP_ADJ` | `0x2220` | vertical clip guard-band | Guard-band the setup clip widens to; the CPU-side clip must agree with these when `CLIP_DISABLE` is not set | |
| `VAP_GB_VERT_DISC_ADJ` | `0x2224` | vertical discard guard-band | Guard-band beyond which setup discards; paired with the vertical clip adjust | |
| `VAP_GB_HORZ_CLIP_ADJ` | `0x2228` | horizontal clip guard-band | Horizontal companion to `GB_VERT_CLIP_ADJ` | |
| `VAP_GB_HORZ_DISC_ADJ` | `0x222c` | horizontal discard guard-band | Horizontal companion to `GB_VERT_DISC_ADJ` | |
| `VAP_PVS_CODE_CNTL_0` | `0x22d0` | PVS code entry/size | PVS instruction-store control; **write-only, dead on RS48x** | |
| `VAP_PVS_STATE_FLUSH_REG` | `0x2284` | write-triggered flush | PVS state-flush handshake; a posted write synchronizes the engine (the R2VB cache barrier emits `0x2284 = 0`, `r300_r2vb.c:849`) -- safe to write, unsafe to read | * |
| `VAP_PVS_VTX_TIMEOUT_REG` | `0x2288` | vertex-timeout latch | `always_emitted` posted sync (`r300_context.c` init, `0xffff`); sits inside the read-wedge window, so it is written but never read | |

### The 16-bit VF_CNTL underflow lever (and its fix)

`VAP_VF_CNTL.NUM_VERTICES` is 16 bits (`__SHIFT 16`, mask `0xffff`). A
non-indexed SW-TCL draw of exactly 65536 vertices truncates the field to 0:
r300g sizes a non-indexed batch by `R300_MAX_DRAW_VBO_SIZE / vertex_size`, and
the smallest SW-TCL vertex is the mandatory 4-float clip position (16 bytes), so
the 1 MiB budget yields exactly 65536. On RS482 (`is_r500 = false`) there is no
`R500_VAP_ALT_NUM_VERTICES` (`0x2088`) / `USE_ALT_NUM_VERTS` (bit 14) fallback,
so `65536 << 16` writes 0; the kernel `r100_cs_track_check` (prim_walk 2)
computes `esize*(nverts-1)*4` with `nverts=0`, underflows to `0xFFFFFFFC`, and
rejects the IB with `-EINVAL` after the half-emitted stream has corrupted the
heap. Fix commit `9899a4d8dd3` ("r300: clamp SWTCL vertex batches to the 16-bit VAP
count limit") caps the r3xx/r4xx byte budget below the wrap and adds the r5xx
alt-count path; validated on RS482 (the 65536-point draw renders).  The SWTCL
path that coexists with R2VB re-ingest emits large-count draws under that
clamp; the R2VB emitter declines draws with `count >= 65536` separately.  Do
not re-introduce an unclamped `NUM_VERTICES` emit.

### Vertex system-value registry (there is no VAP register for it)

`gl_VertexID` / `gl_InstanceID` are **not** VAP state on this path.
`r300_nir_lower_vs_system_values.c` lowers each supported VS system value to a
synthetic vertex **input** attribute: the caller reserves a velem slot, the pass
rewrites the system-value intrinsic into a read of that attribute, and the VAP
fetches it as ordinary vertex data through `VAP_PROG_STREAM_CNTL`. So the
"registry" is a slot-reservation contract in the fetch layout, not a register
block -- consistent with the raw-output oracle, which drives these system values
through the draw module, not through PVS.

### Wedge window: reads and writes are asymmetric, and the boundary is the PVS ports

The read hazard is confined to the PVS/SE_TCL vector-engine ports, not the whole
VAP domain, and posted writes never wedge. The corpus resolves three access
classes:

- **Front-end control/status is read-safe.** `VAP_CNTL` (`0x2080`) and
  `VAP_CNTL_STATUS` (`0x2140`) are observed *responding*: a genuine read returns
  `0x2080 = 0x0014025a` (the bypass control word) and `0x2140 = 0x00000100`
  (`TCL_BYPASS` set). Safe front-end probes therefore read only these two words
  and refuse reads at or above `0x2200`. VAP progress has a direct status
  surface, not only the `RBBM_STATUS` aggregate.
- **The PVS/SE_TCL port window `0x2200-0x22dc` read-wedges.** A read of
  `VAP_CLIP_CNTL` (`0x221c`), `VAP_PVS_STATE_FLUSH_REG` (`0x2284`), or
  `VAP_PVS_CODE_CNTL_0` (`0x22d0`) waits forever for a completion and stalls the
  reset-less K8 northbridge below the core level -- no NMI is delivered, every
  such read costs a physical power cycle (hardware-confirmed, stein finding
  `2026-06-11-rs480-vertex-engine-write-only-read-wedges-asymmetry`). These ports
  clock-gate at rest (`clock_gated_port`).
- **Writes into the PVS window are posted and safe.** The same offsets accept
  posted writes without wedging -- `x86` MMIO writes retire without a completion.
  The R2VB cache barrier deliberately posts `VAP_PVS_STATE_FLUSH_REG = 0`
  (`r300_r2vb.c:849`, `r300_emit.c:1220`) to synchronize the engine, and init
  posts `VAP_PVS_VTX_TIMEOUT_REG` -- so the window is written on the bypass path
  even though it is never read.

The earlier read-reachability inventory
(`external sibling repository `steinmarder` (r300 reverse-engineering lane; not in this Mesa tree) src/re/r300/docs/rs482-register-read-reachability-and-reader-inventory.md`)
extrapolated the single `0x221c` proof to lump `0x2080`/`0x2140` into one excluded
`radeon_rs480_candidate_vap_regs` group; the later hardware-confirmed asymmetry
finding narrows it to the port window above, and this design follows the narrowed
boundary. The practical rule is unchanged for correctness -- program VAP state,
read only `0x2080`/`0x2140` for progress, never read `0x2200+` -- but the reason is
transaction type (non-posted read completion), not a blanket domain-wide gate.

Three RS482 vertex-path failures carry three distinct signatures, and conflating
them mis-diagnoses the frontend. They stay separate:

| Failure | Signature |
| --- | --- |
| Front-end fetch / VAP progression stall -- the `fbo-clearmipmap` clear-quad wedge (HBTCL-01/02): the VAP is stuck assembling a vertex stream whose descriptor does not match the frontend | `RBBM_STATUS` latched `0x8411c100`, `VAP_BUSY = GA_BUSY = 100%`, backend (`RB3D`/`RE`/`TAM`/`PB`/`E2`) idle |
| Under-fed output tuple -- the SOLVED R2VB re-ingest defect: `VAP_VTX_SIZE` trailed the wider `VAP_OUTPUT_VTX_FMT` tuple (8 vs 12 dwords), so the GA latched waiting for the missing dwords per vertex. Fixed by per-output reconstruction (`VAP_VTX_SIZE 0x08 -> 0x0c`, mesa PR#1040/#1041) | `GA_BUSY = 100%`, `VAP_BUSY = 0`, `CP`/`RE`/`RB3D` idle |
| PVS-port MMIO read wedge: a read into `0x2200-0x22dc` waits forever for a non-posted completion and stalls the reset-less K8 northbridge below core level | host/NB completion failure, no NMI, no ordinary GFX-pipeline `RBBM_STATUS` shape; costs a physical power cycle |

The bypass shape converges away from the first. The second is closed. The third
is a probe hazard, never a draw outcome. Under a compounding active-3D wedge the
`0x4000+` space becomes a non-posted-HT-read black hole, independent of the VAP
read asymmetry above.

## Mathematical decomposition of the transform

The vertex TCL pipeline factors into stages, each tagged by where it is safe to
run on RS482:

1. **Model-view-projection transform** `v_clip = M * v_object`, `M` a 4x4. A
   pure linear matrix multiply -- **HW-safe** on the fragment ALU (below).
2. **Clip** against the view volume. **SW-safe**, and -- following Glaeser,
   "Fast Algorithms for 3D-Graphics" (1994) -- done in the *linear* domain
   before any divide (see below).
3. **Perspective divide** `x_ndc = x_clip / w_clip` (and y, z). A per-vertex
   scalar reciprocal -- **SW-safe**; feeding pre-divided window-space vertices
   is exactly the `VAP_VTE_CNTL` bypass mode.
4. **Viewport / assembly** window-space scale and bias, primitive assembly.
   **HW-safe** in the VAP/GA frontend, which is what the bypass draw uses.

Glaeser's decomposition is the spine. The true perspective map
`x~ = lambda*x, y~ = lambda*y, z~ = z` with `lambda = -d/(-z)` is not a
collineation in z (edges bend into hyperbola arcs). Its linear companion,
`x* = lambda*x, y* = lambda*y, z* = k*lambda*z`, *is* a genuine collineation:
the coplanarity determinant transforms as `D' = k * lambda_P * lambda_Q *
lambda_R * D`, so three points stay coplanar iff the originals were, hence lines
map to lines and clipping in the `*`-domain is exact. For `k = 1/d` it closes to
`z* = -z*d / (d - z)` with inverse `z = d*z* / (1 + z*)`. The consequence for
this design: the whole transform up to and including the collineation is linear
and can be a matrix multiply on a hardware block; only the final scalar recovery
divide need be non-linear, and clipping happens before it in the linear domain
where the frontend never has to.

Two more archive results shape the data flow. Hoppe (1999) shows the
post-transform vertex cache must be a small FIFO, not an associative/LRU cache,
to keep strips long -- which is exactly the GA-side FIFO the frontend already
has; the hybrid keeps it fed with bypass vertices rather than re-transforming
shared ones. Mayer (1970, SCOPE) keeps the transform as a tiny basis (the
direction cosines) and never mutates the bulk vertex data, matching the
direct-VB approach where the application buffer is never itself relocated -- the
producer's internal BO carries the transformed result.

Two independent results decades apart converge on which stage to move to
hardware and which to keep in software. Artwick, "Applied Concepts in
Microcomputer Graphics" (1984), profiles the pipeline and measures the
matrix-vector multiply at about 5% of frame time and clipping at about 23%: the
linear transform is cheap, the branchy data-dependent clip is the real cost.
Owens et al., "A Survey of General-Purpose Computation on Graphics Hardware"
(2005), independently identify the vertex processor as the only MIMD,
scatter-capable -- and therefore the most fragile and costly -- pipeline stage.
Together they argue the same split this design takes: put the simple fixed
linear matrix multiply on a hardware block, keep the branchy clip-and-classify
logic in software. Artwick further measures a per-vertex quaternion apply at
about four times the multiplies of a matrix-vector apply, so hypercomplex
rotation (Coxeter, 1946, derives it as the product of two reflections) belongs
only in the once-per-frame matrix-derivation step, never in the per-vertex hot
loop -- the `M` in `v_clip = M * v_object` may be *built* with quaternions, but
each vertex is transformed by the composed 4x4.

### The coordinate contract (HBTCL-04a)

The four stages move a vertex through five concrete representations. Naming them
and their producer/consumer fixes what each later HBTCL-04 step operates on and,
critically, where the perspective divide must sit. Coordinate values in this
contract are FP24 (the fragment ALU domain); the exact-integer window (`m + f <= 17`) binds only where
a value must round-trip as an integer index, never the transformed coordinates.

| Space | Representation | Produced by | Consumed by | VAP / VTE binding |
| --- | --- | --- | --- | --- |
| object | `(x,y,z,w)` FP24: app `w` when the position attribute has four components, else `w=1` | app VBO / producer `DRAW_IMMD` payload | the MVP multiply in the producer | object attributes enter the **producer** as embedded IMMD (or later TAM); they are not the re-ingest `3D_LOAD_VBPNTR` stream |
| clip | `v_clip = M * v_object`, 4D, `w_clip` free | fragment-ALU MVP (04, linear) | SW clip / collineation, or clip-space VAP re-ingest | may be handed to the bypass VAP when `VTX_W0_FMT` enables the VTE reciprocal (see below) |
| collineation (`*`) | Glaeser `(lambda*x, lambda*y, k*lambda*z)`, lines stay lines | the linear companion map | SW clip classify + edge gen (04c/04d) | pure SW; frontend never sees it |
| NDC | `v_ndc = v_clip.xyz / w_clip`, `[-1,1]` | SW perspective divide (04b) or VTE `1/w` | viewport scale/bias | divide is either software or VTE-owned |
| window | `NDC * viewport_scale + bias`, screen coords | producer, or the VAP viewport | the GA setup FIFO | `VTX_XY_FMT`/`VTX_Z_FMT` (pre-divided) with `VPORT_*_ENA` off, or NDC + `VPORT_*_ENA` |

`VAP_VTE_CNTL` selects **coordinate interpretation**, not the VBO fetch path.
Three legal re-ingest shapes:

- **Window-space buffer** (`VTX_XY_FMT = VTX_Z_FMT = 1`, `VPORT_*_ENA` off):
  the producer already divided and applied the viewport; the VAP passes
  vertices through. Demonstrated hang-free on the SWTCL clear path.
- **NDC buffer** (`VTX_*_FMT` clear, `VPORT_*_ENA` on): the producer divided;
  the VTE applies only the affine viewport (`SE_VPORT_*` scale/bias).
- **Clip-space buffer** (`VTX_W0_FMT` set, `VTX_XY_FMT`/`VTX_Z_FMT` clear,
  `VPORT_*_ENA` as programmed): the VTE performs the homogeneous reciprocal
  (`1/w`) and optional viewport. This is the same shape HWTCL and the RS482
  direct-VB path program in `r300_set_viewport_states` / render. Software
  04b is therefore **not** mandatory when re-ingest intentionally uses this
  VTE path; double-dividing (SW 04b then `VTX_W0_FMT`) is a contract bug.

Object-space attributes for the fragment-ALU MVP producer arrive as the
producer's input payload (`DRAW_IMMD` today; texture/TAM later).
`3D_LOAD_VBPNTR` is only the **re-ingest** mechanism that feeds the bypass
VAP after the producer publishes the clip/NDC/window BO.

Clip (04c) and edge generation (04d) that run in software still prefer the
collineation domain before any divide they own. Paths that hand raw clip
to the VTE skip SW 04b and rely on `VTX_W0_FMT` instead.

## The hybrid HBTCL

The design generalizes the silicon-demonstrated R2VB direct-VAP handoff into the
standing vertex path:

- **Transform** runs in SW (Gallium Draw / gallivm) or, where the vertex shader
  can be restaged safely, on the **fragment ALU**. `r300_r2vb_restage_vs_as_fs`
  already clones an arbitrary straight-line, non-texturing, control-flow-free
  application VS NIR and runs it as a fragment producer, so fixed-MVP is not the
  only producer class. Quaternion (4 `DP4`) and octonion (16 `DP4`) kernels are
  demonstrated through this route. The fragment ALU (FP24 VLIW vec4,
  `MAD/DP3/DP4/MIN/MAX/CND/CMP/FRC`, alpha `RCP/RSQ/EX2/LG2`, 64 co-issued
  vector+scalar slots) is the only programmable ALU on the part and is the
  vertex engine here. R400_US code banking exposes a larger diagnostic
  instruction address space but does not lift the 64-slot dependent-program
  ceiling on RS482: a live temporary written in one bank is not usable in the
  next, so banking is not a dependent-chain escape.
- **Perspective divide + viewport** run on the fragment ALU inside every
  producer variant (`r2vb_divide_position`: alpha-pipe `RCP` with a `1/32768`
  FP24 floor, then three viewport MADs), selected by the explicit
  `r300_r2vb_position_space` contract -- `POSITION_CLIP` emits the raw `M*v`
  homogeneous result, `POSITION_WINDOW` emits divided window space with
  `w = 1`. Verified on RS482 against the CPU window-space oracle
  (`r2vb_divide_verify` 3/3, tol 0.05).
- **Clipping** classifies in the raw clip-space domain before the divide.
  The classifier (`r300_r2vb_clip.h`) mirrors the gallium draw software
  clipper bit-for-bit (six hardwired planes, `!(dist >= 0)` NaN-outside form,
  0.5 XY guard-band coefficient, half-Z switch) and carries divide safety
  (`w` below the FP24 reciprocal floor) as a separate FALLBACK class rather
  than a seventh plane bit. PARTIAL triangles are clipped in the raw clip-space
  domain by the Sutherland-Hodgman edge path (`t = d_out / (d_out - d_in)`);
  carried attributes interpolate at the intersection vertices and the polygon
  retriangulates, byte-identical to the gallivm reference on RS482 (04d).
- **Delivery**: a `cb_flush_clean` cache barrier, then `3D_LOAD_VBPNTR`
  re-ingest of the transformed buffer, then a `TCL_BYPASS` draw whose
  `VAP_VTE_CNTL` derives from the producer contract: a window-space source
  fetches verbatim (`VTX_XY_FMT | VTX_Z_FMT | VTX_W0_FMT`), a clip-space
  source runs the hardware viewport transform. Feeding window-space output
  through the clip-mode VTE applies the viewport twice and lands the geometry
  off-target -- the defect class an aggregate register diff cannot see,
  because a post-draw restore write masks it; only the draw-adjacent packet
  chronology exposes it. The route posts writes into the PVS-port window
  (`VAP_PVS_STATE_FLUSH_REG` barriers, `0x2284`); the forbidden operation on the
  `0x2200-0x22dc` window is the *read*, which is the physical-power-cycle
  wedge.
- **Never**: reach `r300_emit_vs_state`, set `has_tcl = true`, clear
  `TCL_BYPASS`, or read the `0x2200-0x22dc` PVS/SE_TCL port window. Progress
  reads stay on the demonstrated-safe front-end status words `VAP_CNTL`
  (`0x2080`) and `VAP_CNTL_STATUS` (`0x2140`); a read at or above `0x2200` is
  the non-posted-completion wedge (posted writes into that window stay safe).
  Each forbidden action re-enters the unrecoverable wedge class.

The fix and the redesign are the same insight at two scales: the RS482 vertex
frontend wedges when it is asked to do transform work, so we do the transform
where it is safe and hand the frontend only vertices it can pass straight
through. Converging the blitter clear quad onto the bypass shape is the first
instance; the hybrid makes it the rule.

## HBTCL-03 audit result: actual R2VB engine state

The HBTCL-03 audit corrects the design in both directions.

What is already more built than the design assumed:

- `r300_r2vb.c` has three producer paths, not only fixed-MVP. The restage path
  (`R300_R2VB_RESTAGE`, `r300_r2vb_restage_vs_as_fs`) clones any straight-line,
  non-texturing, control-flow-free application VS NIR and runs it on the
  fragment ALU. Transform-kernel generalization for the important quaternion and
  octonion cases is therefore mostly already present.

What remains less built than the design assumed:

- Kernels that emit more than 64 ALU slots have no route. Admission measures the
  derived producer FS against the real emit ceiling (a throwaway backend
  compile reading the emitted `alu.length`, memoized per VS), so a dense
  kernel the vectorizing backend packs under 64 slots is admitted even when
  its scalar NIR count exceeds 64; the R400 code-bank mechanism is not the
  escape: on RS482 the alpha-sentinel and dependent-chain probes show bank
  instructions execute but a live temporary written in bank 0 is not usable
  in bank 1 (a 63-slot dependent chain works, the 65-plus variant fails;
  constants survive the boundary). The general escape is explicit state
  transport -- algebraic compaction of the producer, or a producer split
  whose carry crosses through a render target or R2VB buffer -- with gallivm
  as the fallback (HBTCL-04f).
- R2VB is not the standing route. The engine remains gated by explicit opt-in
  environment variables and has no default-on integration into
  `r300_swtcl_draw_vbo`; that promotion remains HBTCL-08.

Geometric clipping is complete: classification (`r300_r2vb_clip.h`), edge
generation with attribute interpolation, and strip/fan/indexed/restart
topology gather are landed and byte-identical to the gallivm reference on
RS482. The divide, the window-space delivery, and general transform restaging
are done; do not rediscover them. The remaining HBTCL-04 scope is the
over-budget producer escape (HBTCL-04f as state transport).

## All-on-pipeline: one role per block, minimal CPU

The endpoint pushes as much of the vertex layer as possible onto the GPU's own
blocks (RBBM_STATUS names them), leaving the CPU only the small branchy stage
Artwick (1984) measured as cheap to keep and Owens (2005) confirmed is the hard
one to put in fixed hardware. Each role below is either already demonstrated on
silicon or a bounded next increment from two demonstrated pieces.

| Block | Role | Evidence |
| --- | --- | --- |
| Fragment ALU (US, pixel path) | The transform: `M*v` as 4 `DP4`, and non-linear per-vertex math -- quaternion rotation as 4 `DP4` (HW-confirmed, 3/3 within 0.05), octonion 16 `DP4`, Walsh-Hadamard multiply-free and bit-exact in the FP24 window (the exact-integer bound is *proven* in Rocq -- open_gororoba `proofs/theories/IDCT8DP4ExactBound.v`: `8*1448^2 < 2^24` and `2^17 < 2^24`, zero admits) | demonstrated on silicon; the 64-ALU ceiling is hard for dependent chains (R400 code banks execute but live temporaries do not cross the bank boundary), so over-budget kernels split with explicit state transport (HBTCL-04f) |
| TAM/TDM/TIM (texture) | Fetch the MVP matrix and vertex attributes as textures -- **in the R2VB producer fragment shader**, which can sample; the VAP-side vertex-texture-fetch is architecturally gated off (`GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS = 0`), so this must live in the producer, not the vertex stage | gate measured; producer wiring to build |
| RB3D (3D backend) | One-pass multi-attribute export: four `C4_32_FP` / `R300_COLOR_FORMAT_ARGB32323232` MRT targets route per-output-location as the FP32 vertex carrier (byte-exact RGBA8 remains a separate numeric-domain proof, not the vertex export format) -- position, normal, texcoord each to its own target in a single transform pass | MRT demonstrated byte-exact; combine with R2VB (HBTCL-09) |
| E2/RB2D/CBA2D (2D blit) | Move transformed vertices GART->VAP-input, or scatter/gather vertex streams, instead of a CPU copy + `cb_flush` | unexplored: the 2D engine appears once in the corpus (H.264 block-copy), never probed for vertices (HBTCL-10) |
| VAP (frontend, bypass) | Assemble and output-format-map pre-transformed vertices; idle `VAP_PROG_STREAM_CNTL_1..7` (`0x2154-0x216c`) give headroom for more synthesized attribute channels | demonstrated; never engages the absent PVS |
| GA / RE / SC | Primitive assembly, setup, rasterization, and a rectangular screen-space reject via `SC_SCISSORS`/`SC_CLIPRECT` (already in the demonstrated-safe bypass shape, outside the wedge window) | demonstrated |
| CPU (minimal) | The branchy stages only: clip classification in the linear domain (per Glaeser), edge generation and attribute interpolation, topology decomposition/gather, and the admission/fallback decision. The perspective divide runs on the fragment ALU (`r2vb_divide_position`) or the VTE, not the CPU. The transform + barrier + re-ingest already run in one IB for the passthrough class -- no mid-draw CPU round-trip -- at about 0.023 us/vertex versus 0.83 us/vertex for gallivm (~36x), and the re-ingest raster is bit-identical to gallivm | demonstrated (measured) |

### Do not re-derive (measured dead ends)

- **PVS sin/cos** (`ME_SIN`/`ME_COS` as a hardware vertex transcendental) is
  superseded: the PVS path is closed, and the most aggressive recovery lever
  (clearing `D18F3x44 SyncOnWdogEn` before a `0x221c` read) froze both cores.
  Lighting transcendentals stay on the fragment ALU (`EX2`/`LG2`/`RCP`/`RSQ`).
- **Sedenion Cariow reduced-multiply** is refuted as an ALU win on R300: uniform
  ALU cost makes the Hadamard-butterfly additions a net loss (171-246 versus the
  164 per-quarter baseline). Fewer multiplies does not mean fewer R300 slots.
- **Clip-enable** invokes the wedge-prone VAP clip; only the `CLIP_DISABLE`
  *write* is safe. Clip stays in software.
- **R2VB/SW-TCL instancing** is broken; do not route per-instance retransform through
  native instancing yet.
- **Self-feeding / GPU-computed-address indirect draws** do not exist in the
  corpus; the closest demonstrated amortization is batching one `WAIT_UNTIL` barrier
  across N draws. Mind the CS triple-buffer pipelining race (fixed `9019491c1fd`)
  for any multi-IB scheme.

## Lighting on the fragment ALU (HBTCL-06 policy)

A full TCL needs a lighting stage, and on RS48x the only live transcendental
unit is the fragment ALU -- the PVS macro-engine sin/cos (`ME_SIN`/`ME_COS`) is
dead with the closed vertex path. The fragment ALU is where R2VB already runs
its vertex math, so lighting is not a new engine, just more fragment slots.

The native fragment-ALU op set is the two co-issued pipes, ground-truthed by the
`translate_rgb_opcode` / `translate_alpha_opcode` switches in
`r300_fragprog_emit.c` (any opcode not in a switch hits `default -> error`, so it
must be lowered before emit). The RGB (vector, `OUTC`) pipe:
`MAD, DP3, DP4, MIN, MAX, CND, CMP, FRC, REPL_ALPHA`. The alpha (scalar, `OUTA`)
pipe: `MAD, DP4, MIN, MAX, CND, CMP, FRC, EX2, LG2, RCP, RSQ`. The transcendentals
`RCP/RSQ/EX2/LG2` are **alpha-pipe-exclusive**, which is the structural key below.
`ROUND` is **not** native -- it has no `OUTC`/`OUTA` slot and no emit case, so it
lowers; the `OUTC` `D2A` slot exists in hardware but the compiler never emits it.
R500 adds native `SIN`/`COS` in the alpha op (`r500_fragprog_emit.c`); on the
R300-class RS482 fragment ALU those are not native. What the r300 screen lowers
before emit (`r300_screen.c`): `lower_sincos` (sin/cos to a range-reduce +
polynomial), `lower_fpow` ("POW is only in the VS", so on the fragment ALU it
becomes `EX2(y * LG2(x))`), `lower_fsqrt` (to an `RSQ`/`RCP` form), `lower_fdph`.

The R2VB restage path (`r300_r2vb_restage_vs_as_fs`) only flips the NIR stage
and does not re-run the screen-level POW lower, so a restaged or
driver-synthesized lighting kernel that still contains `nir_op_fpow` must
lower to `EX2`/`LG2` itself before fragment emit (the R300 fragment emitter
has no POW case).

The standard GL lighting kernel maps to this set when the GL clamps and
attenuation coefficients are explicit (Mesa `emit_lit`, GL 2.1 formula 2.4):

| Lighting term | Fragment mapping | Native? |
| --- | --- | --- |
| `normalize(N)`, `normalize(L)` | `v * RSQ(dot(v,v))` | native `RSQ` + dot; dependent scalar cannot co-issue with its own vector producer |
| diffuse `max(N.L, 0)`, half-vector | `MAX` then `DP3`/`DP4` | native; GL clamps diffuse/specular when `N.L <= 0` |
| specular `pow(max(N.H, 0), s)` when `N.L > 0` | `EX2(s * LG2(...))` after clamps | must be lowered `POW` (screen lower or restage-local EX2/LG2) |
| distance attenuation `1 / (k0 + k1*d + k2*d*d)` | `RCP(DP3((1,d,d*d), (k0,k1,k2)))` | native `RCP`/`DP3`; inverse-square alone is only the default k0=1,k1=0,k2=1 case |
| spotlight cone / anisotropic sin/cos | range-reduce + polynomial | lowered `SIN`/`COS` |

Two constraints bound the policy:

- **FP24 is adequate for color, not for reuse.** The fragment ALU is FP24;
  lighting outputs are `[0,1]` colors where the 16-bit mantissa is ample. The
  exact-integer-to-`2^17` window that governs the transform and index reuse does
  not bind lighting -- lighting values never need to be exact integers.
- **Lighting shares the 64-slot fragment ceiling with the transform, and a slot
  is a co-issued pair.** The authoritative `>64`-slot gate is
  `R300_PFS_MAX_ALU_INST = 64` enforced in `r300_fragprog_emit.c`. Each of the 64
  slots holds a *co-issued* vector (`OUTC`) + scalar (`OUTA`) op (`radeon_code.h`
  `inst[].rgb_inst` + `alpha_inst`), so the real budget is up to 128 ALU ops (64
  vector + 64 scalar). Because the transcendentals are alpha-pipe-exclusive, a
  scalar `RSQ`/`RCP` can co-issue alongside a vector `DP3`/`DP4` in the same slot
  only when the pair scheduler has already cleared the scalar's read-after-write
  dependencies (`NumDependencies` / ready lists in the r300 compiler). A
  `normalize` chain that feeds `RSQ` from a just-written DP still serializes;
  independent lighting terms are "a handful of slots", dependent chains are not
  free. Presubtract (`RC_PRESUB_ADD/SUB/BIAS/INV`) is a source
  modifier, not a slot, and output modifiers (`MOD_MUL2/4/8`, `MOD_DIV`, `CLAMP`)
  fold more work per slot. TEX is a separate 32-slot budget (4 indirection
  phases). Multi-light or spotlight lighting on top of the R2VB transform can
  still exceed the 64 slots; that pushes the combined kernel onto the same
  multi-pass state-transport route as any over-budget producer (HBTCL-04f),
  rather than onto a separate lighting engine.

So lighting introduces no new hardware dependency and no new dead end: it is a
fragment-ALU budget question, folded into the same 64-slot accounting as the
transform, using only ops the compiler already emits or lowers.

**Butterfly / Cayley-Dickson does not lower the per-vertex budget.** A recurring
proposal is to factor the `4x4` MVP (or lighting dots) through a Walsh-Hadamard /
butterfly (Walsh-Hadamard style factorizations sometimes called Cariow in VLSI notes) to cut the op-count. It does not help on this silicon, for a
structural reason: the butterfly trades multiplies for add/sub, which wins only
where multipliers are the scarce resource (VLSI / fixed-point). The r300 fragment
ALU is the opposite regime -- a `DP4` is a 4-wide multiply-accumulate in one slot,
so a dense `4x4` transform is already at its **4-`DP4` floor**. Collapsing to
3 `DP4` + a move is valid only for *affine* MVPs (constant `w_clip`); a
perspective row still needs the fourth DP4 because `w_clip` is not free. A butterfly
fragments that into standalone cross-lane signed adds a `DP4` cannot absorb: the
best case (pure Hadamard `H4`) is 8 ops against the dense 4, and a general `4x4`
is worse. The open_gororoba WHT butterfly kernels confirm the crossover is at
`d >= 64/128`, not the `d = 4` vertex transform. This is the op-count ground on
which the Cayley-Dickson IDCT transfer was already refuted for r300
(`g3dvl` exact-integer IDCT work: r300 is a DP4 dot-machine, so the dense form
is already cheap). The float MVP is strictly weaker for the butterfly than that
exact-integer case, so the refutation transfers a fortiori. Cayley-
Dickson rotation keeps its legitimate home in the once-per-frame *matrix-build*
step (above, Artwick + Coxeter), never the per-vertex hot loop. The glamor
gradient-budget corpus is the on-part precedent for beating the 64-slot ceiling,
and it does so by multi-pass render-to-texture decomposition, RS-interpolator /
vertex-stage work-shift, and CPU setup-time LUT precompute -- the multi-pass DAG
lever, not an op-count refactor of the transform.

## Scoped implementation plan

What already exists on this substrate is more than a sketch: `r300_r2vb.c` is a
3289-line fragment-ALU render-to-vertex-buffer engine with a route classifier
(`R2VB_ROUTE_PASSTHROUGH` / `_CANDIDATE` / `REJECT_HW_TCL` / `_INDEXED` /
`_INSTANCED`), three producer paths including the restage path for arbitrary
straight-line non-texturing application VS NIR, self-test buffers, and a
no-submit `R300_HB_TCL=1 R300_R2VB_TIMING=capture` and
`R300_R2VB_INSPECT` oracles; `r300_hb_tcl.c` carries the static
`0x0014025a` bypass word; `r300_hb_r400_us.h` gates an R400 unified-shader
emission path that is diagnostic-only on RS482 -- the silicon executes bank
instructions but a live temporary does not survive the 64-slot bank boundary,
so banking is not a dependent-chain escape. The plan generalizes the engine
into the standing vertex route.

| Task | Work | Depends on |
| --- | --- | --- |
The `HBTCL-NN` tokens are secondary registry labels for this tracker; the load-bearing identity of each row is the mechanism in the description column (durable mechanism names for branches, commits, and findings).

| HBTCL-01 | No-submit PM4 decode of the clear-quad IB vs a working r3v triangle IB: capture with in-tree `R300_TRACE` / `RADEON_DUMP_PATCHED_IB` (and, when present, the external `steinmarder` r300 retained-IB decode tools), then compare the VAP frontend words; name the single diverging register among `VAP_VF_CNTL` NUM_VERTICES, `VAP_VTE_CNTL` coord space, `VF_MAX_VTX_NUM`, `SC_SCISSORS` after +1440, `ZB_CNTL.Z_ENABLE` | -- |
| HBTCL-02 | Converge `util_blitter`'s clear-quad emit onto the demonstrated-hang-free bypass shape; re-run `fbo-clearmipmap` under the forensic poller, confirm the VAP/GA stall clears | HBTCL-01 |
| HBTCL-03 | DONE: audited the three producer families (fixed-MVP, restage, passthrough), the explicit clip/window coordinate contract, the geometric clipping and topology pipeline, emitted-slot admission, and the typed one-`vec4` budget escape. Perspective divide (04b) and geometric clipping (04c-04e) are landed and byte-identical on RS482; the residual is algebraic compaction and multi-carry transport (04f.4-04f.5) plus standing-route promotion (HBTCL-08) | -- |
| HBTCL-04a | DONE: the coordinate contract section above (object/clip/NDC/window representations, divide placement, re-ingest VTE shapes) | HBTCL-03 |
| HBTCL-04b | DONE: perspective divide + viewport on the fragment ALU in every producer variant, selected by the explicit `r300_r2vb_position_space` contract; window-space delivery fetches verbatim via the source-space VTE. `r2vb_divide_verify` 3/3 on RS482 with delivery coverage matching the gallivm reference exactly | HBTCL-04a |
| HBTCL-04c | DONE: clip classification in the raw clip-space domain -- Draw-parity clip codes, accept/reject/partial/fallback classes (`r300_r2vb_clip.h`), FP24 clip-BO oracle, conservative gated route action; 9/9 corpus classes byte-identical on RS482 | HBTCL-04b |
| HBTCL-04d | DONE: edge generation -- Sutherland-Hodgman intersection of PARTIAL triangles in clip space (`t = d_out / (d_out - d_in)` blends), attribute interpolation, fan retriangulation; 10/10 corpus cases byte-identical on RS482 | HBTCL-04c |
| HBTCL-04e | DONE: topology gather -- strips, fans, indexed draws, primitive restart resolved to a triangle-index list before classification; 14/14 corpus cases byte-identical on RS482; points and lines stay excluded (points gate on HBTCL-07, lines need a 2-vertex clip variant) | HBTCL-04d |
| HBTCL-04f.1 | DONE: admission on actual emitted RC slots (a throwaway backend compile reads the emitted `alu.length`, memoized per VS), so a dense kernel the backend packs under 64 slots is admitted even when its scalar NIR count exceeds 64 | HBTCL-03 |
| HBTCL-04f.2 | DONE: producer split carrying one FP32 `vec4` float-carry through an R2VB buffer; VAP_VTX_SIZE under-feed root-caused and fixed by per-output reconstruction, corpus green on RS482 (mesa PR#1040-#1044) | HBTCL-04f.1 |
| HBTCL-04f.3 | Host-tested primitive; production-route integration is 04f.3R. The one-FP32-`vec4` split classifier distinguishes float, signed integer, unsigned integer, and boolean carries; an integer component enters only when NIR range analysis proves an exact R300 FP24 conversion plus FP32 storage round trip (`abs(sint) <= 2^17`, `uint <= 2^17`) and every post-cut consumer agrees with the producer's logical type. Pass B reconstructs signed and unsigned flat carries directly as `ftrunc` and `ffloor`, outside the interpolation-only float-to-integer epsilon adjustment. The classifier and the pass builders are proven by direct host construction of a fragment-stage producer (#1119) and mirrored on the host (F3-CLASSIFIER-01). On silicon the typed T0-T9 corpus renders through gallivm and admits no split (F3-R0, stein PR#110): `r300_vs_nir_is_fragment_aluable`'s float-only whitelist rejects every typed application VS (`f2i32`, `i2f32`, `flt`, `b2f32`, `imin`/`imax`) before the route reaches `r300_r2vb_split_admitted`, so the primitive is unreachable through the production draw route. Transport wider than one `vec4` remains HBTCL-04f.5 | HBTCL-04f.2 |
| HBTCL-04f.3c | DONE (mesa PR#1126, commit 19eaf0ec242): flat-input typed-source semantic parity. The fragment compile applies `r300_nir_lower_f2i_epsilon` (`x * (1 + 2^-15)`, an away-from-zero nudge compensating interpolated-varying error) before `f2i32`/`f2u32`; the direct Draw VS path `r300_draw_init_vertex_shader` lowers integers with `nir_lower_int_to_float` and omits that nudge. An R2VB producer's generated point attributes are flat, so the fragment epsilon can push a fractional value across the truncation boundary relative to gallivm. An `r300_fs_input_semantics` distinction (`INTERPOLATED` vs `R2VB_FLAT_VERTEX`) skips the epsilon for flat R2VB producer conversions; a host oracle covers a fractional `f2i` case just below and above an integer boundary | HBTCL-04f.3 |
| HBTCL-04f.3R | DONE, DIAGNOSTIC-ONLY (mesa PR#1137, commit 8b475779ef2): the typed split is wired so a controlled corpus can reach it. The admission gate builds the restaged position FS (`r300_r2vb_build_restaged_fs_nir`, the plan consumer in `rs482-producer-alu-compaction-design.md`) and preflight-compiles it, so the backend verdict (`REJECT` unsupported, `OVER_ALU_BUDGET` split-eligible) decides ALU-lowering capability. The pre-lowering `r300_nir_op_is_fragment_aluable` whitelist scan narrows to the structural facts that survive lowering: single-block control flow, plain I/O and uniform/UBO intrinsics, a `gl_Position` output, and a bounded set of position-feeding inputs (up to `R300_R2VB_MAX_PRODUCER_INPUTS`) mapped in the producer's `VARYING_SLOT_VAR0 + location-rank` order, each representable by the producer input contract. Under the exact `R300_R2VB_TYPED_SPLIT=1` diagnostic gate, a narrow typed-source shape reaches the split (Boolean `flt -> b2f32`, signed `f2i32` + constant clamp `-> i2f32`, unsigned `max(x,0)` + `f2u32` + constant clamp `-> u2f32`); `r300_r2vb_typed_split_gate_value` accepts only the string `1`, while unset, empty, `0`, padded, and every other value keep the route closed. An under-budget typed producer declines (`TYPED_SINGLE_PASS_UNPROVEN`). This gate proves the mechanism on a controlled corpus and stays diagnostic-only: it establishes carry transport exactness (04f.3), not source-conversion equivalence (04f.3e), so it is not a production-safe route. Requires a new executable SHA | HBTCL-04f.3, HBTCL-04f.3c, HBTCL-04f.3e, COMP-PLAN-01 |
| HBTCL-04f.3e | OPEN, PRODUCTION BLOCKER: typed source-domain equivalence. The exact-carry proof (04f.3) shows an integer within `+-2^17` round-trips FP24->FP32 exactly; it does not show the FP24 producer's `f2i32`/`f2u32` produced the same integer as the gallivm/Draw software VS, since a runtime float can quantize differently in FP24 before truncation (a value just below `2` truncates to `1` in software but rounds to `2` in FP24 first). The production contract needs two independent predicates: source conversion equivalence (the FP24 producer's typed value equals the Draw reference value) and carry transport exactness (04f.3). An arbitrary `f2i32(attribute)` + clamp bounds the output without proving the conversion matched the reference, so it declines `R300_R2VB_REJECT_TYPED_SOURCE_DOMAIN_UNPROVEN`. Automatic typed selection (HBTCL-08) stays blocked until a static source-domain predicate exists (exact integral floats within `+-2^17`; documented integer system streams; range-contracted integer uniform/push values; Boolean compares with proven threshold separation) | HBTCL-04f.3R |
| HBTCL-04f.4 | PARKED, DEMAND-GATED: standing-route telemetry over real workloads found no over-budget producer population, so implementation waits on measured demand. Semantics-preserving algebraic compaction of the over-budget producer -- designed in `rs482-producer-alu-compaction-design.md` (two-proof certified-rewrite pipeline over the emitted resource vector, shared producer plan with the split; the multiply-minimization family is gated off as a DP4-regime loss). Implementation gated on PROOF-FP24-01 (the FP24 exact-integer window is 2^17, not 2^24) and a real-workload mine; the affine closed form is contract-restricted and `recur90` stays a split stress case | HBTCL-04f.2 |
| HBTCL-04f.5 | OPEN, NOT ACTIVE: no measured multi-carry workload demand. Multi-carry / MRT transport for producers whose escape needs more than one carry stream; gallivm fallback for every unsupported shape. R400 code banks stay diagnostic-only -- bank instructions execute but a live temporary does not survive the bank boundary | HBTCL-04f.2 |
| HBTCL-05 | DONE (corpus-verified): the "VAP register table" section above, with the viewport `SE_VPORT_*`/`VAP_VPORT_*` scale-offset block, `VSM_VTX_ASSM`, and `VTX_TIMEOUT` added; the read/write asymmetry corrected (front-end `0x2080`/`0x2140` read-safe, PVS ports `0x2200+` read-wedge, all writes posted-safe); the 16-bit `VF_CNTL` underflow lever + commit `9899a4d8dd3` (SWTCL 16-bit VAP count clamp); the system-value slot-reservation registry; the R2VB CS-write surface. `TCL_BYPASS`/`CLIP_DISABLE`/`NUM_VERTICES` bitfields confirmed against `r300_reg.h` and the write-sweep corpus | -- |
| HBTCL-06 | DONE (compiler-verified, R400_US excluded from the standing budget): the "Lighting on the fragment ALU" section above -- native op set from the `r300_fragprog_emit.c` co-issue switches (`ROUND` corrected to lowered), the 64-slot ceiling as co-issued vector+scalar pairs (up to 128 ops when independent; dependent alpha-pipe transcendentals still serialize), the lighting-term mapping table, and the butterfly/Cayley-Dickson non-result (dense `4x4` is already at its `DP4` floor; CD stays in the matrix-build step). The 512-slot R400_US path remains probe-gated (`R300_HB_R400_US`) and is not a standing budget for HBTCL lighting | -- |
| HBTCL-07 | Root-cause the R2VB points-topology smear; GA point-setup registers (`GA_POINT_SIZE`, `GA_POINT_MINMAX`) and `VAP_VTX_SIZE` remain open hypotheses after near-zero effect measurements -- keep the RCA root-cause-neutral until the no-submit decode names the carrier | HBTCL-03 |
| HBTCL-08 | Promote the generalized R2VB collineation engine to the standing r300 SW-TCL vertex route (gated first); validate on RS482 across topologies + a piglit GL2.1 subset under the poller with no VAP/GA stall | HBTCL-02, HBTCL-04, HBTCL-07 |
| HBTCL-09 | Combine demonstrated MRT multi-attribute export with R2VB so position, normal, and texcoord can leave the producer in one transform pass | HBTCL-03 |
| HBTCL-10 | Probe E2/RB2D/CBA2D vertex-buffer movement as a possible GART-to-VAP-input mover, using the same hazard-governed no-submit/attended style as the rest of RS482 work | HBTCL-03 |

HBTCL-01 and HBTCL-02 are the immediate `fbo-clearmipmap` fix; HBTCL-03 is the
engine audit; HBTCL-04a-04e are the landed contract, divide, and clip ladder,
04f.1-04f.2 the landed emitted-slot admission and float one-`vec4` split, 04f.3
the host-tested typed split primitive whose diagnostic route reachability landed
(04f.3R, ahead of it the landed flat-input epsilon parity 04f.3c) and whose
production admission blocks on source-domain equivalence (04f.3e), 04f.4 parked
on measured demand and 04f.5 open but inactive for the same reason;
HBTCL-07/08 build the fix out into the standing hybrid; HBTCL-05/06/09/10 are
the register-table, lighting, MRT-export, and movement extensions. All hardware
steps run on the parked, hang-for-inspection box under the wedge-forensics
poller; the decode steps submit nothing.

## Open items

- The `fbo-clearmipmap` clear-quad wedge: the no-submit PM4 IB decode ran
  (HBTCL-01), and the SWTCL TEXCOORD_XY blit now falls back to a plain quad,
  completing the 2013 MSAA-resolve fix. HBTCL-02C is a ledger reconciliation: the
  fbo-clearmipmap reproducer's symptoms fold into the armed stale-US lane, whose
  live-trace RCA covers them, and the row stays open only for the remainder that
  a re-run shows independent of the armed-US history.
- The typed one-`vec4` split is a host-tested primitive unreachable through the
  production draw route: `r300_vs_nir_is_fragment_aluable` scans the pre-lowering
  application VS against a float-only whitelist, so a typed carry op declines the
  route before restaging (F3-R0). The fragment backend lowers those ops
  (`r300_nir_lower_bitwise_to_arith`, `nir_lower_int_to_float`, bool-to-float)
  before RC emission, so diagnostic route reachability (04f.3R, mesa PR#1137)
  builds and preflights the restaged FS instead of the pre-lowering VS, and
  flat-input epsilon parity (04f.3c, mesa PR#1126) landed ahead of it. Production admission blocks further on
  source-domain equivalence (04f.3e): the exact-carry proof establishes transport
  exactness, not that the FP24 producer's float-to-integer conversion matches the
  software reference, so the diagnostic gate proves the mechanism without making
  arbitrary typed vertex source arithmetic production-safe.
- R2VB `points` re-ingest still smears (register cause open;
  `GA_POINT_SIZE` / `GA_POINT_MINMAX` / `VAP_VTX_SIZE` each falsified).
- Application vertex buffers cannot become directly relocated BOs
  (`r300_buffer_create` allocates a CPU shadow unless `PIPE_BIND_CUSTOM`), so
  the producer's internal BO carries the transformed result; the app buffer is
  never mutated (matching Mayer's invariant-data principle).
- Generated C/Clight is proven-to-C only after a concrete kernel is accepted by
  CertiRocq and recorded with its artifact, command, assumptions, and proof
  status; the QuatRotation algebra is proven already, but driver integration and
  RS482 execution remain demonstrated/measured.

## Sources

r300 driver: `r300_chipset.c`, `r300_state.c` (`r300_create_rs_state`),
`r300_emit.c`, `r300_reg.h` (VAP register offsets and bitfields in the table),
`r300_hb_tcl.{c,h}`, `r300_r2vb.c` (the R2VB CS-write surface),
`r300_nir_lower_vs_system_values.c` (the system-value slot-reservation
registry). RE corpus (external sibling repository `steinmarder`, the r300
reverse-engineering lane, not in this Mesa tree): the register-opcode-atom
inventory
(`src/re/r300/docs/isa_references/rs4xx_r300_register_opcode_atom_inventory.tsv`)
for the VAP offset set including the `GB_*_ADJ` guard-band block; the
read-reachability inventory
(`src/re/r300/docs/rs482-register-read-reachability-and-reader-inventory.md`)
for the `radeon_rs480_candidate_vap_regs` read-exclusion; the 16-bit `VF_CNTL`
underflow finding and its fix in commit `9899a4d8dd3`
(`r300: clamp SWTCL vertex batches to the 16-bit VAP count limit`;
`findings/active/2026-05-29-rs482-swtcl-vap-16bit-vertex-count-underflow.md`);
the has_tcl/hardware-unit map, the vertex-engine write-only/read-wedge
asymmetry, the R2VB direct-VAP validator-accepted hang-free submit, and the
platform reset/wedge taxonomy. Archive: Glaeser
(1994) linear-collineation split, Hoppe (1999) FIFO post-transform cache, Mayer
(1970) invariant-data axis transform, Artwick (1984) measured clip-versus-matrix
cost split and per-vertex quaternion cost, Owens et al. (2005) vertex stage as
the fragile MIMD/scatter stage, Coxeter (1946) rotation as two reflections.
Rocq proofs: external `open_gororoba` proof tree (`proofs/theories/IDCT8DP4ExactBound.v`),
`proofs/theories/QuatRotationLaws.v`, `verified/C876_QuaternionRotation.v`, and
the corresponding C911 norm-preservation ledger theorem.
