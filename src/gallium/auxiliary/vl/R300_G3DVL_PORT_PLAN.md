# Port plan: restore the g3dvl shader video decoder for r300 gallium-va (mesa 26)

mesa 26 removed only the shader-based g3dvl DECODE kernels (`vl_idct`, `vl_mc`,
`vl_zscan`, `vl_matrix_filter`, `vl_mpeg12_decoder`, `vl_decoder`).  The
PRESENTATION half survives: `vl_compositor{,_cs,_gfx}.c` (gfx and compute paths),
`vl_csc.c`, `vl_deint_filter{,_cs}.c`, `vl_codec.c` (a new support-dispatch),
`vl_video_buffer.c`, `vl_mpeg12_bitstream.c`, `vl_vertex_buffers.c`.  RS482/R300
has no UVD/VCE, so shader decode is the only HW-accelerated video path; this
branch restores the removed decode kernels and re-wires r300's gallium-va backend.
It is a kept PORT BRANCH, NOT merged to main until it builds and passes the kernel
compile-verify.  Decomposition and admissibility proofs:
`docs/.../rs482-mpeg2-decode-verb-decomposition.md`,
`rs482-h264-mpeg4-decode-verb-factoring.md`, the formal monograph.

## Mined kernels (restored into src/gallium/auxiliary/vl/)

`vl_idct.{c,h}` (868L), `vl_mc.{c,h}` (658L), `vl_zscan.{c,h}` (615L),
`vl_matrix_filter.{c,h}` (307L), `vl_mpeg12_decoder.{c,h}` (1239L) -- from
mesa-21.3.3 (the last pre-removal tag).

`vl_decoder.{c,h}` was also restored but then DELETED: its `vl_create_decoder`
dispatch + `vl_profile_supported`/`vl_level_supported` are superseded by mesa-26's
`vl_codec.c` (`vl_codec_supported`, gated on the `VIDEO_CODEC_*` build defines) and
by the driver instantiating the codec directly through its `create_video_codec`
vtable hook + `vl_create_mpeg12_decoder()`.  `vl_create_decoder` has zero callers
in mesa 26.

## Drift assessment (tractable)

Present in mesa 26 (no port needed): `tgsi_ureg.h` (the shader-gen API the kernels
use), `pipe/p_video_codec.h`, `pipe/p_video_enums.h`, and the shared vl headers
`vl_defines.h`, `vl_types.h`, `vl_video_buffer.h`.

## Empirical drift inventory (clang-22 against mesa 26, build oracle)

Wiring done: meson lists the 5 decode kernels in `files_libgalliumvl` (an
unconditional `files()` list -- compiled whenever libgalliumvl builds, i.e. when a
video frontend is enabled); `_va_drivers` includes `with_gallium_r300`; the build
profiles default `video-codecs = ['mpeg12dec']` + `gallium-va = 'enabled'`.  libva
1.23.0 is found.  The removed `pipe/p_compiler.h` include in `vl_zscan.h` is
replaced by `util/compiler.h`.

Remaining drift building `libgalliumvl.a` (~86 errors, 2 files truncated at
`-ferror-limit`, so the true count is higher).  Classes, fix in this order:

1. Mechanical enum renames (safe, high volume): `PIPE_SHADER_{VERTEX,FRAGMENT}` ->
   `MESA_SHADER_*`; `PIPE_PRIM_*` -> `MESA_PRIM_*`; plus a few fully-removed
   identifiers with no compiler suggestion (TGSI/PIPE_CAP leftovers).
2. Struct-field refactors: `pipe_surface` lost `width`/`height` (derive via
   `u_minify(tex->width0/height0, level)` or the new accessor); `pipe_surface` is
   now passed by pointer at several call sites (the "dereference with *" errors);
   `pipe_sampler_state.normalized_coords` removed (mesa-26 sampler-state shape).
3. Vtable arity changes (DANGEROUS -- a clean compile here can be wrong at
   runtime): `set_sampler_views`, `set_constant_buffer`, `draw_vbo`, and similar
   `pipe_context` hooks changed argument counts/shapes.  MUST be matched to the new
   API contract by reading a live caller (`vl_compositor_gfx.c`, radeonsi) -- NOT
   by adding arguments to silence clang.  The only runtime check is hazard-gated on
   vostro, so a wrong arity sits undetected.

## Milestones (a clean libgalliumvl.a is milestone 1 of 3)

