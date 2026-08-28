# mesa-26-gororoba/build-infra

Canonical build infrastructure for the mesa-26-gororoba Mesa fork.

The entry point requires GNU Make 4.2 or newer because source-root resolution
uses `.SHELLSTATUS` to preserve Python validation failures during Makefile
evaluation.  Parse-time path inputs travel as directly quoted environment
assignments because GNU Make before 4.4 does not place Makefile `export` values
in the environment of every `$(shell ...)` expansion.

The Make process is the caller execution boundary.  GNU Make evaluates
immediate command-line assignments using `:=`, `::=`, `:::=`, or `!=` before
the repository Makefile loads, so those operators receive trusted operator
text only.  Recursive command-line assignments reach the Makefile as data.
The entry point rejects dollar syntax in those values, then validates all
named build selectors, resource counts, and source/build paths before recipe
planning.  `JOBS`, `DISTCC_JOBS`, and `LOCK_WAIT` are typed decimal values.
`BUILD_LOCK` and `SYSCONFDIR` resolve through the same path-character control
as `TOPSRC`, `BUILD_ROOT`, `BUILDDIR`, and `PREFIX`; the install recipe passes
its canonical sysconfdir to the locked shell through the environment instead
of embedding caller text in the shell program.

`MESON`, `NINJA`, `SUDO`, `FLOCK`, and `REMOVE_TREE` are trusted operator
command hooks.  `NINJA_ARGS`, `NINJA_TARGETS`, and `MESON_TEST_ARGS` are
trusted command-argument hooks.  These hooks intentionally execute caller
syntax and never receive untrusted text.

## Canonical profiles

The default profile sits at the top of `build-infra/configs/`; the other six
live in `build-infra/configs/alternates/` and are selected by passing
`PROFILE=` explicitly.  The Makefile resolves a bare profile name against both
directories, so the per-profile `rebuild-`/`install-` targets need no path
prefix.

| Profile | Target | Surface | Type | Location |
|---|---|---|---|---|
| `3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache` (default) | vostro (RS482, r300) | maximal r300 + ati_r300 ICD | debug | `configs/` |
| `4_r300_full_release_x86_64v1-clang22-distcc-cache` | vostro (RS482, r300) | maximal r300 + ati_r300 ICD | release (conformance baseline) | `configs/alternates/` |
| `5_r300_full_release_x86_64v1-gcc-distcc-cache` | vostro (RS482, r300) | r300 + zink override + ati_r300 ICD + h264dec + tests | release (GCC diagnostic gate) | `configs/alternates/` |
| `3_terakan_full_release_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | r600+zink+soft+llvm+amd_terascale + Rusticl | release | `configs/alternates/` |
| `4_terakan_full_debug_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | r600+zink+soft+llvm+amd_terascale + Rusticl | debug | `configs/alternates/` |
| `5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | same as 3_ without Rusticl | release | `configs/alternates/` |
| `6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache` | x130e (PALM, r600) | same as 4_ without Rusticl | debug | `configs/alternates/` |

The clang profiles use `HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc`
and `COMPILER_CHAIN=ccache`.  Profile `5_r300_full_release_*-gcc-*` is the GCC
sibling of profile 4_: it pairs with
`HOSTENV=vostro1000-x86-64-v1-gcc-ccache-distcc` and `COMPILER_FAMILY=gnu`, and
it builds the wider r300 surface -- zink as a loader override, the h264dec
codec, the in-tree tools, the Vulkan layers, and `build-tests` -- because gcc's
warnings-as-errors set reads defect classes clang's does not.  It is a
diagnostic gate; conformance and silicon evidence stay on profile 4_.  A zink
attribution probe sets `VK_ICD_FILENAMES` to the generated `r3v_icd.<cpu>.json`
from the profile install prefix before setting
`MESA_LOADER_DRIVER_OVERRIDE=zink`, so the Vulkan loader selects the `ati_r300`
ICD built by the profile.  A zink run also sets
`R3V_ZINK_BASELINE_SURFACE=1`: zink hard-requires create_renderpass2,
dynamic_rendering, and maintenance5, whose registry dependencies a
Vulkan 1.0 device without multiview cannot satisfy, so the default r3v
surface withholds them and the gate opens the full zink baseline with
that dependency violation as its recorded conformance cost.  Conformance and silicon-evidence runs use profile 4_
(`4_r300_full_release`, now under `alternates/`) because an asserts-live debug
build can abort a CTS/Piglit case that release would pass.  `make install
PROFILE=...` lands in the isolated per-profile prefix `/opt/local/mesa-<profile>`
by default; the shared active trees `/opt/local/mesa-26-gororoba` (release) and
`/opt/local/mesa-gororoba-debug-optimized` (debug) are used only by the
`install-<profile>` targets or when an explicit `PREFIX=` is passed.

## Layout

```text
build-infra/
|-- Makefile                       # entry point
|-- README.md                      # this file
|-- configs/
|   |-- 3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache.meson   # default profile
|   +-- alternates/                # non-default profiles; pass PROFILE= explicitly
|       |-- 4_r300_full_release_x86_64v1-clang22-distcc-cache.meson
|       |-- 5_r300_full_release_x86_64v1-gcc-distcc-cache.meson
|       |-- 3_terakan_full_release_x86_64v1-clang22-distcc-cache.meson
|       |-- 4_terakan_full_debug_x86_64v1-clang22-distcc-cache.meson
|       |-- 5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache.meson
|       +-- 6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache.meson
+-- env/
    |-- vostro1000-x86-64-v1-clang22-ccache-distcc.env  # clang numbered profiles
    |-- vostro1000-x86-64-v1-gcc-ccache-distcc.env      # gcc profile 5_
    |-- generic-x86-64-os.env       # portable ad hoc lane
    +-- Archive/                    # removed host envs retained for provenance
