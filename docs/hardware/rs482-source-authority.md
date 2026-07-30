# RS482 Source and Release Authority Index

This document is the canonical cross-repository integration index for the
RS482 (Vostro 1000, PCI 1002:5974) display and GPU stack. Each layer names its
editable source authority, release or deployment authority, and runtime
identity. One repository may fill more than one role. A repair routes to the
lowest layer whose invariant is demonstrably violated.

## Layer authority table

| Layer | Editable source authority | Release or deployment authority | Runtime identity |
| --- | --- | --- | --- |
| Xorg Server / glamor | `xserver-rs48x` | `PKGBUILD_xorg-server-glamor-r300fix` | `/usr/lib/Xorg`, `libglamoregl.so` |
| Xorg modesetting DDX | `xserver-rs48x` | `PKGBUILD_xorg-server-glamor-r300fix` | `modesetting_drv.so` |
| Radeon DDX | `xf86-video-ati-rs482` | `PKGBUILD_xf86-video-ati-rs482` | `radeon_drv.so` |
| Mesa userspace | `mesa-26-gororoba` | `mesa-26-gororoba` build-infra and immutable image | `libgallium`, r300 DRI driver, `r3v` ICD |
| Radeon kernel | `radeon-custom` until source-pin cutover; `linux-radeon-gororoba` after cutover | `radeon-custom` | `radeon-unified-dkms`, module `srcversion`, module parameters |
| Platform | `vostro1000-re` | `vostro1000-re` | SB600 watchdog, EC thermal, boot configuration |
| Evidence and orchestration | `steinmarder-r300` | `steinmarder-r300` retained bundles | bundle manifests, hashes, and finding documents |

## Ownership rules

`xserver-rs48x` owns Xorg Server, glamor, and built-in modesetting source,
source tests, numeric derivations, and source history.
`PKGBUILD_xorg-server-glamor-r300fix` owns the package recipe, deterministic
source export, package gates, installed manifest, and release qualification.

`xf86-video-ati-rs482` owns external Radeon DDX source, source tests, and
source history. `PKGBUILD_xf86-video-ati-rs482` owns the package recipe,
deterministic source export, package gates, installed manifest, TearFree
configuration, and release qualification.

Package repositories consume source-repository commits and trees. A package
copy of source or a generated patch series serves as release input; editable
source authority remains in the source repository. A source change lands in
its source repository before the corresponding release input advances.

Mesa owns how GL and Vulkan operations become R300 programs and command
streams. `radeon-custom` owns the deployed kernel package, compiler and DKMS
policy, hazard preflight, module configuration, and package verification.
`steinmarder-r300` owns the proof of which layer failed.

## Radeon kernel source cutover

`radeon-custom` remains the deployable and editable kernel authority while
`linux-radeon-gororoba` is a reconstruction candidate. Kernel source authority
moves to `linux-radeon-gororoba` only when every migration gate closes:

1. `linux-radeon-gororoba/MIGRATION_INPUT.toml` pins the `radeon-custom`
   commit, upstream commit and tree, migration manifests, and every declared
   input hash.
2. `radeon-custom/scripts/normalize_legacy_source_tree.sh
   --prove-generated` emits the normalized source manifest and normalization
   record. The retained record hashes that script, `mkregtable.c`, the built
   `mkregtable`, and every byte comparison against the legacy generated
   register-policy output.
3. `linux-radeon-gororoba/scripts/manifest_source_tree.py` emits the candidate
   source-export manifest and compares it with the normalized reference. The
   retained comparison has an empty residual set and carries the tool hash,
   both manifest hashes, and comparison-output hash.
4. `radeon-custom` pins the qualified source commit and tree, builds the source
   archive and DKMS package from that pin, and retains the source-pin,
   source-archive, package-artifact, and installed-payload-manifest SHA-256
   values.
5. The installed module matches the package payload by SHA-256 and
   Build ID plus `/sys/module/radeon/srcversion`. A Vostro boot records the
   pinned source identity, package identity, installed-payload manifest, and
   module identity in one `source-to-payload-v2` stack manifest.
6. `docs/hardware/vostro1000-kernel-modules.md` changes its Radeon source row
   and source-of-record statement from `radeon-custom` to the qualified
   `linux-radeon-gororoba` source pin in the same cutover change. The registry
   continues to name `radeon-custom` as packaging and deployment authority.