1. DONE -- Decode kernels compile: `libgalliumvl.a` links clean on the
   `1_r300_full_release_x86_64v1-clang22-distcc-cache` toolchain, `-Werror`, 0
   `ureg_`/`tgsi_` residual across all five kernels.  All five (matrix_filter,
   zscan, mc, idct, mpeg12_decoder) are pure NIR.
2. r300 backend wiring: add `r300_get_video_param` + `screen.get_video_param` +
   `screen.is_video_format_supported = vl_video_buffer_is_format_supported` +
   `context.create_video_codec` (-> `vl_create_mpeg12_decoder`) to the mesa-26
   r300 screen/context vtables (21.3.3 `r300_screen.c:437/732/734` as reference,
   updated to the new vtable shapes).
3. Hazard-gated runtime: `vainfo` shows the MPEG-2 profile, then end-to-end decode
   of a clip on vostro (tasks 14/28), NIR->RC budget compile-verify (task 27).

### A clean build is NOT validated NIR (the gate between milestone 1 and 3)

Every `create_*_shader` runs at decoder-INIT (`create_fs_state` ->
`finalize_nir` -> `nir_to_rc`), not at build time.  The release build also has
`-DNDEBUG`, so `nir_validate` never runs.  So milestone 1 proves only that the C
is well-formed and the vtable arities match -- it proves NOTHING about whether
the NIR is valid (no undef reads, consistent slots, right component counts) or
whether it lowers to r300 RC within budget.  The first real NIR test is task 27,
at decoder-init, and it is earlier and cheaper than the milestone-3 decode oracle.

MUST do milestone 2 against the DEBUG r300 prefix (`2_r300...debug`), not release:
the debug build runs `nir_validate` at `finalize_nir` (asserts with a location on
malformed NIR) and surfaces RC-budget failures as a clear error.  `vainfo`-level
init then validates and lowers all thirteen shaders before a clip is needed.
Suspect for a budget bust: `create_stage1_frag_shader` (four render targets x
four channels of `matrix_mul` plus ~16 TEX) against RS482's R300-class (not R500)
fragment limits.

MERGE BAR: do NOT merge thirteen never-instantiated shaders to main.  This branch
becomes a merge candidate only after decoder-init validates and lowers to RC on
the debug build -- one small step past milestone 1.  If hardware output is garbage
while the pipeline runs clean, re-derive the unpacked-varying slot map first (the
single structural deviation from the proven shader; internally consistent, so the
highest-probability bug site, not a confirmed one).

## Remaining-class API contracts (mesa 26, verified against source)

Done so far on this branch: superseded `vl_decoder.c` deleted; `p_compiler.h` ->
`util/compiler.h`; `PIPE_SHADER_{VERTEX,FRAGMENT}` -> `MESA_SHADER_*`, `PIPE_PRIM_*`
-> `MESA_PRIM_*`.  Drift 86 -> 56.  Remaining classes, with the exact mesa-26 idiom:

- `pipe_surface` was rebuilt as a lightweight VALUE (p_state.h:411): fields are
  `format:16`, `nr_samples:16`, `first_layer:16`, `last_layer:16`, `level`,
  `texture` -- no `width`, `height`, or `.u` union.  Framebuffer state now embeds
  `struct pipe_surface cbufs[]` BY VALUE, not by pointer.
  * `surf->width` / `surf->height` -> `pipe_surface_size(surf, &w, &h)`
    (`util/u_inlines.h:403`; signature `(const struct pipe_surface *, unsigned *w,
    unsigned *h)`), or `u_minify(surf->texture->width0/height0, surf->level)`.
  * `surf->u.tex.level/first_layer/last_layer` -> `surf->level/first_layer/
    last_layer` (union flattened).  MUST be edited per site, NOT sed'd: a blind
    `.u.tex.` rewrite would corrupt `pipe_sampler_view`, which KEEPS its `.u.tex`.
  * `pipe_context.create_surface` is gone (the "no member create_surface" +
    "assigning to pipe_surface from pipe_surface*; dereference with *" errors): the
    kernels' render-target setup must move to the value-embedded framebuffer model
    that `vl_compositor_gfx.c` uses (build `pipe_surface` inline, assign into
    `fb_state.cbufs[i]` by value).  Deepest sub-fix; do per kernel.
