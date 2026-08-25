# RS482 GPU compute substrate atlas

RS482 (Radeon Xpress 1100/1150, PCI `1002:5974`, `CHIP_RS480`, R300-class
integrated graphics, vertex engine absent) runs its admitted compute kernels on
the CPU direct-SPIR-V route. That route is the oracle every GPU result is
measured against and the measured default for small draws
(`src/amd/r300/common/r300_compute_spirv.c`, executed by
`r300_cpu_compute_job_execute`). This atlas names, for every unit from the PM4
command stream down to the color backend, the arithmetic the unit performs, the
numeric domains it performs it in, and the evidence that backs each statement,
so a compute verb's GPU route is designed against a named unit with a named
exactness bound instead of against the CPU route's behavior.

The pipeline is a compute substrate only in the sense each unit's own datapath
allows: the fragment ALU carries FP24 arithmetic, the sampler carries a
6-bit-weight bilinear MAC, the blend unit carries a clamped monoid, the ROP
carries bitplane logic, the stencil unit carries an 8-bit automaton, and the
occlusion counter carries an exact population count. A verb lands on whichever
of those algebras contains it.

## Evidence classes and citation form

Every claim below carries one class.

- **silicon**: a retained RS482 hardware run witnesses the exact value.
- **known(source)**: read directly from source, a register definition, or a
  generated table in this repository, the kernel fork, or the register
  databases.
- **hypothesized**: a mechanism model consistent with source that no located
  run witnesses.
- **refuted**: a route a named finding closed.

Mesa citations name `file:symbol`, kernel citations name `file:function`
relative to the radeon fork's `drivers/gpu/drm/radeon/`, evidence citations name
a steinmarder finding by filename, and hardware and API citations name public
documents. Line numbers stay out; they drift and the symbol is the durable
locator.

## Command processor and PM4 packet grammar

The CP is a sequencer. It fetches an indirect buffer, decodes packet headers,
writes registers, and starts draws; it holds no ALU, so every arithmetic verb
below belongs to a unit downstream of it (steinmarder
`2026-06-04-rs482-cp-me-ram-8bit-raddr-256word-machine.md`; known(source)). The
CP-as-JIT route--treating `CP_ME_RAM` as writable shader memory--is
**refuted**: a microcode readback matched `R300_cp.bin` in all 256 words, so the
ME RAM holds the shipped microengine program and nothing else (same finding;
silicon).

Three layers admit Type-3 opcodes, and they disagree. The CP microengine's
even-key dispatch table is what the silicon decodes; `r300_packet3_check`
(`r300.c`) is what the kernel validator admits; the r3v and r300g command
builders are what userspace emits. The mismatches are the finding, not an error
to reconcile.

| Type-3 opcode | ME dispatch handler | Kernel validator | Emitted by r3v/r300g |
|---|---|---|---|
| `NOP` 0x10 | not in even table | accepted as the reloc carrier | yes, every relocation |
| `LOAD_MICROCODE` 0x24 | 0xf0c0 | absent, rejected | no |
| `WAIT_FOR_IDLE` 0x26 | 0xf0c1 | absent, rejected | no |
| `3D_DRAW_VBUF` 0x28 | 0xf041 | accepted, tracks `VAP_VF_CNTL` | yes |
| `3D_DRAW_INDX` 0x2a | 0xf184 | accepted | yes |
| `3D_LOAD_VBPNTR` 0x2f | absent from the even table | accepted, per-array reloc | yes, the R2VB re-ingest |
| `INDX_BUFFER` 0x33 | absent from the even table | accepted, size-bounded | yes, indexed draws |
| `3D_DRAW_VBUF_2` 0x34 | 0xf393 | accepted | yes |
| `3D_DRAW_IMMD_2` 0x35 | absent from the even table | accepted, `PRIM_WALK == 3` | yes |
| `3D_DRAW_INDX_2` 0x36 | 0xf38a | accepted | yes |
| `3D_CLEAR_HIZ` 0x37 | not in even table | hyperz owner only | no |
| `3D_CLEAR_CMASK` 0x38 | 0xf38e | cmask owner only | no |

The ME table is microcode-derived (steinmarder
`cp_microengine_pm4_type3_opcode_backend_map.tsv`; known(source)); the validator
column is read from `r300.c:r300_packet3_check` (known(source)). `WAIT_FOR_IDLE`
is dispatched by the microengine and refused by the kernel, so a userspace idle
wait travels as register state rather than as a packet. `3D_LOAD_VBPNTR`,
`INDX_BUFFER`, and `3D_DRAW_IMMD_2` invert that: the kernel admits them and the
even-key decode does not name them. Absence from a partial microcode decode is a
decode gap, not a silicon capability claim, and both directions stay on the probe
frontier. `CP_DMA` appears in neither table, so no CP-side copy engine is
available to a compute route (hypothesized: the opcode is absent from this
generation; the decode gap admits no stronger statement).

Packet grammar itself is three forms: PACKET0 writes N sequential registers,
PACKET2 is filler the parser skips, and PACKET3 carries a semantic command
(steinmarder `rs482-cp.4.md`; known(source)). Relocation rides a PACKET3 `NOP`
whose payload the kernel rewrites in place, so every buffer address a compute
route emits is a kernel-patched value rather than a userspace-computed one.

Seven registers are never read or written on this target: `RBBM_SOFT_RESET`
0x00F0, `CP_RB_BASE` 0x0700, `CP_RB_CNTL` 0x0704, `CP_RB_RPTR` 0x0710,
`CP_RB_WPTR` 0x0714, `CP_RB_WPTR_DELAY` 0x0718, and `RBBM_GUICNTL` 0x172C
(steinmarder `rs482_do_not_touch_registers.tsv`; known(source)).

## Kernel CS validator as the grammar boundary

`r300_cs_parse` is the parser bound to this chip: `radeon_asic.c` assigns
`rs400_asic` for `CHIP_RS480`, `rs400_asic` takes `r300_gfx_ring`, and that ring
sets `.cs_parse = &r300_cs_parse` (known(source), read directly). A GPU compute
route reaches silicon only through the grammar that parser accepts, so the
validator is the outer boundary of every design in this atlas.

PACKET0 admission runs through a per-family safe bitmap in
`r100.c:r100_cs_parse_packet0`. A set bit routes the register into
`r300.c:r300_packet0_check`, whose switch accepts or rejects it; a clear bit
passes the write through uninspected. The bitmap deregulates rather than
authorizes (known(source)).

### Registers the validator tracks and the arithmetic bound to them

`r300_packet0_check` binds arithmetic to tracked state, and
`r100.c:r100_cs_track_check` then proves every surface large enough for the draw
that consumes it. Color buffers must satisfy `pitch * cpp * maxy + offset <=
radeon_bo_size`; depth buffers the same; vertex arrays are sized by primitive
walk--indexed uses `esize * max_indx * 4`, non-indexed uses `esize * (nverts -
1) * 4`, and immediate mode requires `vtx_size * nverts == immd_dwords` exactly
(known(source)). `esize` and `VAP_VTX_SIZE` are both masked to 7 bits, so a
vertex element carries at most 127 dwords, and `VAP_VF_MAX_VTX_INDX` is masked
to 24 bits, which is the hard index ceiling for any indexed compute dispatch.