```

Build outputs land in the gitignored repo-local `build/mesa-<profile>/` tree, so
they never appear in `git status`, and `make clean` removes the active profile's
subdir.  Each worktree gets its own `build/`, so parallel profile builds do not
collide.  (`git clean -xdf` also removes them since they are ignored; prefer
`make clean` to drop one profile without touching the others or the source.)

## Immutable source comparisons

`TOPSRC` selects the committed Mesa Git input.  The default is the worktree
that owns `build-infra/`, and control-source builds continue to pass that path
directly to Meson.  An explicit external `TOPSRC` lets two immutable source
revisions use the same profile, host environment, compiler resolver, warning
policy, and Meson option generator:

```bash
make -C build-infra configure \
  TOPSRC=/path/to/mesa-source-worktree \
  PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache \
  BUILD_ROOT=/path/to/source-identity-root \
  BUILDDIR=/path/to/source-identity-root/build \
  PREFIX=/path/to/source-identity-root/prefix
```

The source selector accepts only a committed, exact Git worktree root
containing `meson.build` and either `meson.options` or the historical
`meson_options.txt` spelling.  External comparisons require both the selected
source worktree and this build-infra control worktree to match their recorded
commits.  The cleanliness check compares the real index to `HEAD`, then
compares worktree bytes through a fresh temporary index.  Staged-only changes,
unignored untracked files, ordinary tracked changes, and tracked changes
hidden by `assume-unchanged` all fail the comparison.

External setup creates `$BUILD_ROOT/.mesa-source-view` from the captured
source commit with argv-only `git archive` and in-process tar extraction.
Meson receives that derived path and may populate required wrap dependencies
there, including zink's Vulkan-Profiles source, while the selected Git
worktree remains unchanged.  External setup passes `--wrap-mode=default` and
clears `MESON_PACKAGE_CACHE_DIR`, so Meson's package cache and extracted
sources stay under the derived view.  Every `[wrap-file]` download declares a
SHA-256 `source_hash`; a downloaded patch declares `patch_hash`.  The control
rejects an absent or malformed hash before publishing the source view, and
Meson verifies the declared hash while fetching the archive.

After setup, the final identity records the derived path and a deterministic
SHA-256 digest over every relative path, file type, permission mode, regular
file size and bytes, and symbolic-link target.  Build, test, install, artifact
reporting, and finalized cleanup recompute that digest and reject source-view
drift.  Make exports `PYTHONDONTWRITEBYTECODE=1` as an enforced build-tool
setting so Python generators import source helpers without adding bytecode
caches to the derived view.  Make also binds Mesa's `MESA_GIT_SHA1_OVERRIDE`
input to the validated source commit, preserving generated build identity when
the derived view has no `.git` directory.  A provisional transaction admits
interrupted-setup cleanup by its exact source tuple and fixed derived path
because Meson may have stopped between wrap extraction and final digest
publication.  Ignored build outputs in the original worktree remain
regenerable, while ignored original `subprojects/` sources remain rejected.

Path selection rejects whitespace and shell metacharacters, resolves symlinks
before containment checks, and keeps the external build root, build directory,
and prefix outside both source worktrees.  The external prefix is a direct
child of its build root, so the shared profile default under `/opt/local`
never aliases two comparison sources.

External and noncanonical build roots use one strict child of these designated
namespaces:

```text
<account-home>/.cache/mesa-26-gororoba/external-builds/
/tmp/mesa-26-gororoba-<uid>/
/var/tmp/mesa-26-gororoba-<uid>/
<workspace>/.mesa-26-gororoba-builds/
```

The account home comes from the account database rather than `HOME`.  Each
account-home or workspace boundary and every selected namespace component
carries the current uid, has no group or world write bit, and contains no
symlink component.  `/tmp` and `/var/tmp` qualify only as root-owned sticky
directories.  A build root inside any Git worktree or bare Git directory is
rejected, including a sibling repository under the same workspace.  The
control worktree's canonical `build/` remains the direct local exception.  A
control-source prefix is either a named Mesa profile prefix under `/opt` or a
direct child of its build root.  System top-level directories never qualify as
install prefixes or build roots.

Reproducible profile targets configure `BUILD_ROOT/prefix`; they never select a
shared `/opt` prefix.  `rebuild-all-tiers` treats its selected `BUILD_ROOT` as a
parent and derives one profile-named build root for each tier, so each root has
one build directory, prefix, policy tuple, and source-identity transaction.

Successful external configuration writes
`$BUILD_ROOT/.mesa-external-source-identity.json` and
`$BUILDDIR/.mesa-source-identity.json`.  Before Meson runs, the build-root
record enters a provisional transaction that reserves the entire root for one
selected source root, commit, tree, control root, control commit, control tree,
derived source-view path, build directory, prefix, and sysconfdir.  Archive
preparation records the initial derived-view digest under the provisional
transaction.  The identity also binds the resolved profile, host environment,
mode, compiler chain, and compiler family.  Meson success records the
post-setup digest and writes matching final records with one transaction
identifier.  Meson failure leaves the root provisional.  A
previous build-directory record may remain after failed reconfiguration, but
its final state cannot satisfy build, test, install, or distclean while the
root transaction stays provisional.  Cleanup accepts a matching provisional
root so it can remove an interrupted build.  Finalization rejects any source
tuple that differs from the provisional reservation and binds the complete
post-setup source view.  A source revision or control-plane commit change uses
a fresh empty build root; the old root remains an immutable attribution
boundary.

Install reconfiguration uses the same transaction.  It verifies the existing
final identity, marks the root provisional before `meson setup`, and finalizes
matching root and build-directory records only after setup, installation, and
install-log ownership repair succeed.  A failure in any of those operations
therefore revokes build, test, install, and artifact consumers until a
successful configure restores one final transaction.

External `clean` verifies the recorded source identity before removing an
existing build directory.  An absent build directory remains a successful
no-op only when the build-root identity matches the requested source and path
tuple; a final root also requires the recorded source-view digest.
Release rebuild targets use `clean-registered-build`: an absent build directory
requires no cleanup, while every existing directory or symlink delegates to
strict `clean` and its identity, containment, and mount checks.
`distclean` verifies the same root, derived view, and prefix identity before
it removes the build directory or archives the prefix, including after
`clean` has already removed the per-build record and when a prior archival
attempt failed.  Artifact reporting joins the same final root identity and
accepts a missing build directory only when the retained root record still
binds the source, control plane, derived view, and prefix.

Each mutating recipe captures source commit, source tree, control commit,
control tree, canonical paths, and device/inode/file-type anchors during
Makefile evaluation, acquires the shared build lease, and revalidates those
values immediately after acquisition.  A revision change or path replacement
completed while a cooperating command waits on the lease fails before layout,
identity, removal, or archival logic runs.  The lease is the coordination
boundary for same-user build processes; direct filesystem mutation by a
process that bypasses Make is outside that cooperative boundary.  Clean,
clean-all, and distclean reject selected targets that contain a Git repository
marker.  Configure, build, test, install, clean, and distclean reject an exact
or descendant build-directory mount point in the active mount namespace.
Install, artifact reporting, and distclean apply the same mount boundary to
the prefix.  Mount topology is checked before any descendant Git-marker walk.
Removal and archival report success only after the command succeeds and the
selected path satisfies its postcondition.

Each compared revision gets a distinct `BUILD_ROOT`, `BUILDDIR`, and `PREFIX`.
`clean-all` remains a control-worktree operation and rejects every external
`TOPSRC`; `make clean` removes only the physically contained canonical build
directory.

The build-infra worktree remains the control plane.  Its profile, host
environment, allowlist, toolchain selection, and Make logic govern every
selected source tree.  Git probes discard ambient `GIT_*` variables, global
and system configuration, hooks, fsmonitor commands, untracked caches, and
replacement objects.  `make source-root-selection-test` calibrates GNU Make
4.2 input transport when that lower-bound executable is installed, exact-root
selection, legacy option-file admission, incomplete, unborn, dirty,
assume-unchanged, ignored-subproject, nested-worktree, and bare-repository
rejection, shell-input rejection, physical containment, namespace ownership,
sibling-worktree protection, clean-all refusal, lease-bound path replacement,
build-root and prefix identity drift, configure and install transactions,
build-policy selector drift, failed reconfiguration revocation, hashed
synthetic Vulkan-Profiles population, source-view drift, unhashed download rejection,
clean-then-distclean archival, archival retry, artifact-report source-tuple
binding, shared prefix refusal, and the external and control Meson source
arguments.  When the host permits private user and mount namespaces, the same
target proves that exact and descendant same-device bind mounts fail before
recursive removal.  `make source-root-control-unit-test` exercises the pure
path, layout, staged-only cleanliness,
dirty external-control rejection, anchor, namespace, and identity-record
invariants directly.  Its temporary repositories keep the control checkout
immutable, so concurrent calibration runs do not invalidate one another.

Qualification, conformance-baseline, and merge-gating invocations set
`REPRODUCIBLE_RUN=1`.  The selector rejects the control worktree as `TOPSRC` and
requires distinct clean detached control and source worktrees.  Cleanliness
includes physical tracked bytes, staged and untracked paths, and ignored
populated `subprojects/`.  Configure archives the selected source commit into
the external source view, and the build identity binds `REPRODUCIBLE_RUN=1` so
later build, test, install, and artifact commands cannot relabel an ordinary
build as reproducible.  Treat both detached worktrees and `BUILD_ROOT` as
single-owner resources throughout the run.  For example:

```sh
MESA_CONTROL_ROOT=/path/to/detached-control
MESA_SOURCE_ROOT=/path/to/detached-source
MESA_BUILD_ROOT=/path/to/dedicated-build-root
make -C "$MESA_CONTROL_ROOT/build-infra" configure \
  REPRODUCIBLE_RUN=1 TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