- `pipe_sampler_state.normalized_coords` -> `unnormalized_coords` (p_state.h:468,
  INVERTED sense): `normalized_coords = 1` -> drop (default 0); `= 0` ->
  `unnormalized_coords = 1`.  Check each site's sense.
- `pipe_screen.get_param`/`get_shader_param` -> the `pipe_caps`/`shader_caps`
  structs (~mesa 23): `screen->get_param(s, PIPE_CAP_MAX_RENDER_TARGETS)` ->
  `screen->caps.max_render_targets` (p_defines.h:1075); `PIPE_SHADER_CAP_MAX_
  INSTRUCTIONS` -> `screen->shader_caps[MESA_SHADER_*].max_instructions`;
  `PIPE_CAP_TGSI_FS_POSITION_IS_SYSVAL` was REMOVED -- FS position is always a
  sysval now, so take that branch unconditionally and delete the query.
- Arity class (DANGEROUS -- compiles wrong, runtime-only check, hazard-gated on
  vostro): `pipe_context` hooks `set_sampler_views`, `set_constant_buffer`,
  `draw_vbo` changed shape.  Match each to a LIVE caller (`vl_compositor_gfx.c`,
  radeonsi `si_*`), not by counting args.  Do LAST, one site at a time, each
  against its reference caller.

## Pure-NIR shader conversion (directive: deprecate TGSI completely)

The render/state plumbing above is IR-agnostic and stays.  The ureg/TGSI shader
GENERATORS (~13 builders, ~293 ureg ops: idct 125, mc 99, zscan 39,
matrix_filter 25, mpeg12_decoder 5) are rewritten as `nir_builder`.  r300 is
NIR-native (`r300_screen.c` advertises `PIPE_SHADER_IR_NIR`; `r300_fs.c` lowers
via `nir_to_rc`), so NIR feeds `create_fs_state`/`create_vs_state` directly.

HAZARD (advisor-flagged): from-scratch NIR decode math compiles clean but can be
garbage; the only oracle is the hazard-gated Vostro probe (tasks 14/28).  Derive
each shader from the monograph math, build on the `1_r300` clang-22/distcc/ccache
toolchain, then differentially validate on hardware.

### r300-native NIR builder template (verified: `r300_fs.c:174`, `vl_compositor_cs.c:119`)

```c
const nir_shader_compiler_options *opt =
    pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opt, "vl:NAME");
/* texture+sampler: one glsl_sampler_type var, nir_var_uniform, .data.binding=i */
const struct glsl_type *st = glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false,
                                               GLSL_TYPE_FLOAT);
nir_variable *samp = nir_variable_create(b.shader, nir_var_uniform, st, "samp");
samp->data.binding = 0;
nir_deref_instr *sd = nir_build_deref_var(&b, samp);
/* input varying texcoord, output color */
nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                        glsl_vec4_type(), "tc");
in->data.location = VARYING_SLOT_VAR0;                 /* TGSI GENERIC[0] */
nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                        glsl_vec4_type(), "col");
out->data.location = FRAG_RESULT_COLOR;
/* ... build dataflow ... */
nir_store_var(&b, out, result, 0xf);
struct pipe_shader_state s = {0};
s.type = PIPE_SHADER_IR_NIR;
s.ir.nir = b.shader;
return pipe->create_fs_state(pipe, &s);   /* r300 finalizes + nir_to_rc */
```

### ureg -> nir_builder op atlas

