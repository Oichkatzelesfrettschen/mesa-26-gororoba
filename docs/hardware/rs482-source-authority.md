# RS482 Source and Release Authority Index

This document is the canonical cross-repository integration index for the
RS482 (Vostro 1000, PCI 1002:5974) display and GPU stack. Each layer names its
editable source authority, release or deployment authority, and runtime
identity. One repository may fill more than one role. A repair routes to the
lowest layer whose invariant is demonstrably violated.

## Layer authority table

<!-- markdownlint-disable MD013 -->

| Layer | Editable source authority | Release or deployment authority | Runtime identity |
| --- | --- | --- | --- |
| Xorg Server / glamor | `xserver-rs48x` | `PKGBUILD_xorg-server-glamor-r300fix` | `/usr/lib/Xorg`, `libglamoregl.so` |
| Xorg modesetting DDX | `xserver-rs48x` | `PKGBUILD_xorg-server-glamor-r300fix` | `modesetting_drv.so` |
| Radeon DDX | `xf86-video-ati-rs482` | `PKGBUILD_xf86-video-ati-rs482` | `radeon_drv.so` |
| Mesa userspace | `mesa-26-gororoba` | `mesa-26-gororoba` build-infra and immutable image | `libgallium`, r300 DRI driver, `r3v` ICD |
| Radeon kernel | `linux-radeon-gororoba` | `radeon-custom` | source commit and tree, module SHA-256, GNU Build ID, `srcversion`, build profile, module parameters |
| Platform | `vostro1000-re` | `vostro1000-re` | SB600 watchdog, EC thermal, boot configuration |
| Evidence and orchestration | `steinmarder-r300` | `steinmarder-r300` retained bundles | bundle manifests, hashes, and finding documents |

<!-- markdownlint-enable MD013 -->

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
streams. `linux-radeon-gororoba` owns modified Radeon kernel source, source
generators, register policy tables, source tests, and RAD-06. `radeon-custom`
owns the source pin, package, compiler and DKMS policy, initramfs and modprobe
policy, hazard preflight, package verification, and deployment.
`steinmarder-r300` owns retained target evidence and hardware verdicts.

## Radeon kernel source authority

The source cutover is complete. `linux-radeon-gororoba` is canonical at
the signed `radeon-unified-0.3-pkgrel91-source-equivalent` boundary. Its source
closure removes generated register headers and the prebuilt `mkregtable`
binary, restores the generator inputs, and proves both source equivalence and
generated output equivalence. `radeon-custom` consumes an immutable signed
commit and tree instead of constructing the active module from its historical
patch files.

The public cutover anchors bind each proof layer to a tracked record:

<!-- markdownlint-disable MD013 -->

| Proof layer | Record | Exact identity |
| --- | --- | --- |
| Signed source equivalence | `linux-radeon-gororoba/docs/source-equivalence-attestation.toml` | tag object `81a2510e34d1d62f5486683bcd6e240db8223b7d`, commit `9079be562eebd184da9cf891fbc6a72d5ac0d9f3`, driver tree `b0f40a1970f57d00b690890120ad1ba8fd1e474c` |
| Normalized source and generated output | `linux-radeon-gororoba/MIGRATION_INPUT.toml` | base manifest SHA-256 `1f085124056f24cdfe26ff3c19e615c32cefdb5fef5a9343205acce17e7d94c7`, migration manifest SHA-256 `71ae424fc8af1828200ec98e9233b646c6483c11ecfb778d91ec64ef6bf9f5dc`, generated output proof SHA-256 `1efe8d77577c8c9173093d5c47a8df2905fd487471a22c5c9d0507693502909a` |
| Active package source pin | `radeon-custom/packaging/arch/radeon-unified-dkms/source-identity.toml` | signed tag object `c3745d24ea7481ec56c5c0b1aa397be4b8788b72`, source commit `2433cbd69cd99d1dd002447bb4d481ed66141562`, driver tree `e3432f8dda41e2fcb93fad23a0f3825541c15e93` |
| Loaded module identity | `steinmarder-r300/src/re/r300/results/cachyos-vostro1000-rs482-radeon-unified-0.7-1-production-identity/` | manifest SHA-256 `84340d65c87cb4ca3aa1f01faaa559a00d7950a55fad4cee344b988d3eeff386`, ledger SHA-256 `cc8a82f210cdccc847f9320faa7dc9f6136e537ef3555c78d867f0055ca70e42` |

