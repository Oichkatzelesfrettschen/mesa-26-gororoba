# Vostro 1000 (RS485M / K8 / SB600) out-of-tree kernel modules

The Dell Vostro 1000 target that validates the r300 gallium and r3v Vulkan
drivers needs several out-of-tree kernel modules that no in-tree driver
supplies. This document is the registry: what each module does, why the
hardware needs it, where its source lives, and how it relates to the
userspace driver work. It is referenced from the sibling repositories so
the requirement is discoverable from any entry point (see Cross-references).

The modules divide by ownership. `linux-radeon-gororoba` owns modified Radeon
DRM source. `radeon-custom` owns its source pin, package, DKMS lifecycle, and
deployment policy. Platform work for the southbridge watchdog and EC thermal
path lives in `vostro1000-re`.

## Why this hardware needs out-of-tree modules

The RS482 IGP shares the K8 northbridge. A GPU command stream fault can hang
the ring. The production policy keeps `radeon.lockup_timeout=0`. A separate
attended reset run must prove host survival and GPU recovery before automatic
reset becomes admissible. The retained
`steinmarder-r300/src/re/r300/findings/rs480-reset-recovery-patch-status-table.md`
records a `u_blitter` clear stalled in `radeon_fence_default_wait` with TGSI
and NIR shaders and records GPU recovery as not achieved. The SB600 TCO
watchdog is the only reset independent of the CPU cores, so it is the
backstop when both CPU cores hit a northbridge freeze that no software
detector can catch. Neither reset path nor the EC thermal
sensors are exposed by any in-tree driver on this platform.

## Radeon DRM modules (GPU reset + hazard mitigation)

The unified package exports the signed canonical source directly. Historical
patches remain reconstruction evidence and do not construct the active module.

<!-- markdownlint-disable MD013 -->

| Package | Module | Purpose | Source and deployment authority |
| --- | --- | --- | --- |
| `radeon-unified-dkms` | `radeon` | Carries the RS480 reset ladder, parked device containment, register policy, and Palm safety mechanisms. The production profile excludes development triggers, and the RS482 board policy admits PCI `1002:5974` | source in `linux-radeon-gororoba`; package and deployment in `radeon-custom`; exact current identities in `rs482-source-authority.md` |

<!-- markdownlint-enable MD013 -->

`rs482-source-authority.md` is the canonical integration registry for the
active recipe, loaded source commit and driver tree, module hashes, GNU Build
ID, srcversion, profile, and evidence bundle. This module registry keeps the
mechanism and ownership surface stable and does not duplicate those changing
deployment identities.

The former `radeon-rs480-safe-regs-dkms` and `radeon-palm-gate-dkms` package
identities are superseded. `radeon-unified-dkms` provides, replaces, and
conflicts with them so one Radeon module source owns the machine. Their source
and package snapshots remain historical evidence. The declarations live in
`radeon-custom/packaging/arch/radeon-unified-dkms/PKGBUILD` under
`_common_provides`, `_legacy_conflicts`, and `_legacy_replaces`.

The boot identity capture records successful ring and indirect buffer tests.
It runs no controlled workload and proves no reset recovery or graphics
conformance. `radeon.lockup_timeout=0` remains the production default.

## Platform modules (Vostro 1000 southbridge + EC)

Hardware-specific to this board; they stay in `vostro1000-re` where they
are developed, packaged, and installed.

<!-- markdownlint-disable MD013 -->

| Package | Module | Purpose | Source |
| --- | --- | --- | --- |
| `sp5100-tco-ioapic` | `sp5100_tco` | Bind the SB600 TCO watchdog at its documented base `0xFEC000F0` inside the IOAPIC page (`devm_ioremap` fallback, not exclusive `request_mem_region`); the CPU-independent reset backstop | `vostro1000-re/.../southbridge-sb600/sp5100-tco-ioapic-dkms/` |
| `vostro1000-ec-fan` | `vostro1000_ec_fan` | Read-only hwmon exposing the PC87591x EC fan tachometer and thermal sensor via `ec_read()`; `dell_smm_hwmon` is dead on this board | `vostro1000-re/.../drivers/vostro1000-ec-fan-dkms/` |
| `sb600-wdt-relocate` | `sb600_wdt_relocate` | SUPERSEDED: watchdog base relocation attempt; the relocation target is write-unsafe (lands on the display-adjacent FCH decoder). Retained for the record; replaced by `sp5100-tco-ioapic` | `vostro1000-re/.../southbridge-sb600/sb600-wdt-relocate-dkms/` |

<!-- markdownlint-enable MD013 -->

## Userspace recovery posture (not a kernel module)

<!-- markdownlint-disable MD013 -->

| Package | Purpose | Source |
| --- | --- | --- |
| `vostro1000-wedge-recovery` | Layered panic-reboot sysctls (`hardlockup_panic`, `hung_task_panic`, `softlockup_panic`) plus `sb600-guard` (run one hazardous op under the hardware watchdog); depends on `sp5100-tco-ioapic` | `vostro1000-re/.../recovery/vostro1000-wedge-recovery/` |

<!-- markdownlint-enable MD013 -->

The posture reboots on the wedge classes it can detect. It is the safety net
that keeps hardware bring-up iterable, but it is not the fix. The Radeon
`gpu_reset` work above is the mechanism under test for GPU recovery. A
`radeon_fence_default_wait` hang is an interruptible sleep, so it does not
trip `hung_task_panic`; test via `PIGLIT_PLATFORM=gbm` surfaceless (which
keeps the box alive and the hang inspectable) rather than an X session
(which hard-locks the scanout path and reboots).

## Cross-references

This file is the canonical module registry. `linux-radeon-gororoba/README.md`
defines source ownership. `radeon-custom/README.md` and
`radeon-custom/docs/radeon-unified-0.7-1-release-attestation.toml` define
package and release identity. Steinmarder records the loaded deployment in
`steinmarder-r300/src/re/r300/docs/rs482-radeon-production-deployment-identity.md`
and its sealed result bundle. Platform modules live under
`vostro1000-re/systems/dell-vostro-1000/`.