The defaults `r100.c:r100_cs_track_clear` installs are worst-case
sentinels--per-CB `pitch = 8192`, `cpp = 16`, `vtx_size = 0x7F`,
`immd_dwords = 0xFFFFFFFF`--so a compute command stream that omits a register write inherits a sentinel
that fails the size check rather than a permissive default (known(source)).

### Family gating, corrected

`radeon_family.h` orders the enum `CHIP_R300 < CHIP_RV350 < CHIP_R420 <
CHIP_RS400 < CHIP_RS480 < CHIP_RV515` (known(source), read directly). RS480
therefore sits **above** the RV350 and R420 thresholds and below the RV515 one,
which splits the family gates in `r300_packet0_check` two ways.

- Excluded by `>= CHIP_RV515`: `VAP_ALT_NUM_VERTICES` 0x2088 is rejected, color
  buffer format code 5 is rejected, and `TX_FORMAT2` bit 14 (`TXFORMAT_MSB`,
  carrying width_11/height_11 and ATI1N) is rejected outright. `SC_SCISSOR1`
  additionally subtracts 1440 when deriving `maxy` below that threshold, and
  `r100_cs_track_texture_check` withholds the extra high width and height bits.
- Admitted because RS480 clears the lower thresholds: `R300_TX_FORMAT_ATI2N`
  passes the `>= CHIP_R420` gate, and `GB_Z_PEQ_CONFIG` 0x4028 passes the `>=
  CHIP_RV350` gate once the submitter owns hyperz.

Hyperz and cmask are single-owner resources: `SC_HYPERZ_EN` has bit 0 silently
cleared for a non-owner, `ZB_BW_CNTL` rejects the HiZ, compression, and
fast-fill bits for a non-owner, `ZB_MASK_OFFSET`, `ZMASK_PITCH`, `HIZ_OFFSET`,
and `HIZ_PITCH` reject any nonzero value, and `RB3D_CCTL` rejects
`CMASK_ENABLE` (known(source)). A compute route that wants ZMASK or a fast
depth clear must hold that ownership.

RS480 alone instantiates the R400 extended fragment register file (`US_CODE_BANK`
0x46B8, `US_CODE_EXT` 0x46BC, `US_ALU_EXT_ADDR_0..63` 0x4AC0-0x4BBC) on
R300-class silicon. The stock bitmap marks those addresses checked with no case
to accept them, so they reject by default; `reg_srcs/rs480` clears exactly those
bits, and `radeon_rs4xx_dev.c:radeon_rs4xx_dev_apply_r400_us_reg_safe` installs
that bitmap only when the module runs the mutate profile **and**
`radeon_rs480_r400_us_cs == 1` **and** the family is `CHIP_RS480`
(known(source)). Once armed the writes are unvalidated plain values. Production
keeps the gate closed, so a GPU verb route designed for more than 64 ALU
instructions is unreachable on a production module.

### The TCL-bypass width invariant

`r300.c:r300_cs_tcl_bypass_vtx_output_check` runs before the stock bounds check
and encodes an RS482-observed hang as a static parse rejection: an underfed
TCL-bypass draw leaves the geometry assembler waiting for dwords that never
arrive and wedges the vertex front end and the ring (known(source), and the
wedge class is silicon--a VAP/PVS wedge recovers only through a cold power
cycle, steinmarder `rs482-safety.7.md` and `rs482-fsm-map.7.md`).

The checker (`r300_tcl_bypass_vtx_check.h:r300_tcl_bypass_vtx_check_reason`)
enters scope only when the command stream writes `VAP_CNTL_STATUS` with
`R300_VAP_TCL_BYPASS` set, both `VAP_OUT_VTX_FMT` words, `VAP_VTX_SIZE`, and a
complete program-stream element list through `LAST_VEC`. In scope it computes
required dwords as 4 for position plus the capped per-texcoord component counts,
and delivered dwords by walking the stream selectors. It models exactly two
selector patterns: identity `0xF688`, where one fetched dword delivers one lane
for any `DATA_TYPE` in `FLOAT_1..FLOAT_4`, and XY01 `0xFB08`, accepted only with
`DATA_TYPE = FLOAT_2`, where 2 fetched dwords deliver 4 lanes through a
synthesized Z=0 and W=1. Delivered below required is a **reject**; an underfeed
on a non-identity list is a reject; an overfeed is a decline; every unmodeled
selector, a nonzero `SKIP_DWORDS`, a duplicate destination location, and an
identity element with `DATA_TYPE` above 3 decline rather than reject. A compute
route that carries results back through the vertex path designs inside those two
selector patterns or it is unmodeled by the checker that protects it.

### Memory model, reset, and hazard classes

VRAM is a north-bridge UMA carveout: `rs400.c:rs400_mc_init` derives the base
from `RADEON_NB_TOM`, and the box carries a fixed 128 MiB interval
(vostro1000-re `rs482-vram-gtt-capacity-and-placement.md`; known(source) plus
observation). GART is a single-level page table whose entries carry
`RS400_PTE_READABLE`, `WRITEABLE`, and `UNSNOOPED`, and `rs400.c:rs400_gart_enable`
programs `RS480_AGP_MODE_CNTL` with `REQ_TYPE_SNOOP_DIS`, disabling request
snooping globally (known(source)). Whether a per-PTE snoop bit overrides that
global disable is unresolved in both evidence repositories and stays
**hypothesized**; the memory-model document owns that question.

The fork adds an admission epoch around every RS400/RS480 MMIO, aperture, and
page-table access, keyed on `radeon.h:radeon_rs4xx_hardware_target`. A failed
reset latches `gpu_parked`, after which every MMIO leaf returns without issuing
a non-posted HyperTransport transaction and command submission refuses
(known(source)); an attended park on RS482 returned `-EIO` from object creation
and idle waits and `-EBUSY` from submission, and only a physical cold power cycle
restored production (`radeon-custom` deployment record; silicon). Nonbaseline
reset masks are compile-time restricted to the 3D-domain bits with static
assertions, one-shot, and gated on the mutate profile, so host, display, clock,
2D, and video bits are unreachable through that path (known(source)).

Two hazard classes separate. A CP-ring wedge recovers through the validator
preflight, the fence watchdog, and `radeon_gpu_reset`; a VAP/PVS wedge does not
recover in-session at all. Reading the VAP/PVS window 0x2200-0x2504,
`BIF_SLAVE_CNTL` 0x180C, or `CRTC8_IDX` 0x03B4 has frozen the host through a
north-bridge stall, and `D18F3x44 = 0x0A100040` shows the K8 watchdog enabled
with sync-flood on timeout (vostro1000-re
`k8-integrated-northbridge-and-rs482-host-bridge-decomposition.md`; silicon).

## VAP and the vertex fetcher

