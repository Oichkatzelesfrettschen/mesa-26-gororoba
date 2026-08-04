# Program L: R2VB FLOAT_2 source contract

## Scope

This is the first Program L implementation sequence.  It defines the neutral
R300 F32 vertex-format semantics and the pure source-transaction contract
needed for a future legacy R2VB FLOAT_2 producer input.

The intended transaction is:

```text
R32G32_FLOAT application record
-> physical R300 FLOAT_2 fetch
-> PSC XY01 expansion
-> logical producer input (x, y, 0, 1)
-> FP32x4 transformed output carrier
-> existing FLOAT_4 final delivery
```

The slot stream remains FLOAT_4 at four dwords.  The FLOAT_2 model stream is
two dwords, so the complete producer fetch is exactly six dwords per vertex.

## Authorities

`r300_vertex_format.h` owns only mechanical format semantics:

- numeric class;
- API-visible record bytes;
- hardware fetch dwords;
- R300 data type;
- synthesized component selectors.

It does not declare route eligibility.  `r300_vertex_format_pipe.h` is the
Gallium-only adapter between `pipe_format` and the neutral format identity.
`r300_r2vb_source_contract.h` owns the bounded Program L source policy:

- FLOAT_3 and FLOAT_4 remain admitted controls;
- FLOAT_2 requires its own exact-value experimental gate;
- FLOAT_1 remains outside the producer contract;
- semantic and hardware-fetch bounds are calculated and checked separately;
- the exact allocation or suballocation, not a parent BO's spare capacity, is
  the validation authority.

## Build integration

The source-contract and Gallium-adapter translation units are normal Meson
tests when both `with_tests` and Gallium r300 are enabled.  Their build owner is
`src/amd/r300/common/meson.build`: the neutral vocabulary has one common test
entry point, while the pipe-format adapter remains conditional on r300g.

The tests are host-only.  They create no winsys, BO, command stream, or kernel
submission and therefore establish no runtime or silicon result.

## Nonclaims

This sequence does not yet:

- wire FLOAT_2 into the live Gallium R2VB draw path;
- route the existing FLOAT_3/FLOAT_4 producer through the neutral contract;
- alter `R300_R2VB_STANDING` or its closed source-domain matrix;
- admit FLOAT_2 as a final delivery format;
- reopen or replace the closed legacy output-format verdict;
- submit PM4 to the kernel;
- establish kernel validation of synthesized PSC lanes;
- earn a runtime, silicon, Vulkan, or performance verdict.

## Next Program L slice

First, refactor the existing FLOAT_3 and FLOAT_4 producer preflight, stream
construction, and PSC validation through the neutral format authority while
requiring byte-identical PM4 and unchanged route outcomes.  Only after that
control remains exact may the separate FLOAT_2 source gate reach a no-submit
six-dword `FLOAT_4 + FLOAT_2` transaction.

Final delivery remains FP32x4.  Automatic live promotion stays disabled until
the kernel validates XY01/XYZ1 synthesized-lane tuples and Steinmarder retains
the bounded FLOAT_2 silicon ladder.
