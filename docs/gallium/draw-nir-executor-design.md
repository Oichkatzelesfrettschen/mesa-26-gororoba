# Design: a direct-NIR software vertex-shader executor for the draw module

## Goal

Give the Gallium draw module a vertex-shader executor that runs NIR
directly, so a driver handing draw a PIPE_SHADER_IR_NIR shader (r300's
SW-TCL path, r300_vs_draw.c r300_draw_init_vertex_shader) no longer
round-trips through TGSI to reach the only CPU-side interpreter in the
tree. nir_to_tgsi (draw_vs_exec.c create path) stops being a mandatory
step for NIR input; the TGSI input case and every other consumer of
struct draw_vertex_shader stay unchanged.

## Constraints

- No LLVM on r300 SW-TCL profiles. draw_create_vertex_shader takes the
  LLVM branch only when draw->pt.middle.llvm is set (draw_vs.c) and
  falls through to draw_create_vs_exec otherwise; the new executor must
  work with draw->llvm == NULL, the same precondition the TGSI-exec path
  asserts today.
- Clip/edgeflag/viewport/clipdistance semantics preserved.
  draw_create_vertex_shader scans vs->info.output_semantic_name/index[]
  to populate position_output, edgeflag_output, clipvertex_output,
  viewport_index_output, and ccdistance_output[]; the new executor must
  populate vs->base.info in the same shape before that scan runs.
- TGSI-input drivers byte-for-byte unregressed. draw_create_vs_exec is
  one factory for both input kinds; the tgsi_dup_tokens branch stays
  untouched, so the guarantee holds by construction.
- draw_find_shader_output vocabulary is load-bearing for r300:
  r300_state_derived.c and r300_render.c locate POSITION / PCOORD /
  FACE / GENERIC slots in the post-transform vertex through it, and the
  returned index feeds r300_draw_emit_attrib directly.

## Current bridge anatomy

struct exec_vertex_shader wraps struct draw_vertex_shader plus a
tgsi_exec_machine pointer. create retypes NIR input to TGSI via
nir_to_tgsi, or deep-copies TGSI tokens, then tgsi_scan_shader fills
tgsi_shader_info and the shader points at the machine draw_vs_init
created once per context. prepare rebinds the shared machine on every
bind (tgsi_exec_machine_bind_shader) because the machine is shared
across every exec shader instance. run_linear marshals constants
(tgsi_exec_set_constant_buffers), seeds system values from
draw->instance_id / start_instance / fetch_elts / eltBias / start_index
gated on the info uses_* flags, swizzles AOS inputs into the machine's
SOA Inputs in MAX_TGSI_VERTICES (4) chunks, runs
tgsi_exec_machine_run, and unswizzles Outputs back to AOS with the
clamp_vertex_color SATURATE gate keyed on output_semantic_name.
delete frees the token copy; the machine outlives it.

## No existing NIR CPU interpreter exists to reuse

Searched src/compiler/nir for an evaluator. nir_opt_constant_folding is
a shader-to-shader pass, not an executor. nir_eval_const_opcode
(nir_constant_expressions.h) evaluates any single ALU opcode over
nir_const_value operands but knows nothing of intrinsics, control flow,
or textures. nir_to_tgsi is a compiler, not an interpreter. The only
extant CPU vertex executor is tgsi_exec_machine, reached exclusively
through the bridge this design removes; the bridge remains only as the
calibration fallback below. A new draw-owned interpreter is therefore
the design, not one of two options.

## Design: direct-NIR scalar interpreter owned by draw

Interpret nir_shader directly instead of lowering to a bespoke vec4 IR:
nir_eval_const_opcode already implements every nir_op as a pure
function, so an interpreter keeping an SSA-def-indexed value table
delegates ALL ALU instructions to it unmodified and hand-writes only:

- intrinsics: load_ubo (constants), load_input / store_output (vertex
  IO), the system-value loads (load_vertex_id, load_vertex_id_zero_base,
  load_base_vertex, load_instance_id, load_base_instance);
- control flow: nir_if / nir_loop evaluation against per-vertex
  condition values -- real, not folded: r300_vs_draw.c states the
  interpreted shader keeps its loops and branches (the HW-TCL
  flattening is deliberately skipped on this path);
- texture intrinsics if a corpus shader uses them (open question below).

Scalar-per-vertex execution replaces tgsi_exec's SIMD-4 quads; the
path is already documented as slow-by-design for r300-without-LLVM, and
4-vertex chunking can return later for locality without correctness
impact.

### Factory split

draw_create_vs_exec keeps its type check. TGSI branch: unchanged
(tokens, scan, machine vtable). NIR branch: build struct
nir_vertex_shader { draw_vertex_shader base; nir_shader *nir;
interpreter state } with its own vtable:

- create: take ownership of the ralloc'd nir_shader (delete frees it
  the way draw_vs_llvm.c does); run nir_lower_uniforms_to_ubo when the
  options do not already lower, mirroring the LLVM factory, so all
  uniform reads become load_ubo; call nir_tgsi_scan_shader(nir,
  &vs->base.info, true) -- the SAME function draw_vs_llvm.c uses, whose
  header comment says the plain C draw path needs it too; precompute
  the def-to-slot table once here, not per vertex.