| ureg (TGSI) | nir_builder | Notes |
|---|---|---|
| `ureg_create(MESA_SHADER_FRAGMENT)` | `nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opt, name)` | opt from `screen->nir_options[stage]` |
| `ureg_DECL_vs_input(u, i)` | `nir_variable_create(.., nir_var_shader_in, vec4, ..)`, `.location=VERT_ATTRIB_GENERIC0+i`; `nir_load_var` | |
| `ureg_DECL_fs_input(GENERIC, k, LINEAR)` | in var `.location=VARYING_SLOT_VAR0+k`; `nir_load_var` | interp is default smooth |
| `ureg_DECL_output(POSITION,0)` (VS) | out var `.location=VARYING_SLOT_POS` | |
| `ureg_DECL_output(GENERIC,k)` (VS) | out var `.location=VARYING_SLOT_VAR0+k` | |
| `ureg_DECL_output(COLOR,0)` (FS) | out var `.location=FRAG_RESULT_COLOR` | |
| `ureg_DECL_sampler(i)`+`_sampler_view` | one `glsl_sampler_type` uniform var, `.binding=i`, `nir_build_deref_var` | combined in NIR |
| `ureg_DECL_system_value(POSITION)` | `nir_load_frag_coord(&b)` | replaces PIPE_CAP_TGSI_FS_POSITION_IS_SYSVAL branch -- always sysval |
| `ureg_MOV(dst,src)` | direct SSA use / `nir_mov` | |
| `ureg_ADD` | `nir_fadd` | |
| `ureg_MUL` | `nir_fmul` | |
| `ureg_MAD(d,a,b,c)` | `nir_ffma(b,a,b,c)` | a*b+c |
| `ureg_TEX(d,2D,coord,samp)` | `nir_tex(&b, coord, .texture_deref=sd, .sampler_deref=sd)` | coord = vec2 (xy) |
| `ureg_imm1f/2f/4f` | `nir_imm_float/vec2/vec4` | |
| `ureg_writemask(d, XY)` | `nir_channels(def, 0x3)` / component build | |
| `ureg_END`+`ureg_create_shader_and_destroy` | `s.type=IR_NIR; s.ir.nir=b.shader; create_fs_state` | |

### Worked: vl_matrix_filter shaders in NIR (separable conv = monograph P3)

VS passthrough (`pos -> {POSITION, GENERIC0=pos}`):

```c
nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
    pipe->screen->nir_options[MESA_SHADER_VERTEX], "vl:mf_vs");
nir_variable *ip = nir_variable_create(b.shader, nir_var_shader_in,
    glsl_vec4_type(), "ipos"); ip->data.location = VERT_ATTRIB_GENERIC0;
nir_variable *op = nir_variable_create(b.shader, nir_var_shader_out,
    glsl_vec4_type(), "opos"); op->data.location = VARYING_SLOT_POS;
nir_variable *ot = nir_variable_create(b.shader, nir_var_shader_out,
    glsl_vec4_type(), "otex"); ot->data.location = VARYING_SLOT_VAR0;
nir_def *p = nir_load_var(&b, ip);
nir_store_var(&b, op, p, 0xf);
nir_store_var(&b, ot, p, 0xf);     /* vl quad: texcoord == position */
```

FS (`sum_i matrix[i] * TEX(vtex + offsets[i])`, the >= rank-1 P3 convolution):

```c
/* template header above; samp at binding 0, in tc=VAR0, out col=COLOR */
nir_def *vtex = nir_trim_vector(&b, nir_load_var(&b, in), 2);   /* xy */
nir_def *sum  = nir_imm_vec4(&b, 0, 0, 0, 0);
for (i = 0; i < num_offsets; ++i) {
    if (matrix_values[i] == 0.0f) continue;
    nir_def *c = is_vec_zero(offsets[i]) ? vtex :
        nir_fadd(&b, vtex, nir_imm_vec2(&b, offsets[i].x, offsets[i].y));
    nir_def *t = nir_tex(&b, c, .texture_deref = sd, .sampler_deref = sd);
    sum = nir_ffma(&b, t, nir_imm_vec4(&b, matrix_values[i], matrix_values[i],
                                       matrix_values[i], matrix_values[i]), sum);
}
nir_store_var(&b, out, sum, 0xf);
```

The blend ADD/ONE/ONE state (A8) still accumulates across the instanced quads, so
per-pass output is one tap; identical numerics to the TGSI version, ALU/TEX within
D6.  `nir_ffma` is the FP24 MAC (L1); the offsets land on the A3 grid.

### Per-kernel order + monograph anchor

1. `vl_matrix_filter` (above) -- P3 separable convolution.  Validated template.
2. `vl_zscan` -- gather/scatter by zigzag index texture; inverse-quant MAC + clamp.
3. `vl_mc` -- `calc_line` interlace select (`frac(pos.y/2) >= 0.5`, now
   `nir_load_frag_coord`), half-pel bilinear (L3), residual ADD + `MOV_SAT`
   (`nir_fsat`).
4. `vl_idct` -- separable DCT-III matrix-MAC, two passes (Theorem S4); the 125-op
   kernel; stage/transpose intermediate via the multipass surface.
5. `vl_mpeg12_decoder` -- orchestration; 5 ureg ops are glue, wire the NIR shaders.

### Hardware validation loop (every shader)

