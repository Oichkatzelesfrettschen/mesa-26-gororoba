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
mathematics: Cayley-Dickson algebra, quaternion and octonion norms, and the
FP24 DP4 exact-integer bound. Everything about the RS482 silicon -- that a draw
executes, that the wedge is in the VAP/GA, that a register shape submits
hang-free, that MRT export is byte-exact, that the transform runs at a measured
rate -- is **demonstrated on silicon** or **measured**, never "proven." A
finite hardware run demonstrates; it does not prove.

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
   is exactly the `VTE_CNTL` bypass mode.
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
(2005), independently identifies the vertex processor as the only MIMD,
scatter-capable -- and therefore the most fragile and costly -- pipeline stage.
Together they argue the same split this design takes: put the simple fixed
linear matrix multiply on a hardware block, keep the branchy clip-and-classify
logic in software. Artwick further measures a per-vertex quaternion apply at
about four times the multiplies of a matrix-vector apply, so hypercomplex
rotation (Coxeter, 1946, derives it as the product of two reflections) belongs
only in the once-per-frame matrix-derivation step, never in the per-vertex hot
loop -- the `M` in `v_clip = M * v_object` may be *built* with quaternions, but
each vertex is transformed by the composed 4x4.

## The hybrid HBTCL

The design generalizes the silicon-demonstrated R2VB direct-VAP handoff into the standing
vertex path:

- **Transform** runs in SW (Gallium Draw / gallivm) or, where the vertex shader
  matches the MVP shape (`r300_vs_is_mvp`), on the **fragment ALU** as four
  `DP4`s writing `C4_32_FP` clip-space vertices to a GART buffer. The fragment
  ALU (FP24 VLIW vec4, `MAD/DP3/DP4/MIN/MAX/CMP`, alpha `RCP/RSQ/EX2/LG2`, hard
  64-ALU ceiling) is the only programmable ALU on the part and is the vertex
  engine here.
- **Clip and perspective divide** run in SW per the decomposition above.
- **Delivery**: a `cb_flush_clean` cache barrier, then `3D_LOAD_VBPNTR`
  re-ingest of the transformed buffer, then a `TCL_BYPASS` draw with the shape
  in the previous section. No register in `0x2200-0x2504` is ever written by an
  emit path and none is ever read.
- **Never**: reach `r300_emit_vs_state`, set `has_tcl = true`, clear
  `TCL_BYPASS`, or read anywhere in the vertex-engine window. Each of those
  re-enters the unrecoverable wedge class.

The fix and the redesign are the same insight at two scales: the RS482 vertex
frontend wedges when it is asked to do transform work, so we do the transform
where it is safe and hand the frontend only vertices it can pass straight
through. Converging the blitter clear quad onto the bypass shape is the first
instance; the hybrid makes it the rule.

## All-on-pipeline: one role per block, minimal CPU

The endpoint pushes as much of the vertex layer as possible onto the GPU's own
blocks (RBBM_STATUS names them), leaving the CPU only the small branchy stage
Artwick (1984) measured as cheap to keep and Owens (2005) confirmed is the hard
one to put in fixed hardware. Each role below is either already demonstrated on
silicon or a bounded next increment from two demonstrated pieces.

| Block | Role | Evidence |
| --- | --- | --- |
| Fragment ALU (US, pixel path) | The transform: `M*v` as 4 `DP4`, and non-linear per-vertex math -- quaternion rotation as 4 `DP4` (HW-confirmed, 3/3 within 0.05), octonion 16 `DP4`, Walsh-Hadamard multiply-free and bit-exact in the FP24 window (the exact-integer bound is *proven* in Rocq -- open_gororoba `proofs/theories/IDCT8DP4ExactBound.v`: `8*1448^2 < 2^24` and `2^17 < 2^24`, zero admits) | demonstrated on silicon; 64-ALU ceiling, R400_US lifts to 512 |
| TAM/TDM/TIM (texture) | Fetch the MVP matrix and vertex attributes as textures -- **in the R2VB producer fragment shader**, which can sample; the VAP-side vertex-texture-fetch is architecturally gated off (`GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS = 0`), so this must live in the producer, not the vertex stage | gate measured; producer wiring to build |
| RB3D (3D backend) | One-pass multi-attribute export: four `R8G8B8A8` MRT targets route per-output-location, byte-exact, zero deqp regressions -- position, normal, texcoord each to its own target in a single transform pass | MRT demonstrated byte-exact; combine with R2VB (HBTCL-09) |
| E2/RB2D/CBA2D (2D blit) | Move transformed vertices GART->VAP-input, or scatter/gather vertex streams, instead of a CPU copy + `cb_flush` | unexplored: the 2D engine appears once in the corpus (H.264 block-copy), never probed for vertices (HBTCL-10) |
| VAP (frontend, bypass) | Assemble and output-format-map pre-transformed vertices; idle `VAP_PROG_STREAM_CNTL_1..7` (`0x2154-0x216c`) give headroom for more synthesized attribute channels | demonstrated; never engages the absent PVS |
| GA / RE / SC | Primitive assembly, setup, rasterization, and a rectangular screen-space reject via `SC_SCISSORS`/`SC_CLIPRECT` (already in the demonstrated-safe bypass shape, outside the wedge window) | demonstrated |
| CPU (minimal) | Only the branchy clip (linear domain, per Glaeser) and the scalar perspective divide. The transform + barrier + re-ingest already run in one IB for the passthrough class -- no mid-draw CPU round-trip -- at about 0.023 us/vertex versus 0.83 us/vertex for gallivm (~36x), and the re-ingest raster is bit-identical to gallivm | demonstrated (measured) |

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
- **r300vk instancing** is broken; do not route per-instance retransform through
  native instancing yet.
