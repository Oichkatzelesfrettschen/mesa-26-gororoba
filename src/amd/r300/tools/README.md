# R300 Raw Tools

These tools are Mesa-side bring-up aids for the RS482/RS485 Rulkan
lane.  They must not call GLX, OpenGL, or the Gallium state tracker.

## `r300_raw_nop`

`r300_raw_nop` emits a one-dword packet stream and can submit it through
the radeon DRM CS path when `R300_TRACE_HAZARD_ACCEPTED=1` is set.

## `r300_raw_shader_triangle`

`r300_raw_shader_triangle` is the program-mode scaffold for the raw
shader triangle lane:

```bash
r300_raw_shader_triangle --program solid-triangle --no-submit
r300_raw_shader_triangle --program fragment-uniform --no-submit
r300_raw_shader_triangle --program fragment-varying --no-submit
r300_raw_shader_triangle --program fragment-texture --no-submit
```

The program modes mirror the Vostro RS482/RS485 GL oracles:

| Program | Runtime input model | Expected pixel |
|---|---|---|
| `solid-triangle` | shader-colored triangle corpus | `32,159,223,255` |
| `fragment-uniform` | uniform seed into fragment ALU | `43,159,223,255` |
| `fragment-varying` | interpolated vertex color into fragment ALU | `43,159,223,255` |
| `fragment-texture` | texture fetch into fragment ALU | `43,159,223,255` |

This tool is no-submit only until it owns fresh BO allocation, BO
initialization, relocation chunks, fence waiting, readback, and dmesg
health evidence.  `--submit` must keep failing until those pieces exist.
