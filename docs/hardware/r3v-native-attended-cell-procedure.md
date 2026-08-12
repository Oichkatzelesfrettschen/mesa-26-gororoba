# R3V native attended-cell procedure

This document is the procedure for the second attended native
triangle-cell submission from the native R3V ICD, a physically
attended run on the RS482 host under a separate explicit
authorization. The ordinal counts attended triangle-cell runs, not
native `DRM_RADEON_CS` submissions globally; other native controls
(the direct-write control among them) carry their own submissions
outside this count. It states what
must hold before the run, what it records, which observations falsify
it, and how it rolls back. Executing it requires that authorization;
reading and rehearsing it does not.

## Boundary this procedure crosses

A historical attended run already carries the first live
`DRM_RADEON_CS` submission from the native lane. That run submitted the
bare, inherited-state cell -- a recording with no first-draw state
contract prefix. The radeon kernel driver accepted the submission and
the completion retired clean: no CS validation error, no reset, no
lockup line. The color target showed no modification: the oracle's
`executed` verdict was false, so the falsifier fired on that run. The
historical cause remains underdetermined: the run did not retain the
predecessor values of `US_OUT_FMT_0`, `COLOR_CHANNEL_MASK`, and
`SC_SCREENDOOR`, so which register, if any, produced that outcome is
unidentified. A later RS482 silicon matrix (steinmarder-r300 bundle
`rs482-first-draw-color-write-gate-discrimination`) proved that each
of the three, left at a killing value, is independently sufficient to
reproduce the same observation, byte-identical to the sentinel fill,
and the submitted cell established none of them.
The successor establishes all three, which removes the
predecessor-state dependence rather than settling the historical
mechanism. A ring wedge occurred later in the same boot; the run that
produced it, and whether it traces to the attended submission or to
unrelated same-boot activity, is unresolved.

The current run submits the 234-dword self-contained cell that opens
with the first-draw state contract
(`src/amd/r300/common/r300_first_draw_state.c`), establishing every
register the draw depends on before the draw itself, rather than
inheriting GPU state from whatever ran before it. This is a separate
authorization from the historical run, and its intended outcome is the
first correct-pixel witness: a color target that actually carries the
drawn triangle rather than its sentinel fill.

`docs/hardware/r3v-implementation-boundaries.md` holds the ordered
development list this run completes; it is step 9 there.

## Preconditions

The run proceeds only when all of the following hold.

- The cell under test is the fixed TCL-bypass triangle, its fragment
  program compiled by `r300_tcl_bypass_fs_tool` and its checked-in block
  proven to regenerate by `r300-tcl-bypass-fs-block-regeneration`.
- The live submitting runner is the Meson target
  `r3v_native_attended_cell`, invoked as
  `r3v_native_attended_cell <evidence-dir>`. It statically links the
  native implementation, so `submit_manifest.json` identifies this
  executable as the issuer. The drm-shim harness
  `r3v_native_triangle_cell_harness` is a host-model test and is not the
  live submitter.
- The executable comes from the release conformance profile, not the
  assertions-live debug profile. Build that profile through the checked-in
  build entry point:

  ```sh
  make -C build-infra rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache
  ```

  Set `R3V_BUILD_DIR` to the resulting
  `mesa-4_r300_full_release_x86_64v1-clang22-distcc-cache` build directory and
  set `R3V_NATIVE_MANIFEST_DIR` to a fresh evidence directory before retaining
  the Meson options:

  ```sh
  meson configure "$R3V_BUILD_DIR" \
    >"$R3V_NATIVE_MANIFEST_DIR/build-options.txt"
  grep -Eq 'buildtype.*release' \
    "$R3V_NATIVE_MANIFEST_DIR/build-options.txt"
  grep -Eq 'b_ndebug.*true' \
    "$R3V_NATIVE_MANIFEST_DIR/build-options.txt"
  sha256sum \
    "$R3V_BUILD_DIR/src/amd/r300/vulkan/r3v_native_attended_cell" \
    "$R3V_BUILD_DIR/src/amd/r300/vulkan/libvulkan_r3v_native.so" \
    >"$R3V_NATIVE_MANIFEST_DIR/build-artifacts.sha256"
  ```

  A debugoptimized or debug option set blocks the run. The retained options
  and artifact digests bind the live executable and native DSO to the release
  build that the preflight suite qualified.