- **Self-feeding / GPU-computed-address indirect draws** do not exist in the
  corpus; the closest demonstrated amortization is batching one `WAIT_UNTIL` barrier
  across N draws. Mind the CS triple-buffer pipelining race (fixed `9019491c1fd`)
  for any multi-IB scheme.

## Scoped implementation plan

What already exists on this substrate is more than a sketch: `r300_r2vb.c` is a
3289-line fragment-ALU render-to-vertex-buffer engine with a route classifier
(`R2VB_ROUTE_PASSTHROUGH` / `_CANDIDATE` / `REJECT_HW_TCL` / `_INDEXED` /
`_INSTANCED`), a transform fragment shader, self-test buffers, and a no-submit
`R300_R2VB_VS_DUMP` oracle; `r300_hb_tcl.c` carries the static `0x0014025a`
bypass word; `r300_hb_r400_us.h` gates an R400 unified-shader path that lifts
the ALU/TEX slot count from 64 to 512. The plan generalizes this into the
standing vertex route.

| Task | Work | Depends on |
| --- | --- | --- |
| HBTCL-01 | No-submit PM4 decode (the silicon-demonstrated `R300_TRACE` capture + 325-row atom decoder, box-safe) of the clear-quad IB vs a working r3v triangle IB; name the single diverging VAP frontend register (`VAP_VF_CNTL` NUM_VERTICES, `VAP_VTE_CNTL` coord space, `VF_MAX_VTX_NUM`, `SC_SCISSORS` after +1440, `ZB_CNTL.Z_ENABLE`) | -- |
| HBTCL-02 | Converge `util_blitter`'s clear-quad emit onto the demonstrated-hang-free bypass shape; re-run `fbo-clearmipmap` under the forensic poller, confirm the VAP/GA stall clears | HBTCL-01 |
| HBTCL-03 | Audit the 3289-line `r300_r2vb.c` engine against this design: routes, demonstrated topologies, the transform FS, the open points smear; produce the gap list | -- |
| HBTCL-04 | Implement the Glaeser linear-collineation split in the R2VB transform FS (fragment-ALU `M*v`, SW scalar divide, SW clip in the linear domain); use the R400_US ceiling lift where a VS exceeds 64 ALU | HBTCL-03 |
| HBTCL-05 | Fold the concrete VAP register table (offsets, wedge window, the 16-bit `VF_CNTL` underflow lever + its mesa fix, the vertex system-value registry, the R2VB CS-write surface) into this doc | -- |
| HBTCL-06 | Assess the hardware sin/cos (not GL-reachable) plus the fragment-ALU transcendentals for the lighting stage of a full TCL | -- |
| HBTCL-07 | Root-cause the R2VB points-topology smear (GA point-setup registers) via the HBTCL-01 decode method | HBTCL-03 |
| HBTCL-08 | Promote the generalized R2VB collineation engine to the standing r300 SW-TCL vertex route (gated first); validate on RS482 across topologies + a piglit GL2.1 subset under the poller with no VAP/GA stall | HBTCL-02, HBTCL-04, HBTCL-07 |

HBTCL-01 and HBTCL-02 are the immediate `fbo-clearmipmap` fix; HBTCL-03/04/07/08
build the fix out into the standing hybrid; HBTCL-05/06 are the register-table
and lighting extensions. All hardware steps run on the parked, hang-for-
inspection box under the wedge-forensics poller; the decode steps submit
nothing.

## Open items

- The exact diverging value in the clear-quad IB (VTE_CNTL vs scissor vs VF
  NUM_VERTICES): pending the no-submit PM4 IB decode.
- R2VB `points` re-ingest still smears (register cause open;
  `GA_POINT_SIZE` / `GA_POINT_MINMAX` / `VAP_VTX_SIZE` each falsified).
- Application vertex buffers cannot become directly relocated BOs
  (`r300_buffer_create` allocates a CPU shadow unless `PIPE_BIND_CUSTOM`), so
  the producer's internal BO carries the transformed result; the app buffer is
  never mutated (matching Mayer's invariant-data principle).

## Sources

r300 driver: `r300_chipset.c`, `r300_state.c` (`r300_create_rs_state`),
`r300_emit.c`, `r300_reg.h`, `r300_hb_tcl.{c,h}`, `r300_r2vb.c`. RE corpus
(steinmarder-r300 findings): the has_tcl/hardware-unit map, the vertex-engine
write-only/read-wedge asymmetry, the R2VB direct-VAP validator-accepted
hang-free submit, and the platform reset/wedge taxonomy. Archive: Glaeser
(1994) linear-collineation split, Hoppe (1999) FIFO post-transform cache, Mayer
(1970) invariant-data axis transform, Artwick (1984) measured clip-versus-matrix
cost split and per-vertex quaternion cost, Owens et al. (2005) vertex stage as
the fragile MIMD/scatter stage, Coxeter (1946) rotation as two reflections.
