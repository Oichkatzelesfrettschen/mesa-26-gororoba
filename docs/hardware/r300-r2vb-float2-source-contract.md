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
- keeps final delivery at FP32x4.

## Validator agreement

The kernel synthesized-lane validator decodes the PSC element list in
`r300_tcl_bypass_vtx_check.h` (linux-radeon-gororoba): VAP_PROG_STREAM_CNTL
words are tracked per register, elements walk through `LAST_VEC`, the
identity selector keeps the anchored VTX_SIZE-as-delivered arithmetic, and
a `FLOAT_2 + XY01` element requires VAP_VTX_SIZE to equal the summed fetch
widths -- six for the `FLOAT_4 + FLOAT_2` tuple -- with delivery counted
as full four-lane vectors.

The userspace counterpart is the fetched tuple pass
(`r300_r2vb_float2_tuple_pass.c`): a two-array `3D_LOAD_VBPNTR` fetch (the
FLOAT_4 slot array and the FLOAT_2 model array), the two-element PSC tuple
with the XY01 selector, VAP_VTX_SIZE 6, and a vertex-list POINTS draw into
the producer carrier through the straight RGBA C4_32_FP select, so the
expected carrier slot is the XY01 expansion `(x, y, 0.0, 1.0)` per record.
`r300-r2vb-float2-tuple-replay` proves the agreement offline: the kernel
replay accepts the stream with its three relocations, the width predicate
passes the tuple, and the undersized-VTX_SIZE arm rejects through the same
predicate.

Automatic live promotion remains disabled.  The attended tuple cell establishes
one F32 `FLOAT_2 + XY01 + VAP_VTX_SIZE 6` packet on PCI `1002:5974` RS485M;
`r3v-native-attended-float2-tuple-procedure.md` owns the executed-run record and
its exact scope.  Public GPU-route dispatch and standing promotion remain
separate frontiers.