<!-- markdownlint-enable MD013 -->

The current identities stay on separate axes:

<!-- markdownlint-disable MD013 -->

| Identity axis | Authority | Current evidence |
| --- | --- | --- |
| Modified source | `linux-radeon-gororoba` | canonical source continues beyond the shipped pins |
| Active package recipe | `radeon-custom` 0.8-1 | signed tag object `c3745d24ea7481ec56c5c0b1aa397be4b8788b72`, source commit `2433cbd69cd99d1dd002447bb4d481ed66141562`, driver tree `e3432f8dda41e2fcb93fad23a0f3825541c15e93` |
| Loaded target deployment | `radeon-custom` 0.7-1 | source commit `293a4ae3fe82cd03585ef3157e82b0b59b641b47`, driver tree `d57a22ad5356637d7075cb2aba83e22af71f7bfb`, srcversion `A7F72BE636B52D7EED42415` |
| Loaded binary | `steinmarder-r300` retained identity bundle | compressed SHA-256 `6d058f68aefab94350e96a9e376e3ff577512cd4d4919b627e85b678ca1b0301`, GNU Build ID `a5f1ae7e6e040b20c53278d2978ea7a17a29b696` |
| Parked device behavior | `steinmarder-r300` retained 0.6-1 bundle | latest attended park verdict; later package identity does not promote this behavior evidence |

<!-- markdownlint-enable MD013 -->

The 0.8 package gates build and verify the split package set. The target kernel
gate compiles its verified production package on RS482. The repository carries
no 0.8-1 signed release attestation or loaded module identity. The loaded
production authority therefore remains 0.7-1.

The 0.7 identity bundle joins package, DKMS source, installed module bytes,
loaded srcversion, loaded GNU Build ID, build profile, and PCI `1002:5974`.
It records successful boot ring and indirect buffer initialization. It runs no
controlled graphics workload and establishes no conformance, reset, register,
performance, or silicon safety verdict.

The target capture observes installed package version 0.7-1. It does not bind
the installed files to the signed release archive bytes. Exact signed archive
installation provenance therefore remains open even though the loaded module
to source identity join closes.

The durable cutover proof retains the migration input, source closure,
normalization and export manifests, generated output comparison, source tag,
package identity, and installed module joins. A source commit becomes a
deployed authority only after `radeon-custom` advances the signed pin and
retained evidence identifies the loaded module.

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
post-cutover v2 specimens. It rejects malformed or incorrectly sized Git and
SHA-256 identities, empty or nonhexadecimal Build IDs, a kernel manifest
without the module Build ID, a Radeon DDX manifest without DDX provenance, and
a post-cutover kernel manifest without equivalence evidence. The
[`jsonschema`](https://github.com/python-jsonschema/jsonschema) 4.26.0 package
implements the Draft 2020-12 validation used by this test and supports every
repository Python target. The requirements file pins this sole direct Python
dependency. Install it in a dedicated environment before running the offline
verifier:

```sh
rs482_schema_venv=$(mktemp -d)
python3 -m venv "$rs482_schema_venv"
"$rs482_schema_venv/bin/python" -m pip install \
  --requirement docs/hardware/tests/requirements-rs482-stack-manifest.txt
"$rs482_schema_venv/bin/python" -c \
  'from importlib.metadata import version; print(version("jsonschema"))'
"$rs482_schema_venv/bin/python" \
  docs/hardware/tests/test_rs482_stack_manifest_schema.py -v
```

Dependency installation is the only package-resolution step. The verifier
reads the tracked schema and fixtures without network access.

## Registry currency

The loaded target deployment row in the current identities table tracks the
installed Radeon package version. Verify it with `pacman -Q
radeon-unified-dkms` on the target. A version drift between that row and the
installed package invalidates the loaded deployment identity until corrected.
`docs/hardware/vostro1000-kernel-modules.md` tracks stable module mechanisms
and ownership rather than changing package versions.
