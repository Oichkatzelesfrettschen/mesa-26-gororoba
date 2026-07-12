# r300 TGSI-boundary inventory

This Mesa-local inventory records runtime shader-IR boundaries reachable through
r300. It distinguishes executable TGSI crossings from source names and semantic
metadata, which do not by themselves create a shader-IR boundary.

## Runtime boundaries

| Boundary | Source symbols | Current role | Retirement condition |
| --- | --- | --- | --- |
| r300 shader-state capability | `r300_init_shader_caps`, `PIPE_SHADER_IR_TGSI` | Allows callers to submit TGSI vertex and fragment state to r300. | No r300-reachable caller creates TGSI state. |
| r300 fragment and vertex ingress | `r300_create_fs_state`, `r300_create_vs_state`, `tgsi_to_nir` | Converts residual TGSI state to NIR before r300 compilation. | All r300-reachable shader constructors submit NIR. |
| SW-TCL vertex execution | `draw_create_vs_exec`, `nir_to_tgsi`, `tgsi_exec_machine_run` | Retains the NIR-to-TGSI bridge for the default and unsupported direct-executor paths. | The direct NIR executor covers r300 SW-TCL input, or SW-TCL no longer depends on the draw executor. |
| u_blitter helper construction | `get_vs_passthrough_*`, `util_make_*_shader` | Uses NIR on a NIR-capable screen by default; `U_BLITTER_FORCE_TGSI=1` and unsupported helper cases retain TGSI constructors. | Every r300-reachable helper has a NIR constructor and the force selector is retired. |
| VL H.264 vertex helper | `vl_h264_emit_create`, `util_make_vertex_passthrough_shader` | The H.264 emitter still creates its shared vertex shader through the TGSI helper. Its fragment kernels use the `vl_nir_*` builders. | The emitter uses `vl_nir_vs_passthrough` or an equivalent NIR vertex constructor. |

## NIR-authored paths

`r300_fs.c`, `r300_r2vb.c`, and the VL fragment-kernel builders create NIR
state directly. Those paths may refer to TGSI semantic names or to the retained
draw bridge, but they do not add an independent TGSI shader-state ingress.

## Completion gate

Remove `PIPE_SHADER_IR_TGSI` from r300 only after the residual helper sources
construct NIR and the SW-TCL vertex route no longer needs the draw
`nir_to_tgsi` plus `tgsi_exec` path. The two `tgsi_to_nir` conversions then
become unreachable and can be removed with the capability advertisement.
