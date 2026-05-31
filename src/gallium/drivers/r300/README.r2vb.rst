RS482 render-to-vertex-buffer (R2VB) synthesized-vertex path
============================================================

Why this exists
---------------

RS480/RS482-class IGPs have no hardware vertex shader (``num_vert_fpus == 0``),
so every draw transforms vertices on the CPU through the gallivm SWTCL draw
module.  On this silicon that path is throughput-bound at roughly one million
vertex transforms per second, flat in vertex count.  The fragment ALU runs the
same ``mat4*vec4`` transform two-and-a-half orders of magnitude faster, and a
GART read-back of the result is a small fixed cost (the part is UMA, so the
"render target" and the "vertex buffer" are the same memory).

The R2VB idea spends that asymmetry: render the transformed clip-space vertices
into a GART buffer through the color buffer, make them visible with the
``cb_flush_clean`` cache barrier, then re-ingest the same buffer as the vertex
array via an in-IB ``R300_PACKET3_3D_LOAD_VBPNTR`` and draw it with the VAP in
``TCL_BYPASS`` (the pre-transformed, no-PVS path RS480 already uses for every
SWTCL draw).  The rebind is entirely command-stream side -- the CP executes the
``LOAD_VBPNTR`` after the barrier; there is no CPU trap and no re-issued ioctl.

``draw-use-llvm`` is orthogonal
-------------------------------

``draw-use-llvm=true`` routes the gallivm draw module through the LLVM JIT and
raises the CPU vertex-transform floor; it is a production performance lever for
the SWTCL path and is independent of R2VB.  R2VB moves the transform off the CPU
entirely.  The two do not compete: a build may enable the JIT for ordinary draws
and still use R2VB for transform-heavy workloads.

Status
------

``r300_r2vb.c`` (``r300_emit_rs482_r2vb_compute_loop``) emits the two-pass
sequence, grounded in the driver's verified emitters: the color-target setup and
draw mirror the SWTCL vertex-list emitters, the barrier matches
``r300_emit_gpu_flush`` / the ``cb_flush_clean`` sequence in ``r300_context.c``,
and the ``LOAD_VBPNTR`` mirrors ``r300_emit_vertex_arrays_swtcl`` exactly
(``size|stride<<8`` in dwords, the reserved zero dword, the NOP-form relocation).
The output buffer is an FP32x4 (``R300_COLOR_FORMAT_ARGB32323232``) target, since
clip-space positions are signed and carry ``w``.

The CB-write -> barrier -> vertex-fetch data path is coherency-validated (a
fragment-rendered buffer round-trips into the vertex array and rasters the
correct geometry).  What remains unmeasured is the timing of the gallivm-free
direct-VAP draw; that is a raw PM4 submit and the function is therefore built but
not yet wired to a caller.  The caller binds the "vertex compute" fragment
program, the GART framebuffer, and the pass-1 geometry through the normal r300
pipe state path before calling the loop; see the contract comment in
``r300_r2vb.c``.

That split is deliberate.  Today stage 1 is fragment-generated, and stages 2-3
are the producer-agnostic barrier plus ``LOAD_VBPNTR`` / ``TCL_BYPASS`` oracle
half.  If a future hazard-gated RS482 audit proves the PVS bank can safely
produce the same FP32x4 clip-space buffer, only stage 1 changes; the current
R2VB implementation is not itself evidence that hardware PVS already works on
the normal RS482 Mesa route.
