# RS485M direct native write control

Status: EXECUTION-READY pending the separate explicit authorization. See
Artifact identity for what stands proven and
`docs/hardware/r3v-native-direct-write-attended-procedure.md` for the
live-run procedure.

## What this control decides

The successor cell reaches silicon through the native transport, retires, and
is expected to leave a triangle in its color target. The first attended run
reached silicon, retired clean, and left the target byte-identical to its
sentinel fill. Two models explain that outcome and the cell alone cannot
separate them: the 3D pipeline produced no color write, or the transport
itself -- allocation, mapping, cache publication, relocation, submission,
completion, readback -- never carried a device write back to the host at all.

This control writes the target through the same transport, using the minimum
kernel-qualified command stream capable of one bounded memory write,
independent of the 3D pipeline where R300 permits that independence. A
control that lands its bytes proves the transport carries device writes and
leaves the pipeline as the only remaining explanation.

## Failure classification

A control that does not land its bytes is classified INCONCLUSIVE / CONTROL
FAILED. The positive control failed, so the experiment cannot establish
GPU-to-host write visibility through this control path, and no pipeline
hypothesis follows from the failure. Causes this outcome does not
distinguish among: the control command never executed; the write bound to
the wrong target; the relocation resolved to the wrong address; the
direct-write packet semantics differ from what the stream assumes;
completion signaled without the expected write; a cache-visibility failure
held the write off the host-visible domain; or host readback itself failed.
A successful result proves only that this exact direct-write command stream
produced observable device writes through the same BO, cache, completion,
and readback substrate exercised here -- not general transport correctness.

## Sharing the successor's mechanism

The control's evidence rests on using the successor's mechanism unchanged.
Each of these is the same object, call, or value the successor uses; a control
that substitutes any of them decides a different question.

- GEM memory class: `RADEON_GEM_DOMAIN_GTT`, allocated through the same
  `radeon_drm_vk_bo` path with the same alignment and size.
- Mapping: the same `radeon_drm_vk_bo_map` CPU mapping, cached, as the
  unsnooped-GART model requires.
- Cache publication: the same `radeon_drm_vk_bo_cache_sync` calls at the same
  points -- after the host write, before submission, after completion, and
  before readback.
- Relocation: the same relocation-site construction and the same
  `RADEON_CS_KEEP_TILING_FLAGS` submission flag, so the color buffer is
  resolved by the same rule.
- Transport: the same `DRM_RADEON_CS` ioctl through the same command-stream
  builder, armed under `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1` by the control's
  own stream digest, not the successor's.
- Completion: the same `DRM_RADEON_GEM_WAIT_IDLE` call through
  `radeon_drm_vk_completion_await`, which makes at most three attempts. The
  canonical kernel's `radeon_gem_wait_idle_ioctl` bounds each attempt with
  interruptible `dma_resv_wait_timeout(..., true, 30 * HZ)`; see Containment
  for the recovery sequence after the final attempt returns without retirement.
- Readback invalidation: the same invalidate before the host reads.

The command stream is what differs. The selected primitive is the 2D
engine's solid rectangle fill, driven by PACKET0 writes the r300 user-CS
parser admits, with the destination bound by the `DST_PITCH_OFFSET`
relocation case; `docs/hardware/r300-direct-write-2d-fill-authority.md`
carries the register contract and its kernel-source derivation. The fill
touches no VAP, RS, US, RB3D, or ZB state and performs no source fetch,
so the write path is brush value, ROP, 2D destination -- outside the
color-write gates the first run implicated. Whether the RS485M 2D engine
writes an unsnooped-GART destination coherently under the in-stream
flush and wait remains the silicon hypothesis this control tests.

## The observable

The first run used one draw color and one sentinel, so a byte-order error and
a partial write were not separable and a single value could not locate a
failure inside the target.

The target is a 64x64 linear ARGB8888 surface: width 64 pixels, render
height 64 rows, pitch 64 pixels = 256 bytes per row, target byte range
0..16383. Row 64 is the canary row, byte range 16384..16639. The minimum
allocation covering target plus canary is 16640 bytes. If the control
allocates 65536 bytes, allocation size and oracle-covered range are separate
facts: allocated bytes 65536, oracle-covered bytes 0..16639, unused tail
16640..65535. The unused tail is not itself a canary; this control scans
only byte range 0..16639, the oracle-covered region. The canary is the
row-64 range 16384..16639 alone.

Pixel A sits at (x=16, y=16), byte offset 16*256 + 16*4 = 4160. Pixel B sits
at (x=21, y=47), byte offset 47*256 + 21*4 = 12116. Pixel B sits off the
x = y diagonal, so a transposed row/column address computation moves it
and the readback reports the miss; a diagonal pair would leave
transposition invisible.

The control writes two distinct values at two distinct locations:

```text
pixel A, byte offset 4160    0x11223344
pixel B, byte offset 12116   0xa1b2c3d4
every 32-bit target pixel whose byte offset is in 0..16383,
  except pixel A and pixel B = 0xa5a5a5a5 sentinel
every 32-bit canary pixel whose byte offset is in 16384..16639
  = 0xa5a5a5a5 sentinel
```

