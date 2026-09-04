# R300 2D solid-fill primitive authority for the direct write control

This memorandum resolves the direct-write control's open primitive
question (`docs/hardware/r300-direct-write-control.md`): which
kernel-qualified command stream performs one bounded memory write
independent of the 3D pipeline. The answer is the 2D engine's solid
rectangle fill, driven entirely by PACKET0 register writes the radeon
user-CS parser admits. Every claim below names its source; the parser
facts are KERNEL_SOURCE_DERIVED from linux-radeon-gororoba at the
deployed driver checkpoint, and the silicon behavior of the fill on
RS485M remains the hypothesis the control exists to test.

## Parser admission

`r300_packet3_check` names only 3D packets and the NOP, so a 2D
PACKET3 (`BITBLT_MULTI` and its siblings) is refused through
`DRM_RADEON_CS` on r300-class parts. The 2D engine is equally
programmable through PACKET0 writes, and `reg_srcs/r300` places the
whole solid-fill register set on the safe list, where the bitmap
passes them unchecked:

```text
0x1438 DST_Y_X               0x146C DP_GUI_MASTER_CNTL
0x143C DST_HEIGHT_WIDTH      0x147C DP_BRUSH_FRGD_CLR
0x1598 DST_WIDTH_HEIGHT      0x15C0 CLR_CMP_CNTL
0x16C0 DP_CNTL               0x16CC DP_WRITE_MSK
0x16E8 DEFAULT_SC_BOTTOM_RIGHT
0x16EC SC_TOP_LEFT           0x16F0 SC_BOTTOM_RIGHT
0x1714 DSTCACHE_CTLSTAT      0x1720 WAIT_UNTIL
```

`RADEON_DST_PITCH_OFFSET` (0x142C) is a named case in
`r300_packet0_check` handled by `r100_reloc_pitch_offset`: it consumes
one relocation, adds `reloc->gpu_offset >> 10` into the low 22 bits of
the written value, and under `RADEON_CS_KEEP_TILING_FLAGS` preserves
the caller's pitch field (`value & 0xffc00000`). The destination
therefore binds to a GEM BO by relocation, the same admission rule the
successor's color buffer uses.

## Register and packet contract

The kernel's own 2D emitter (`r100_copy_blit`) fixes the field
encodings this stream reuses:

- `DST_PITCH_OFFSET`: `(pitch << 22) | (offset >> 10)`, pitch in
  64-byte units, so the 256-byte-per-row target carries pitch 4 and a
  BO-base offset of zero; the 1 KiB offset granularity is why per-pixel
  addressing rides in `DST_Y_X` rather than the offset field.
- `DP_GUI_MASTER_CNTL`: `GMC_DST_PITCH_OFFSET_CNTL | GMC_BRUSH_SOLID_COLOR
  | (COLOR_FORMAT_ARGB8888 << 8) | ROP3_P | GMC_CLR_CMP_CNTL_DIS |
  GMC_WR_MSK_DIS`, selecting the solid brush as the ROP source with
  write masking and color compare off.
- `DP_BRUSH_FRGD_CLR`: the 32-bit fill value.
- `DP_CNTL`: left-to-right, top-to-bottom (`DST_X_LEFT_TO_RIGHT |
  DST_Y_TOP_TO_BOTTOM`).
- `DST_Y_X`: `(y << 16) | x`; `DST_WIDTH_HEIGHT`: `(width << 16) |
  height`, and this write launches the fill.
- Post-fill publication inside the stream, as `r100_copy_blit` ends:
  `DSTCACHE_CTLSTAT = RB2D_DC_FLUSH_ALL`, then `WAIT_UNTIL =
  WAIT_2D_IDLECLEAN | WAIT_HOST_IDLECLEAN | WAIT_DMA_GUI_IDLE`.

The control issues two 1x1 fills -- value `0x11223344` at (16,16) and
value `0xa1b2c3d4` at (47,47) -- as two `DP_BRUSH_FRGD_CLR` /
`DST_Y_X` / `DST_WIDTH_HEIGHT` groups under one `DP_GUI_MASTER_CNTL`,
then one flush and wait. Completion and readback ride the successor's
transport unchanged: `GEM_WAIT_IDLE`, cache invalidation, host read.

## What this removes and what remains

The stream touches no VAP, RS, US, RB3D, or ZB state: the write path
is brush value -> ROP -> 2D destination, so the first-draw color-write
gates (`US_OUT_FMT_0`, `RB3D_COLOR_CHANNEL_MASK`, `SC_SCREENDOOR`) sit
outside it, and the fill has no source fetch, so the GPU performs one
write and no read. What remains untested until silicon: whether the
RS485M 2D engine writes an unsnooped-GART destination coherently under
this flush/wait sequence, and whether the 2D default scissor admits
the 64x64 extent without explicit `SC_TOP_LEFT`/`SC_BOTTOM_RIGHT`
programming -- the stream programs the scissor registers explicitly so
the second question does not gate the first. A failed fill classifies
as CONTROL FAILED / INCONCLUSIVE under the direct-write control's own
rule.

The parser admits the stream without bounding the 2D destination: the
CS tracker's extent checks cover the 3D color-buffer path
(`r100_cs_track_check` over the RB3D state), and `r100_reloc_pitch_offset`
patches the DST_PITCH_OFFSET relocation without comparing the fill
extent to the buffer object's size, and the relocation domains steer
placement rather than gate validation. Replay probes against the
retained cell confirm each: an undersized 4096-byte color BO table, a
zero write-domain table, and a VRAM write-domain table all replay
ACCEPT-NO-DRAW through `replay_r300_cs_track`. Kernel admission is
therefore packet-grammar and safe-list admission only; the write's
containment inside the allocated target rests on the cell's own
pitch/offset/extent correctness, which the emitter's unit test, the
byte-identity check against the retained IB, and the canary row carry.

## Evidence chain

- `r300_packet3_check`, `r300_packet0_check`, `r100_reloc_pitch_offset`,
  `reg_srcs/r300`: parser admission (kernel source, deployed checkpoint
  lineage `293a4ae3fe82`).
- `r100_copy_blit`: field encodings and the post-blit flush/wait order
  (kernel ring emitter; its PACKET3 framing is what the user-CS path
  replaces with PACKET0 writes).
- `replay_r300_cs_track` and the run-form correspondence gate: offline
  proof that the exact control stream is admitted, before any silicon
  contact.
