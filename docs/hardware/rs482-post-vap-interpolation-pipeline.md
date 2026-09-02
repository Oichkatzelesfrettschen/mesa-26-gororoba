# RS482 post-VAP interpolation pipeline for Flat, Smooth, and NoPerspective varyings

The R3V native route delivers every varying through the TCL-bypass
triangle cell: the host executes the vertex job, the post-VS stage
rewrites records, the clipper cuts, and the device fetches records
through VAP, interpolates them in the RS, and executes the US program.
This document is the register-level model that decides where a Flat, a
Smooth, and a NoPerspective varying can ride on RS482 (Radeon Xpress
200M, CHIP_RS480, R300-class US/PFS fixed VLIW) and what bounds the
count.  Evidence classes are named per row: `manual` is AMD R3xx 3D
Registers; `kernel` is `drivers/gpu/drm/radeon/r300.c`; `driver` is
`src/amd/r300/`; `silicon` is a retained steinmarder-r300 receipt.

## Record fetch (VAP)

- `VAP_VTX_SIZE` (0x20b4, `DWORDS_PER_VTX` bits 6:0): the record stride
  the immediate-mode fetch walks (manual).  The kernel checker reads this
  word alone into `track->vtx_size` (`r300_packet0_check`, kernel) and
  judges every `3D_DRAW_IMMD` against it through `r300_tcl_bypass_vtx_check`
  and `r100_cs_track_check`; the width is the sum of the declared texture
  components, so a widened record passes without a checker change
  (silicon: rungs B and D, PASS at 12 and 16, REJECT at the narrower
  size).
- `VAP_OUTPUT_VTX_FMT_0` (0x2090): `POS_PRESENT` bit 0, `COLOR_n_PRESENT`
  bits 1..4, `PT_SIZE_PRESENT` bit 16.  `VAP_OUTPUT_VTX_FMT_1` (0x2094):
  one 3-bit component count per texture unit 0..7 (manual).  The
  deployed 0.8.12 checker (`r300_tcl_bypass_vtx_check`, kernel source
  checkpoint `0104ede3f196`) reads both words and admits a draw whose
  stream satisfies its modeled predicate: `POS_PRESENT` set and no FMT0
  bit beyond `COLOR_0_PRESENT`, the FMT1 texture component counts
  decoded into the required width (4 for position, 4 more for COLOR0,
  the declared components per texture unit), complete PSC stream words,
  identity or XY01 selectors, one destination per element, and a
  terminating `LAST_VEC`; `VAP_VTX_SIZE` at or above that width passes
  and a narrower record rejects.  COLOR1..3, point size, an unknown
  selector, a duplicate destination, a skipped dword, or incomplete
  state declines the draw to `r100_cs_track_check` alone.  The direct
  GA Flat cell (`POS|COLOR0`, width 8) passes under that predicate
  (silicon: `r3v-native-public-flat-color0-two-draw-first-delivery-rs482`,
  replayed byte-equal under 0.8.12 as
  `r300-tcl-bypass-vtx-check-color0-width-transition-rs482`).
- `VAP_PROG_STREAM_CNTL_n` / `_EXT_n` (0x2150.., 0x21e0..): per-element
  data type, skip, destination vector, last-vector flag, swizzle, and
  write enable (manual).  The carrier cells differ from the varying cell
  first at `VAP_PROG_STREAM_CNTL_0` (silicon: rungs B, D, and the
  flat-mixed receipt, index 399, `0x26030003 -> 0x06030003`).

## Interpolation (RS)

- `RS_IP_n` (0x4310 + 4n): the source of interpolator n -- a texture
  address with per-lane `SEL_S/T/R/Q` selects or a color with `COL_FMT`
  (manual).  The manual documents `RS_IP_0..7` for R300; `r300_reg.h`
  names `R300_RS_IP_0..3`; the R3V plans hold the RS vector budget at
  `R300_NOPERSPECTIVE_CARRIER_RS_VECTOR_BUDGET` (driver).  Hypothesis:
  interpolators 4..7 exist on RS482 as the manual states; no retained
  receipt exercises a fifth vector, so the four-vector boundary is the
  next probe, not a silicon fact.