RS482 has no vertex ALU. `r300_chipset.c:r300_parse_chipset` never sets
`num_vert_fpus` for `CHIP_RS480`, so it stays zero and `has_hardware_tcl` is
false; `r300_chip_identity.c:r300_rs480_die_facts` records
`.vertex_engine_absent = true` (known(source)). A `PVS_NUM_FPUS = 2` readback is
a software TCL-bypass artifact rather than a unit count (steinmarder
`2026-06-09-rs482-vap-cntl-num-fpus-provenance.md`; silicon), and forcing a
hardware-TCL draw hangs the ring (steinmarder `rs482-fsm-map.7.md`; silicon
negative). What VAP gives a compute route is a typed gather with a fixed
swizzle, not arithmetic.

The vertex fetcher decodes a packed record into a float stream. `DATA_TYPE`
values are `FLOAT_1..FLOAT_4` (0-3), `BYTE` (4), `D3DCOLOR` (5, unorm8x4),
`SHORT_2` (6), `SHORT_4` (7), `VECTOR_3_TTT` (8), `VECTOR_3_EET` (9), `FLOAT_8`
(10), `FLT16_2` (11), and `FLT16_4` (12), each modified by the per-element
`SIGNED` and `NORMALIZE` flags (steinmarder `r300_vertex_stream_format.tsv`;
known(source)). `FLOAT_4` ingestion is observed; `FLOAT_8` and the FLT16 pair
remain probe targets, and the FLT16 route is blocked on this chip by the Draw
module's normalization (`r300_numeric_domain.c` VAP_FORMAT_INGEST domain and
steinmarder `rs482-vap-rs.4.md`; hypothesized for the unprobed types). Ingestion
is a lossless format decode rather than a compute or reduction step, so the
domain is marked outside native compute.

`VAP_VTX_SIZE` carries `DWORDS_PER_VTX[6:0]`, `VAP_VF_MAX_VTX_INDX` carries a
24-bit `MAX_INDX`, `VAP_CNTL_STATUS` bit 8 is `TCL_BYPASS`, and the eight
`VAP_PROG_STREAM_CNTL` registers carry per-element `DATA_TYPE`, `SKIP_DWORDS`,
`DST_VEC_LOC`, `LAST_VEC`, `SIGNED`, and `NORMALIZE` (steinmarder
`rs482_canonical_register_reference.md`; known(source)). Five registers are
validated as CS-writable on the R2VB substrate: `VAP_VTE_CNTL` 0x20b0,
`VAP_VTX_SIZE` 0x20b4, `VAP_VF_MAX_VTX_INDX` 0x2134, and `VAP_CLIP_CNTL` 0x221c
with `CLIP_DISABLE` (steinmarder
`rs482_r2vb_substrate_validated_cs_write_registers.tsv`; silicon, validator
accepted). The `VAP_PORT_DATA0` immediate ports are write-only and unwritable
through the command stream on this route.

## GA, SU, SC, and RS: index generation and interpolation

Rasterization is the dispatch mechanism. A triangle covering an N-by-M region
generates one fragment per texel with its coordinate already formed, which is
exactly the index class `R300_GRID_INDEX_COORD` in
`r300_grid_fold.h:r300_grid_index_exact`: a position-addressed kernel never
materializes the linear index as one FP24 number, so each axis stays under the
2048 raster cap and the full 2048x2048 raster is honest (known(source)). A
linear-index kernel materializes `gid = y * width + x` and is bounded by the
FP24 exact-integer ceiling instead; a strided kernel materializes `a * gid + b`
and is bounded by the same ceiling applied to the result. A 3D grid folds z into
raster rows as `y_raster = z * dim_y + y`, recovered as `floor(y_raster /
dim_y)`, with `dim_y * dim_z <= 2048` as the binding constraint.

RS interpolation is an affine MAC: an attribute at a fragment is
`lambda * t(p0) + (1 - lambda) * t(p1)`, which makes the interpolator a
per-fragment linear-blend unit reachable without an ALU instruction (steinmarder
`rs482-vap-rs.4.md`; derivation, hypothesized until the width probe runs). The
interpolator's fractional bit width is uncharacterized, so no exactness bound
attaches to an interpolated value and an integer-cast falsifier remains open.
Routing is set by `RS_INST_COUNT` 0x4304, `RS_IP_0..7` 0x4310-0x432C, and
`RS_INST_0..` 0x4330, with swizzle selects X=0, Y=1, Z=2, W=3, `FP_ZERO`=4, and
`FP_ONE`=5--the two constant selects give a compute kernel an operand-free
zero and one (steinmarder `r300_raster_backend_state.tsv`; known(source)). The
fragment stage accepts at most 10 inputs.

Scissor, point size, and line width act as coverage masks rather than
arithmetic. `SC_SCISSOR1` bounds the generated region, `GA_POLY_MODE` selects
fill mode per winding, and `GB_ENABLE` carries the per-primitive stuff enables
and the per-unit texture-coordinate source select. Die facts cap point size at
64 and hardware line width at 8
(`r300_chip_identity.c:r300_rs480_die_facts`; known(source)); the
`POINTSIZE_MAX` register field maximum of 10922 is a different quantity, the
field's own range in subpixel units.

## TX: the sampler

The sampler is a typed random-access gather with one arithmetic function: a
rank-1 bilinear outer product. Weights resolve to at least 6 bits--a sweep of
the inter-texel span of a 0..255 ramp produced 65 distinct levels, so about 64
weight steps--and corner taps round half-up as `(a + b + c + d + 2) >> 2`,
exact in 27 of 27 cases (steinmarder
`2026-05-28-rs482-tx-bilinear-filter-weight-resolution-and-float-payload.md`;
silicon). Float payloads are point-sampled rather than filtered on this chip
(same finding; silicon), so an FP16 or FP32 texture gives a compute kernel exact
texel delivery and no interpolation. `LOG4_BILINEAR_REDUCE` in the catalog uses
one linear corner tap as a 2x2 sum-over-4, exact when the sum is divisible by 4
under the UNORM8 carrier and quantized within one byte otherwise
(`r300_numeric_domain.c`; silicon).

Format decode lives in `TX_FORMAT1_n`: `TXFORMAT` bits 0-4 select the texel
format, `SIGNED_COMP0..3` bits 5-8 set per-component signedness independently,
and `SEL_ALPHA`, `SEL_RED`, `SEL_GREEN`, and `SEL_BLUE` (bits 9-20) give a free
per-channel swizzle on the fetch (`rs482_gfx_3_0_0.reg`; known(source)). Per-axis
address mode comes from `CLAMP_S`, `CLAMP_T`, and `CLAMP_R` in `TX_FILTER0_n`.
The kernel's own format table bounds what reaches silicon: 1, 2, 4, 8, and 16
bytes per texel across the named codes, with `FL_R32G32B32A32` at 16 and
`FL_I32` at 4 (`r300.c:r300_packet0_check`; known(source)).

Sixteen texture units exist across the family
(`r300_chipset.c`; known(source)). Dimension ceilings separate by role: the
sampler axis caps at 2048, the render span at 2560, and a tiled row at 2048
(`r300_chip_identity.c:r300_rs480_die_facts`; known(source)), which is why the
grid fold picks 2048--the smaller cap keeps a surface both renderable and
sampleable. The virtualization document owns how a logical extent above 2048
decomposes.