- `r300-tcl-bypass-offline-replay` and
  `r3v-native-submit-object-replay` pass on the RS482 host itself. The
  replay binary is a build output, not an operator-supplied binary:
  `build-infra/r3v/build_kernel_replay.py` compiles
  `replay_r300_cs_track` from the pinned kernel tree the running kernel
  was built from, runs that tree's own
  `r300_cs_grammar_correspondence` fidelity gate against
  `r300_packet0_check`, and writes a provenance record binding the
  output ELF to the exact kernel commit, per-file source hashes,
  compiler identity, and compile argv.
- The four-suite preflight runs from that same release build without a
  rebuild, and its complete output becomes the recorded vector:

  ```sh
  R3V_SUITE_REPORT="$R3V_NATIVE_MANIFEST_DIR/preflight-suites.txt"
  meson test -C "$R3V_BUILD_DIR" --no-rebuild --print-errorlogs \
    --suite r300 --suite r3v --suite radeon-drm-vk --suite drm-shim \
    >"$R3V_SUITE_REPORT" 2>&1
  cat "$R3V_SUITE_REPORT"
  ```

  A nonzero suite status blocks the run. The qualification inventory gate
  then confirms the required test set is complete and the replay tool's
  provenance is current; retain its complete output as well:

  ```sh
  R3V_QUALIFICATION_REPORT="$R3V_NATIVE_MANIFEST_DIR/qualification-inventory.txt"
  R3V_CS_TRACK_REPLAY_TOOL=<path to replay_r300_cs_track> \
  R3V_CS_TRACK_CONTROLS=<path to its controls fixture> \
  R3V_CS_TRACK_REPLAY_PROVENANCE=<path to the provenance record> \
  python3 src/amd/r300/vulkan/tests/r3v_qualification_inventory.py \
    "$R3V_BUILD_DIR" --qualification \
    >"$R3V_QUALIFICATION_REPORT" 2>&1
  cat "$R3V_QUALIFICATION_REPORT"
  ```

  The inventory verifies the replay tool against its provenance record
  -- the correspondence gate passed and the ELF's SHA-256 matches the
  recorded hash -- rather than trusting a binary handed to it by name
  alone. Any drm-shim residue matches the calibrated signature.
