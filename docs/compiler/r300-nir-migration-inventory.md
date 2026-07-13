# r300 TGSI-boundary inventory

This Mesa-local inventory records runtime shader-IR boundaries reachable through
r300. It distinguishes executable TGSI crossings from source names and semantic
metadata, which do not by themselves create a shader-IR boundary.

## Runtime boundaries

| Boundary | Source symbols | Current role |
| --- | --- | --- |
| r300 shader-state capability | `r300_init_shader_caps` | Advertises NIR for vertex and fragment state. |
| r300 fragment and vertex ingress | `r300_create_fs_state`, `r300_create_vs_state` | Rejects state whose type is not `PIPE_SHADER_IR_NIR`. |
| SW-TCL vertex execution | `r300_draw_init_vertex_shader`, `draw_vs_nir_supported`, `draw_create_vs_nir` | Admits r300's normalized NIR to the direct executor and rejects shapes outside its coverage. |
| generic Draw compatibility | `draw_create_vs_exec`, `nir_to_tgsi`, `tgsi_exec_machine_run` | Retains TGSI input and unsupported-NIR compatibility for other Draw drivers. r300 does not reach this bridge. |
| u_blitter helper construction | `util_blitter_create`, `get_vs_passthrough_*`, `util_make_*_shader_nir` | Uses NIR for NIR-only drivers. `U_BLITTER_FORCE_TGSI=1` applies only when both stages advertise TGSI. |
| VL H.264 vertex helper | `vl_h264_emit_create`, `vl_nir_vs_passthrough` | Creates position and two varying outputs directly in NIR. |

## NIR-authored paths

`r300_fs.c`, `r300_r2vb.c`, and the VL fragment-kernel builders create NIR
state directly. Those paths may refer to TGSI semantic names or to the retained
draw bridge, but they do not add an independent TGSI shader-state ingress.

## Remaining TGSI vocabulary

TGSI semantic enums remain in Draw and r300 metadata because they label vertex
and fragment linkage slots. The generic Draw TGSI executor remains for drivers
that advertise TGSI. Neither surface accepts TGSI shader state into r300.