| Cutover state | Editable kernel source | Package source input | Deployment authority |
| --- | --- | --- | --- |
| Before cutover | `radeon-custom` | `radeon-custom` source and patch stack | `radeon-custom` |
| After cutover | `linux-radeon-gororoba` | Immutable qualified commit and tree pin | `radeon-custom` |

After cutover, `linux-radeon-gororoba` owns kernel source, source generators,
register policy tables, source tests, and RAD-06. `radeon-custom` continues to
own packaging, DKMS glue, compiler policy, initramfs and modprobe policy,
hazard preflight, the source pin, package verification, and deployment.

The cutover bundle retains the machine-readable migration input, source-closure
policy, normalization record, normalized and exported source manifests, source
comparison output, generated-output comparison output, qualified source pin,
source archive, built package, installed-payload manifest, and installed module
identity. `bundle_hashes.sha256` names these artifacts after their producers
finish. A prose statement of equivalence carries no cutover authority.

## Layer discriminator for observed defects

- Reproduces in standalone EGL/Gallium: Mesa or kernel.
- Requires a particular Xorg/glamor-generated program or resource: inspect
  Xorg first, then Mesa.
- Xorg constructed the wrong program or state: `xserver-rs48x`.
- Xorg source is correct but the package export, recipe, or installed payload
  differs: `PKGBUILD_xorg-server-glamor-r300fix`.
- Radeon DDX constructed the wrong KMS or presentation request:
  `xf86-video-ati-rs482`.
- Radeon DDX source is correct but the package export, recipe, or installed
  payload differs: `PKGBUILD_xf86-video-ati-rs482`.
- Xorg constructed the correct program but hardware executed stale state:
  Mesa or kernel.
- Mesa emitted the correct patched IB but cross-IB behavior is wrong:
  kernel/ring/hardware boundary (`radeon-custom` plus silicon evidence).

Xorg can trigger a Mesa defect without owning the fix; the test is which
layer's invariant is violated, not which client exposed it.
The source-versus-package discriminators consume the source-to-release join
below.

## Source-to-release join

Every packaged Xorg or DDX verdict records:

- source repository and commit;
- source payload tree object;
- package repository and commit;
- deterministic source-export or patch-series SHA-256;
- package name and version;
- package artifact SHA-256;
- installed-payload manifest SHA-256;
- installed executable or module SHA-256 and Build ID.

Every immutable Mesa verdict records the source commit and tree, build
manifest, immutable-image manifest, installed-payload manifest, mapped
`libgallium` path, SHA-256, and Build ID.

Every kernel verdict records the qualified source commit and tree, source-pin
and source-archive SHA-256, packaging commit, package-artifact SHA-256,
installed-payload-manifest SHA-256, module SHA-256, module Build ID, and module
`srcversion`. After the kernel source cutover, it also records the
normalization, export, generator, comparison, and machine-record hashes defined
by the cutover gates.

Source tests prove source behavior. Package gates prove the declared source
reaches the package payload. Runtime and silicon evidence prove the installed
payload executed. Each evidence class closes only its own claim.

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

The optional `provenance_contract` field preserves the original required-field
contract for historical manifests. Its absence classifies a manifest as legacy
evidence and leaves the source-to-payload claim open. The value
`source-to-payload-v2` activates the decision-grade contract:

- Xorg Server and the active DDX carry source, release, package, installed
  payload, binary, and Build ID identities.
- Mesa carries source, build, immutable-image, installed-payload, mapped DSO,
  and Build ID identities.
- The kernel carries source pin, source archive, release, package, installed
  payload, module SHA-256, module Build ID, and `srcversion` identities.
- A `linux-radeon-gororoba` source identity also carries the complete,
  independently reproducible equivalence record.

The calibrated verifier accepts a legacy specimen and both current and
post-cutover v2 specimens. It rejects a malformed source pin, a kernel manifest
without the module Build ID, a Radeon DDX manifest without DDX provenance, and
a post-cutover kernel manifest without equivalence evidence:

```sh
python3 docs/hardware/tests/test_rs482_stack_manifest_schema.py -v
```

## Registry currency

The kernel-module registry for this platform is
`docs/hardware/vostro1000-kernel-modules.md`; its package versions track the
installed box state (verify with `pacman -Q radeon-unified-dkms xorg-server
xf86-video-ati` on the target). A version drift between the registry and the
installed package invalidates claimed image provenance until corrected.
