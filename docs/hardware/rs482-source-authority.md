# RS482 Source Authority Index

This document is the canonical cross-repository integration index for the
RS482 (Vostro 1000, PCI 1002:5974) display and GPU stack. Each layer of the
stack has exactly one repository that owns its production source and release
artifacts. `radeon-custom/README.md` and the sibling repositories point here;
a repair routes to the lowest layer whose invariant is demonstrably violated.

## Layer authority table

| Layer | Repository | Build artifact | Runtime identity |
| --- | --- | --- | --- |
| Xorg Server / glamor | `PKGBUILD_xorg-server-glamor-r300fix` | `xorg-server` packages | `/usr/lib/Xorg`, `libglamoregl.so` |
| Xorg modesetting DDX | `PKGBUILD_xorg-server-glamor-r300fix` | `xorg-server` package | `modesetting_drv.so` |
| Radeon DDX | `PKGBUILD_xf86-video-ati-rs482` | `xf86-video-ati` package | `radeon_drv.so` |
| Mesa userspace | `mesa-26-gororoba` | immutable Mesa image or package | `libgallium`, r300 DRI driver, `r3v` ICD |
| Radeon kernel | `radeon-custom` | `radeon-unified-dkms` | module `srcversion`, module parameters |
| Platform | `vostro1000-re` | DKMS and configuration packages | SB600 watchdog, EC thermal, boot configuration |
| Evidence | `steinmarder-r300` | none (evidence only) | bundle SHA256SUMS and finding documents |

## Ownership rule

Xorg owns what request, shader, resource, and display policy it constructs.
Mesa owns how those GL operations become R300 programs and command streams.
`radeon-custom` owns how those streams cross the kernel, ring, fence, reset,
and KMS boundaries. `steinmarder-r300` owns the proof of which layer failed.

## Layer discriminator for observed defects

- Reproduces in standalone EGL/Gallium: Mesa or kernel.
- Requires a particular Xorg/glamor-generated program or resource: inspect
  Xorg first, then Mesa.
- Xorg constructed the wrong program or state: Xorg/glamor repository.
- Xorg constructed the correct program but hardware executed stale state:
  Mesa or kernel.
- Mesa emitted the correct patched IB but cross-IB behavior is wrong:
  kernel/ring/hardware boundary (`radeon-custom` plus silicon evidence).

Xorg can trigger a Mesa defect without owning the fix; the test is which
layer's invariant is violated, not which client exposed it.

## Build-model separation

The three active build models stay distinct; their deployment constraints
differ and they do not normalize into one:

- Xorg package: machine-neutral x86-64 release package, clean-chroot
  reproducible, installed through pacman.
- Experimental Mesa: box-built for the K8 target, immutable per-SHA image,
  selected at runtime through the loader environment
  (`LIBGL_DRIVERS_PATH`, `LD_LIBRARY_PATH`, `VK_ICD_FILENAMES`).
- Radeon kernel: DKMS build against the exact installed target kernel.

## DDX identity is an experimental variable

Every hardware verdict taken with Xorg present records the active DDX by
name (`modesetting` or `radeon`), because the two paths differ in page-flip
behavior, vblank bookkeeping, TearFree, KMS request patterns, and submission
cadence. A result under one DDX does not transfer to the other. The standing
qualified configuration is `modesetting` plus glamor
(`PKGBUILD_xorg-server-glamor-r300fix/rs48x-runtime-qualification-21.1.24-1.5.md`).

## Machine-readable manifest

`rs482-stack-manifest.schema.json` in this directory is the JSON Schema for
the per-run stack manifest. A hardware runner that involves Xorg refuses to
start when a required layer field is unspecified. Manifest instances are
evidence and live with their bundles in `steinmarder-r300`.

## Registry currency

The kernel-module registry for this platform is
`docs/hardware/vostro1000-kernel-modules.md`; its package versions track the
installed box state (verify with `pacman -Q radeon-unified-dkms xorg-server
xf86-video-ati` on the target). A version drift between the registry and the
installed package invalidates claimed image provenance until corrected.
