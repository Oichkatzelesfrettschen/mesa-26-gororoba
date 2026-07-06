# HBTCL-01 no-submit PM4 decode runbook

## Goal

Name the single diverging frontend value in the retained `fbo-clearmipmap` clear-quad command stream without submitting another hazardous GL draw.

This is the immediate precondition for HBTCL-02. The hybrid vertex TCL design narrows the wedge to bypass-mode VAP/GA vertex assembly: command submission succeeds, the GPU hangs while executing the command stream, `RBBM_STATUS` shows `VAP_BUSY=100%` and `GA_BUSY=100%`, and `RADEON_DEBUG=nocbzb` refutes the CBZB color-clear path. The remaining problem is a value divergence in the ordinary blitter-quad draw, not a code-path fork and not a hardware-TCL engagement.

## Safety contract

HBTCL-01 is decode-only. It does not submit a GL draw, write a hazardous register, or invoke the reset path.

Run only the no-submit PM4/atom decode workflow already used for R300_TRACE and R2VB investigation. The Vostro may be queried for retained artifacts, but the decode itself is host-side analysis of captured IB/CS data.

Before collecting or touching any Vostro-side artifact, these live preconditions hold:

- `radeon.lockup_timeout=0` is live.
- The CPU1 PCI-status heartbeat is running.
- The wedge poller is available.
- Hang-for-inspection posture is active for GPU hazard work.
- No reset probe or hazardous reader runs unless the matching `steinmarder-r300/hazard_policy.json` gate is explicitly set and the hazard check is clean.

## Inputs

Decode and compare these streams:

1. The retained wedging `fbo-clearmipmap` clear-quad IB/CS, especially the 672-dword submission associated with the unsignaled fence.
2. A working r3v triangle or bypass draw IB/CS.
3. Any R2VB direct-VAP handoff decode already demonstrated to submit hang-free.

Use the existing PM4 atom decoder, R300_TRACE artifacts, and r300 emit metadata. If `RADEON_DUMP_CS=1` is unavailable or empty for this path, use the R2VB no-submit decode method rather than rerunning a live wedge.

## Registers and values to resolve

The decode report names the first diverging value among these frontend suspects, with the literal PM4 packet, register, value, and emitter that produced it:

- `VAP_VF_CNTL`, especially `NUM_VERTICES` and any 16-bit count or underflow shape.
- `VAP_VTE_CNTL`, especially clip-space versus window-space coordinate interpretation.
- `VAP_CNTL` / `VF_MAX_VTX_NUM`, especially the demonstrated bypass word `0x0014025a` versus any blitter draw value.
- `SC_SCISSORS_TL` / `SC_SCISSORS_BR` and `SC_CLIPRECT_TL_0` / `SC_CLIPRECT_BR_0`, including the non-r500 `+1440` arithmetic.
- `ZB_CNTL.Z_ENABLE`, confirming whether the draw reaches a z-buffer check with no valid z/s target.
- `PSC`, `VAP_VTX_SIZE`, and `VAP_PROG_STREAM_CNTL*`, especially stride or attribute-layout mismatches.

## Procedure

1. Locate the retained clear-quad CS artifact and record its path, timestamp, fence sequence, dword count, and collection method.
2. Locate the working comparison CS artifact and record the same metadata.
3. Decode both streams without submission.
4. Slice each decode to the final draw and the immediately preceding state packets.
5. Build a side-by-side table for the suspect registers above.
6. For every divergent value, identify the r300 emitter symbol and record the symbol-discovery method, such as `rg --fixed-strings`, `global -r`, `ast-grep`, or clangd references.
7. Select the HBTCL-02 target only when exactly one divergence explains the VAP/GA stall and the alternatives are falsified or lower-ranked.

## Decode report template

```text
Artifact A: wedging clear quad
  path:
  source:
  fence seqno:
  dwords:
  final draw packet:

Artifact B: working bypass/r3v draw
  path:
  source:
  fence seqno:
  dwords:
  final draw packet:

Suspect register comparison:
  VAP_VF_CNTL:
  VAP_VTE_CNTL:
  VAP_CNTL / VF_MAX_VTX_NUM:
  SC_SCISSORS / SC_CLIPRECT:
  ZB_CNTL:
  PSC / VAP_VTX_SIZE / VAP_PROG_STREAM_CNTL:

Emitter map:
  register -> value -> emitter -> discovery method

Selected HBTCL-02 target:
  register/value:
  emitter:
  rationale:
  falsified alternatives:
```

## Acceptance criteria

- No new GL wedge is triggered while completing HBTCL-01.
- The report identifies one concrete value, register, and emitter as the HBTCL-02 target, or explicitly reports that the retained artifact is insufficient and names the missing non-submit artifact needed.
- Evidence language follows the design convention: Rocq-backed mathematics may be called proven; hardware observations are demonstrated or measured.
- The next patch is bounded: HBTCL-02 changes only the identified clear-quad bypass value or atom and verifies `fbo-clearmipmap` under the forensic poller.

## References

- `docs/hardware/rs482-hybrid-vertex-tcl-design.md`
- `src/gallium/drivers/r300/r300_r2vb.c`
- `src/gallium/drivers/r300/r300_hb_tcl.c`
- `src/gallium/drivers/r300/r300_emit.c`
- `src/gallium/drivers/r300/r300_reg.h`