```sh
# build on the 1_r300 clang-22 + distcc + ccache toolchain
ninja -C build-port src/gallium/auxiliary/libgalliumvl.a   # NIR->RC compile-verify (task 27)
# then on cachyos-vostro1000 (hazard-gated, tasks 14/28): vainfo MPEG2 profile,
# decode a known clip, diff frames vs a reference decoder (the only correctness oracle)
```

### Status + remaining-kernel conversion specifics

DONE (compiles clean on 1_r300, committed): `vl_nir.{c,h}` scaffolding;
`vl_matrix_filter` fully pure-NIR (validated template).

The scaffolding currently assumes a passthrough VS and 2D float samplers.  The
remaining three kernels break those assumptions, so extend `vl_nir` first:

- Configurable sampler dim: replace the fixed-2D loop in `vl_nir_fs_begin` with a
  per-sampler `enum glsl_sampler_dim` (zscan quant and idct matrix are 3D /
  `GLSL_SAMPLER_DIM_3D`); give `vl_nir_tex` a dim/coord-size argument.
- Custom VS: zscan's VS is not a passthrough; add a builder that exposes
  `vl_nir` I/O setup but lets the kernel emit the position/texcoord math.
- Per-channel pack: the TGSI `WRITEMASK_X << i` idiom that fills channel i of a
  vec4 becomes building a `nir_vec`/`nir_vec4` from `num_channels` scalar defs.

`vl_zscan` (2 shaders) -- `create_vert_shader` (CCN 46): `o_vpos.xy=(vpos+vrect)
*scale`, `zw=1`; per-channel texcoord from `block_num` via `nir_ffract`/
`nir_ffloor` + the `1/(blocks_per_line*BLOCK_WIDTH)*(i - num_channels/2)` offset,
packed as a `nir_vec4{x=MAD(vrect,1/bpl,tmp), y=vrect, z=vpos, w=MUL(tmp,bpl/
total)}`.  `create_frag_shader` (CCN 39): samp0=src(2D), samp1=scan(2D),
samp2=quant(3D); per channel `t = tex(tex(vtex,scan).x-coord, src)`,
`q = tex(vtex,quant)*16`, pack channels, `frag = t * q` (inverse scan + inverse
quant, monograph S-stage).

`vl_mc` (4 shaders) -- `calc_position`/`calc_line` (interlace select
`frac(frag_coord.y/2) >= 0.5` via `nir_load_frag_coord`, replacing the removed
PIPE_CAP_TGSI_FS_POSITION_IS_SYSVAL branch); `create_ref_*` (half-pel bilinear
TX = L3, residual ADD + `nir_fsat` = MOV_SAT); `create_ycbcr_*` (the per-plane
fetch + combine).

### Coupling finding (cflow): mc + idct + mpeg12_decoder convert TOGETHER

DONE pure-NIR (compiles clean, committed): `vl_matrix_filter`, `vl_zscan`.  Those
were independent.  The remaining three are NOT independent -- `cflow` on
`vl_mpeg12_decoder.c` shows `mc_vert_shader_callback` (mpeg12_decoder.c:1066)
calls `vl_idct_stage2_vert_shader()`, and `mc_frag_shader_callback` (:1088) emits
the source TEX into mc's ycbcr fragment shader.  mc's ycbcr builders take these
callbacks as `vl_mc_ycbcr_vert_shader`/`vl_mc_ycbcr_frag_shader` typedefs whose
signature is `(..., struct ureg_program *shader, ...)`.  So the conversion is one
coordinated change, in this order:

1. `vl_mc.h` -- change the two callback typedefs (verified at vl_mc.h:68-76).
   From:
   ```c
   typedef void (*vl_mc_ycbcr_vert_shader)(void *priv, struct vl_mc *mc,
       struct ureg_program *shader, unsigned first_output, struct ureg_dst tex);
   typedef void (*vl_mc_ycbcr_frag_shader)(void *priv, struct vl_mc *mc,
       struct ureg_program *shader, unsigned first_input, struct ureg_dst dst);
   ```
   To (the frag callback RETURNS the textured value rather than writing a passed
   dst, since NIR is SSA; the vert callback gets the nir_builder + the position):
   ```c
   typedef void (*vl_mc_ycbcr_vert_shader)(void *priv, struct vl_mc *mc,
       nir_builder *b, unsigned first_output, nir_def *tex);
   typedef nir_def *(*vl_mc_ycbcr_frag_shader)(void *priv, struct vl_mc *mc,
       nir_builder *b, unsigned first_input, nir_def *dst_in);
   ```
   `vl_mc.h` then includes `compiler/nir/nir_builder.h` (or fwd-declares the nir
   types) instead of `tgsi/tgsi_ureg.h`.