Texture instructions are a separate 3-bit field with five values: `NOP` 0, `LD`
1, `KIL` 2, `TXP` 3, and `TXB` 4 (`r300_reg.h:R300_TEX_INST_MASK`;
known(source)). No `TXD` or `TXL` encoding exists at this level. A texture
instruction cannot write an output register, so an ALU instruction always
follows a fetch, even a neutral one. Each dependent fetch opens a new node, and
the node index caps at 3, giving 4 indirection levels total
(`r300_fragprog_emit.c:begin_tex`; known(source)); a gather kernel's dependent
chain depth is bounded there, not by the TEX budget.

## US: the fragment ALU

The fragment ALU is the only general arithmetic unit on the chip. It issues one
RGB operation and one alpha operation per instruction slot, over three arguments
each.

### Opcode tables

RGB lane output opcodes (`r300_reg.h:R300_ALU_OUTC_MAD` and its neighbors;
known(source)): `MAD` 0, `DP3` 1, `DP4` 2, `D2A` 3, `MIN` 4, `MAX` 5, `CND` 7,
`CMP` 8, `FRC` 9, `REPL_ALPHA` 10.

Alpha lane output opcodes (`r300_reg.h:R300_ALU_OUTA_MAD` and its neighbors;
known(source)): `MAD` 0, `DP4` 1, `MIN` 2, `MAX` 3, `CND` 5, `CMP` 6, `FRC` 7,
`EX2` 8, `LG2` 9, `RCP` 10, `RSQ` 11.

`ADD` and `MUL` are `MAD` with a neutral operand, `DP3` pairs `OUTC_DP3` with
`OUTA_DP4`, floor is `FRC` plus `MAD`, and `RSQ` uses the absolute-value
argument modifier. Per-argument modifiers are `NOP`, `NEG`, `ABS`, and `NAB`; a
presubtract unit supplies `1 - 2*SRC0`, `SRC1 - SRC0`, `SRC1 + SRC0`, and `1 -
SRC0` to either lane; output modifiers apply a power-of-two scale (`MUL2`,
`MUL4`, `MUL8`, `DIV2`, `DIV4`, `DIV8`) before the write; and an independent
clamp bit per lane saturates the result to [0,1] (`r300_reg.h`; known(source)).
Argument selectors include constant `ZERO`, `ONE`, and `HALF` in both lanes, so
those three constants cost no register.

### The CMP/CND select substrate

No compare-and-set opcode exists in either lane: `SGE`, `SLT`, `SEQ`, and `SNE`
have no hardware form and the compiler synthesizes them from `MAD` plus `CMP`
(`r300_reg.h`, the comment block preceding the ALU definitions;
known(source)). Every boolean and select in a fragment-ALU compute kernel
therefore reduces to `CMP` (return arg1 when arg2 < 0, else arg0) or `CND`
(return arg0 when arg2 > 0.5, else arg1). A verb that needs a predicate budgets
the extra instruction that forms its sign or its 0.5 crossing.

`KIL` is a texture-unit instruction issued with no destination and no sampler
unit, not an ALU operation (`r300_fragprog_emit.c:emit_tex`; known(source)).
`KIL` plus the surviving writes is a predicated masked store, confirmed on
silicon (steinmarder `rs482-rb3d-zb.4.md`; silicon).

### FP24 numerics and the coefficient ledger

The datapath type is FP24 as s1e7m16: one sign bit, seven exponent bits, sixteen
stored mantissa bits, a 17-bit significand with the implicit leading one,
exponent bias 62, normal range [2^-61, 2^65], round toward zero. There are no
NaNs, no infinities, and no subnormals; underflow flushes to zero and overflow
saturates (`r300_grid_fold.h` and `r300_numeric_domain.h`; known(source)).

Every integer in [0, 2^17] is exactly representable and 2^17 + 1 is the first
that is not, so `R300_FP24_EXACT_INT_CEILING` is 131072 (same headers;
known(source), and the value is machine-checked in the `open_gororoba`
`fp24_int_exact_inclusive` proof). The coefficient ledger that follows from the
17-bit significand (steinmarder
`rs482_fp24_substrate_coefficient_ledger_v1/coefficients.tsv`;
silicon-corroborated):

- exact integer maximum 131072, with ulp 2 across [2^17, 2^18);
- universal square operand bound 362, since `362^2 = 131044 <= 2^17`;
- universal DP4 operand bound 181, since `4 * 181^2 = 131044 <= 2^17`;
- a negative product truncates one guard bit, so a sign-mixed accumulation
  loses precision a same-sign one keeps;
- a fixed-point `Qm.f` value needs `m + f <= 17`.

Addition rounds toward zero, which is measured rather than inferred (steinmarder
`2026-05-28-rs482-fp24-add-rounding-mode-and-eft-compiler-fold.md`; silicon).
RTZ addition is what closes the double-double route: an error-free
transformation needs round-to-nearest, so FP64 by double-double on this ALU is
**refuted** (steinmarder `rs482-caveats.7.md`). FP32 and FP48 arithmetic are
likewise outside the envelope--FP32 is storage and transport only. Single-limb
exact-integer 8-point IDCT is **refuted** by the standard's own coefficient
width exceeding the FP24 window (steinmarder
`exact-idct-fp24-window-conservation-wall.md`).

Dot products carry named exact domains
(`r300_numeric_domain.c`; silicon): `U7_DOT`, unsigned 7-bit operands with
`4 * 127^2 = 64516 < 2^17`, confirmed 6/6 on RS482 plus a 4/4 byte-exact Vulkan
readback; `I8_MAG_DOT`, signed 8-bit magnitudes under the same bound, confirmed
in the same probe including signed cancellation; `U7_CONV5`, a five-term 7-bit
convolution column with `5 * 127^2 = 80645 < 2^17`, which is what makes a
32-by-32-to-64-bit multiply exact when split into five 7-bit limbs, confirmed
with `0xFFFFFFFF` squared bit-exact and a 1024/1024 exact Vulkan replay. The die
fact `dp4_limb_ceiling_bits = 7` names that limb width. `U8_OFFGRID` is the
boundary: `4 * 255^2 = 260100 > 2^17`, and an odd result above the window read
back 259844 for a true 259845, losing the eighteenth significand
bit (silicon)--the law is 17 significand bits rather than a hard value cutoff.

### Budget

64 ALU instructions, 32 TEX instructions, 96 total, 4 texture indirection
levels, 32 temporaries, and 32 constants
(`compiler/classic/r300_classic_target.c` and
`compiler/radeon_code.h`; known(source), corroborated by the live GL oracle
reporting the same limits). There is no flow control. The 64-ALU ceiling is
enforced at emission with "Too many ALU instructions used", and the indirection
ceiling with "Too many texture indirections"
(`r300_fragprog_emit.c`; known(source)). The R400 banking machinery that lets
later silicon exceed 64 per bank is absent from R300-class silicon and, on the
kernel side, is reachable only through the mutate-gated allowlist. A compute
verb longer than 64 ALU operations is therefore a multipass kernel, which is
what `MULTIPASS_SCAN` names.

### Output formats and channel routing

