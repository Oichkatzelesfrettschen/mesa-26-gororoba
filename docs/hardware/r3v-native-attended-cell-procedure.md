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
  build entry point. Both source paths name distinct detached worktrees at the
  same intended commit, and the build root is dedicated to this run:

  ```sh
  MESA_CONTROL_ROOT=/path/to/detached-control
  MESA_SOURCE_ROOT=/path/to/detached-source
  MESA_BUILD_ROOT=/path/to/dedicated-build-root
  make -C "$MESA_CONTROL_ROOT/build-infra" \
    rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache \
    REPRODUCIBLE_RUN=1 TOPSRC="$MESA_SOURCE_ROOT" \
    BUILD_ROOT="$MESA_BUILD_ROOT"
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
    "$R3V_BUILD_DIR/src/amd/r300/vulkan/libvulkan_r3v.so" \
    >"$R3V_NATIVE_MANIFEST_DIR/build-artifacts.sha256"
  ```

  A debugoptimized or debug option set blocks the run. The retained options
  and artifact digests bind the live executable and native DSO to the release
  build that the preflight suite qualified.
- `r300-tcl-bypass-offline-replay` and
  `r3v-native-submit-object-replay` pass on the RS482 host itself. The
  single-IB runners use the build output
  `scripts/replay_r300_tcl_bypass_ib` through `R3V_KERNEL_REPLAY_TOOL`;
  the tool accepts one `ib.bin` and the `--set-vtx-size` control that
  those runners exercise. The full CS-track runner uses the separate
  `replay_r300_cs_track` build output through
  `R3V_CS_TRACK_REPLAY_TOOL`. `build-infra/r3v/build_kernel_replay.py`
  compiles that tool from the pinned kernel tree the running kernel was
  built from, runs that tree's own `r300_cs_grammar_correspondence`
  fidelity gate against `r300_packet0_check`, and writes a provenance
  record binding the output ELF to the exact kernel commit, per-file
  source hashes, compiler identity, and compile argv.
- The four-suite preflight runs from that same release build without a
  rebuild, and its complete output becomes the recorded vector:

  ```sh
  R3V_SUITE_REPORT="$R3V_NATIVE_MANIFEST_DIR/preflight-suites.txt"
  R3V_KERNEL_REPLAY_TOOL=<path to scripts/replay_r300_tcl_bypass_ib> \
  env -u R3V_NATIVE_MANIFEST_DIR \
  meson test -C "$R3V_BUILD_DIR" --no-rebuild --print-errorlogs \
    --suite r300 --suite r3v --suite radeon-drm-vk --suite drm-shim \
    >"$R3V_SUITE_REPORT" 2>&1
  cat "$R3V_SUITE_REPORT"
  ```

  The suite launcher withholds `R3V_NATIVE_MANIFEST_DIR`, except for
  `r3v-native-float2-tuple-cell-external-manifest-ignored`, whose Meson
  test definition intentionally sets it to the build directory. The
  other drm-shim harnesses retain manifests and write `attempt.token`
  into whatever directory that variable names, so a suite run that
  inherits the evidence directory fills it with host-model artifacts and
  the harnesses' one-attempt and retention-order checks fail against each
  other (ten such failures on a clean build when the variable leaks).
  The evidence directory receives the suite report alone.

  A nonzero suite status blocks the run. The qualification inventory gate
  then confirms the required test set is complete and the replay tool's
  provenance is current; retain its complete output as well:

  ```sh
  R3V_QUALIFICATION_REPORT="$R3V_NATIVE_MANIFEST_DIR/qualification-inventory.txt"
  R3V_CS_TRACK_REPLAY_TOOL=<path to replay_r300_cs_track> \
  R3V_CS_TRACK_CONTROLS=<path to its controls fixture> \
  R3V_CS_TRACK_REPLAY_PROVENANCE=<path to the provenance record> \
  env -u R3V_NATIVE_MANIFEST_DIR \
  python3 src/amd/r300/vulkan/tests/r3v_qualification_inventory.py \
    "$R3V_BUILD_DIR" --qualification \
    >"$R3V_QUALIFICATION_REPORT" 2>&1
  cat "$R3V_QUALIFICATION_REPORT"
  ```

  The inventory verifies the replay tool against its provenance record
  -- the correspondence gate passed and the ELF's SHA-256 matches the
  recorded hash -- rather than trusting a binary handed to it by name
  alone. Any drm-shim residue matches the calibrated signature.
