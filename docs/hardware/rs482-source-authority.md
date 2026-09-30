# RS485M Source and Release Authority Index

This document is the canonical cross-repository integration index for the
RS485M (Vostro 1000, PCI 1002:5974) display and GPU stack. Each layer names its
editable source authority, release or deployment authority, and runtime
identity. One repository may fill more than one role. A repair routes to the
lowest layer whose invariant is demonstrably violated.

## Layer authority table

<!-- markdownlint-disable MD013 -->

| Layer | Editable source authority | Release or deployment authority | Runtime identity |
| --- | --- | --- | --- |
| Xorg Server / glamor | `xserver-rs48x` | `PKGBUILD_xorg-server-glamor-r300fix` | `/usr/lib/Xorg`, `libglamoregl.so` |
| Xorg modesetting DDX | `xserver-rs48x` | `PKGBUILD_xorg-server-glamor-r300fix` | `modesetting_drv.so` |
| Radeon DDX | `xf86-video-ati-rs485m` | `PKGBUILD_xf86-video-ati-rs485m` | `radeon_drv.so` |
| Mesa userspace | `mesa-26-gororoba` | `mesa-26-gororoba` PKGBUILDs and build-infra | `libgallium`, r300 DRI driver, `r3v` ICD |
| Radeon kernel | `linux-radeon-gororoba` | `radeon-custom` | source commit and tree, module SHA-256, GNU Build ID, `srcversion`, build profile, module parameters |
| Platform | `vostro1000-re` | `vostro1000-re` | SB600 watchdog, EC thermal, boot configuration |
| Evidence and orchestration | `steinmarder-r300` | `steinmarder-r300` retained bundles | bundle manifests, hashes, and finding documents |

<!-- markdownlint-enable MD013 -->

## Ownership rules

`xserver-rs48x` owns Xorg Server, glamor, and built-in modesetting source,
source tests, numeric derivations, and source history.
`PKGBUILD_xorg-server-glamor-r300fix` owns the package recipe, deterministic
source export, package gates, installed manifest, and release qualification.

`xf86-video-ati-rs485m` owns external Radeon DDX source, source tests, and
source history. `PKGBUILD_xf86-video-ati-rs485m` owns the package recipe,
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

The cutover anchors bind the migration boundary to tracked records. They remain
the foundation for later package pins; the current package and runtime
identities are recorded separately below.

<!-- markdownlint-disable MD013 -->

| Proof layer | Record | Exact identity |
| --- | --- | --- |
| Signed source equivalence | `linux-radeon-gororoba/docs/source-equivalence-attestation.toml` | tag object `81a2510e34d1d62f5486683bcd6e240db8223b7d`, commit `9079be562eebd184da9cf891fbc6a72d5ac0d9f3`, driver tree `b0f40a1970f57d00b690890120ad1ba8fd1e474c` |
| Normalized source and generated output | `linux-radeon-gororoba/MIGRATION_INPUT.toml` | base manifest SHA-256 `1f085124056f24cdfe26ff3c19e615c32cefdb5fef5a9343205acce17e7d94c7`, migration manifest SHA-256 `71ae424fc8af1828200ec98e9233b646c6483c11ecfb778d91ec64ef6bf9f5dc`, generated output proof SHA-256 `1efe8d77577c8c9173093d5c47a8df2905fd487471a22c5c9d0507693502909a` |

<!-- markdownlint-enable MD013 -->

The current identities stay on separate axes:

<!-- markdownlint-disable MD013 -->