`US_OUT_FMT_0..3` at 0x46A4-0x46B0 select the export format with a 5-bit
`OUT_FMT`: `C4_8` 0, `C4_10` 1, `C4_10_GAMMA` 2, `C_16` 3, `C2_16` 4, `C4_16` 5,
`C_16_MPEG` 6, `C2_16_MPEG` 7, `C2_4` 8, `C_3_3_2` 9, `C_6_5_6` 0xa,
`C_11_11_10` 0xb, `C_10_11_11` 0xc, `C_2_10_10_10` 0xd, `C_16_FP` 0x10,
`C2_16_FP` 0x11, `C4_16_FP` 0x12 (S10E5), `C_32_FP` 0x13, `C2_32_FP` 0x14, and
`C4_32_FP` 0x15 (S23E8), with `C0_SEL` through `C3_SEL` routing constant-color
channels and `OUT_SIGN` selecting signed export
(`r300_reg.h:R300_US_OUT_FMT_0`; known(source)). The R2VB carrier uses
`C4_32_FP` with a BGRA select (steinmarder
`rs482_r2vb_substrate_validated_cs_write_registers.tsv`; silicon). `US_CONFIG`
carries `NLEVEL[2:0]` and `US_CODE_OFFSET` carries `ALU_OFFSET[5:0]` and
`ALU_SIZE[12:6]`, which is the program identity a readback decodes.

Sine and cosine exist in silicon on this generation but are unreachable through
the GL path (steinmarder `rs482-caveats.7.md`; known(source)), so the
transcendental verbs reach them through the alpha lane's `EX2`, `LG2`, `RCP`,
and `RSQ` and a range reduction, which is why those verbs carry a relative
tolerance rather than an exactness bound.

## FG: alpha test and fog

The fog and alpha block is eight registers. `FG_ALPHA_FUNC` carries an 8-bit
reference value and a 3-bit compare operation, `FG_FOG_BLEND` selects a linear,
exponential, or squared-exponential blend function, and `FG_DEPTH_SRC` selects
shader-computed against interpolated depth
(`rs482_gfx_3_0_0.reg`; known(source)). For a compute route the alpha test is a
second predicate alongside `KIL`, applied to a value the ALU already produced,
and the depth source select decides whether a kernel's own output can drive the
depth comparison. No numeric characterization of the fog blend exists in either
evidence repository, so its arithmetic stays **hypothesized** and it appears on
the probe frontier.

## RB3D: blend, ROP, and the color backend

### Blend as a reduction monoid

The blend unit is a read-modify-write reduction whose combine function is
selected by `RB3D_BLENDCNTL` `COMB_FCN` bits 12-13 and whose operands come from
`SRC_BLEND` bits 16-21 and `DST_BLEND` bits 24-29. Combine codes are
`ADD_CLAMP` 0, `ADD_NOCLAMP` 1, `SUB_CLAMP` 2, `SUB_NOCLAMP` 3, `MIN` 4, `MAX`
5, `RSUB_CLAMP` 6, and `RSUB_NOCLAMP` 7; factor codes include `GL_ZERO` 32,
`GL_ONE` 33, `SRC_COLOR` 34, and `DST_COLOR` 36 (steinmarder
`r300_output_op_encoding.tsv`; known(source)). Arithmetic is on the render
target's format range, clamped or wrapped per format, so the domain's rounding
model is clamp rather than an FP24 rule--the blend unit is a separate
reduction stage downstream of the ALU, not an ALU operation
(`r300_numeric_domain.c`, RB3D_BLEND domain; known(source)).

Four blend reductions are silicon-confirmed on RS482
(`r300_numeric_domain.c` catalog rows; silicon): `BLEND_ACC_REDUCTION`, a
histogram-style accumulate through `COMB_FCN_ADD` with factors ONE and ONE;
`REDUCE_MIN` through `R300_COMB_FCN_MIN`, 6/6 byte-exact; `REDUCE_MAX` through
`R300_COMB_FCN_MAX`, 6/6 byte-exact; and `SATURATING_DIFF` through
`R300_COMB_FCN_SUB_CLAMP`, realizing `max(a - b, 0)`, 6/6 byte-exact. `CONSTFILL`
is the degenerate case, a store realized as an RB3D clear with no per-element
ALU work (silicon).

`RB3D_CBLEND` carries `ALPHA_BLEND_ENABLE`, `SEPARATE_ALPHA_ENABLE`, and
`READ_ENABLE`; `RB3D_ABLEND` carries the alpha factor and alpha combine
function, so color and alpha reduce independently. `RB3D_BLEND_COLOR` supplies a
UNORM8 constant per channel, and `RB3D_COLOR_CHANNEL_MASK` gives a per-channel
write enable that is separate from the shader's own destination write mask
(`rs482_gfx_3_0_0.reg`; known(source)). The channel mask matters to the
validator as well: `r100_cs_track_check` skips the color-buffer size check
entirely unless one of the clear, channel-mask, or blend-read flags is set.

### ROP as bitplane logic

`RB3D_ROPCNTL` bits 8-11 select one of sixteen logic operations applied to the
raw color-target bits rather than to numeric values: `CLEAR` 0, `NOR` 1,
`AND_INVERTED` 2, `COPY_INVERTED` 3, `AND_REVERSE` 4, `INVERT` 5, `XOR` 6,
`NAND` 7, `AND` 8, `EQUIV` 9, `NOOP` 10, `OR_INVERTED` 11, `COPY` 12,
`OR_REVERSE` 13, `OR` 14, `SET` 15 (steinmarder `r300_output_op_encoding.tsv`
and `rs482_gfx_3_0_0.reg`; known(source)).

Only `XOR` has a located silicon witness. The steinmarder precision ladder
records the XOR route as `hw_pass` with the explicit qualification that it does
not generalize to the other operations, and the all-sixteen truth-table matrix
as a named unrun probe (`rs482_precision_ladder_v1/precision_ladder.tsv`;
silicon for XOR, known(source) for the rest). The most recent carrier-algebra
decomposition states the same split--ROP_BOOL XOR confirmed, AND, OR, and NOT
open (steinmarder
`2026-06-10-rs480-fourteen-carrier-algebras-falsification-decomposition.md`).
The driver catalog's `BITWISE_LOGICOP_MAP` row asserts AND, OR, and XOR are all
hardware-confirmed bit-exact; that assertion is **ahead of the located
evidence**, so AND and OR read as hypothesized here and the truth-table matrix
sits at the head of the probe frontier.

Bitplane logic operates on whatever the render-target format holds, so its width
is the format's channel width: at most 10 bits per channel for the integer
formats, since no pure-integer 32-bit color format exists on this chip
(steinmarder `r3v-compute.7.md`; known(source)).

### Formats and cache publication

Integer color is bounded at 10 bits per channel; FP16 render targets are
complete and round-trip exactly, confirmed for 1.5, 100.25, -7.5, and 42.0
(steinmarder `2026-06-10-rs480-fourteen-carrier-algebras-falsification-decomposition.md`;
silicon). The 128-bit `ARGB32323232` path is validated as transport, and no FP32
arithmetic identity is claimed for it; FP32 render targets are absent, the FP32
color framebuffer reports incomplete, and `EXT_color_buffer_float` is not
exposed, so an FP32 result leaves through a byte-encoded carrier
(`r300_numeric_domain.c` FP32_STORAGE domain; silicon). CMASK is **refuted** on
R3xx (steinmarder `rs482-rb3d-zb.4.md`).