make -C "$MESA_CONTROL_ROOT/build-infra" build \
  REPRODUCIBLE_RUN=1 TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
make -C "$MESA_CONTROL_ROOT/build-infra" test \
  REPRODUCIBLE_RUN=1 TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
```

Build-control environment names use the `MESA_*` namespace. Legacy names that
contain `GOROROBA_` fail with their replacement instead of being ignored. The
two non-mechanical runner renames are `GOROROBA_MESA_PREFIX` to
`MESA_INSTALL_PREFIX` and `GOROROBA_MESA_ENV` to `MESA_ENV_FILE`; deployment
consent moved from `MESA_GOROROBA_DEPLOY_ACCEPTED` to `MESA_DEPLOY_ACCEPTED`.
`make naming-policy-test` permits these spellings only in compatibility checks
and retained review evidence.

Schema-7 external source identities cannot be relabeled as schema 8. Their
recorded control commit describes the older build, so replacing it with the
new control commit would make the record false. Remove an obsolete schema-7
build root only with the same external `TOPSRC`, `BUILD_ROOT`, `BUILDDIR`,
profile, host environment, compiler selectors, and prefix that created it:

```sh
make -C build-infra remove-identity-v7-build-root REPRODUCIBLE_RUN=0 \
  TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
```

This destructive cleanup verifies both final schema-7 records, the historical
control commit and tree through Git, transaction equality, the source-view
digest, and the current source-view bytes. It removes the build root only when
that directory contains exactly the root identity, source view, and selected
build directory. An installed prefix, any additional entry, a mount boundary,
or a nested Git repository makes the command refuse the removal. Create a new
schema-8 build through the ordinary configure path after cleanup.

## Build-system policy

- Meson native files carry Mesa options.
- Make is the only build orchestration layer above Meson.
- Host-specific LLVM command names are generated into
  `$BUILDDIR/mesa-toolchain.meson` during `make configure`.
- `make configure` and `make install` re-assert every `[project options]`
  entry from the profile as `-D` flags (via
  `scripts/meson_profile_dflags.py`).  Native-file values are defaults only;
  after Meson drops a retired choice (for example the old `amd_r300`
  vulkan-drivers token), coredata resets to the option default
  (`vulkan-drivers=auto`) and the native file alone does not re-apply.  On
  x86_64, `auto` pulls lavapipe while r300 profiles keep `llvm=disabled`, so
  configure aborts.  The CLI `-D` pass heals that drift without a wipe.
- Warnings are errors in every configure path: profiles set `werror = true`,
  Make always passes `-Dwerror=true`, packaging PKGBUILDs that meson-setup
  this tree pass `-Dwerror=true`, and `make audit-werror` fails closed if any
  of those gates are missing.
- New build-system behavior belongs in `build-infra/Makefile` or Meson files.
  Do not add standalone helper scripts for compiler selection, audit policy,
  or clean/build orchestration.  Make-invoked implementation bodies under
  `scripts/` (profile audit, profile `-D` extraction) stay allowed.

## Build lease

`make -C build-infra` acquires one exclusive `flock` lease before every
operation that changes a build directory: configure, build, test, clean,
clean-all, distclean, and install.  The default lease is shared across this
user's Mesa worktrees at `~/.cache/mesa-26-gororoba/mesa-build.lock`; set
`BUILD_LOCK=` only when an intentionally separate build domain needs its own
lease.  `LOCK_WAIT` defaults to 7200 seconds and accepts `0` for a fail-fast
caller.

Use Make for all configured builds.  A direct `meson setup`, `ninja -C`, or
`meson test -C` invocation bypasses the lease because Meson and Ninja do not
provide a repository-level build-domain lock.  For a focused target, retain
the lease through Make:

```bash
make -C build-infra build \
  PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache \
  NINJA_TARGETS=src/amd/r300/vulkan/r3v_native_descriptor_test
