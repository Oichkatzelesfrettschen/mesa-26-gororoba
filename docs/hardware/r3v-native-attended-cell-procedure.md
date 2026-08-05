# R3V native attended-cell procedure

The first live `DRM_RADEON_CS` from the native R3V ICD is a physically
attended run on the RS482 host under a separate explicit authorization.
This document is the procedure that run follows: what must hold before
it, what it records, which observations falsify it, and how it rolls
back. Executing it requires that authorization; reading and rehearsing
it does not.

## Boundary this procedure crosses

Every result the native lane holds today is pre-hardware: host unit
tests, build and link separation, no-submit PM4 with retained evidence,
the drm-shim host model, and offline replay through kernel decision code
compiled from the RS482 host's own kernel source. None of it is silicon
execution. The attended run is the first submission the radeon kernel
driver validates and the command processor executes, so it is the first
opportunity for a GPU hang, a bus fault, or a host lockup.

`docs/hardware/r3v-implementation-boundaries.md` holds the ordered
development list this run completes; it is step 9 there.

## Preconditions

The run proceeds only when all of the following hold.

- The cell under test is the fixed TCL-bypass triangle, its fragment
  program compiled by `r300_tcl_bypass_fs_tool` and its checked-in block
  proven to regenerate by `r300-tcl-bypass-fs-block-regeneration`.
- `r300-tcl-bypass-offline-replay` and
  `r3v-native-submit-object-replay` pass on the RS482 host itself with
  `R3V_KERNEL_REPLAY_TOOL` pointing at a replay binary built from the
  kernel source tree the running kernel was built from.
- The four-suite run on the RS482 host reproduces its recorded vector,
  and any drm-shim residue matches the calibrated signature.
- `r3v_native_arming_runner` reports `armed` for the exact evidence
  directory the run will use.
- The host has an off-box log path (netconsole or serial), because a
  hard lockup takes the on-box journal with it.
- The operator is physically at the machine and able to power-cycle it.

## Arming

The submission gate is a conjunction; every factor is declared by the
operator and matched by the driver, and no factor has a bypass.

1. Build the cell and read its digest from the runner's report.
2. Declare the authorization:
   - `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
   - `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest from step 1>`
   - `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
   - `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<contents of
     /sys/module/radeon/srcversion>`
   - `R3V_NATIVE_MANIFEST_DIR=<fresh directory>`
3. Run `r3v_native_arming_runner <evidence-dir>` and require the
   `armed` verdict. The runner creates no device and issues no ioctl.
4. The submitting run may proceed only after step 3 reports `armed`.

Reaching the ioctl writes `attempt.token` into the evidence directory by
exclusive creation, so the directory admits one attempt. A second run
needs a new directory and a fresh authorization decision.

The chip factor admits PCI `1002:5974` alone. RS485-marketed `1002:5975`
is a supported r3v identity but not an authorized attended-run chip,
because the falsifiers below were written against RS482 behavior.

## Predictions

Recorded before the run; the observation stands as made.

- The `DRM_RADEON_CS` ioctl returns 0.
- `GEM_WAIT_IDLE` on the completion buffer returns within the bounded
  retry window rather than escalating to device loss.
- The color target's oracle verdict is `executed`, `interior_pass`,
  `exterior_pass`, and `canary_pass` all true: interior samples carry
  `0xff00ff00`, exterior samples and the canary row past the 64-row
  render extent still carry `0xa5a5a5a5`.
- `dmesg` gains no radeon CS validation error, no GPU reset, and no
  lockup line across the run.

## Falsifiers

Any of these ends the run; the observation is the finding, and the
prediction is not revised after the fact.

- The ioctl returns nonzero: the kernel CS parser rejected an object the
  offline replay accepted, which means the offline decision code and the
  running kernel disagree. Capture the errno and the retained submit
  object; open an RCA on the divergence.
- `GEM_WAIT_IDLE` reaches its bound: the submission did not retire.
  Treat as a hang; do not resubmit.
- The oracle reports `executed` false: the command processor wrote
  nothing, so the cell is inert rather than correct.
- The oracle reports `interior_pass` false while `executed` is true: the
  draw reached memory with the wrong color, coverage, or byte order.
  The draw-color byte order is unproven until this run, so an interior
  mismatch that is a channel permutation of `0xff00ff00` is a color-order
  finding, not a raster failure.
- The oracle reports `exterior_pass` or `canary_pass` false: the write
  landed outside the intended extent, which implicates the pitch word or
  the color-target binding and is the most dangerous outcome because it
  means the GPU wrote memory the cell did not describe.
- `dmesg` gains a radeon CS validation error, a reset, or a lockup line.
- The host stops responding.

## Rollback

- Do not resubmit after any falsifier. The evidence directory is
  disarmed by its attempt token, which is the intended state.
- A wedged GPU without a wedged host: capture `dmesg` off-box, then
  reboot cleanly.
- A wedged host: power-cycle. The off-box log is the only record that
  survives, which is why it is a precondition.
- The native ICD is a separate library from the Gallium-backed one and
  is selected only by its own manifest, so removing
  `r3v_native_icd.<cpu>.json` from the loader's search path restores the
  host to its pre-run driver configuration without rebuilding.

## Retained record

The run keeps, in the evidence directory and mirrored into the r300
evidence repository:

- `ib.bin`, `relocs.bin`, `manifest.json` -- the semantic cell;
- `submit_relocs.bin`, `submit_manifest.json` -- the exact submit
  object, whose relocation list carries the completion reference the
  semantic cell does not;
- `attempt.token` -- proof the directory was armed once;
- the arming runner's full report, including every declared and observed
  factor;
- the color target's bytes and the oracle verdict;
- `dmesg` before and after, captured off-box;
- kernel release, radeon module srcversion, Mesa commit, and the ELF
  identity of the native library that ran.

A run whose retained record is incomplete produces no claim. The driver
enforces the first half of that rule itself: evidence retention failure
refuses the submission before the ioctl.
