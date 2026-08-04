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
R300 format identity, then forwards the mapped identity into the same source
contract. It adds no route policy of its own.

`r300_r2vb_source_contract.h` owns the bounded source policy and exact
slot-plus-model tuple:

- `FLOAT_3` and `FLOAT_4` remain admitted controls;
- `FLOAT_2` requires its own exact-value experimental gate;
- `FLOAT_1` remains outside the producer contract;
- semantic and hardware-fetch bounds are calculated and checked separately;
- the exact allocation or suballocation, not a parent BO's spare capacity, is
  the validation authority;
- the first PSC register pair contains the slot and model elements;
- the model element alone carries `LAST_VEC`;
- later PSC register pairs remain zero;
- `VAP_VTX_SIZE` equals the physical slot-plus-model dword total.

## Build integration

The source-contract and Gallium-adapter translation units are normal Meson tests
when tests and Gallium r300 are enabled. Their build owner is
`src/amd/r300/common/meson.build`, using the `r300_format_contract_*` variables.

The tests are host-only. They create no winsys, BO, command stream, or kernel
submission and establish no runtime or silicon result.

## Current boundary

The current live path:

- does not admit `FLOAT_2`;
- has not routed existing `FLOAT_3`/`FLOAT_4` construction through the neutral
  contract;
- leaves `R300_R2VB_STANDING` and the closed automatic source-domain matrix
  unchanged;
- keeps final delivery at FP32x4;
- has no kernel validation claim for synthesized `XY01` lanes.

The host contract now derives and mutation-checks exact `FLOAT_2`, `FLOAT_3`,
and `FLOAT_4` PSC/VAP tuples. This is a no-submit structural authority, not a
live producer route.

## Next integration step

Refactor the existing `FLOAT_3` and `FLOAT_4` producer preflight, stream
construction, and PSC validation through the neutral format and tuple
authorities while requiring byte-identical PM4 and unchanged route outcomes.

Only after that control remains exact may a separate `FLOAT_2` source gate reach
a no-submit six-dword `FLOAT_4 + FLOAT_2` transaction.

Automatic live promotion remains disabled until userspace and kernel validators
agree on the synthesized-lane contract and a bounded RS480-family silicon
ladder supplies source, output, and kernel-window evidence.
