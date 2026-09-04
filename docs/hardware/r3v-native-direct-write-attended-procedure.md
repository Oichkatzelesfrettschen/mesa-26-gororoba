# R3V native direct-write attended procedure

This document is the procedure for the attended direct-write control
run: one live `DRM_RADEON_CS` submission of the fixed 2D solid-fill
cell from the native R3V ICD, physically attended on the RS485M host,
on a sacrificial boot, under a separate explicit authorization. The
control's mechanism, register authority, observable, and failure
classification live in `docs/hardware/r300-direct-write-control.md`
and `docs/hardware/r300-direct-write-2d-fill-authority.md`; this
document carries only the live-run procedure. Executing it requires
that authorization; reading and rehearsing it does not.

The run executable is `r3v_native_attended_direct_write`. The triangle
cell's attended runner is a separate executable under its own
procedure (`docs/hardware/r3v-native-attended-cell-procedure.md`);
each cell's digest authorizes only its own stream, and the parity test
`r3v-native-direct-write-authority-parity` proves each runner records
only its own cell.

## Boot class

The run consumes a sacrificial boot: a boot whose post-run GPU state
carries no evidence value, separate from any boot reserved for the
successor triangle witness. Between the boot and the run, no GL, EGL,
Vulkan, or other 3D client touches the GPU: the control's meaning
rests on the GPU state the boot left, and an intervening client
replaces that state with its own.

## Identity bindings

The run binds to one frozen identity chain, each element captured in
the evidence directory before the submission:

- Mesa source SHA: the clean worktree SHA the native library was built
  from, with `git status --porcelain=v2` empty at build time.
- Native issuer identity: the queue resolves the regular executable or
  loaded native DSO containing the identity helper, verifies its
  executable mappings against that file, and records its mapped path and
  BLAKE3 in `submit_manifest.json`. The attended direct-write runner is
  the static executable `r3v_native_attended_direct_write`, so the
  manifest names that executable rather than assuming a DSO name.
- Direct-write IB digest: the `ib_blake3` reported by
  `r3v_native_direct_write_arming_runner` on this build, which the
  authority-parity test proves equal to the manifest writer's and the
  queue-retained stream's.
- Kernel release (`uname -r`) and radeon module srcversion
  (`/sys/module/radeon/srcversion`).
- Boot ID (`/proc/sys/kernel/random/boot_id`), so the retained record
  names the exact boot the run consumed.
- Off-box log destination (netconsole or serial), verified live before
  arming.
- Fresh evidence directory, holding no `attempt.token`.

The dual-host qualification bundle for the control
(steinmarder-r300, `direct_write_control_dual_host_qualification_*`)
names the qualified Mesa SHA and the deployed kernel identity
(driver tree `d57a22ad5356`, srcversion `A7F72BE636B52D7EED42415`).
A run whose captured identities differ from a current qualification
bundle requalifies before arming: the qualification is what binds the
replay verdicts to the running kernel.

## Preconditions

The run proceeds only when all of the following hold on the RS485M host
itself:

- The four suites (`r300`, `r3v`, `radeon-drm-vk`, `drm-shim`)
  reproduce their qualified vector, and the qualification inventory
  reports the full required set with the replay tool verified against
  its provenance record, as in the triangle procedure's precondition
  block.
- `r3v-native-direct-write-submit-object-replay` passes with the
  host-built `replay_r300_cs_track`: the exact retained submit object
  replays `ACCEPT-NO-DRAW`, and the malformed controls reject.
- `radeon_deployment_preflight.sh` reports `verdict=CLEAR` on the boot
  that will carry the run.
- `r3v_native_direct_write_arming_runner <evidence-dir>` reports
  `armed` for the exact evidence directory the run will use.
- The operator is physically at the machine and able to power-cycle
  it.

## Arming

The submission gate is the same conjunction the triangle run uses; the
declared digest is the control's own.

1. Run `r3v_native_direct_write_arming_runner <evidence-dir>` and read
   `ib_blake3=` from its report. The runner creates no Vulkan device,
   opens no DRM fd, and issues no ioctl.