2. `vl_mpeg12_decoder.c` -- rewrite `mc_vert_shader_callback` (emit the GENERIC
   texcoord output via nir; it also chains `vl_idct_stage2_vert_shader`) and
   `mc_frag_shader_callback` (the source `nir_tex`).  11 ureg ops here.
3. `vl_idct.c` -- `vl_idct_stage2_vert_shader`/`_frag_shader` first (the callback
   targets), then `create_stage1_*`, `create_mismatch_*`, and `matrix_mul`
   (the separable DCT-III matrix-MAC over the 3D matrix texture).
4. `vl_mc.c` -- `calc_position`/`calc_line` (NIR helpers; calc_line uses
   `nir_load_frag_coord`), `create_ref_vert/frag` (the IF/ENDIF y-adjust becomes
   `nir_bcsel`; CMP becomes `nir_bcsel`), `create_ycbcr_vert/frag` (IF/ELSE/KILL
   become `nir_push_if`/`nir_discard`; the callbacks fill the body).

cloc: the cluster is 1966 LOC.  lizard CCN hotspots: `vl_mpeg12_end_frame` 24,
`vl_create_mpeg12_decoder` 16, `vl_mpeg12_decode_macroblock` 15 (state plumbing,
IR-agnostic -- those do NOT change), `create_stage1_frag_shader` 9,
`create_ycbcr_vert_shader` (control flow).  Gate when done: `semgrep`/`grep`
for residual `ureg_`/`tgsi_` across the five kernels must be 0.

`vl_idct` (6 shaders) -- the heaviest; `matrix_mul` is the separable DCT-III
matrix-MAC (Theorem S4) reading a 3D matrix texture; stage1 transposes into the
intermediate surface, stage2 completes the second 1-D pass; `mismatch` applies
the IEEE-1180 oddification.  NIR->RC 64-ALU/32-TEX budget check (task 27) matters
most here.  `calc_addr`/`increment_addr`/`fetch_four` are gather-address helpers
-> `nir` integer/float address math feeding `nir_tex` coords.

## Build / iterate / verify order

1. Wire meson; build vl/ kernels (video enabled) under the canonical clang-22
   ccache+distcc path; fix the surfaced drift errors iteratively (expect items
   2-3 plus a few ureg/enum renames the compiler flags).
