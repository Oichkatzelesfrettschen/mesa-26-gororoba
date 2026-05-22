# R300 Raw Tools

These tools are Mesa-side bring-up aids for the RS482/RS485 Vulkan
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

## `r300_float24`

`r300_float24` packs IEEE 754 single-precision floats into the R300 PFS_PARAM
24-bit format used for fragment shader constant loads (PFS_PARAM_0..31).  It
mirrors `r300_emit.c:pack_float24` exactly and requires only libc.

The format is: bit 23 = sign, bits 22:16 = frexpf exponent + 62 (7-bit field),
bits 15:0 = top 16 bits of the IEEE 754 mantissa.

```bash
# Run known-value and round-trip self-tests:
r300_float24 --test

# Pack arbitrary finite floats (inf, nan, and out-of-range exponents are rejected):
r300_float24 1.0 -1.0 3.14159
```