- The loaded Radeon substrate is the production package built at the
  production compiled profile, and its identity agrees with the loaded
  module. `Substrate admission` below carries the decision table and the
  transition this check refuses without.
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
  registration only; they do not prove GPU reset or silicon recovery. An
  inactive `/dev/watchdog0` reboots nothing after a wedge, so registration
  admits preparation and offline verification and leaves hardware
  submission closed; `Watchdog gate` below carries the arming a submission
  requires, and this preflight arms nothing. A missing identity, a nonzero
  `radeon.lockup_timeout`, or a missing watchdog/recovery setting blocks the
  run.
- `r3v_native_arming_runner` reports `armed` for the exact evidence
  directory the run will use.
- The host has an off-box log path (netconsole or serial), because a
  hard lockup takes the on-box journal with it.
- The operator is physically at the machine and able to power-cycle it.

## Substrate admission

A development module with all mutation controls disarmed is still a
development substrate. Do not substitute its current knob state for the
production module's compiled-profile identity. The runtime knobs state
what is armed; the compiled profile states what the module can expose,
and an attended submission rests on the second.

`radeon-unified-dkms` and `radeon-unified-dkms-dev` come from one signed
source pin and carry the same virtual driver identity
(`Provides: radeon-unified=<version>`), and they conflict with each
other, so the transition between them is explicit and mutually
exclusive. A `Provides` match therefore satisfies a dependency solver
and not this gate: the packages are separate deployment substrates whose
loaded behavior differs by what the build compiled in.

| Loaded substrate | Result |
| --- | --- |
| Production package, production build profile, matching source identity | Admit preflight |
| Development package, even with every hazard arm zero | Refuse; require production transition and reboot |
| Production package but stale dev profile override present | Refuse |
| Package identity and loaded module disagree | Refuse |
| Watchdog registered but inactive | Admit build and replay preparation; refuse hardware submission, per `Watchdog gate` |
| Watchdog absent | Refuse hardware submission |

When the development package is loaded, stop before arming. Select the
off profile, replace development with production atomically, reboot,
verify the loaded production module, then establish the watchdog. The
replacement runs as one `pacman -U` transaction over the driver and the
policy package together, so no boot carries a driver from one
transaction beside a policy from another:

```sh
sudo radeon-profile-dev select off
sudo pacman -U -- \
  ./radeon-unified-dkms-<version>-*.pkg.tar.zst \
  ./radeon-rs482-policy-<version>-*.pkg.tar.zst
```

The production-admission hook refuses while a foreign development
override remains. That refusal is the gate working; it takes a
correction to the substrate rather than a bypass.

Establish the recovery mechanism in the production boot alone. Loading
`sp5100_tco` in a development boot proves nothing about the boot the
submission runs in, so the watchdog is registered after the reboot, and
`Watchdog gate` states what registration then admits.

## Watchdog gate

The SB600 TCO watchdog reboots this machine after a wedge only while it
is counting. These are the measured properties of that counter on the
Vostro 1000, from the retained tick measurement
(`sb600-watchdog-tick-32768hz-pet-ineffective`):

- `WatchDogCount` ticks at 32.768 kHz.
- The count is 16 bits, so a full count is an approximately 2.0-second
  maximum window.
- The operational grace is 1.7 seconds, leaving roughly 0.3 second for a
  disarm to land ahead of the fire.
- `WDIOC_KEEPALIVE` does not reload the counter, so the window admits no
  extension and a guarded interval closes inside it or the machine
  reboots.
- `sp5100_tco` loads with `heartbeat=65535` so the count holds 0xffff at
  probe and the first open counts from the full window.
- PM index 0x69 bit 0 (`WatchDogTimerDisable`) is the confirmed disarm.

The gate follows from those properties:

