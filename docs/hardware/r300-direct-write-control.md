# RS482 direct native write control

## What this control decides

The successor cell reaches silicon through the native transport, retires, and
is expected to leave a triangle in its color target. The first attended run
reached silicon, retired clean, and left the target byte-identical to its
sentinel fill. Two models explain that outcome and the cell alone cannot
separate them: the 3D pipeline produced no color write, or the transport
itself -- allocation, mapping, cache publication, relocation, submission,
completion, readback -- never carried a device write back to the host at all.

This control writes the target through the same transport with the 3D
pipeline removed from the path. A control that lands its bytes proves the
transport carries device writes and leaves the pipeline as the only remaining
explanation. A control that does not land its bytes moves the fault to the
transport and makes every pipeline hypothesis premature.

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
  builder, under the same exact-value hazard gate.
- Completion: the same `DRM_RADEON_GEM_WAIT_IDLE`, with the same timeout.
- Readback invalidation: the same invalidate before the host reads.

The command stream is what differs. The control emits the first-draw state
contract and then a write whose effect does not depend on rasterization,
fragment shading, or the color-write gates the first run implicated.

## The observable

The first run used one draw color and one sentinel, so a byte-order error and
a partial write were not separable and a single value could not locate a
failure inside the target.

The control writes two distinct values at two distinct locations:

```text
pixel A at (16, 16)   0x11223344
pixel B at (47, 47)   0xa1b2c3d4
every other in-target pixel   0xa5a5a5a5 sentinel
rows 64 and beyond            0xa5a5a5a5 sentinel canary
```

Both values are asymmetric across all four byte lanes and differ from each
other in every lane, so the readback separates the cases a single value fuses:

- both values at both locations: the transport carries device writes, in the
  expected byte order, at the expected addresses;
- both values present with lanes permuted: the transport carries writes and
  the host and device disagree on byte order, which also reinterprets the
  first run's uniform sentinel;
- one value present: the write reached the target and the address arithmetic
  or the extent is wrong;
- values present at transposed locations: pitch or row-stride error;
- neither value present, sentinel intact: the transport carries no device
  write, and the first run's result is explained without reference to the
  pipeline;
- canary rows disturbed: the write passed the render extent, which is a
  containment failure and stops the sequence.

## Gates before execution

The control executes on a sacrificial boot under its own authorization, after
every gate below reports clear.

1. `replay_r300_cs_track` accepts the control's stream against the control's
   own bundle, and every parser-invalid control against that stream rejects.
2. The exact submit object the run will issue is retained and replayed
   byte-identically offline before the run, the way
   `run_submit_object_replay.sh` replays the successor's.
3. The stream is byte-deterministic across two emissions and its digest is
   declared to the arming gate. The successor's digest does not authorize the
   control: they are different streams and each is declared on its own.
4. `radeon_deployment_preflight.sh` reports `verdict=CLEAR` on the boot that
   will carry the run, with the RS482 present, `lockup_timeout=0`, no fence
   waiter, no lockup or reset or park signature for the boot, a fresh evidence
   directory, and an off-box log path.
5. The one-shot `attempt.token` is written durably before the ioctl, so a
   host that dies during the run cannot present as an unattempted one.

## Containment

The control runs on a sacrificial boot, not on the boot reserved for the
successor: a run that wedges the ring or parks the GPU changes the state the
successor would inherit, and the successor's own evidence requires a cold boot
with no intervening 3D client.

Recovery is a physical power cycle. `lockup_timeout=0` means no automatic
reset runs, so a wedged ring stays wedged, and the parked latch is unreachable
along that path. No userspace unwinder attaches to a task blocked in
`radeon_fence_default_wait`: the wait is uninterruptible, and attaching to it
has already cost a session. The record comes from kernel stacks, the journal,
and netconsole.

## What the control does not decide

A control that lands its bytes proves the transport carries device writes for
its own command stream. It does not prove the 3D pipeline writes color, does
not prove the successor's first-draw contract is complete, and does not
authorize the successor run. It removes one explanation and leaves the rest.