- The RS482 recovery stack is registered before the hazard gate opens. The
  Radeon module is the deployed `radeon-unified-dkms` build, the SB600 TCO
  module is registered as `sp5100_tco`, the watchdog device and
  `sb600-guard` are present, and the wedge-recovery sysctls are loaded. Read
  and retain these facts before setting
  `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`:

  ```sh
  R3V_RECOVERY_REPORT="$R3V_NATIVE_MANIFEST_DIR/recovery-preflight.txt"
  {
    printf 'kernel.release='
    uname -r
    printf 'radeon.package='
    pacman -Q radeon-unified-dkms
    printf 'radeon.policy_package='
    pacman -Q radeon-rs482-policy
    printf 'watchdog.package='
    pacman -Q sp5100-tco-ioapic-dkms
    printf 'recovery.package='
    pacman -Q vostro1000-wedge-recovery
    printf 'radeon.srcversion='
    cat /sys/module/radeon/srcversion
    printf 'radeon.modinfo_srcversion='
    modinfo -F srcversion radeon
    printf 'radeon.lockup_timeout='
    cat /sys/module/radeon/parameters/lockup_timeout
    printf 'sp5100_tco.loaded='
    test -d /sys/module/sp5100_tco && echo yes || echo no
    printf 'watchdog.device='
    test -e /dev/watchdog0 && echo /dev/watchdog0 || \
      test -e /dev/watchdog && echo /dev/watchdog || echo missing
    printf 'sb600_guard='
    command -v sb600-guard || true
    for setting in kernel.panic kernel.panic_on_oops \
      kernel.nmi_watchdog kernel.hardlockup_panic \
      kernel.softlockup_panic kernel.hung_task_panic \
      kernel.hung_task_timeout_secs; do
      printf '%s=' "$setting"
      sysctl -n "$setting"
    done
  } >"$R3V_RECOVERY_REPORT" 2>&1
  cat "$R3V_RECOVERY_REPORT"
  pacman -Q radeon-unified-dkms >/dev/null
  pacman -Q radeon-rs482-policy >/dev/null
  pacman -Q sp5100-tco-ioapic-dkms >/dev/null
  pacman -Q vostro1000-wedge-recovery >/dev/null
  test -d /sys/module/radeon
  test -e /sys/module/radeon/srcversion
  test "$(cat /sys/module/radeon/srcversion)" = "$(modinfo -F srcversion radeon)"
  test -e /sys/module/radeon/parameters/lockup_timeout
  test "$(cat /sys/module/radeon/parameters/lockup_timeout)" = 0
  test -d /sys/module/sp5100_tco
  test -e /dev/watchdog0 || test -e /dev/watchdog
  test -x "$(command -v sb600-guard)"
  test "$(sysctl -n kernel.panic)" = 10
  test "$(sysctl -n kernel.panic_on_oops)" = 1
  test "$(sysctl -n kernel.nmi_watchdog)" = 1
  test "$(sysctl -n kernel.hardlockup_panic)" = 1
  test "$(sysctl -n kernel.softlockup_panic)" = 1
  test "$(sysctl -n kernel.hung_task_panic)" = 1
  test "$(sysctl -n kernel.hung_task_timeout_secs)" = 120
  ```

  A loaded module, watchdog node, and sysctl posture prove deployment and
  registration only; they do not prove GPU reset or silicon recovery.
  `sb600-guard` remains an attended recovery backstop and is not invoked by
  this preflight. A missing identity, a nonzero `radeon.lockup_timeout`, or a
  missing watchdog/recovery setting blocks the run.
- `r3v_native_arming_runner` reports `armed` for the exact evidence
  directory the run will use.
- The host has an off-box log path (netconsole or serial), because a
  hard lockup takes the on-box journal with it.
- The operator is physically at the machine and able to power-cycle it.

## Arming

The submission gate is a conjunction; every factor is declared by the
operator and matched by the driver, and no factor has a bypass.

1. Build the cell and read its digest from the runner's report. This
   procedure submits the maximum-extent 64x64 reference cell, the one
   target the checked-in attended submitter records and retains; the
   default report names exactly that cell.  The runner's
   `--extent <w> <h>` option is a no-submit digest and IB-generation
   facility for the host-model extent family -- it names the IB the
   recorder would install for a non-maximum target, but no checked-in
   submitter records that target, so a non-maximum digest arms nothing.
   Non-maximum silicon execution is blocked on a future parameterized
   public-route runner with extent-aware retention; when that runner
   exists, its oracle is `r300_tcl_bypass_triangle_extent_oracle` at
   the declared extent, whose analytic triangle is the NDC reference
   payload through the viewport transform, whose canary covers the
   sub-pitch padding band and the rows past the render extent, and
   whose passes require positive sample counts, so an extent too small
   to witness fails closed rather than reporting a vacuous pass.
2. Declare the authorization:
   - `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`
   - `R3V_NATIVE_AUTHORIZED_IB_BLAKE3=<digest from step 1>`
   - `R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE=<uname -r>`
   - `R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION=<contents of
     /sys/module/radeon/srcversion>`
   - `R3V_NATIVE_MANIFEST_DIR=<fresh directory>`