- prepare: assert !draw->llvm; no shared-machine rebind hazard exists
  because each shader owns its NIR and scratch.
- run_linear (same signature): bind the draw_buffer_info constants
  array as the load_ubo backing store; seed the sysval intrinsic
  bindings from the identical draw fields and arithmetic the TGSI path
  reads (including the basevertex derivation), so numeric results match
  bit-for-bit; bind AOS input rows per info.num_inputs slot; walk the
  function body per vertex; write outputs per info.num_outputs slot
  with the identical clamp_vertex_color SATURATE gate keyed on
  output_semantic_name (info-driven logic that ports verbatim).
- delete: ralloc_free the nir_shader and scratch; no token FREE.

### Info population and output-vocabulary continuity

nir_tgsi_scan_shader fills num_inputs/num_outputs,
output_semantic_name/index[], the uses_* flags, and every field
draw_context.c and draw_vs.c read. draw_find_shader_output linear-scans
that array and returns the slot index r300 then uses as a
vertex-output-buffer offset. THE INTERPRETER'S OUTPUT ARRAY INDEX FOR
SLOT N MUST EQUAL THE SLOT THE SCAN DESCRIBES AT N -- driven by the
same NIR output location assignment the scan reads, never by SSA def
order or interpreter-private numbering. Getting this wrong corrupts
POSITION/PCOORD/FACE/GENERIC routing with no assertion firing; it is
the design's primary silent-regression hazard and the calibration
corpus tests it directly.

## Calibration plan

Known-good: pass-through position+color VS; a PCOORD/point-sprite VS;
a wide-point derivative-injection VS; at least one VS with a live
nir_if/loop. Run old bridge and new interpreter on identical input,
diff outputs within an epsilon accounting for SIMD-4 vs scalar FP
ordering. Known-bad probes: all uses_* sysval flags at once; CLIPDIST
at multiple indices; missing CLIPVERTEX (exercising the
position_output fallback). Leave `DRAW_NIR_EXEC` unset to retain the
`nir_to_tgsi` plus `tgsi_exec` branch. Set `DRAW_NIR_EXEC=1` only when
calibrating the direct executor.

## Calibration results

Built and run on ATI RS480 (RS482 IGP) through a surfaceless-EGL FBO harness: a
24-shader corpus covering arithmetic and mul-add, dot2/dot3,
cross/reflect/normalize/length,
min/max/clamp/mix/step/smoothstep, floor/fract/mod, abs/sign, mat2 and mat3
transforms, the transcendentals sin/cos/exp2/log2/pow/sqrt/rsqrt, float-domain
compare and select (sge/slt/fcsel_gt from GLSL comparisons and ?:), and control
flow.  Each shader renders one triangle; the raw RGBA8 readback is diffed
between the bridge (DRAW_NIR_EXEC unset) and the interpreter (set).  All 24 are
byte-for-byte identical -- transcendentals included, since the 8-bit readback
absorbs the tgsi_exec-approximation vs nir_eval_const_opcode-libm difference
well within one LSB, so the precision-parity epsilon never had to be spent.

Control flow reaches the interpreter on this path: DRAW_NIR_EXEC_STATS reports a
live nir_loop (the uniform-bounded loop, which loop unrolling cannot resolve to
a static trip count) and surviving nir_if nodes (a loop with a data-dependent
break).  Interpreting either requires phi evaluation: nir_lower_io/DCE leave
loop-carried and if-merge values as phi nodes, so interp_block resolves each phi
by selecting the source whose predecessor block is the one the walk arrived on
(interp maintains prev_block).  Shaders whose branches nir_opt_peephole_select
flattened to fcsel_gt reach the interpreter as straight-line SSA and exercise
the select opcode instead.

DRAW_NIR_EXEC_STATS (opt-in) dumps the post-lowering opcode/intrinsic set and
if/loop counts per shader so the corpus proves which coverage it reached rather
than asserting it.

## Test obligations

r300 SW-TCL config against the corpus plus the piglit/deqp subsets that
exercise the draw_find_shader_output call sites (point sprites,
two-sided color, wide-point derivatives); one TGSI-input driver's suite
unmodified to confirm the untouched branch.

## Open questions

- Do any r300 SW-TCL vertex shaders reach texture sampling through
  draw?  tgsi_exec wires sampler/image/buffer on every bind; the
  interpreter needs equivalents only if a real shader uses them.
- Per-vertex vs 4-chunk batching: performance only, deferred.
- Precision parity epsilon for transcendental/fused ops between
  tgsi_exec and nir_eval_const_opcode, decided before treating diffs as
  regressions.
- Whether the SSA value table persists on the shader (reset per run) or
  allocates per run_linear call.
- Sharing the nir_lower_uniforms_to_ubo idempotency check between the
  LLVM and exec factories to avoid drift.