Publication runs through the destination cache control register.
`RB3D_DSTCACHE_CTLSTAT` 0x4e4c reads `0x00000002` at rest and is read-safe only
when idle; the 2D `DSTCACHE_CTLSTAT` 0x1714 is a different register and is
blind-read safe (steinmarder `rs482_cache_ctlstat_register_lineage.tsv`;
silicon). The at-rest value is pinned as a die fact in
`r300_chip_identity.c:r300_rs480_die_facts` and cross-checked against
`r300_reg.h` by the chip-identity test.

## ZB: depth, stencil, and the occlusion counter

Depth comparison offers the eight standard functions `NEVER` through `ALWAYS`
(codes 0-7), selected by `Z_TEST` in `ZB_ZSTENCILCNTL`, alongside
`STENCIL_TEST` and the three stencil operation fields `STENCIL_FAIL`,
`STENCIL_ZPASS`, and `STENCIL_ZFAIL`
(`radeon_rs4xx_regbits_gen.h`, the fully decoded legacy shadow of the same
register; known(source)). `ZB_CNTL` carries `STENCIL_ENABLE`, `Z_ENABLE`,
`Z_WRITE_ENABLE`, `Z_SIGNED_COMPARE`, and `STENCIL_FRONT_BACK`;
`ZB_STENCILREFMASK` carries an 8-bit reference, compare mask, and write mask;
and `ZB_ZTOP` enables the depth test ahead of the shader
(`rs482_gfx_3_0_0.reg`; known(source)).

Stencil operations form an 8-bit per-pixel automaton: `KEEP` 0, `ZERO` 1,
`REPLACE` 2, `INCR` 3, `DECR` 4, `INVERT` 5, `INCR_WRAP` 6, `DECR_WRAP` 7
(steinmarder `r300_output_op_encoding.tsv`; known(source)). `INCR` is
silicon-confirmed as an 8-bit counter at `ZB_ZSTENCILCNTL` 0x4F04 (steinmarder
`rs482-rb3d-zb.4.md`; silicon), and `STENCIL_VERSIONED_CAS`--a versioned
compare-and-swap built from compare `EQUAL`, operation `INCR`, and a
samples-passed query--is confirmed by a probe ladder
(`r300_numeric_domain.c`; silicon). `DECR`, `INVERT`, and the wrapping pair have
no located witness: the carrier-algebra decomposition lists all of them as gaps.
The driver catalog's `STENCIL_INVERT_NOT` row claims a 0xA5 to 0x5A bit-exact
RS482 result from a named probe, and neither that probe script nor its bundle is
locatable in either evidence repository, so `INVERT` reads as hypothesized here
with its bundle recovery on the probe frontier.

The occlusion counter is an exact reduction. `ZB_ZPASS_ADDR` and
`ZB_ZPASS_DATA` return the surviving-fragment count, confirmed on a working
nonzero path, and the domain is explicitly unbounded by the FP24 window because
the count never passes through the ALU
(`r300_numeric_domain.c` ZPASS_COUNT domain and steinmarder `rs482-rb3d-zb.4.md`;
silicon). Exactness against a CPU oracle at large counts is source-grounded
rather than measured.

HiZ RAM is absent on RS480: `hiz_ram_dwords` is zero in the family capability
table and a live debug query reports no HiZ RAM (steinmarder
`r300_chip_family_caps.tsv`; silicon negative). ZMASK is present at 5120 dwords
with 8x8 Z compression, live-positive and under-mined. `ZB_FORMAT` carries
`DEPTHFORMAT` bits 0-3, and the kernel accepts format codes 0 and 1 at 2 bytes
per pixel and code 2 at 4, rejecting everything else
(`r300.c:r300_packet0_check`; known(source)); the enum naming those codes is not
located in either evidence repository, so which code is which depth layout stays
on the probe frontier.

## R2VB: the fragment-to-vertex carrier

R2VB closes the loop. A fragment program writes a GART buffer through the color
backend, and the same indirect buffer re-ingests it as a vertex stream through
`3D_LOAD_VBPNTR` plus a TCL-bypass draw (steinmarder `rs482.7.md`; silicon).
That round trip is the only mechanism by which a fragment-ALU result becomes an
input to another pipeline stage without a CPU read-back, so it carries every GPU
verb route whose consumer is the pipeline itself.

Delivery is FP32x4: `US_OUT_FMT_0` set to `C4_32_FP` with a BGRA select,
`RB3D_COLOROFFSET0`, `RB3D_COLORPITCH0`, and `RB3D_ROPCNTL` on the producer
side, and `VAP_VTX_SIZE` at 4 dwords with `VAP_CLIP_CNTL` `CLIP_DISABLE` on the
consumer side--fourteen CS-write registers validated in total (steinmarder
`rs482_r2vb_substrate_validated_cs_write_registers.tsv`; silicon, validator
accepted).

Typed carry admits three integer types over that float transport: a boolean maps
to `R2VB_BOOL1`, a signed integer within [-131072, 131072] maps to `R2VB_SINT`,
and an unsigned integer at or below 131072 maps to `R2VB_UINT`. Values outside
those ranges and any mixed-signedness tuple decline. The inclusive bound is the
FP24 exact-integer ceiling, machine-checked by the `fp24_int_exact_inclusive`
proof (steinmarder `2026-07-14-typed-carry-cpu-classifier-decline-enum.md`;
known(source) for the classifier logic). The classifier is source-verified and
its production route is unreachable, because the vertex-shader admission
whitelist is float-only--so typed carry is a designed contract without a
shipping path.

The fetched producer completes the route: the producer draw writes the carrier
and a second pass fetches it as vertex data rather than having the CPU
re-stage it. The identity carrier's GPU route is the one compute verb route that
executes end to end.

The throughput law is why the GPU route is worth building at all and why it is
not the default today. Software TCL transforms about 1.0M vertices per second
while the fragment ALU sustains about 354M operations per second (steinmarder
`rs482.7.md`; silicon), a ratio near 350 in the ALU's favor--but the measured
route dispatch for small draws still favors the CPU: 95.3 microseconds against
114.6 for the immediate GPU route, and 101.7 against 114.2 for the fetched
route, 12 of 12 with p = 0.00049. The admission and oracle document owns that
comparison; the consequence for this atlas is that a GPU verb route earns its
place at a work size large enough to amortize dispatch, not at every size.

## Memory and caches

GART pages carry read, write, and unsnooped attributes, and request snooping is
disabled globally at AGP-mode configuration, so a GPU write is not guaranteed
visible to a cached CPU read without an explicit action
(`rs400.c:rs400_gart_set_page` and `rs400.c:rs400_gart_enable`; known(source)).
A retained capture of the first 64 page-table entries of a 1 GiB aperture shows
unsnooped clear with read and write set, and the sampled GART-table CPU leaves
resolve to cache-disabled through PAT index 2 over write-back MTRR; the
allocation class named coherent is allocation terminology and not an observed
coherence property (vostro1000-re `uma-gart-cacheability-graph.md`;
observation). The framebuffer CPU mapping resolves to write-combining. The K8
host GART is off for the graphics translation path, so the RS482 GTT is the
active translation.