| Identity axis | Authority | Exact identity and claim boundary |
| --- | --- | --- |
| Modified source | `linux-radeon-gororoba` | current main commit `0598947dbffe97ec03119d54b3fe41ad8775c6a8`, driver tree `e3eb7f883f27123adb5dccb42a758ee4a384fcc8`; the driver tree equals the package pin, main advances only in documentation, and main carries no package or runtime claim |
| Active package recipe | `radeon-custom` 0.8.21-1 | package commit `00b0a71f26205fc1b3e3ea91a193a5db19a08b44`, recipe tree `6d80ac59c152f9f085facb9db3ff05fe2ef5a71e`, `PKGBUILD` blob `b8fe6949a25a9f4b1a8c18f16dd48bbccba3551b`, source identity blob `94d718847f53428b381ef6c690ea0aae20a7dd4c`; the recipe pins signed source tag object `53a055cbdbac3cda5fc5648b91a0d33ac191d605`, source commit `3e0e85ecb516e1523c99e84cd3f58a985a55b026`, and driver tree `e3eb7f883f27123adb5dccb42a758ee4a384fcc8` |
| Target deployment runtime | `steinmarder-r300/results/rs485m-radeon-unified-0.8.21-1-deployment-runtime/` | retaining commit `8070fac12cacc41f8d879f68e508fd81cf3c1197`, manifest SHA-256 `6e3978ac138839b92f831650a727daecdf6810cfe009ae97dc42973ae042a3f5`, hash ledger SHA-256 `6c496a4f19eb49d3bc94b74ae58c1ace4f7ed84e2f8a79593dd326c3d0d508bc`; records production packages 0.8.21-1 installed from the protected gate artifact, on-disk source commit `3e0e85ecb516e1523c99e84cd3f58a985a55b026`, driver tree `e3eb7f883f27123adb5dccb42a758ee4a384fcc8`, and matching loaded/on-disk srcversion `047F8935D11DA0AA57B72CB`; DKMS modules serve 7.2.5-1-cachyos and 6.18.50-1-cachyos-lts, both boot entries verify their pinned image digests, and the boot ID changes across the reboot and stays stable across the read-only capture |
| Loaded module byte identity | `steinmarder-r300/results/r3v-zmask-public-lifecycle-rs485m-53d8342-silicon-3064aff97/` | retaining commit `b5c1fa23f18fe5d72c744b7f0a466e1be1f351e4`, manifest SHA-256 `2c36f50513cad706ac25b0dfe61a733c91876747e7c4a46a5552f0acccfcc4fa`, hash ledger SHA-256 `c51721e4bdf412d2a8e02a3a0779863c8b70f8b9005eb00c69e6d6f70c125556`, compressed module SHA-256 `74175bc8ab43b6dc89b7f174924a28e0dca7630668f469b68543dd04c2b42a00`, GNU Build ID `18ab65bacbb629910bb9457a8897ccb9c0b37497`, and srcversion `047F8935D11DA0AA57B72CB`; `capture_identity.json` joins the loaded module's build-ID note to the on-disk 0.8.21 module, and no newer retained bundle records the loaded module bytes |
| Parked-device behavior | `steinmarder-r300/src/re/r300/results/cachyos_vostro1000_rs482_parked_entry_contract_matrix_20260805T055406Z/` | retaining commit `baa6b2d496c52392c0ecb5e18306db02e9dfd6cf`, outcome SHA-256 `f053e84ec97332abb5ec9c0611ac84d988c5070bdd2bc28eb22d1e10da82c243`, hash ledger SHA-256 `ab36a1a974679a8f9cb8c7da5bf0fd4452dbba3a5ca6151f5001841d926d96ae`; measures the 0.6-1 parked-entry contract, while later package and deployment identities carry no newer parked-device run |

<!-- markdownlint-enable MD013 -->

The active 0.8.21-1 recipe and on-disk module identify source commit
`3e0e85ecb516e1523c99e84cd3f58a985a55b026`. The installed production package
is the protected gate artifact: the GitHub-hosted gate built it, the dependent
target run compiled it against the running kernel, and the target installed
those exact bytes and rebooted into them. The loaded module srcversion matches
the on-disk module. The ZMASK lifecycle capture on the same boot records the
loaded build-ID note, so loaded byte identity joins the deployment through
the lifecycle bundle rather than the deployment capture itself. GPU workloads
retain separate evidence requirements.

The 0.8.19-1 deployment record remains in
`steinmarder-r300/results/radeon_unified_0819_production_deployment_runtime_vostro1000_rs485m_5974/`
at retaining commit `5ae37da93a756e30a1faaa9db406a8a545d378f0`, manifest
SHA-256 `568240e0e9000c77bf4df76fa0911c24e5fdf3ed7a0dfc2ec4396e027fc5e496`,
and hash ledger SHA-256
`8244131386d322947b99492b2fc9e34b326f5871e71d8ecfdf2e186930620c6f`; it
records srcversion `6AB2F38F6A1C43C583AA9A7`. The 0.8.14-1 package and module
identity remains in `steinmarder-r300/results/rs485m-radeon-package-module-identity/`
at retaining commit `65ba5b43af60280b50ca1267c67f9726903b13c8`, and the 0.7-1
production identity, the previous byte-level loaded-module record, remains in
`steinmarder-r300/src/re/r300/results/cachyos-vostro1000-rs482-radeon-unified-0.7-1-production-identity/`
at retaining commit `55e74d6bbb7cdc061ed0c154f22cd8ede35a7ca1`.