2. Declare the authorization:
   - `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
   - `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<ib_blake3 from step 1>`
   - `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
   - `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<contents of
     /sys/module/radeon/srcversion>`
   - `R3V_NATIVE_MANIFEST_DIR=<the same fresh directory>`
3. Re-run the arming runner and require the `armed` verdict.
4. Only then run, with no `LD_PRELOAD` in the environment:

   ```sh
   r3v_native_attended_direct_write <evidence-dir>
   ```

The runner refuses a nonempty `LD_PRELOAD` outright, enumerates PCI
`1002:5974` alone, allocates only the 65536-byte GTT color target,
records only the direct-write cell, submits once, and never retries.
Reaching the ioctl writes `attempt.token` by exclusive creation, so
the directory admits one attempt; a second run needs a new directory
and a fresh authorization decision.

## Predictions

Recorded before the run; the observation stands as made.

- The `DRM_RADEON_CS` ioctl returns 0 and the completion wait retires
  within its bound.
- The oracle reports `executed`, `value_a_pass`, `value_b_pass`,
  `sentinel_pass`, and `canary_pass` all true: pixel A (16,16) carries
  `0x11223344`, pixel B (21,47) carries `0xa1b2c3d4`, every other
  target and canary pixel still carries `0xa5a5a5a5`, and the
  allocation tail past the canary row is undisturbed.
- The runner prints `verdict: CONTROL_PASS` and exits 0.
- `dmesg` gains no radeon CS validation error, reset, or lockup line.

## Outcome classes

The runner prints exactly one verdict and exits nonzero for every
class except `CONTROL_PASS`:

- `CONTROL_PASS`: all five oracle checks true and the submission
  retired clean.
- `CONTROL_FAILED_INCONCLUSIVE`: the submission retired but the
  expected values did not land as predicted. Under the control
  document's failure classification this is INCONCLUSIVE / CONTROL
  FAILED: the positive control failed, so the run establishes neither
  write visibility nor a transport defect, and no pipeline hypothesis
  follows.
- `CONTAINMENT_FAILURE`: the canary row or the allocation tail is
  disturbed. The GPU wrote memory the cell did not describe; this
  outcome stops the sequence regardless of what else passed.
- `SUBMISSION_REFUSED`: the run refused before or at the submission
  boundary -- interposer present, directory disagreement, wrong chip,
  arming refusal, or a non-device-lost submission error.
- `COMPLETION_FAILURE`: the submission entered the kernel and the
  bounded completion wait did not retire it. Treat as a hang; do not
  resubmit.
- `RETENTION_FAILURE`: the target could not be mapped or durably
  retained; the run produces no claim.

## Rollback

- One attempt, no resubmission, whatever the outcome.
- A wedged GPU with a responsive host: capture `dmesg` off-box, then
  reboot cleanly. `lockup_timeout=0` means no automatic reset runs,
  and no unwinder attaches to a task blocked in
  `radeon_fence_default_wait`.
- An unresponsive host: physical power cycle. The off-box log is the
  record that survives.
- The boot is sacrificial by declaration, so the post-run state needs
  no preservation beyond the retained record.

## Retained record

The run keeps, in the evidence directory and mirrored into the r300
evidence repository:

- `ib.bin`, `relocs.bin`, `manifest.json`, `submit_relocs.bin`,
  `submit_manifest.json`, `attempt.token` -- written by the queue
  before the ioctl, as in the triangle procedure;
- `color_target.bin` -- the complete 65536-byte allocation, written
  through the durable evidence writer before the verdict prints;
- `direct_write_outcome.json` -- the verdict, submit result, all five
  oracle fields, the tail count, and the observed pixel values;
- the arming runner's full report, `dmesg` before and after captured
  off-box, and the identity bindings above.

A run whose retained record is incomplete produces no claim; the
runner enforces this by classifying retention failure as its own
outcome before any oracle verdict prints.
