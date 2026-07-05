# Vostro 1000 (RS482 / K8 / SB600) out-of-tree kernel modules

The Dell Vostro 1000 target that validates the r300 gallium and r3v Vulkan
drivers needs several out-of-tree kernel modules that no in-tree driver
supplies. This document is the registry: what each module does, why the
hardware needs it, where its source lives, and how it relates to the
userspace driver work. It is referenced from the sibling repositories so
the requirement is discoverable from any entry point (see Cross-references).

The modules divide by ownership: **radeon DRM** work (GPU reset / register
hazard mitigation) tracks the userspace driver and consolidates into
`~/workspaces/radeon-custom`; **platform** work (southbridge watchdog, EC
thermal) is Vostro-hardware-specific and lives in `vostro1000-re`.

## Why this hardware needs out-of-tree modules

The RS482 IGP shares the K8 northbridge. A GPU command-stream fault hangs
the ring, and with `radeon.lockup_timeout=0` the kernel waits on the fence
forever rather than resetting the GPU -- an unrecoverable soft hang (proven:
a `u_blitter` clear stalls in `radeon_fence_default_wait`, TGSI or NIR
shaders alike). The SB600 TCO watchdog is the only reset independent of the
CPU cores, so it is the backstop for the both-core northbridge freeze that
no software detector can catch. Neither reset path nor the EC thermal
sensors are exposed by any in-tree driver on this platform.

## Radeon DRM modules (GPU reset + hazard mitigation)

These patch the in-tree `radeon` driver to add a working RS480-class GPU
reset and to guard the register read/write hazards that stall the K8
northbridge. They are the kernel-side complement to the r300/r3v userspace
drivers and the direct fix for the unrecoverable fence-hang class.

| Package | Module | Purpose | Source (current) |
| --- | --- | --- | --- |
| `radeon-unified-dkms` v0.3 | `radeon` | Master patch series (0001-0027+): RS480/R600 hazard mitigation plus `gpu_reset` recovery; Arch + Debian packaging adapters | `steinmarder/src/re/radeon/` |
| `radeon-rs480-safe-regs` v0.2 | `radeon` | `rs480_safe_regs` debugfs reader (gated-readback safe register set) | `steinmarder-r300/src/re/r300/PKGBUILDs/radeon-rs480-safe-regs-dkms/` |
| `radeon-palm-gate` v1.0 | `radeon` | Palm/Warrior gate: `mc_wait_for_idle` timeout, `pci_config_reset_safe` gate, SMX_DC_CTL0 validator; ships pre-patched source | `steinmarder/mesa-rekit/staged/radeon-palm-gate-dkms/` |

Key recovery patches in `steinmarder/src/re/radeon/patches/rs480/`:
`0003-rs480-crash-shim-recovery.patch` (gpu_reset shim),
`0041`/`0042-rs480-rbbm-soft-reset-recovery-probe` (RBBM soft-reset probe).
Upstream reference tree:
`steinmarder-r600-terakan/docs/external_sources/linux_6_18_32_radeon_drm/`.

**Consolidation (planned): `~/workspaces/radeon-custom`.** The scattered
radeon DKMS work above collapses into one dedicated repo -- a patch series
over the vendored upstream radeon subtree, DKMS + PKGBUILD -- so the reset
work is a single first-class artifact instead of three staging locations.
Building and installing it (with a non-zero `lockup_timeout`) is what turns
the infinite fence hang into a recoverable GPU reset.

## Platform modules (Vostro 1000 southbridge + EC)

Hardware-specific to this board; they stay in `vostro1000-re` where they
are developed, packaged, and installed.

| Package | Module | Purpose | Source |
| --- | --- | --- | --- |
| `sp5100-tco-ioapic` | `sp5100_tco` | Bind the SB600 TCO watchdog at its documented base `0xFEC000F0` inside the IOAPIC page (`devm_ioremap` fallback, not exclusive `request_mem_region`); the CPU-independent reset backstop | `vostro1000-re/.../southbridge-sb600/sp5100-tco-ioapic-dkms/` |
| `vostro1000-ec-fan` | `vostro1000_ec_fan` | Read-only hwmon exposing the PC87591x EC fan tachometer and thermal sensor via `ec_read()`; `dell_smm_hwmon` is dead on this board | `vostro1000-re/.../drivers/vostro1000-ec-fan-dkms/` |
| `sb600-wdt-relocate` | `sb600_wdt_relocate` | SUPERSEDED: watchdog base relocation attempt; the relocation target is write-unsafe (lands on the display-adjacent FCH decoder). Retained for the record; replaced by `sp5100-tco-ioapic` | `vostro1000-re/.../southbridge-sb600/sb600-wdt-relocate-dkms/` |

## Userspace recovery posture (not a kernel module)

| Package | Purpose | Source |
| --- | --- | --- |
| `vostro1000-wedge-recovery` | Layered panic-reboot sysctls (`hardlockup_panic`, `hung_task_panic`, `softlockup_panic`) plus `sb600-guard` (run one hazardous op under the hardware watchdog); depends on `sp5100-tco-ioapic` | `vostro1000-re/.../recovery/vostro1000-wedge-recovery/` |

The posture reboots on the wedge classes it can detect. It is the safety net
that keeps hardware bring-up iterable, but it is not the fix: the radeon
`gpu_reset` work above is what makes the GPU itself recoverable. Note that a
`radeon_fence_default_wait` hang is an interruptible sleep, so it does not
trip `hung_task_panic`; test via `PIGLIT_PLATFORM=gbm` surfaceless (which
keeps the box alive and the hang inspectable) rather than an X session
(which hard-locks the scanout path and reboots).

## Cross-references

Prominent docs in the sibling repositories point here so the requirement is
reachable from any entry point:
`mesa-26-gororoba` (this file), `vostro1000-re/systems/dell-vostro-1000/`,
`steinmarder/src/re/radeon/`, and `steinmarder-r300`.