make -C build-infra test \
  PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache \
  MESON_TEST_ARGS='--print-errorlogs r3v-native-descriptor'
```

Run `make -C build-infra build-lease-test` to prove that a held lease rejects
configure, build, test, clean, and clean-all before they touch a build tree.

## Common flows

Before a long build, run the host audit:

```bash
make audit PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
           HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc
```

For a distcc lane, the audit skips option, discovery, and local-only host tokens,
then resolves every remote compiler hostname and compiles warning-clean C and
C++ probes on each volunteer with fallback disabled.  The ccache lane dispatches
the compiler paths resolved through `CCACHE_PATH`; the direct distcc lane
dispatches the versioned compiler names that volunteers resolve through their
own `PATH`.  GNU lanes also read the GCC and G++ majors from each remote-produced
object's `.comment` section and fail closed when either identity is missing or
differs from the client major.  A syntactically valid host allocation cannot
pass while every compile runs on the client, a volunteer lacks one language
compiler, or one configured volunteer uses a different GNU compiler major.

r300 DEBUG build (vostro, **default install target** -- assertions live,
gallium-xa XA tracker, valgrind/libunwind/perfetto instrumentation):
```bash
make rebuild-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache
# Package and install as the system Mesa (replaces stock mesa or release build):
cd build-infra/packaging/mesa-gororoba-debug && makepkg --noconfirm && yes | sudo pacman -U mesa-gororoba-debug-*.pkg.tar.zst
```

r300 RELEASE build (vostro, conformance-baseline -- use only for CTS/Piglit/deqp runs
where assertions-live behavior would contaminate pass/fail):
```bash
MESA_CONTROL_ROOT=/path/to/detached-control
MESA_SOURCE_ROOT=/path/to/detached-source
MESA_BUILD_ROOT=/path/to/dedicated-build-root
make -C "$MESA_CONTROL_ROOT/build-infra" \
  rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache \
  REPRODUCIBLE_RUN=1 TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