```text
Watchdog registered but inactive:
    admits preparation and offline verification;
    refuses hardware submission.

Watchdog armed around a measured sub-1.7-second hazardous interval:
    admits one hardware submission.

Hazardous interval not isolatable or not bounded below 1.7 seconds:
    refuses submission unless the human operator explicitly waives
    automatic recovery and accepts manual power-cycle recovery.
```

The hazardous interval is `DRM_IOCTL_RADEON_CS` through fence
completion, because a ring wedge becomes observable while waiting for
completion rather than at the ioctl return. `vkQueueSubmit` is a wider
interval than that: it also carries the deferred copies, the relocation
list, the completion allocation, the reference cache publication, and
the `attempt.token` write, so a filesystem stall inside it would fire
the watchdog on a healthy submission and spend the attempt. The driver
already brackets the exact interval through the optional submission
trace, whose events are `R3V_NATIVE_SUBMISSION_TRACE_CS_IOCTL_ENTER`,
`..._CS_IOCTL_RETURN`, `..._COMPLETION_WAIT_BEGIN`, and
`..._COMPLETION_WAIT_RETURN`; a runner installs the hook, so the arming
mechanism stays outside driver source.

The runner therefore separates into three phases, and the watchdog
covers the second alone:

1. Preparation: allocation, poisoning, recording, parser replay, and
   digest verification. The watchdog is registered and inactive.
2. Guarded execution: arm at `CS_IOCTL_ENTER`, hold across the ioctl and
   the completion wait, and disarm at `COMPLETION_WAIT_RETURN`, or at
   `CS_IOCTL_RETURN` when the ioctl refuses and no wait follows.
3. Post-completion: disarm first, then read back, score, and seal. A
   reboot after a good submission but before retention destroys the
   result and spends the attempt, so the disarm precedes the first
   target read.

Bound the interval before the attempt by submitting an already-qualified
control cell with the trace hook installed and reading its transport
bracket, whose enter and return timestamps the runner reports. The
retained route timings bound the regime at roughly 102 to 115
microseconds per submission, four orders of magnitude under the grace;
those cells carry fewer relocation sites and no texture fetch, so they
bound the regime rather than this cell, and the control run measures
this one.

The arming mechanism is `r3v_native_watchdog_bracket`, a co-process that
holds the privilege the counter needs while the ICD, the render node,
and the evidence writes stay with the invoking user. The runner names it
through `R3V_NATIVE_WATCHDOG_BRACKET_COMMAND`, an absolute path with
optional single-space-separated arguments that the runner execs
directly, so no shell parses the value:

```sh
export R3V_NATIVE_WATCHDOG_BRACKET_COMMAND="/usr/bin/sudo -n \
  <build-dir>/src/amd/r300/vulkan/r3v_native_watchdog_bracket"
```

The runner opens the bracket in its preparation phase and walks the
counter's state ladder there, because `r3v_native_arming_disarm` writes
`attempt.token` ahead of the trace: an arm that first fails inside
`vkQueueSubmit` refuses the submission with the attempt already spent.

The driver's `.start` sets START and TRIGGER while its `.ping` sets
TRIGGER alone, so the measured-ineffective ping refutes `.start`'s reload
along with it. Every arm therefore rewrites `WatchDogCount` explicitly
through `WDIOC_SETTIMEOUT` while the PM bit holds the counter halted, and
reads the register back before clearing the bit. The helper sets PM 0x69
bit 0 before it ever opens the device, so the open that starts the
countdown finds the counter inhibited:

```text
set PM 0x69 bit 0          halt first
open /dev/watchdog0        the hardware stays inhibited
WDIOC_SETTIMEOUT 65535     rewrite WatchDogCount
read halted count
```

Each state is judged by two reads across a bounded interval, because one
`WDIOC_GETTIMELEFT` read separates a running counter from a halted one
not at all. The observation interval is 5 ms, which is 164 ticks at
32.768 kHz. The ladder states its relations before it reads them, and the
depleting active phase is what makes the rewrite observable: a counter
halted at its loaded value could not show a reload at all.

```text
loaded    L0 >= 0x8000   heartbeat=65535 reached WatchDogCount
active    A0 > A1        clearing the PM bit starts the count
halted    H0 == H1       setting the PM bit stops it
reloaded  R0 > H0        WDIOC_SETTIMEOUT rewrites the register
rearmed   B0 > B1        the rewritten count runs
```