- `RS_INST_n` (0x4330 + 4n, 0..7): `TEX_ID` bits 2:0 and `COL_ID` bits
  13:11 name the interpolator, `TEX_CN_WRITE` bit 3 and `COL_CN_WRITE`
  bit 14 enable the write, `TEX_ADDR` bits 10:6 and `COL_ADDR` bits 21:17
  name the US input register, `TEX_ADJ` bit 22 moves the sample point
  (manual).  `TEX_ADJ` leaves the RS482 target unchanged (silicon:
  `2026-08-28-rs482-gb-select-w-select-affine-rs-inst-tex-adj-perspective-perturbed`).
- `RS_COUNT` (0x4300): `IT_COUNT` bits 6:0 texture components, `IC_COUNT`
  bits 10:7 colors, `HIRES_EN` bit 18; `RS_INST_COUNT` (0x4304) bits 3:0
  (manual).
- `GB_SELECT.W_SELECT` (0x401c bit 4): 0 hands the RS the position's
  1/W, 1 hands it the constant 1.0 (manual).  The word is one per draw
  and reaches every interpolator, so it serves an interface whose every
  varying is NoPerspective (silicon: rung A affine 882/882) and refuses a
  Smooth location beside it (driver: `r3v_interpolation_route_select_noperspective`).
- `GA_COLOR_CONTROL` (0x4278): `RGBn_SHADING` and `ALPHAn_SHADING` 2-bit
  fields per color 0..3 (solid 0, flat 1, gouraud 2), `PROVOKING_VERTEX`
  bits 17:16 (manual).  The fields act on the color path alone; a
  texture interpolator is always gouraud under the RS's reciprocal W
  (manual).  The direct GA Flat cell writes `0x0000aaa5` -- color 0 flat,
  provoking first -- and the device selects the provoking vertex's color
  per primitive (silicon: `direct-flat-color0-delivered`, three distinct
  records, coverage exact).
- The kernel checker tracks none of `RS_IP`, `RS_INST`, `RS_COUNT`,
  `GB_SELECT`, or `GA_COLOR_CONTROL` (kernel: `r300_packet0_check`), so
  their admission is the driver's plan validators and stream checks.

## Where each varying kind rides

| Kind | Path | Mechanism | Receipt |
|---|---|---|---|
| Smooth | texture interpolator, `W_SELECT` 0 | the RS's perspective interpolation | every varying cell |
| Flat, alone at location 0 | color 0 under `GA_COLOR_CONTROL` flat, provoking first | device provoking selection | `r3v-native-public-flat-color0-two-draw-first-delivery-rs482` |
| Flat, any location | its texture interpolator | host replication of the provoking record ahead of the clipper; equal endpoints interpolate to the constant | `r3v-native-public-flat-two-draw-first-delivery-rs482` |
| NoPerspective, alone | texture interpolator, `W_SELECT` 1 | window-linear interpolation of every lane | rung A |
| NoPerspective beside Smooth | `TC1 = a * c`, `TC2.x = c`, `W_SELECT` 0 | US recovers `interp(a c) * rcp(interp(c))` | rung D |
| Flat beside NoPerspective | rung D's cell with TC0 replicated on the host | replication precedes the packing | `r3v-native-noperspective-flat-mixed-carrier-receipt-vostro1000_rs485m_5974` |
| Flat beside Smooth and NoPerspective | four interpolators: Flat, Smooth, `a * c`, `c` | the four-vector RS budget boundary | none |

Replication composes with every texture-path route because it rewrites
records ahead of the clipper and the packing: a clipped edge between two
equal records yields the same record, the reciprocal packing multiplies a
constant by `c` and the US divides it back, and `W_SELECT` 1 interpolates
a constant to itself.  The routes that consume a whole-draw word
(`W_SELECT`) or a per-primitive color selection (`GA_COLOR_CONTROL`) are
the ones a second varying kind cannot share, which is why the mixed
carrier keeps `W_SELECT` 0 and carries the reciprocal per record.

## Open rows

- Four-vector boundary: a cell fetching four RS vectors (Flat, Smooth,
  premultiplied NoPerspective, carrier) at `VAP_VTX_SIZE` 20, judged by
  the plan validator, the kernel width replay, and one attended census.
- Interpolators 4..7: a fifth vector on the manual's `RS_IP_4`, judged
  by the same ladder, decides whether the budget is the register file's
  or the driver's.
- Per-channel `GA_COLOR_CONTROL`: color 0 flat beside color 1 gouraud in
  one draw is documented and unexercised; it would carry a Flat and a
  Smooth vector on the color path and leave every texture interpolator to
  the carrier.