make -C "$MESA_CONTROL_ROOT/build-infra" \
  install-4_r300_full_release_x86_64v1-clang22-distcc-cache \
  REPRODUCIBLE_RUN=1 TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
make -C "$MESA_CONTROL_ROOT/build-infra" artifact-check \
  PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache \
  PREFIX="$MESA_BUILD_ROOT/prefix" REPRODUCIBLE_RUN=1 \
  TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
```

r600/terakan RELEASE build (x130e, Rusticl enabled):
```bash
MESA_CONTROL_ROOT=/path/to/detached-control
MESA_SOURCE_ROOT=/path/to/detached-source
MESA_BUILD_ROOT=/path/to/dedicated-build-root
make -C "$MESA_CONTROL_ROOT/build-infra" \
  rebuild-3_terakan_full_release_x86_64v1-clang22-distcc-cache \
  REPRODUCIBLE_RUN=1 TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
make -C "$MESA_CONTROL_ROOT/build-infra" \
  install-3_terakan_full_release_x86_64v1-clang22-distcc-cache \
  REPRODUCIBLE_RUN=1 TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
```

r600/terakan RELEASE build (x130e, no Rusticl -- use when bindgen breaks):
```bash
MESA_CONTROL_ROOT=/path/to/detached-control
MESA_SOURCE_ROOT=/path/to/detached-source
MESA_BUILD_ROOT=/path/to/dedicated-build-root
make -C "$MESA_CONTROL_ROOT/build-infra" \
  rebuild-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
  REPRODUCIBLE_RUN=1 TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
make -C "$MESA_CONTROL_ROOT/build-infra" \
  install-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
  REPRODUCIBLE_RUN=1 TOPSRC="$MESA_SOURCE_ROOT" BUILD_ROOT="$MESA_BUILD_ROOT"
```

Show available profiles + hostenvs:
```bash
make list
```

Full reset of a profile (removes builddir and archives install prefix aside):
```bash
make distclean PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache
```

Runtime smoke test (terakan):
```bash
export PREFIX=/opt/local/mesa-26-gororoba
export LD_LIBRARY_PATH=$PREFIX/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
export VK_DRIVER_FILES=$PREFIX/share/vulkan/icd.d/terascale_icd.x86_64.json
vulkaninfo --summary
```

## Cache discipline

Warm incremental: `ccache -> distcc -> clang` (use `COMPILER_CHAIN=ccache`).
The distcc-pump profiles and Make targets were removed with the lane
consolidation; remaining pump notes live only in archived provenance docs.

## Adding a profile

1. Create `configs/<new-profile>.meson` with `[built-in options]` + `[project options]`.
2. Add `rebuild-<new-profile>:` and `install-<new-profile>:` targets in the Makefile.
3. Document the profile purpose in `build-infra/CLAUDE.md` under "Build profiles and host envs".
