# Vostro 1000 (RS482 / K8 / SB600) out-of-tree kernel modules

This is the cross-repository registry for kernel-side dependencies used while
validating the r300 Gallium driver and the experimental r3v Vulkan ICD on the
Dell Vostro 1000. It identifies ownership and current verdicts; it is not the
build source for the modules it lists.

The claim-routing rules are defined in
[`rs482-source-authority.md`](rs482-source-authority.md). In particular:

- `radeon-custom` owns the executable Radeon patch/package mechanism;
- `steinmarder-r300` owns retained RS482 hardware evidence and verdicts;
- `mesa-26-gororoba` owns userspace driver behavior and this integration index;
- `vostro1000-re` owns non-Radeon board support.

## Radeon DRM package

| Active package | Module | Purpose | Source of record |
| --- | --- | --- | --- |
| `radeon-unified-dkms` 0.3-83 (snapshot on 2026-07-10) | `radeon` | Unified RS480 reset instrumentation, safe-register readers, hazard gates, failed-reset parking/host-containment, experimental reset-mask candidates, Palm safety gates, and Arch/Debian DKMS adapters | `radeon-custom:packaging/arch/radeon-unified-dkms/`; the ordered patch manifest is `dkms.conf` |

The package version above is a dated observation, not a second package manifest.
Future patch membership and package metadata must be read from `radeon-custom`.

The earlier package names `radeon-rs480-safe-regs-dkms` and
`radeon-palm-gate-dkms` are not independent active sources. Their mechanisms
were folded into `radeon-unified-dkms`; the unified PKGBUILD provides/replaces
those package identities and conflicts with the old DKMS packages. The original
Steinmarder locations remain provenance/evidence, not build inputs.

## Reset and containment verdict

Installed code must not be described as recovered hardware. The current retained
RS482 verdict is:

| Acceptance property | Verdict | Owning evidence |
| --- | --- | --- |
| Reset ladder executes | hardware-run, partial | 0043 clears VAP but GA remains busy |
| Host survives failed reset, parking, and client thaw/close | hardware-pass | Fire 28 / RAD-05i containment |
| GPU resumes accelerated work | not achieved | GA-rooted wedge remains; GPU stays parked |
| Display scanout recovers in place | not achieved | display remains black/parked; reboot required |
| 0060 SIGBUS isolation gate demonstrably fires | unverified | gate installed/armed, but no retained firing line |
| 0063-0068 non-baseline reset masks | installed, not fired | package 0.3-83 loads BASELINE; experimental candidates remain unrun |

Therefore `radeon.lockup_timeout=0` remains the safe default. Do not enable
automatic reset merely because the DKMS package builds, installs, or contains a
reset implementation. The authoritative per-patch table is
`steinmarder-r300:src/re/r300/findings/rs480-reset-recovery-patch-status-table.md`.

## SB600 watchdog boundary

The `sp5100-tco-ioapic` module remains useful board substrate and carries the
warm-boot fired-latch handling needed by the lab stack. It is **not** a proven
deferrable dead-man fuse for RAD-05 reset fires. Retained calibration shows that
`WDIOC_SETTIMEOUT`, `WDIOC_KEEPALIVE`, and magic close do not postpone the real
reset event; active feeding is retired and the watchdog is not opened/armed as a
fire-timing safety claim.

The `rs480-reset-hazard-stack` package may still require the watchdog substrate
so the machine is configured consistently and the fired latch is handled, while
its runtime preflight, boot-persistent netconsole, and manual recovery remain the
actual campaign controls. Package presence alone never proves a run safe.

## Board-specific platform modules

These stay in `vostro1000-re`; they are not copied into Mesa or `radeon-custom`.

| Package | Module | Purpose | Source |
| --- | --- | --- | --- |
| `sp5100-tco-ioapic` | `sp5100_tco` | Bind the SB600 TCO watchdog at its documented IOAPIC-page base and clear the fired latch; diagnostic/platform substrate, not a deferrable RAD-05 fuse | `vostro1000-re:systems/dell-vostro-1000/.../sp5100-tco-ioapic-dkms/` |
| `vostro1000-ec-fan` | `vostro1000_ec_fan` | Read-only hwmon for the PC87591x EC fan tachometer and thermal sensor | `vostro1000-re:systems/dell-vostro-1000/.../vostro1000-ec-fan-dkms/` |
| `sb600-wdt-relocate` | `sb600_wdt_relocate` | Superseded relocation experiment; retained as evidence only | `vostro1000-re:systems/dell-vostro-1000/.../sb600-wdt-relocate-dkms/` |

## Userspace recovery posture

`vostro1000-wedge-recovery` supplies panic/reboot policy and run wrappers for
wedge classes the host can still detect. Direct K8 northbridge stalls caused by
a non-posted MMIO read can stop both cores before software recovery executes;
netconsole and retained manifests are therefore evidence channels, not recovery
mechanisms.

For ordinary graphics diagnosis, prefer surfaceless GBM runs so a failed command
stream does not also own the display session. Destructive reset campaigns require
the dedicated hazard policy and preflight in the owning repositories.