2. Re-add the r300 backend (item 4); build the r300 driver with va.
3. Compile-verify the IDCT/MC kernels reach the r300 RC within the
   64-ALU/32-TEX budget (task 27; the monograph's L2.1/B2/B4 bounds).
4. Merge to main only once it builds clean.
5. Pull on cachyos-vostro1000; rebuild via build-infra
   `1_r300_full_release_x86_64v1-clang22-distcc-cache` (and/or the -debug profile
   to /opt/user); install via PKGBUILD; probe MPEG-2 decode hazard-gated
   (tasks 14/28) against the real system dirs.

## Scope

MPEG-2 main first (I-frame-only is the minimal viable path, then P/B); then
MPEG-4 ASP (reuses the kernels + quarter-pel); H.264 needs the additional 6-tap
luma + integer transform + intra-wavefront work (separate, larger). "Even
minimal, it plays."

## Authoritative target and execution boundary

- Canonical file to maintain in this lane:
  `src/gallium/auxiliary/vl/R300_G3DVL_PORT_PLAN.md` in `mesa-26-gororoba`.
- Execution workspace for active changes: isolated Git worktree under `/tmp`
  with one mechanism-named branch per lane.
- Keep Mesa build/install/test standalone.  Steinmarder is evidence and RCA
  support; do not make Mesa build steps depend on steinmarder paths.

## Coupled surface map (Mesa + steinmarder)

| Surface | File(s) | Why coupled |
|---|---|---|
| NIR helper substrate | `vl_nir.{c,h}` | Shared builder path used by decode kernels. |
| Decode kernel core | `vl_matrix_filter.c`, `vl_zscan.c`, `vl_mc.c`, `vl_idct.c`, `vl_mpeg12_decoder.c` | Shader decode execution graph; callback and resource contracts cross files. |
| Decode integration | `r300_video.{c,h}`, `r300_context.c`, `r300_screen.c` | Gallium video capability advertisement and codec constructor wiring. |
| Build wiring | `src/gallium/auxiliary/meson.build` | Ensures kernels are in `files_libgalliumvl` when video frontend is enabled. |
| Formal derivation anchor | `steinmarder/src/re/r300/docs/rs482-fragment-simt-mac-formal-monograph.md` | First-principles bounds and admissibility constraints for shader math. |
| Lane policy/ledger | `steinmarder/src/re/r300/docs/RS482_RS485_NEXT_WORK_ROADMAP.md` | Evidence gates and ordering discipline for RS482/RS485 work. |

## Recursive execution graph (mechanism-ordered)

1. **r300_g3dvl_surface_inventory**
   - Freeze the coupled file set and function map.
   - Output: per-file owner and contract table.
2. **r300_g3dvl_shader_graph_lock**
   - Confirm shader-call graph edges and callback signatures across
     `vl_mc`/`vl_idct`/`vl_mpeg12_decoder`.
   - Falsifier: any remaining TGSI-shaped callback signature.
3. **r300_g3dvl_nir_contract_close**
   - Complete any missing `vl_nir` helper capabilities (sampler dims,
     non-passthrough VS helper, channel pack helpers) required by remaining
     decode shaders.
   - Falsifier: kernel-local duplicated helper logic that should be shared.
4. **r300_g3dvl_decode_kernel_close**
   - Finish decode-kernel NIR conversion and remove residual TGSI decode-path
     constructs.
   - Falsifier: `ureg_`/`tgsi_` hits in decode kernels.
5. **r300_g3dvl_backend_contract_close**
   - Keep `r300_video` capability wiring and `create_video_codec` contract
     synchronized with mesa-26 pipe API and video-frontend expectations.
   - Falsifier: `vainfo` path misses MPEG-2 capability after build/install.
6. **r300_g3dvl_budget_and_init_gate**
   - Validate decoder-init shader creation/lowering path and RC budget behavior.
   - Falsifier: init-time compile/lower errors or RC budget overflow.
7. **r300_g3dvl_runtime_oracle**
   - Run hazard-gated runtime decode oracle and compare decoded output against a
     reference decoder.
   - Falsifier: frame mismatch or runtime instability.
8. **r300_g3dvl_submission_batch**
   - Commit mechanism-bounded batches, open PR, and merge only after gates hold.

## Falsification and gate matrix

| Claim class | Expected evidence | Falsifier | Gate command or artifact |
|---|---|---|---|
| Decode kernels are pure-NIR | No decode-path TGSI/ureg residue | Any decode-file `ureg_` or `tgsi_` symbol | `rg '\\bureg_|tgsi_ureg' src/gallium/auxiliary/vl/*.{c,h}` |
| API drift is fully closed | Build clean with current pipe API signatures | Arity or struct-shape compile error in decode path | `ninja -C <builddir> src/gallium/auxiliary/libgalliumvl.a` |
| r300 wiring is complete | Video caps and codec constructor exposed | Missing MPEG-2 profile/entrypoint in frontend probe | `vainfo` on target runtime lane |
| NIR lowers correctly at init | Decoder-init reaches finalized NIR and RC lowering | Init-time lower/validate failure | debug build init trace and compiler output |
| Runtime path is correct | Known clip decodes with expected frames | Corrupt output, mismatch, or instability | retained hazard-gated runtime bundle |

## Code-review and commit batching protocol

1. One mechanism per commit subject (`r300:` / `vl:` prefix and stable mechanism
   name).
2. No formatting-only churn mixed with behavior changes.
3. Every batch states what is proven: build-only, init-lowering, or runtime.
4. Every unresolved risk remains explicit in this plan until closed by evidence.
5. PR merge bar: never merge decode-kernel expansions that have not crossed at
   least the build and decoder-init gates.

## Repo-wide Git LFS normalization lane

Repo-wide LFS pointer audit result (this iteration):

- `mesa-26-gororoba`: no Git LFS pointer files found.
- `steinmarder`: no Git LFS pointer files found.

Normalization policy for future sweeps:

1. Detect pointer files by exact LFS header signature.
2. Convert only when real content is available and attributable.
3. Preserve provenance and hashes in the commit narrative for converted files.