The historical 0.8.11-1 deployment record remains in
`steinmarder-r300/results/cachyos-vostro1000-rs482-radeon-unified-0.8.11-1-deployment-runtime/`
at retaining commit `59f9361e277bb63c52d335eda9009aa94b7d989c`, manifest
SHA-256 `2ab2b00758b5226ac096da4d63c30652ebceef245f56373374b8f6fc21171ec6`,
and hash ledger SHA-256
`7bff34920965ddb4292b54a7bc9313f1f38102dd1592339a09d6cfd1ff6ff1e7`.

The 0.8.11-1 deployment bundle joins the installed package and board policy to
the recipe's source commit and driver tree, built DKMS modules for both served
kernels, the running kernel, the loaded srcversion, and the boot-image digests
pinned by both boot entries. The observed boot records successful ring and
indirect-buffer initialization. The capture retains no loaded module bytes or
Build ID and opens no DRM device. Neither bundle establishes API
conformance, reset behavior, workload performance, or general silicon safety.

The 0.6-1 parked-entry matrix remains the newest retained parked-device run.
The newer package contains the parked-device mechanisms in source, but source
presence and ordinary reboot evidence do not reproduce a park. A newer package
inherits no parked-device verdict until an attended run records the same
behavior against that exact deployment.

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
  `xf86-video-ati-rs485m`.
- Radeon DDX source is correct but the package export, recipe, or installed
  payload differs: `PKGBUILD_xf86-video-ati-rs485m`.
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

Each component retains its native build procedure. CachyOS installation and
configuration changes enter through the owning PKGBUILD and package payload:

- Xorg package: machine-neutral x86-64 release package, clean-chroot
  reproducible, installed through pacman.
- Mesa: target-qualified source/profile builds stage unprivileged package
  payloads for stock `/usr` installation through pacman. Release and debug
  packages conflict with stock Mesa and each other. Experimental profiles
  retain build-owned staging and explicit launchers.
- Radeon kernel: radeon-custom pins signed linux-radeon-gororoba source and
  packages DKMS builds against the exact installed target kernels. Module,
  initramfs, and modprobe policy remain package-owned.
- Platform: vostro1000-re owns the laptop-specific PKGBUILDs and their
  reviewed watchdog, thermal, and boot configuration inputs.

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
`source-to-payload-v2` activates the qualification contract.
`source-to-payload-v3` retains those provenance requirements and selects the
RS485M DDX repository names:

- Xorg Server and the active DDX carry source, release, package, installed
  payload, binary, and Build ID identities.
- Mesa carries source, build, immutable-image, installed-payload, mapped DSO,
  and Build ID identities.
- The kernel carries source pin, source archive, release, package, installed
  payload, module SHA-256, module Build ID, and `srcversion` identities.
- A `linux-radeon-gororoba` source identity also carries the complete,
  independently reproducible equivalence record.

The calibrated verifier accepts legacy and post-cutover v2 specimens with
the historical DDX names. A v3 specimen requires `xf86-video-ati-rs485m`
paired with `PKGBUILD_xf86-video-ati-rs485m`; mixed pairs and contract/name
mismatches fail. Repository-name migration preserves historical v2 captures.
The authority table names the RS485M repositories after the source and
package identity cutover. Historical v2 manifests retain their original
slugs. It rejects malformed or incorrectly sized Git and SHA-256 identities,
empty or nonhexadecimal Build IDs, a kernel manifest without the module Build
ID, a Radeon DDX manifest without DDX provenance, and a post-cutover kernel
manifest without equivalence evidence. The
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

The authority-row verifier uses the Python standard library and rejects a
current identity table that merges evidence axes, omits the package recipe
objects, or leaves a retained deployment or parked-device run without an exact
bundle and hash identity:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 \
  docs/hardware/tests/test_rs482_source_authority.py -v
```

## Registry currency

The loaded target deployment row in the current identities table tracks the
installed Radeon package version. Verify it with `pacman -Q
radeon-unified-dkms` on the target. A version drift between that row and the
installed package invalidates the loaded deployment identity until corrected.
`docs/hardware/vostro1000-kernel-modules.md` tracks stable module mechanisms
and ownership rather than changing package versions.

The retained 0.8.21-1 observation closes package-version and srcversion
correspondence for its recorded boot. The observation leaves loaded byte
identity, reboot activation, RB2D execution, reset recovery, and API conformance
as separately qualified surfaces. Any package, module, boot, or policy change
requires a fresh corresponding capture before a hardware verdict uses it.
Historical interpolation and parked-device receipts retain their original
source and execution boundaries.