The ladder ran on the Vostro 1000's SB600 and verified every relation:

```text
L0=65535  A0=65535 A1=65367  H0=65367 H1=65367  R0=65535  B0=65535 B1=65369
```

The active phase fell 168 counts across a nominal 5 ms and the rearmed
phase 166, which puts the tick between the two at the 32.768 kHz the
retained measurement reports, since `nanosleep` overshoots and so bounds
the implied rate from above. `H0 == H1` is the load-bearing observation:
PM 0x69 bit 0 halts the counter, which is what makes a disarm possible
at all. `R0 > H0` shows `WDIOC_SETTIMEOUT` rewriting a depleted count
back to full, so no arm depends on `.start` reloading.

Each later arm repeats the operative subset -- assert halted, reload,
verify the count near full, clear the halt, verify the countdown -- and
acknowledges `armed verified` with its readings. An `armed unverified`
acknowledgement refuses the submission before the ioctl.

Exit is fail-closed while armed. A normal exit runs from the disarmed
state, keeps the PM halt, performs the watchdog core's magic close, and
confirms PM 0x69 bit 0 still reads set; the count cannot answer that,
because reopening the device would arm it. Every abnormal exit -- a
signal, a closed command stream, a parent that wedged the machine --
leaves an armed counter running, so the reset the gate promises still
lands.

A helper that answers without touching hardware measures the pipe round
trip and nothing else. That figure is
`watchdog.stub_trace_interval_ns`, roughly 110 ns here, and it is not
the guarded interval: the guarded interval begins after the co-process
confirms the counter is running and ends after it confirms the counter
is halted, so it carries the reload, both two-read observations, and
both acknowledgements.

The third clause of the gate takes `--waiver <path>`, naming a document
the operator writes for one run. An exported variable outlives the
decision it recorded and authorizes whatever runs next, so the waiver
binds to the run instead:

```text
boot_id=<contents of /proc/sys/kernel/random/boot_id>
attempt_id=<basename of the evidence directory>
ib_blake3=<the cell digest the runner reports>
runner_blake3=<BLAKE3 of the submitting image>
timestamp=<YYYY-MM-DDTHH:MM:SSZ>
operator_reason=<why automatic recovery is waived>
```

Every field is matched against the live run, and the timestamp admits an
age of 0 to 3600 seconds, so a waiver written for another boot, another
attempt, another cell, another runner image, or another hour admits
nothing. The runner prints the exact bindings when it refuses, so the
operator writes what the run is rather than transcribing it. Without
either the bracket or an admitted waiver the runner refuses before it
creates the instance.

Record these fields:

```text
watchdog.driver
watchdog.device
watchdog.tick_hz
watchdog.counter_bits
watchdog.measured_max_window_ms
watchdog.operational_grace_ms
watchdog.keepalive_reload_effective = false
watchdog.reset_path_verified
watchdog.guarded_interval
watchdog.armed_before_submit
watchdog.arm_acknowledgement
watchdog.completion_observed
watchdog.disarm_result
watchdog.guarded_interval_us
```

`watchdog.reset_path_verified` reports what the run can show. Firing the
counter is the only demonstration that the reset path works, and an
attended submission does not fire it, so this field carries forward the
guard's own qualification evidence or reads `unverified` with the
reason.

Verify the production boot in this order before the preflight runs: a
new boot ID; `radeon-unified-dkms` installed and `radeon-unified-dkms-dev`
absent; the loaded module reporting the production build profile; the
loaded `srcversion` matching the installed module; the source commit,
driver tree, and policy digest matching the package manifest; any
remaining `profile_dev` parameter reading off; `lockup_timeout` at 0;
every hazard arm absent or zero; `rs480_safe_regs` at its
policy-qualified value; the PCI identity still `1002:5974`; and no new
kernel or display anomaly.

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
- The ICD is selected only by its own manifest, so removing
  `r3v_icd.<cpu>.json` from the loader's search path restores the host to
  its pre-run driver configuration without rebuilding.

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