The rule a host read of device output must invalidate before reading is
**hypothesized**: the mechanism follows from unsnooped pages plus cached TTM
mappings, and no located run resolves whether a per-PTE snoop bit overrides the
global disable. The `EFFECTIVE_PER_PTE_SNOOP_SEMANTICS` question stays open in
both evidence repositories.

Two cache control registers publish results. `RB3D_DSTCACHE_CTLSTAT` 0x4e4c
publishes color writes and reads `0x00000002` at rest; `ZB_ZCACHE_CTLSTAT`
0x4f18 publishes depth and stencil writes and reads `0x00000001` at rest. Both
values are pinned as die facts
(`r300_chip_identity.c:r300_rs480_die_facts`; silicon). The 0x4e4c read is safe
only against an idle engine.

The GTT size sets differ by scope rather than disagreeing.
`rs400.c:rs400_gart_adjust_size` admits {32, 64, 128, 256, 512, 1024, 2048} MiB
and forces anything else to 32 with an error (known(source)); vostro1000-re
characterizes {128, 256, 512, 1024} with their placements on this box,
before-VRAM for the first three and after-VRAM for 1024 (observation). The first
is what the kernel accepts, the second is what has been placed and measured. No
candidate is selected as optimal. The fork additionally segregates GTT objects at
or below 512 KiB to a top-down placement so small allocations do not perforate
the middle of the aperture, declared a heuristic rather than a measured optimum
(`radeon_object.c:radeon_ttm_placement_from_domain`; known(source)).

Bandwidth is calculated, never measured. The DRAM array offers 8.5 GB/s at
DDR2-533 across 128 bits, and the HyperTransport uplink offers 3.2 GB/s in one
direction at 800 MHz DDR by 16 bits, which makes the host bridge the structural
ceiling for GPU traffic (vostro1000-re
`k8-integrated-northbridge-and-rs482-host-bridge-decomposition.md`; derived). No
retained run establishes actual payload bandwidth.

## Numeric type matrix

Each cell names the mechanism, its exactness bound, and its evidence class, or
`none` where no unit holds the type. Composition marks a type realized by
splitting into limbs a unit does hold, rather than by a native datapath.

### Integer types

| Type | fetch (VAP) | sample (TX) | ALU (US) | interpolate (RS) | reduce/blend (CB) | logic (ROP) | count/compare (ZB) | carry (R2VB) |
|---|---|---|---|---|---|---|---|---|
| int/uint 1 | `DATA_TYPE` decode into a 0/1 float, exact, known(source) | LUT texel, exact, known(source) | CMP/CND select on a 0/1 value, exact, silicon | affine blend of 0/1 is not boolean, none | blend MIN/MAX act as AND/OR on 0/1, exact, silicon | any of 16 ops on the low bit; XOR silicon, rest hypothesized | stencil compare and `KIL` predicate, exact, silicon | `R2VB_BOOL1`, exact, known(source) |
| int/uint 4 | packed in a wider element, exact, known(source) | `Y4X4` and `W4Z4Y4X4` texels, exact, known(source) | exact inside the FP24 window, silicon | none | `C2_4` target range, clamped, known(source) | nibble lanes of the target word, XOR silicon | none | packed inside the SINT/UINT carry, exact, known(source) |
| int/uint 8 | `BYTE` and `D3DCOLOR` with signed and normalize flags, exact, known(source) | UNORM8 texels bilerped at 6-bit weight, biased rounding, silicon | exact inside the FP24 window; DP4 exact to the 7-bit limb (`U7_DOT`, `I8_MAG_DOT`), 8-bit 4-term dots go off-grid above 2^17, silicon | affine MAC, width uncharacterized, hypothesized | ADD, SUB clamp, MIN, MAX on the 8-bit target range, silicon | full-word logic on 8-bit channels; XOR silicon, rest hypothesized | 8-bit stencil automaton; INCR silicon, INVERT and DECR and wrap hypothesized | inside the SINT/UINT carry, exact, known(source) |
| int/uint 16 | `SHORT_2` and `SHORT_4`, exact, known(source) | `X16` and `Y16X16` texels, exact, known(source) | exact inside the FP24 window; `Q16_16` two-limb add exact since `2*(2^16-1)+1 < 2^17`, silicon for the MAC path | none | 10-bit-per-channel integer target ceiling bounds it, known(source) | `C_11_11_10` and `C_2_10_10_10` channel widths, XOR silicon | none | inside the SINT/UINT carry to 131072, exact, known(source) |
| int/uint 24 | packed in a 32-bit element, exact, known(source) | `Z11Y11X10` and `W2Z10Y10X10` approximate it, known(source) | above the exact window; composition through 7-bit limbs, silicon | none | none | 24 bits of the target word, XOR silicon | none | exceeds the 131072 carry bound, declines, known(source) |
| int/uint 32 | `FLOAT_4` word transport, exact, silicon | `FL_I32` and `FL_R32G32B32A32` texels, exact, silicon | composition: five 7-bit limbs through `U7_CONV5`, `MULTILIMB7_U32_MUL` bit-exact for `0xFFFFFFFF` squared, silicon | none | none | 32-bit target word; XOR silicon, rest hypothesized | none | word transported through FP32x4, exact, silicon; typed carry declines above 131072 |
| int/uint 64 | none | none | composition of 32-bit limb products through the same column law, hypothesized | none | none | none | none | none |

### Floating-point types

| Type | fetch (VAP) | sample (TX) | ALU (US) | interpolate (RS) | reduce/blend (CB) | logic (ROP) | count/compare (ZB) | carry (R2VB) |
|---|---|---|---|---|---|---|---|---|
| fp4 | none as a type; packed in a wider element only, known(source) | none | none | none | none | none | none | none |
| fp8 | none as a type; packed in a wider element only, known(source) | LUT decode through a texel table, numeric-derived, hypothesized | none natively; decoded to FP24 first | none | none | none | none | none |
| fp16 | `FLT16_2` and `FLT16_4` exist in the enum; the route is blocked on this chip by Draw normalization, hypothesized | 16F texels point-sampled rather than filtered, silicon | storage only natively; IEEE semantics emulated on FP24 by two-limb base-64 significand columns with RNE extraction, `IEEE16_MUL_RNE` 12/12 exact, silicon | none | `C4_16_FP` target complete, round-trip exact, silicon | none | none | transported inside FP32x4, exact, silicon |
| fp24 | none as a stream type | none as a texel type | the native datapath: s1e7m16, RTZ, exact integers to 131072, DP4 operand bound 181, square bound 362, silicon | affine MAC in the interpolator, width uncharacterized, hypothesized | blend acts on the target format, not on FP24, known(source) | none | none | exported through FP32x4 with no precision gained, silicon |
| fp32 | `FLOAT_1` through `FLOAT_4`, exact transport, silicon | `FL_R32G32B32A32` accepted as a DP4 input, silicon | transport only: the FP24 significand is 17 bits against FP32's 24, so arithmetic narrows, silicon | none | render target absent, framebuffer incomplete, `EXT_color_buffer_float` unexposed, silicon | none | none | `C4_32_FP` delivery, byte-exact round trip, silicon |
| fp64 | none | none | refuted: double-double needs round-to-nearest and the adder rounds toward zero | none | none | none | none | none |