3. Run `r3v_native_arming_runner <evidence-dir>`, retain its complete
   report before submission, and require the `armed` verdict:

   ```sh
   R3V_NATIVE_ARMING_REPORT="$R3V_NATIVE_MANIFEST_DIR/arming-report.txt"
   if r3v_native_arming_runner "$R3V_NATIVE_MANIFEST_DIR" \
     >"$R3V_NATIVE_ARMING_REPORT" 2>&1; then
     arming_status=0
   else
     arming_status=$?
   fi
   cat "$R3V_NATIVE_ARMING_REPORT"
   test "$arming_status" -eq 0
   grep -F 'verdict: armed' "$R3V_NATIVE_ARMING_REPORT"
   ```

   The runner creates no device and issues no ioctl. A refusal retains its
   report and keeps the submission closed.
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

- The ioctl returns nonzero: classify the live failure from the errno,
  kernel trace, and failing stage before naming its cause. A CS packet,
  relocation, or parser signature supports a parser-divergence RCA against
  the offline replay. A transport, BO allocation or pinning, address
  mapping, scheduling, or completion failure is a separate live-path
  finding; the offline parser does not exercise those paths. Retain the
  numeric errno and string, stage, dmesg delta, and submit object, and open
  the corresponding RCA.
- `GEM_WAIT_IDLE` reaches its bound: the submission did not retire.
  Treat as a hang; do not resubmit.
- The oracle reports `executed` false: the command processor wrote
  nothing, so the cell is inert rather than correct.
- The oracle reports `interior_pass` false while `executed` is true: the
  draw reached memory with the wrong color, coverage, or byte order.
  An interior mismatch that is a channel permutation of `0xff00ff00`
  is a color-order finding, not a raster failure. `0xff00ff00` is
  symmetric under red/blue exchange and under alpha/green exchange, so
  a passing run witnesses that the alpha/green channel pair carries
  0xff and the red/blue pair carries zero, and separates neither pair
  internally; full channel-order identity requires a later asymmetric
  3D color witness.
- The oracle reports `exterior_pass` false while `canary_pass` remains true:
  an in-extent sample outside the analytic triangle changed from its
  sentinel. This is a coverage, scissor, viewport, or triangle-setup finding
  and does not by itself show an out-of-extent write.
- The oracle reports `canary_pass` false: a pixel in the sub-pitch padding
  band or a row past the 64-row render extent changed from its sentinel.
  This is the extent, pitch, or color-target binding containment finding and
  is the dangerous outcome because the GPU wrote memory the cell did not
  describe.
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

- `ib.bin`, `relocs.bin`, `manifest.json` -- the semantic cell, the IB
  and relocation list before the completion reference folds in;
- `submit_relocs.bin`, `submit_manifest.json` -- the exact submit
  object: the three CS chunk IDs and dword lengths, the CS flags, the
  BO table, the completion BO, and the relocation list with the
  completion reference the semantic cell does not carry;
- `attempt.token` -- the declared IB BLAKE3 digest and the wall-clock
  instant the directory was disarmed, written by exclusive creation and
  fsynced durably before the ioctl, so a power failure past the ioctl
  cannot leave the directory apparently unused;
- `build-options.txt`, `build-artifacts.sha256`, `preflight-suites.txt`,
  `qualification-inventory.txt`, `recovery-preflight.txt`, and
  `arming-report.txt`, all captured before the ioctl;
- the arming runner's full report, including every declared and observed
  factor;
- the color target's bytes and the oracle verdict;
- `dmesg` before and after, captured off-box;
- kernel release, radeon module srcversion, Mesa commit, and the
  executable or loaded native DSO identity that issued the ioctl.

A run whose retained record is incomplete produces no claim. The driver
enforces the first half of that rule itself: evidence retention failure
refuses the submission before the ioctl.
