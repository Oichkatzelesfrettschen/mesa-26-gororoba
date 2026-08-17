# R300 compiler boundary

This directory owns the R300 through R500 shader compiler.  Gallium and the
direct R300 Vulkan implementation are clients of this code; neither front end
owns it.

The production compiler consumes only compiler-local headers, Mesa-wide
compiler and utility interfaces, and API-neutral contracts from
`src/amd/r300/common`.  In particular:

- `r300_capabilities.h` carries hardware facts into compiler entry points;
- `r300_shader_semantics.h` describes shader inputs and outputs;
- `r300_carrier_policy.h` and `r300_numeric_domain.h` describe the numeric
  carrier contract; and
- `r300_reg.h` remains the shared register authority in
  `src/amd/r300/common`.

`src/gallium/drivers/r300/r300_chipset.c` remains the Gallium-side adapter that
derives `struct r300_capabilities` from a PCI identity.  Driver objects such as
`r300_screen`, `r300_context`, `pipe_screen`, and `pipe_shader_state` must not
enter production files in this directory.

## Test placement

Place a test in `tests/` here when the mechanism under test is a compiler IR,
lowering, scheduler, register allocator, code emitter, or compiler-output
generator.  A compiler test may construct a Gallium fixture while the
front-end adapter is still shared, but the production code it exercises must
remain front-end-neutral.

Place a test in `src/gallium/drivers/r300/tests` when its subject is a Gallium
adapter or driver mechanism: R2VB planning/submission, pipe-format conversion,
compute admission, SW-TCL backpressure, or video-shader integration.  Test
placement follows the mechanism being tested, not the directory from which an
included helper originated.

The next separation step is to replace the remaining Gallium fixtures in
compiler tests with direct capability and compiler-state fixtures where doing
so preserves the same oracle.  That work must not be conflated with this
directory relocation or used to weaken the existing compiler corpus.