## Verb-to-unit binding

One distinction governs the whole table. A row's **operation** carries a silicon
witness in the catalog whenever its status is hardware-confirmed; the row's **GPU
route** is a dispatchable path under an exact gate, and thirteen of fifteen rows
have no such route built. Absent route with a confirmed operation means the
arithmetic is proven and the plumbing is not, which is precisely what this atlas
exists to unblock. `r300_compute_verb.c:r300_compute_verb_queue_conformant`
therefore returns false against the full table, and only a gated claim can pass.

| Verb | Unit binding | Exactness bound | Route today | Probe still owed |
|---|---|---|---|---|
| `IDENTITY_MAP` | R2VB carrier: US export to `C4_32_FP`, fetched re-ingest | FP24 exact window for the GPU route; the CPU route moves every 32-bit pattern | CPU and GPU both executing | none for the route; wider source formats |
| `CONST_FILL` | RB3D clear, no per-element ALU | bit exact | CPU and GPU absent, operation confirmed | route construction only |
| `UNARY_AFFINE_MAP` | US FP24 ALU, one MAD | FP24 exact window | absent, operation confirmed | route construction only |
| `BINARY_ARITHMETIC_MAP` | US FP24 ALU | FP24 exact window | absent, operation confirmed | route construction only |
| `UNARY_TRANSCENDENTAL_MAP` | US alpha lane `EX2`, `LG2`, `RCP`, `RSQ` plus range reduction | 3% relative, not bit-exact | absent, family validated within tolerance | per-function bound tightening |
| `BINARY_TRANSCENDENTAL_MAP` | US alpha lane, pow and div compositions | 3% relative | absent, family validated within tolerance | per-function bound tightening |
| `BITWISE_LOGICOP_MAP` | RB3D ROP over the packed RGBA8 target | bit exact | absent | AND and OR truth tables; the catalog claims them, the corpus witnesses XOR only |
| `MULTITAP_GATHER` | TX fetch plus US FP24 accumulate | FP24 exact window | absent, operation confirmed | route construction; indirection depth budget |
| `PREDICATED_STORE` | TX-issued `KIL` plus RB3D write | bit exact | absent, operation confirmed | route construction only |
| `MULTIPASS_SCAN` | US FP24 ALU across ping-pong passes | FP24 exact window | absent | cross-leaf reduction at the pass boundary |
| `REDUCE` | RB3D blend `ADD`, `MIN`, `MAX` | FP24 exact window declared; the blend acts on the target format range | absent, MIN and MAX 6/6 byte-exact | interaction of the declared FP24 bound with the clamped target range |
| `SATURATING_DIFF` | RB3D blend `SUB_CLAMP` | bit exact, `max(a - b, 0)` | absent, 6/6 byte-exact | route construction only |
| `PARALLEL_4OUT_MAP` | US FP24 ALU across four render targets | FP24 exact window | absent | multiple-render-target emission on this chip |
| `STENCIL_INVERT` | ZB stencil operation `INVERT` | bit exact over [0,255] | absent | the INVERT witness itself: the catalog names a probe neither evidence repository holds |
| `BITWISE_NOT_MAP` | host executor; ROP `INVERT` is the candidate carrier | bit exact | CPU executing, GPU absent | ROP `INVERT` truth table |

Grid index classes bind three rows: `IDENTITY_MAP` and `BITWISE_NOT_MAP` are
linear, so their invocation counts are bounded at 2^17 + 1 by
`r300_grid_fold.h:r300_grid_linear_index_exact`; `MULTITAP_GATHER` is coordinate
addressed and takes the 2048x2048 raster bound instead. Every other row declares
no index class and is position addressed
(`r300_compute_verb.c`; known(source)).

Only two job operations dispatch to a row today: identity and bitwise-not
(`r300_compute_verb.c:r300_compute_verb_for_job`). Five refusal classes stay
closed at admission--arbitrary scatter, image store, shared memory or barrier,
general atomic, and integer shift--under six failure clauses that refuse at
admission, gate exactness per verb, forbid a fallback after submit, refuse
before a write, quarantine on oracle divergence, and advertise only after
silicon (same file; known(source)).

## Probe frontier

Ordered by what unblocks the most verb routes.

- **ROP truth-table matrix**, all sixteen codes against a known-good oracle. XOR
  is witnessed; AND and OR are asserted by the driver catalog without a located
  finding; the remaining thirteen are register-grounded only. Unblocks
  `BITWISE_LOGICOP_MAP` and `BITWISE_NOT_MAP`, and reconciles the catalog with
  the corpus.
- **Stencil `INVERT` witness recovery**, then `DECR`, `INCR_WRAP`, and
  `DECR_WRAP`. The catalog names an RS482 probe and a 0xA5 to 0x5A result whose
  script and bundle are absent from both evidence repositories; recover the
  bundle or rerun. Unblocks `STENCIL_INVERT`.
- **DP4 with non-dyadic operands** and the **FMA fusion boundary**: whether the
  multiply and add of a MAD round once or twice decides which limb schemes hold.
  Bounds every FP24 verb.
- **RS interpolator fractional width**, with the integer-cast exactness
  falsifier. Until it runs, no interpolated value carries an exactness bound and
  the interpolator is unusable as a compute stage.
- **FG numeric characterization**: the fog blend functions and the alpha-test
  compare have no measured behavior.
- **Z-buffer depth-format enum**: the kernel accepts codes 0, 1, and 2 with 2,
  2, and 4 bytes per pixel; which layout each code names is unlocated.
- **16F and 32F sampling ladder**: float payloads are point-sampled, and no
  ladder establishes the fetch behavior across filter modes and formats.
- **`CP_DMA` presence**: absent from the microcode decode corpus, which is a
  decode gap rather than an absence claim. A copy engine would change the
  carrier design.
- **ME dispatch decode for `3D_LOAD_VBPNTR`, `3D_DRAW_IMMD_2`, and
  `INDX_BUFFER`**: the kernel admits all three and the even-key table names none,
  so the decode is incomplete in a way that touches the packets the R2VB route
  depends on.
- **VAP `FLOAT_8` and FLT16 ingestion**, currently blocked by Draw
  normalization; widening the ingest morphism widens every carrier source format.
- **Per-PTE against global GART snoop precedence**, which decides whether a host
  read of device output needs an explicit invalidate.
- **Multiple-render-target emission**, blend clamp matrices, and depth fast
  clear, none of which have been exercised. Unblocks `PARALLEL_4OUT_MAP`.

## Related documents

- `docs/hardware/r3v-implementation-boundaries.md`--what the Vulkan driver
  implements and the ordered native migration path.
- `docs/hardware/rs482-2048-4096-virtualization.md`--how a logical extent above
  the 2048 sampler cap decomposes while preserving representation.
- `docs/hardware/r3v-vulkan-memory-model-over-radeon-drm.md`--stage-by-stage
  ownership of the Vulkan memory model over radeon DRM, and the open snoop
  semantics question.
- `docs/hardware/rs482-native-delivery-route-admission-and-oracle-model.md`--how
  a delivery route is admitted, which oracle decides it, and the measured
  CPU-against-GPU dispatch comparison.
