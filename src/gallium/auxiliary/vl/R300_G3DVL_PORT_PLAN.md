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

1. Decode kernels compile: `libgalliumvl.a` builds clean (the drift above).
2. r300 backend wiring: add `r300_get_video_param` + `screen.get_video_param` +
   `screen.is_video_format_supported = vl_video_buffer_is_format_supported` +
   `context.create_video_codec` (-> `vl_create_mpeg12_decoder`) to the mesa-26
   r300 screen/context vtables (21.3.3 `r300_screen.c:437/732/734` as reference,
   updated to the new vtable shapes).
3. Hazard-gated runtime: `vainfo` shows the MPEG-2 profile, then end-to-end decode
   of a clip on vostro (tasks 14/28), NIR->RC budget compile-verify (task 27).

Only after milestone 1 builds clean does this branch become a merge candidate; the
backend (milestone 2) makes va advertise a profile, and milestone 3 is "it plays."

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
