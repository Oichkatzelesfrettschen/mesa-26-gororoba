# r300 TGSI-reference inventory

Verification snapshot of every TGSI crossing that the r300 TGSI-to-NIR
migration must remove, classified per the migration plan (steinmarder
docs/repo-topology-migration/r300-tgsi-to-nir-migration.md). Anchors
re-verified on the post-r3v-rename tree; the enumeration matches the
plan's baked tables exactly, with line drift from the polygon-stipple
and rename commits recorded here as the new canonical anchors.

## Hard TGSI crossings

| Crossing | Anchor | Classification |
|---|---|---|
| Caps advertise NIR+TGSI (VS, FS) | r300_screen.c:493, :516 | r300-owned-remove (caps flip, last) |
| tgsi_to_nir FS bridge | r300_state.c:1395 | r300-owned-temporary-bridge (blitter+VL feed; exempt until those phases land) |
| tgsi_to_nir VS bridge | r300_state.c:2512 | r300-owned-temporary-bridge (same) |
| SW-TCL nir_to_tgsi | draw_vs_exec.c:249 | shared-draw-blocker |
| SW-TCL tgsi_exec | draw_vs_exec.c:191 | shared-draw-blocker |
| u_blitter TGSI constructors | u_simple_shaders.c (8 r300-reachable of 14) | shared-blitter-source; u_blitter consults supported_irs nowhere (0 hits) |
| VL ureg vertex shaders | vl_compositor_gfx.c (183 ureg hits), vl_deint_filter.c (112) | shared-vl-video; unconditional in files_libgalliumvl |
| r3v ureg FP synthesis | r3v_pipeline.c:2630, :2735, :2849, :2932 | r3v-authoring |
| r3v passthrough VS helper | r3v_pipeline.c:2683 (util_make_vertex_passthrough_shader) | r3v-authoring |
| r3v fragment_tex helper | r3v_pipeline.c:4431 (util_make_fragment_tex_shader) | r3v-authoring |

## Soft TGSI-semantic metadata (complete closure list, 21 hits)

r300_state_derived.c: 17 hits (draw_find_shader_output and
r300_draw_emit_attrib keyed by TGSI_SEMANTIC_POSITION/PSIZE/COLOR/
BCOLOR/FACE/GENERIC/PCOORD/FOG). r300_render.c: 4 hits (:1705 region).
Classification: r300-soft-semantic-metadata -- convertible only after
the draw NIR executor exposes a non-TGSI output vocabulary
(draw_vs.h tgsi_shader_info field).

## Already clean

nir_to_rc.c carries exactly one TGSI token, in a comment
(TGSI_FILE_NULL); the classic FS backend and HW-TCL VS consume rc IR
from NIR with zero TGSI. XA has zero ureg/tgsi references.

## Test-or-probe debt

r300_r2vb.c no-submit shape-probe comments pin "the real nir_to_tgsi
output"; retire when the shared-draw executor replaces the bridge.
