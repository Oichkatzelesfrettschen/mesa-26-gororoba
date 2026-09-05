# ZB HyperZ ownership admission

The RS485M boots with its Z engine initialized: the radeon driver reports
`1 quad pipes, 1 Z pipes initialized` from `r300_gpu_init`, `CHIP_RS480`
carries `RV3xx_ZMASK_SIZE` (5120 dwords, 20 KB of on-die ZMASK SRAM), and
the ZB block exposes `ZB_ZCACHE_CTLSTAT`, `ZB_ZMASK_OFFSET`, `ZB_HIZ_OFFSET`,
and `ZB_BW_CNTL`. The kernel guards the HyperZ part of that block behind
ownership, and the driver now judges every submission against that
ownership one layer before the kernel does.

## The kernel's rule

`radeon_kms.c` grants HyperZ to one file descriptor at a time through
`RADEON_INFO_WANT_HYPERZ`: the value is both request and answer, a returned
1 is ownership, and device close releases it. `r300_packet0_check` and
`r300_packet3_check` then judge every stream against `hyperz_filp`:

| Register or packet | Non-owner disposition |
| --- | --- |
| `ZB_BW_CNTL` bits HIZ_ENABLE, RD_COMP_ENABLE, WR_COMP_ENABLE, FAST_FILL_ENABLE | reject |
| `ZB_ZMASK_OFFSET`, `ZB_ZMASK_PITCH`, `ZB_HIZ_OFFSET`, `ZB_HIZ_PITCH` nonzero | reject |
| `GB_Z_PEQ_CONFIG` nonzero (any value below `CHIP_RV350`) | reject |
| `SC_HYPERZ` bit 0 | cleared in the stream, no error |
| `PACKET3 3D_CLEAR_HIZ`, `PACKET3 3D_CLEAR_ZMASK` | reject |

A rejection is `-EINVAL` from `DRM_RADEON_CS`, which the driver reports as
device loss. The silent clear is worse: the draw runs without the state its
recording assumed and every layer reports success.

## The same rule one layer earlier

`src/amd/r300/common/r300_zb_hyperz_admission.{c,h}` carries the table
above as data and walks a stream the way `radeon_cs_packet_parse` frames
it: type-0 register runs including `ONE_REG_WR`, type-2 fillers, type-3
opcodes. `r300_zb_hyperz_admit_stream` answers ADMIT when no HyperZ write
is present or ownership is held, REFUSE_OWNERSHIP with the first gated site
otherwise, and REFUSE_STREAM for a malformed header. The silent-clear row
refuses like the others, so `SC_HYPERZ` is never sent by a non-owner.

`r3v_native_hyperz_admit` runs in the native queue on the admitted IB,
after route admission and before the relocation list and digest bind it. A
stream with no HyperZ write costs one scan and no ioctl. A stream with one
acquires ownership through the transport's `RADEON_INFO_WANT_HYPERZ`
wrapper when the device holds none, admits when the kernel grants it, and
otherwise refuses by name with the row, the IB index, and the kernel rule
that would have fired. The device releases the block at destruction.

The kernel's rejections are untouched. What changes is who sees the
refusal first and what it says: the driver, naming the register and the
rule, with ownership acquired where the kernel permits it, instead of the
CS ioctl failing on a stream the driver already committed to.

## What the driver emits today

`r300_zb_depth_state_emit` writes `ZB_BW_CNTL` as `HIZ_DISABLE |
FAST_FILL_DISABLE` and touches no ZMASK or HiZ register, so every stream the
driver emits admits without ownership; the test pins that. Ownership is
acquired only when a stream carries a HyperZ write, which no route does
yet.

## The first silicon cell, and why it is not in this change

The control already exists: `r300_zb_depth_control_cell` draws two
triangles at two depths against a host-filled sentinel surface with HyperZ
disabled and reads both halves back. The first HyperZ cell is the smallest
ZMASK-backed configuration on the same geometry: ownership acquired, a
`ZB_ZMASK_OFFSET` and `ZB_ZMASK_PITCH` bound to a dedicated allocation,
`3D_CLEAR_ZMASK` over the tile range, `ZB_BW_CNTL` with `FAST_FILL_ENABLE`
alone, HiZ and compression left disabled, and the same readback; a
`RD_COMP` or `WR_COMP` arm follows only after that receipt. The cell needs
the ZMASK initialization and clear protocol established from the register
documents before it is armed, and it runs as its own attended attempt
under a sealed prediction in steinmarder-r300. The admission in this
change is what lets that cell reach the kernel as an owner.

## Verification

* `r300-zb-hyperz-admission`: every gated bit of every row refuses unowned
  and admits owned; an ungated value of a gated register admits; the
  driver's own depth state admits unowned; each row appended to it refuses
  at its own IB index; multi-register and `ONE_REG_WR` runs resolve the
  register the kernel resolves; type-2 fillers skip; a run past the end
  and a type-1 header refuse the stream.
* The r300 and r3v suites: no route emits a HyperZ write, so every
  existing stream admits and no submission acquires ownership.