Both values are asymmetric across all four byte lanes and differ from each
other in every lane, so the readback separates the cases a single value fuses:

- both values at both locations: the transport carries device writes, in the
  expected byte order, at the expected addresses;
- both values present with lanes permuted: the transport carries writes and
  the host and device disagree on byte order, which also reinterprets the
  first run's uniform sentinel;
- one value present: candidate classes include address arithmetic, extent,
  or partial execution; the readback alone does not pick one;
- values present at transposed locations: candidate classes include pitch
  and row-stride error; the readback alone does not pick one;
- neither expected value present, whatever else the target carries:
  CONTROL FAILED / INCONCLUSIVE under the Failure classification above;
  the experiment establishes neither positive write visibility nor a
  pipeline mechanism; stop and retain the bundle, and apply the
  containment rule below when the canary range is disturbed;
- canary range disturbed: the write passed the render extent into byte range
  16384..16639, which is a containment failure and stops the sequence.

## Gates before execution

The control executes on a sacrificial boot under `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`,
the native R3V arming gate, after every factor in that gate's conjunction
reports clear: the exact stream digest, the qualified kernel, the loaded
kernel module, and a fresh evidence directory. `R300_TRACE_HAZARD_ACCEPTED`
arms a different submission path and does not arm this control.
`r3v_native_submit_hazard_accepted` and `r3v_native_arming_evaluate` accept
only the exact string `1`; unset, empty, `0`, padded, and every other value
keep native submission closed.

1. `replay_r300_cs_track` accepts the control's stream against the control's
   own bundle, and every parser-invalid control against that stream rejects.
2. The exact submit object the run will issue is retained and replayed
   byte-identically offline before the run, the way
   `run_submit_object_replay.sh` replays the successor's.
3. The stream is byte-deterministic across two emissions, and its own digest
   is declared to the arming gate as a required factor. Neither the
   successor's digest nor the reference cell's digest authorizes the
   control: each stream is a distinct object, and each declares its own
   digest.
4. `radeon_deployment_preflight.sh` reports `verdict=CLEAR` on the boot that
   will carry the run, with the RS485M present, `lockup_timeout=0`, no fence
   waiter, no lockup or reset or park signature for the boot, a fresh evidence
   directory, and an off-box log path.
5. The one-shot `attempt.token` is written durably before the ioctl, so a
   host that dies during the run cannot present as an unattempted one.

## Containment

The control runs on a sacrificial boot, not on the boot reserved for the
successor: a run that wedges the ring or parks the GPU changes the state the
successor would inherit, and the successor's own evidence requires a cold boot
with no intervening 3D client.

Recovery follows one sequence. `radeon_drm_vk_completion_await` makes at most
three `DRM_RADEON_GEM_WAIT_IDLE` calls; the canonical kernel bounds each call
at `30 * HZ` and permits signal interruption. After the final call returns:
do not resubmit; capture off-box logs; if the host is responsive, reboot; if
the host is unresponsive, physical power cycle. `lockup_timeout=0` disables
the automatic Radeon lockup-reset path and leaves the per-ioctl reservation
wait bound intact. The record comes from kernel stacks, the journal, and
netconsole.

## Artifact identity

The emitter (`src/amd/r300/common/r300_direct_write.c`), the oracle, and
the `r300_direct_write_manifest` writer exist and are test-covered: the
writer pins the stream dword count, packet/register contract, relocation
site, BO role and size, and BLAKE3 digest, and the
`r300-direct-write-manifest-integration` test proves the published
artifacts against each other. The register contract's kernel-source
derivation lives in `docs/hardware/r300-direct-write-2d-fill-authority.md`.
This document quotes no digest: the manifest the run's own build writes
is the digest authority, and the arming gate takes the digest from that
manifest, so a stale doc constant can never authorize a stream.

The host-side chain past the manifest stands proven: the native
recorder installs the cell (`r3v_native_record_direct_write`), the
queue retains the exact submit object, the retained object replays
through the kernel CS parser (`r3v-native-direct-write-submit-object-replay`),
the arming gate arms under the control's own digest and refuses the
triangle digest and every single-fact mutation
(`r3v-native-direct-write-arming-positive`,
`r3v-native-direct-write-arming-runner`), one digest names the cell
across every producing and retaining authority
(`r3v-native-direct-write-authority-parity`). Dual-host dual-profile
qualification rides the steinmarder-r300
`direct_write_control_dual_host_qualification_*` bundle series; a live
run requires a bundle at the exact source SHA it will execute. The
live-run executable is `r3v_native_attended_direct_write` under
`docs/hardware/r3v-native-direct-write-attended-procedure.md`.

Execution remains BLOCKED on the separate explicit authorization every
live submission requires, and on that procedure's operational
preconditions: the sacrificial boot, the attended operator, and the
off-box log path.

## What the control does not decide

A control that lands its bytes proves the transport carries device writes for
its own command stream. It does not prove the 3D pipeline writes color, does
not prove the successor's first-draw contract is complete, and does not
authorize the successor run. It removes one explanation and leaves the rest.
