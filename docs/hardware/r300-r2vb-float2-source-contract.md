# R300 R2VB FLOAT_2 source contract

## Scope

This document defines the neutral R300 F32 vertex-format semantics and the pure
source transaction required for a future Gallium r300 R2VB `FLOAT_2` producer
input. The wider Gallium-backed, native, and Vulkan-completion boundaries live
in `r3v-implementation-boundaries.md`.

The intended transaction is:

```text
R32G32_FLOAT application record
-> physical R300 FLOAT_2 fetch
-> PSC XY01 expansion
-> logical producer input (x, y, 0, 1)
-> FP32x4 transformed output carrier
-> existing FLOAT_4 final delivery
```

The slot stream remains `FLOAT_4` at four dwords. The `FLOAT_2` model stream is
two dwords, so the complete producer fetch is exactly six dwords per vertex.

## Authorities

`r300_vertex_format.h` owns mechanical format semantics:

- numeric class;
- API-visible record bytes;
- hardware fetch dwords;
- R300 data type;
- synthesized component selectors.

It does not declare route eligibility.

`r300_vertex_format_pipe.h` adapts between Gallium `pipe_format` and the neutral
R300 format identity.

`r300_r2vb_source_contract.h` owns the bounded source policy:

- `FLOAT_3` and `FLOAT_4` remain admitted controls;
- `FLOAT_2` requires its own exact-value experimental gate;
- `FLOAT_1` remains outside the producer contract;
- semantic and hardware-fetch bounds are calculated and checked separately;
- the exact allocation or suballocation, not a parent BO's spare capacity, is
  the validation authority.

## Build integration

The source-contract and Gallium-adapter translation units are normal Meson tests
when tests and Gallium r300 are enabled. Their build owner is
`src/amd/r300/common/meson.build`.

The tests are host-only. They create no winsys, BO, command stream, or kernel
submission and establish no runtime or silicon result.

## Current boundary

The current live path:

- routes `FLOAT_3` and `FLOAT_4` producer preflight, stream construction, and
  PSC validation through the neutral format authority; the
  `r300_r2vb_psc_byte_identity_test` pin holds the legacy and neutral PSC
  words dword-equal;
- carries the exact-value `R300_R2VB_FLOAT2_SOURCE` experimental gate through
  the `_gated` producer variants; every production admission path passes
  `float2_enabled = false` literally, so `FLOAT_2` structurally cannot enter
  the automatic route;
- captures the six-dword `FLOAT_4 + FLOAT_2` producer tuple without submit;
  the `r300_r2vb_float2_tuple_test` pin holds the tuple, the `XY01`
  expansion, and the known-bad declines;
- derives the slot-plus-model PSC/VAP tuple from the source contract through
  `r300_r2vb_source_tuple_init` and validates emitted tuples, including the
  zero tail, through `r300_r2vb_source_tuple_matches`; the neutral PSC field
  encodings are pinned against `r300_reg.h` by static assertion, and
  `check_source_tuples` holds the `FLOAT_2` tuple to the same golden words
  as the capture pin;
- leaves `R300_R2VB_STANDING` and the closed automatic source-domain matrix
  unchanged; `R300_R2VB_FLOAT2_SOURCE` never joins `standing_gates[]`;
- keeps final delivery at FP32x4;
- has no kernel validation claim for synthesized `XY01` lanes.

## Next integration step

Extend the userspace and kernel synthesized-lane validators from
identity-only PSC to the explicit per-format selector contract
(`FLOAT_2 -> X, Y, ZERO, ONE`), keeping `semantic_end` and `hardware_end`
distinct. The kernel counterpart (VAP_PROG_STREAM_CNTL(_EXT), VAP_VTX_SIZE,
and LOAD_VBPNTR acceptance of the `FLOAT_2 + XY01 + vtx_size 6` tuple) lands
in the Radeon kernel source repository.

Automatic live promotion remains disabled until userspace and kernel validators
agree on the synthesized-lane contract and a bounded RS480-family silicon
ladder supplies source, output, and kernel-window evidence.
