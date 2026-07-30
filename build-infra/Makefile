# build-infra/Makefile -- mesa-26-gororoba build entry point
#
# Selects a PROFILE -> meson config file -> builddir under
# ../build/mesa-<profile>/.  TOPSRC may name another immutable Mesa worktree
# when two source revisions need one build policy and toolchain.
#
# Usage:
#   make configure PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache
#   make build     PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache
#   make rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache
#   make rebuild-3_terakan_full_release_x86_64v1-clang22-distcc-cache
#   make install   PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache
#   make audit     PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc
#   make list                                # show available PROFILEs
#
# Canonical profiles:
#   4_r300_full_release_x86_64v1-clang22-distcc-cache  r300 + ati_r300 release (vostro)
#   3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache    r300 + ati_r300 debug (vostro)
#   3_terakan_full_release_x86_64v1-clang22-distcc-cache  r600 + terascale release + Rusticl (x130e)
#   4_terakan_full_debug_x86_64v1-clang22-distcc-cache    r600 + terascale debug + Rusticl (x130e)
#   5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache  r600 + terascale release, no Rusticl
#   6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache    r600 + terascale debug, no Rusticl
#
# See also:
#   build-infra/README.md                    -- how to use
#   build-infra/configs/*.meson              -- canonical option sets
#   build-infra/env/*.env                    -- per-host toolchain env

SHELL := /bin/bash

# --- configurable parameters -------------------------------------------------
PROFILE  ?= 3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache
# Default HOSTENV must match the default PROFILE's lane.  The default profile is
# the maximal r300 vostro debug build (clang22, asserts live plus valgrind,
# libunwind, and perfetto), so the default env is the vostro clang22 env, not
# an archived btver1 env -- otherwise a bare `make configure` builds the r300
# vostro profile with Bobcat flags and no LLVM-22 pin.  The release sibling
# (4_r300_full_release, the conformance-baseline profile) and the x130e Terakan
# lanes live under configs/alternates/ and inherit this clang22 ccache/distcc
# host env unless the caller passes a different HOSTENV explicitly.
HOSTENV  ?= vostro1000-x86-64-v1-clang22-ccache-distcc
INSTALL_NAMESPACE ?= mesa
CONTROL_ROOT := $(abspath $(CURDIR)/..)
ifeq ($(origin TOPSRC),undefined)
TOPSRC := $(CONTROL_ROOT)
endif
ifeq ($(origin BUILD_ROOT),undefined)
BUILD_ROOT := $(CONTROL_ROOT)/build
endif
ifeq ($(origin BUILDDIR),undefined)
BUILDDIR := $(BUILD_ROOT)/mesa-$(PROFILE)
endif
# Keep profile lanes isolated by default because meson install does not remove
# artifacts that are absent from the current profile.  Override PREFIX=... on
# the make line when intentionally installing into a shared active tree.
ifeq ($(origin PREFIX),undefined)
PREFIX := /opt/local/$(INSTALL_NAMESPACE)-$(PROFILE)
endif
# drirc/driconf is host configuration, not an install artifact: meson bakes
# SYSCONFDIR into the driver objects (util/xmlconfig.c reads SYSCONFDIR/drirc),
# so an install-prefix-derived sysconfdir leaves a repacked or relocated driver
# pointing at a directory that never exists.  Bake the real /etc for every
# profile; the admin's /etc/drirc then applies to devenv and packaged drivers
# alike.
SYSCONFDIR ?= /etc

# Preserve command-line path bytes in exported variables.  The control script
# receives them through the process environment, validates their character
# domain, resolves symlinks, and returns canonical paths before any recipe
# interpolates them into shell syntax.
GOROROBA_TOPSRC_INPUT := $(value TOPSRC)
GOROROBA_BUILD_ROOT_INPUT := $(value BUILD_ROOT)
GOROROBA_BUILDDIR_INPUT := $(value BUILDDIR)
GOROROBA_PREFIX_INPUT := $(value PREFIX)
GOROROBA_PROFILE_INPUT := $(value PROFILE)
export GOROROBA_TOPSRC_INPUT
export GOROROBA_BUILD_ROOT_INPUT
export GOROROBA_BUILDDIR_INPUT
export GOROROBA_PREFIX_INPUT
export GOROROBA_PROFILE_INPUT

SOURCE_ROOT_CONTROL := $(CURDIR)/scripts/source_root_control.py
SOURCE_ROOT_FIELDS := $(shell python3 $(SOURCE_ROOT_CONTROL) resolve-make)
SOURCE_ROOT_RESOLVE_STATUS := $(.SHELLSTATUS)
ifneq ($(SOURCE_ROOT_RESOLVE_STATUS),0)
$(error source root and build path resolution failed)
endif
ifneq ($(words $(SOURCE_ROOT_FIELDS)),7)
$(error source root control returned an invalid field set)
endif
override TOPSRC := $(word 1,$(SOURCE_ROOT_FIELDS))
override BUILD_ROOT := $(word 2,$(SOURCE_ROOT_FIELDS))
override BUILDDIR := $(word 3,$(SOURCE_ROOT_FIELDS))
override PREFIX := $(word 4,$(SOURCE_ROOT_FIELDS))
SOURCE_COMMIT := $(word 5,$(SOURCE_ROOT_FIELDS))
SOURCE_TREE := $(word 6,$(SOURCE_ROOT_FIELDS))
CONTROL_COMMIT := $(word 7,$(SOURCE_ROOT_FIELDS))
# Pair-member descriptor: one space-separated row per build variant.
# Field order: profile hostenv builddir prefix namespace.  Accessors:
# $(call PAIR_PROFILE,LIST), $(call PAIR_HOSTENV,LIST), etc.  Adding a
# new variant means defining one new PAIR_<NAME> list, not five
# parallel _PROFILE/_HOSTENV/_BUILDDIR/_PREFIX/_NAMESPACE variables.
PAIR_PROFILE   = $(word 1,$1)
PAIR_HOSTENV   = $(word 2,$1)
PAIR_BUILDDIR  = $(word 3,$1)
PAIR_PREFIX    = $(word 4,$1)
PAIR_NAMESPACE = $(word 5,$1)

JOBS     ?= $(shell nproc)
DISTCC_JOBS ?= 6
# Build serialization lease: held across every Make-managed build-directory
# mutation so concurrent agents cannot configure, build, test, or clean the
# same build domain at once.  Direct Meson and Ninja commands bypass it.
BUILD_LOCK ?= $(HOME)/.cache/mesa-26-gororoba/mesa-build.lock
LOCK_WAIT ?= 7200
MESON ?= meson
NINJA ?= ninja
SUDO ?= sudo
FLOCK ?= flock
NINJA_TARGETS ?=
# Extra flags for the build ninja invocation.  NINJA_ARGS=-k0 keeps compiling
# after the first failure, so one warnings-as-errors run reports every
# diagnostic in the tree and the fixes get scoped from a complete list.
NINJA_ARGS ?=
MESON_TEST_ARGS ?=

define ACQUIRE_BUILD_LOCK
mkdir -p "$(dir $(BUILD_LOCK))"; \
exec 9>"$(BUILD_LOCK)"; \
$(FLOCK) -x -w "$(LOCK_WAIT)" 9 || { \
  echo "build lease unavailable: $(BUILD_LOCK)" >&2; \
  exit 1; \
};
endef

define CHECK_SOURCE_ROOT
python3 "$(SOURCE_ROOT_CONTROL)" check-source || exit 2
endef

define CHECK_LAYOUT
python3 "$(SOURCE_ROOT_CONTROL)" check-layout --operation="$1" || exit 2
endef

define PREPARE_SOURCE_IDENTITY
python3 "$(SOURCE_ROOT_CONTROL)" prepare-identity || exit 2
endef

define WRITE_SOURCE_IDENTITY
python3 "$(SOURCE_ROOT_CONTROL)" write-identity || exit 2
endef

define VERIFY_SOURCE_IDENTITY
python3 "$(SOURCE_ROOT_CONTROL)" verify-identity || exit 2
endef

# --- internal paths ---------------------------------------------------------
# The default profile sits at the top of configs/; every other profile (the
# release r300 sibling and the x130e terakan lanes) sits in configs/alternates/.
# Resolve PROFILE against both directories so a bare basename still names a
# profile and the per-profile rebuild-/install- targets need no path prefix.
CONFIG   := $(firstword $(wildcard $(CURDIR)/configs/$(PROFILE).meson $(CURDIR)/configs/alternates/$(PROFILE).meson))
ENVFILE  := $(CURDIR)/env/$(HOSTENV).env
TOOLCHAIN_NATIVE := $(BUILDDIR)/gororoba-toolchain.meson
COMPILER_CHAIN ?= ccache

# Compiler family the toolchain overlay resolves.  llvm selects a coherent
# clang/clang++/llvm-config major and the matching LLVM binutils; gnu selects
# gcc/g++ and leaves Meson on the GNU binutils it finds.  The gnu lane needs a
# profile that declares llvm='disabled', because gcc supplies no llvm-config and
# a mismatched one would configure Mesa against a different toolchain's LLVM.
COMPILER_FAMILY ?= llvm

ifeq (,$(CONFIG))
$(error unknown PROFILE='$(PROFILE)'; see `make list`)
endif

# `meson setup --native-file` loads option defaults from CONFIG.  The
# generated toolchain overlay keeps compiler package suffixes host-local
# instead of committing one LLVM major into every native file.
#
# PROFILE_DFLAGS re-asserts every [project options] entry as a -D flag on
# every setup/reconfigure.  Native-file values are defaults only: after a
# Meson option choice is removed (for example the retired amd_r300 selector),
# coredata resets the option to its default (vulkan-drivers=auto) and the
# native file does not re-apply.  auto on x86_64 pulls lavapipe while r300
# profiles keep llvm=disabled, so configure aborts.  CLI -D flags override
# that reset and keep the profile's declared surface.
PROFILE_DFLAGS_SCRIPT := $(CURDIR)/scripts/meson_profile_dflags.py
# -Dwerror=true is always on the CLI so a profile that drops the option, or a
# coredata reset of werror, cannot re-open a warnings-are-nonfatal configure.
# Profile dflags still re-assert every other project option (and werror again).
SETUP_ARGS := --native-file=$(CONFIG) --native-file=$(TOOLCHAIN_NATIVE) --reconfigure -Dwerror=true
# MODE=stable pins the same safe -O2 release level through a command-line
# override without enabling LTO.  Default (unset / clean / rebuild) keeps the
# config level.
ifeq ($(MODE),stable)
SETUP_ARGS += -Doptimization=2 -Db_lto=false
endif
MESON_SETUP_LOCAL_UNSET := CCACHE_PREFIX DISTCC_HOSTS DISTCC_PUMP INCLUDE_SERVER_PORT INCLUDE_SERVER_ARGS INCLUDE_SERVER_PID
LOCKED_TOOL_ENV := \
    PATH="$$PATH" \
    CCACHE_BASEDIR="$${CCACHE_BASEDIR:-}" \
    CCACHE_COMPILERCHECK="$${CCACHE_COMPILERCHECK:-}" \
    CCACHE_DEPEND="$${CCACHE_DEPEND:-}" \
    CCACHE_DIR="$${CCACHE_DIR:-}" \
    CCACHE_MAXSIZE="$${CCACHE_MAXSIZE:-}" \
    CCACHE_PATH="$${CCACHE_PATH:-}" \
    CCACHE_PREFIX="$${CCACHE_PREFIX:-}" \
    DISTCC_HOSTS="$${DISTCC_HOSTS:-}" \
    SCCACHE_DIR="$${SCCACHE_DIR:-}"

.PHONY: list source-root-check source-root-selection-test \
        vostro-prefix-check audit configure build clean clean-all distclean install \
        artifact-check test \
        rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache \
        install-4_r300_full_release_x86_64v1-clang22-distcc-cache \
        rebuild-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache \
        install-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache \
        rebuild-3_terakan_full_release_x86_64v1-clang22-distcc-cache \
        install-3_terakan_full_release_x86_64v1-clang22-distcc-cache \
        rebuild-4_terakan_full_debug_x86_64v1-clang22-distcc-cache \
        install-4_terakan_full_debug_x86_64v1-clang22-distcc-cache \
        rebuild-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
        install-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
        rebuild-6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache \
        install-6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache \
        rebuild-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache \
        install-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache \
        rebuild-2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache \
        install-2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache \
        rebuild-all-tiers audit-werror driver-root-audit \
        build-lease-test build-job-policy-test \
        pkg-mesa-gororoba deploy-mesa-gororoba \
        help

# Component-policy gate: every profile config must stay inside its
# allowlist row (build-infra/component-policy/radeon-minimal-allowlist.toml).
.PHONY: profile-audit
profile-audit:
	python3 $(CURDIR)/scripts/profile_audit.py --repo-root $(CURDIR)/..

source-root-check:
	@$(CHECK_SOURCE_ROOT)
	@echo "OK    source root: $(TOPSRC)"
	@echo "OK    source commit: $(SOURCE_COMMIT)"
	@echo "OK    source tree: $(SOURCE_TREE)"

# Driver-root gate: each AMD GPU family's Vulkan driver keeps its source root
# at the family's vulkan/ directory, and the build graph enters it there.
.PHONY: driver-root-audit
driver-root-audit:
	python3 $(CURDIR)/scripts/driver_root_audit.py --self-test
	python3 $(CURDIR)/scripts/driver_root_audit.py --repo-root $(CURDIR)/..

help:
	@echo "usage: make <target> PROFILE=<profile> [HOSTENV=<env>] [TOPSRC=<mesa-worktree>] [BUILDDIR=<path>] [PREFIX=<path>] [SYSCONFDIR=<path>]"
	@echo ""
	@echo "targets:"
	@echo "  configure         meson setup the builddir from configs/[alternates/]\$$(PROFILE).meson"
	@echo "  build             locked ninja -C \$$(BUILDDIR); NINJA_TARGETS=one target, NINJA_ARGS=-k0"
	@echo "  clean             rm -rf \$$(BUILDDIR) (active profile only)"
	@echo "  clean-all         remove the fixed repo-local build/ tree (all profiles)"
	@echo "  distclean         atomically clean + archive \$$(PREFIX) under one lease"
	@echo "  audit             verify host tools, env policy, profile-audit, and profile requirements"
	@echo "  profile-audit     fail when a profile selects drivers outside its allowlist row"
	@echo "  driver-root-audit fail when an AMD Vulkan driver root or its subdir() entry drifts"
	@echo "  source-root-check require TOPSRC to be an exact Mesa Git worktree root"
	@echo "  source-root-selection-test calibrate external TOPSRC selection and path guards"
	@echo "  install           sudo meson install --no-rebuild -C \$$(BUILDDIR)"
	@echo "  artifact-check    list installed Terakan/r600/r3v artifacts under \$$(PREFIX); warn on stale r300 ICDs"
	@echo "  test              locked meson test -C \$$(BUILDDIR); set MESON_TEST_ARGS for one test"
	@echo "  build-lease-test  verify mutation leases and path-safety guards"
	@echo "  build-job-policy-test verify nproc local jobs and the six-job distcc ceiling"
	@echo "  rebuild-<profile> clean + configure + build for that profile"
	@echo "  list              show available PROFILEs and HOSTENVs"
	@echo ""
	@echo "profile:   $(PROFILE)"
	@echo "hostenv:   $(HOSTENV)"
	@echo "topsrc:    $(TOPSRC)"
	@echo "builddir:  $(BUILDDIR)"
	@echo "prefix:    $(PREFIX)"
	@echo "compiler chain: $(COMPILER_CHAIN)"

list:
	@echo "available PROFILEs (configs/*.meson + configs/alternates/*.meson):"; \
	for f in configs/*.meson configs/alternates/*.meson; do echo "  $$(basename $$f .meson)"; done
	@echo "available HOSTENVs (env/*.env):"; \
	for f in env/*.env; do echo "  $$(basename $$f .env)"; done

vostro-prefix-check:
	@case "$(PROFILE):$(PREFIX)" in \
		[12]_r300_*:/usr/local/mesa-26-gororoba*|r300*-vostro-*:/usr/local/mesa-26-gororoba*) \
			echo "Vostro r300/r3v profiles install under /opt/local/mesa-26-gororoba*; refusing PREFIX=$(PREFIX)" >&2; \
			exit 2 ;; \
	esac

# Mechanical invariant gate (a prerequisite of audit, so `make audit` enforces it):
# every build profile must set warnings-as-errors; packaging PKGBUILDs that
# run meson setup with gallium-drivers/vulkan-drivers (this Mesa tree) must
# pass -Dwerror=true (sibling packages such as libva keep their own policy);
# and the /opt-scoped sanitizer and -O0 tiers must never carry system-Mesa
# activation (no /usr loader symlinks via _link_into_usr, no ld.so.conf.d
# drop-in).  These are the things that would let warnings pass silently or let
# an ASan driver become the /usr default and black-screen the login.  Cultural
# convention rots; this fails the build instead.
audit-werror:
	@rc=0; \
	for f in configs/*.meson configs/alternates/*.meson; do \
		grep -q '^werror = true' "$$f" || { echo "FAIL  $$f lacks 'werror = true'" >&2; rc=1; }; \
		if grep -qE '^werror[[:space:]]*=[[:space:]]*false' "$$f"; then echo "FAIL  $$f sets werror=false" >&2; rc=1; fi; \
	done; \
	for pb in packaging/*/PKGBUILD; do \
		[ -f "$$pb" ] || continue; \
		if grep -q 'meson setup' "$$pb" && grep -qE -- '-D[[:space:]]*(gallium-drivers|vulkan-drivers)=' "$$pb"; then \
			grep -qE -- '-D[[:space:]]*werror=true' "$$pb" \
				|| { echo "FAIL  $$pb runs mesa meson setup without -Dwerror=true" >&2; rc=1; }; \
		fi; \
	done; \
	for p in mesa-gororoba-debug-asan mesa-gororoba-debug-o0; do \
		pb="packaging/$$p/PKGBUILD"; \
		[ -f "$$pb" ] || continue; \
		if grep -q '_link_into_usr' "$$pb"; then echo "FAIL  $$pb (scoped tier) must not _link_into_usr" >&2; rc=1; fi; \
		if grep -qE 'ld\.so\.conf\.d/[^"]*\.conf' "$$pb"; then echo "FAIL  $$pb (scoped tier) must not drop an ld.so.conf.d activation" >&2; rc=1; fi; \
	done; \
	if [ $$rc -eq 0 ]; then echo "OK    audit-werror: profiles werror=true; packaging meson -Dwerror=true; prime/-o0 no /usr activation"; fi; \
	exit $$rc

audit: source-root-check source-root-selection-test \
       vostro-prefix-check audit-werror profile-audit driver-root-audit
	@failures=0; warnings=0; \
	fail() { echo "FAIL  $$*" >&2; failures=$$((failures + 1)); }; \
	warn() { echo "WARN  $$*" >&2; warnings=$$((warnings + 1)); }; \
	ok() { echo "OK    $$*"; }; \
	need_command() { if command -v "$$1" >/dev/null 2>&1; then ok "command $$1"; else fail "missing command $$1"; fi; }; \
	need_file() { if [ -f "$$1" ]; then ok "file $$1"; else fail "missing file $$1"; fi; }; \
	llvm_version_major() { \
		case "$$1" in \
			clang|clang++) "$$1" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p;q' ;; \
			llvm-config) "$$1" --version 2>/dev/null | sed 's/[.].*//;q' ;; \
			*) "$$1" --version 2>/dev/null | sed -n 's/.*LLVM version \([0-9][0-9]*\).*/\1/p' | sed -n '1p' ;; \
		esac; \
	}; \
	find_llvm_tool() { \
		tool="$$1"; version="$$2"; \
		for candidate in "$$tool-$$version" "$$tool$$version" "$$tool"; do \
			if command -v "$$candidate" >/dev/null 2>&1; then \
				if [ "$$candidate" = "$$tool" ]; then \
					detected="$$(llvm_version_major "$$candidate" || true)"; \
					[ "$$detected" = "$$version" ] || continue; \
				fi; \
				printf '%s\n' "$$candidate"; return 0; \
			fi; \
		done; \
		return 1; \
	}; \
	find_versioned_llvm_tool() { \
		tool="$$1"; version="$$2"; \
		for candidate in "$$tool-$$version" "$$tool$$version"; do \
			if command -v "$$candidate" >/dev/null 2>&1; then \
				printf '%s\n' "$$candidate"; return 0; \
			fi; \
		done; \
		return 1; \
	}; \
	find_audit_compiler_tool() { \
		case "$(COMPILER_CHAIN)" in \
			ccache|distcc) find_versioned_llvm_tool "$$1" "$$2" ;; \
			direct) find_llvm_tool "$$1" "$$2" ;; \
			*) return 1 ;; \
		esac; \
	}; \
	llvm_compiler_requirement() { \
		case "$(COMPILER_CHAIN)" in \
			ccache|distcc) printf '%s\n' "version-suffixed clang/clang++" ;; \
			*) printf '%s\n' "clang/clang++" ;; \
		esac; \
	}; \
	llvm_has_core() { \
		version="$$1"; \
		need_config="$${2:-1}"; \
		find_audit_compiler_tool clang "$$version" >/dev/null 2>&1 && \
		find_audit_compiler_tool clang++ "$$version" >/dev/null 2>&1 && \
		{ [ "$$need_config" -eq 0 ] || find_llvm_tool llvm-config "$$version" >/dev/null 2>&1; }; \
	}; \
	need_llvm_family() { \
		need_config="$${1:-1}"; \
		requested="$${MESA_LLVM_VERSION:-$${GOROROBA_LLVM_VERSION:-}}"; selected=""; \
		compiler_requirement="$$(llvm_compiler_requirement)"; \
		if [ -n "$$requested" ]; then \
			if llvm_has_core "$$requested" "$$need_config"; then selected="$$requested"; \
			else \
				if [ "$$need_config" = "1" ]; then \
					fail "requested LLVM major $$requested lacks required $$compiler_requirement/llvm-config tools on PATH"; \
				else \
					fail "requested LLVM major $$requested lacks required $$compiler_requirement tools on PATH"; \
				fi; \
				return; \
			fi; \
		else \
			for version in 22 21 20 19 18 17 16 15 14 13 12 11 10; do \
				if llvm_has_core "$$version" "$$need_config"; then selected="$$version"; break; fi; \
			done; \
			version=30; \
			while [ -z "$$selected" ] && [ "$$version" -ge 10 ]; do \
				if llvm_has_core "$$version" "$$need_config"; then selected="$$version"; break; fi; \
				version=$$((version - 1)); \
			done; \
		fi; \
		if [ -n "$$selected" ]; then \
			audit_clang_tool="$$(find_audit_compiler_tool clang "$$selected")"; \
			if [ "$$need_config" -eq 1 ]; then ok "LLVM family $$selected with $$compiler_requirement/llvm-config"; else ok "LLVM compiler family $$selected with $$compiler_requirement"; fi; \
		else fail "missing coherent LLVM compiler family"; fi; \
	}; \
	need_pkg_config() { if pkg-config --exists "$$1" 2>/dev/null; then ok "pkg-config $$1"; else fail "missing pkg-config dependency $$1"; fi; }; \
	echo "Terakan host audit"; \
	echo "profile: $(PROFILE)"; \
	echo "hostenv: $(HOSTENV)"; \
	echo "topsrc:  $(TOPSRC)"; echo; \
	need_file "$(CONFIG)"; need_file "$(ENVFILE)"; \
	if [ -f "$(ENVFILE)" ]; then . "$(ENVFILE)" || fail "cannot source host env $(ENVFILE)"; else fail "cannot source host env $(ENVFILE)"; fi; \
	if [ "$$failures" -gt 0 ]; then echo; echo "Terakan host audit failed: $$failures failure(s), $$warnings warning(s)." >&2; exit 1; fi; \
	if grep -Eq 'gallium-rusticl[[:space:]]*=[[:space:]]*true' "$(CONFIG)"; then rusticl_enabled=1; else rusticl_enabled=0; fi; \
	if grep -Eq "^[[:space:]]*llvm[[:space:]]*=[[:space:]]*'disabled'" "$(CONFIG)"; then needs_llvm_config=0; else needs_llvm_config=1; fi; \
	if ! grep -Eq "^[[:space:]]*platforms[[:space:]]*=" "$(CONFIG)"; then wayland_platform=1; \
	elif grep -Eq "^[[:space:]]*platforms[[:space:]]*=[^#]*'wayland'" "$(CONFIG)"; then wayland_platform=1; \
	elif grep -Eq "^[[:space:]]*platforms[[:space:]]*=[^#]*'auto'" "$(CONFIG)"; then wayland_platform=1; \
	else wayland_platform=0; fi; \
	for tool in meson ninja pkg-config python3; do need_command "$$tool"; done; \
	case "$(COMPILER_CHAIN)" in \
		ccache) need_command ccache; \
		        if grep -Eq 'export CCACHE_PREFIX=.*distcc' "$(ENVFILE)"; then \
		            need_command distcc; distcc_active=1; \
		        else distcc_active=0; fi ;; \
		distcc) need_command distcc; distcc_active=1 ;; \
		direct) distcc_active=0 ;; \
		*) fail "unknown COMPILER_CHAIN=$(COMPILER_CHAIN)"; distcc_active=0 ;; \
	esac; \
	need_llvm_family "$$needs_llvm_config"; \
	if [ "$$rusticl_enabled" -eq 1 ]; then need_command sccache; need_command rustc; \
	else \
		if command -v sccache >/dev/null 2>&1; then ok "optional command sccache"; else warn "optional sccache missing; required only for Rusticl recovery profiles"; fi; \
		if command -v rustc >/dev/null 2>&1; then ok "optional command rustc"; else warn "optional rustc missing; required only for Rusticl recovery profiles"; fi; \
	fi; \
	if python3 -c 'import mako, ply' >/dev/null 2>&1; then ok "python modules mako ply"; else fail "missing Python module mako or ply"; fi; \
	if python3 -c 'import sys; sys.version_info < (3, 12) or __import__("packaging")' >/dev/null 2>&1; then ok "python packaging requirement"; else fail "Python 3.12+ needs module packaging"; fi; \
	for pc in libdrm xcb-dri3 xcb-present xshmfence x11-xcb xrandr xcb-randr xcb-sync xcb-xfixes vulkan; do need_pkg_config "$$pc"; done; \
	if [ "$$wayland_platform" -eq 1 ]; then \
		for pc in wayland-client wayland-server wayland-protocols; do need_pkg_config "$$pc"; done; \
	else ok "profile does not request Wayland platform"; fi; \
	if command -v glslangValidator >/dev/null 2>&1; then ok "command glslangValidator"; else fail "missing command glslangValidator"; fi; \
	if [ "$$distcc_active" = "1" ]; then \
		if [ -r "$$HOME/.distcc/hosts" ]; then \
			if grep -Eq '(^|[[:space:]])[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+([/:[:space:]]|$$)' "$$HOME/.distcc/hosts"; then fail "raw IPv4 address found in ~/.distcc/hosts; use mDNS hostnames"; \
			else ok "distcc hosts use hostname policy"; fi; \
		else warn "~/.distcc/hosts is missing; distcc builds may run locally only"; fi; \
		if [ "$${GOROROBA_DISTCC_JOBS:-}" = "$(DISTCC_JOBS)" ]; then \
			ok "distcc aggregate build ceiling is $(DISTCC_JOBS) jobs"; \
		else \
			fail "distcc aggregate build ceiling is $${GOROROBA_DISTCC_JOBS:-unset}; expected $(DISTCC_JOBS)"; \
		fi; \
		distcc_slots="$$(printf '%s\n' "$${DISTCC_HOSTS:-}" | grep -oE '/[0-9]+' | tr -d / | awk '{sum += $$1} END {print sum + 0}')"; \
		if [ "$$distcc_slots" = "$(DISTCC_JOBS)" ]; then \
			ok "distcc host allocation totals $(DISTCC_JOBS) slots"; \
		else \
			fail "distcc host allocation totals $$distcc_slots slots; expected $(DISTCC_JOBS)"; \
		fi; \
		remote_distcc_spec=""; \
		for distcc_spec in $${DISTCC_HOSTS:-}; do \
			case "$$distcc_spec" in --*|localhost|localhost/*|localhost,*) continue ;; esac; \
			remote_distcc_spec="$$distcc_spec"; \
			break; \
		done; \
		if [ -z "$$remote_distcc_spec" ]; then \
			fail "distcc host allocation has no remote compiler"; \
		else \
			remote_distcc_host="$${remote_distcc_spec%%/*}"; \
			remote_distcc_host="$${remote_distcc_host%%,*}"; \
			if getent ahostsv4 "$$remote_distcc_host" >/dev/null 2>&1; then \
				ok "distcc remote hostname resolves: $$remote_distcc_host"; \
			else \
				fail "distcc remote hostname does not resolve: $$remote_distcc_host"; \
			fi; \
			probe_dir="$$(mktemp -d)"; \
			printf '%s\n' 'int gororoba_distcc_probe(void) { return 22; }' > "$$probe_dir/probe.c"; \
			if [ -n "$${audit_clang_tool:-}" ] && \
			   DISTCC_HOSTS="$$remote_distcc_spec" DISTCC_FALLBACK=0 \
			   distcc "$$audit_clang_tool" -Wall -Wextra -Werror \
			      -c "$$probe_dir/probe.c" -o "$$probe_dir/probe.o" \
			      >/dev/null 2>"$$probe_dir/probe.err"; then \
				ok "distcc remote Clang dispatch: $$remote_distcc_host ($$audit_clang_tool)"; \
			else \
				fail "distcc remote Clang dispatch failed: $$remote_distcc_host ($${audit_clang_tool:-unresolved})"; \
				sed -n '1,12p' "$$probe_dir/probe.err" >&2; \
			fi; \
			rm -rf "$$probe_dir"; \
		fi; \
	else ok "distcc not in active compiler chain; skipping host policy check"; fi; \
		if grep -Eq 'export CCACHE_PREFIX=.*distcc' "$(ENVFILE)"; then \
			ok "active lane defaults CCACHE_PREFIX to distcc"; \
		else \
			warn "active lane does not default CCACHE_PREFIX to distcc"; \
		fi; \
		if grep -q 'unset DISTCC_PUMP' "$(ENVFILE)"; then \
			ok "active lane clears distcc-pump state"; \
		else \
			warn "active lane does not clear distcc-pump state"; \
		fi; \
	if [ "$$rusticl_enabled" -eq 1 ]; then \
		echo; echo "Rusticl recovery lane checks"; need_command bindgen; need_command rustfmt; \
	else ok "profile disables Rusticl for Terakan daily lane"; fi; \
	echo; echo "Debian package hints for missing daily-lane tools:"; \
	echo "  meson ninja-build clang pkg-config ccache distcc sccache"; \
	echo "  libdrm-dev libxcb-dri3-dev libxcb-present-dev libxshmfence-dev"; \
	echo "  libx11-xcb-dev libxrandr-dev libxcb-randr0-dev libxcb-sync-dev"; \
	echo "  libxcb-xfixes0-dev libwayland-dev wayland-protocols python3-mako"; \
	echo "  python3-ply python3-packaging libvulkan-dev glslang-tools"; \
	if [ "$$rusticl_enabled" -eq 1 ]; then echo "  rust-bindgen llvm-dev libclang-dev"; fi; \
	if [ "$$failures" -gt 0 ]; then echo; echo "Terakan host audit failed: $$failures failure(s), $$warnings warning(s)." >&2; exit 1; fi; \
	echo; echo "Terakan host audit passed with $$warnings warning(s)."

configure: source-root-check vostro-prefix-check
	@$(call CHECK_LAYOUT,configure); \
	$(ACQUIRE_BUILD_LOCK) \
	$(PREPARE_SOURCE_IDENTITY); \
	mkdir -p "$(dir $(BUILDDIR))"; \
	test -f $(ENVFILE) && . $(ENVFILE) || true; \
	version_major() { \
		case "$$1" in \
			clang|clang++) "$$1" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p;q' ;; \
			llvm-config) "$$1" --version 2>/dev/null | sed 's/[.].*//;q' ;; \
			*) "$$1" --version 2>/dev/null | sed -n 's/.*LLVM version \([0-9][0-9]*\).*/\1/p' | sed -n '1p' ;; \
		esac; \
	}; \
	find_tool() { \
		tool="$$1"; version="$$2"; \
		for candidate in "$$tool-$$version" "$$tool$$version" "$$tool"; do \
			if command -v "$$candidate" >/dev/null 2>&1; then \
				if [ "$$candidate" = "$$tool" ]; then \
					detected="$$(version_major "$$candidate" || true)"; \
					[ "$$detected" = "$$version" ] || continue; \
				fi; \
				printf '%s\n' "$$candidate"; return 0; \
			fi; \
		done; \
		return 1; \
	}; \
	find_versioned_tool() { \
		tool="$$1"; version="$$2"; \
		for candidate in "$$tool-$$version" "$$tool$$version"; do \
			if command -v "$$candidate" >/dev/null 2>&1; then \
				printf '%s\n' "$$candidate"; return 0; \
			fi; \
		done; \
		return 1; \
	}; \
	find_compiler_tool() { \
		case "$(COMPILER_CHAIN)" in \
			ccache|distcc) find_versioned_tool "$$1" "$$2" ;; \
			direct) find_tool "$$1" "$$2" ;; \
			*) return 1 ;; \
		esac; \
	}; \
	resolve_tool() { \
		tool_name="$$(find_tool "$$1" "$$2")" || return 1; \
		command -v "$$tool_name"; \
	}; \
	has_core() { \
		version="$$1"; \
		need_config="$${2:-1}"; \
		find_compiler_tool clang "$$version" >/dev/null 2>&1 && \
		find_compiler_tool clang++ "$$version" >/dev/null 2>&1 && \
		{ [ "$$need_config" -eq 0 ] || find_tool llvm-config "$$version" >/dev/null 2>&1; }; \
	}; \
	if [ "$(COMPILER_FAMILY)" = "gnu" ]; then \
		grep -Eq "^[[:space:]]*llvm[[:space:]]*=[[:space:]]*'disabled'" "$(CONFIG)" || { \
			echo "COMPILER_FAMILY=gnu requires a profile with llvm='disabled'; $(CONFIG) enables LLVM" >&2; exit 1; }; \
		requested_gcc="$${GOROROBA_GCC_VERSION:-}"; \
		if [ -n "$$requested_gcc" ]; then \
			cc_cmd="$$(find_versioned_tool gcc "$$requested_gcc")" || { \
				echo "requested GCC major $$requested_gcc lacks gcc-$$requested_gcc" >&2; exit 1; }; \
			cxx_cmd="$$(find_versioned_tool g++ "$$requested_gcc")" || { \
				echo "requested GCC major $$requested_gcc lacks g++-$$requested_gcc" >&2; exit 1; }; \
		else \
			command -v gcc >/dev/null 2>&1 || { echo "no gcc on PATH" >&2; exit 1; }; \
			command -v g++ >/dev/null 2>&1 || { echo "no g++ on PATH" >&2; exit 1; }; \
			cc_cmd=gcc; cxx_cmd=g++; \
		fi; \
		cc_direct_cmd="$$(command -v "$$cc_cmd")"; \
		cxx_direct_cmd="$$(command -v "$$cxx_cmd")"; \
		llvm_config_cmd=""; llvm_ar_cmd=""; llvm_nm_cmd=""; \
		llvm_strip_cmd=""; llvm_objcopy_cmd=""; \
		toolchain_label="GCC $$("$$cc_cmd" -dumpversion | sed 's/[.].*//')"; \
	else \
	if grep -Eq "^[[:space:]]*llvm[[:space:]]*=[[:space:]]*'disabled'" "$(CONFIG)"; then needs_llvm_config=0; else needs_llvm_config=1; fi; \
	requested="$${MESA_LLVM_VERSION:-$${GOROROBA_LLVM_VERSION:-}}"; \
	llvm_version=""; \
	if [ -n "$$requested" ]; then \
		if ! has_core "$$requested" "$$needs_llvm_config"; then \
			if [ "$$needs_llvm_config" -eq 1 ]; then \
				echo "requested LLVM major $$requested lacks required clang/clang++/llvm-config tools" >&2; \
			else \
				echo "requested LLVM major $$requested lacks required clang/clang++ tools" >&2; \
			fi; \
			exit 1; \
		fi; \
		llvm_version="$$requested"; \
	else \
		for candidate_version in 22 21 20 19 18 17 16 15 14 13 12 11 10; do \
			if has_core "$$candidate_version" "$$needs_llvm_config"; then llvm_version="$$candidate_version"; break; fi; \
		done; \
		if [ -z "$$llvm_version" ]; then \
			candidate_version=30; \
			while [ "$$candidate_version" -ge 10 ]; do \
				if has_core "$$candidate_version" "$$needs_llvm_config"; then llvm_version="$$candidate_version"; break; fi; \
				candidate_version=$$((candidate_version - 1)); \
			done; \
		fi; \
	fi; \
	[ -n "$$llvm_version" ] || { echo "no matching LLVM compiler family found on PATH" >&2; exit 1; }; \
	cc_cmd="$$(find_compiler_tool clang "$$llvm_version")"; \
	cxx_cmd="$$(find_compiler_tool clang++ "$$llvm_version")"; \
	cc_direct_cmd="$$(resolve_tool clang "$$llvm_version")"; \
	cxx_direct_cmd="$$(resolve_tool clang++ "$$llvm_version")"; \
	llvm_config_cmd=""; \
	if [ "$$needs_llvm_config" -eq 1 ]; then llvm_config_cmd="$$(resolve_tool llvm-config "$$llvm_version")"; fi; \
	llvm_ar_cmd="$$(resolve_tool llvm-ar "$$llvm_version" || true)"; \
	llvm_nm_cmd="$$(resolve_tool llvm-nm "$$llvm_version" || true)"; \
	llvm_strip_cmd="$$(resolve_tool llvm-strip "$$llvm_version" || true)"; \
	llvm_objcopy_cmd="$$(resolve_tool llvm-objcopy "$$llvm_version" || true)"; \
	toolchain_label="LLVM $$llvm_version"; \
	fi; \
	mkdir -p "$$(dirname "$(TOOLCHAIN_NATIVE)")"; \
	{ \
		echo "# Generated by build-infra/Makefile."; \
		echo "# Do not edit; rerun make configure."; \
		echo; \
		echo "[binaries]"; \
		case "$(COMPILER_CHAIN)" in \
			ccache) \
				printf "c = ['ccache', '%s']\n" "$$cc_cmd"; \
				printf "cpp = ['ccache', '%s']\n" "$$cxx_cmd"; \
				;; \
			distcc) \
				printf "c = ['distcc', '%s']\n" "$$cc_cmd"; \
				printf "cpp = ['distcc', '%s']\n" "$$cxx_cmd"; \
				;; \
			direct) \
				printf "c = '%s'\n" "$$cc_direct_cmd"; \
				printf "cpp = '%s'\n" "$$cxx_direct_cmd"; \
				;; \
			*) echo "unknown COMPILER_CHAIN=$(COMPILER_CHAIN)" >&2; exit 2 ;; \
		esac; \
		echo "rust = ['sccache', 'rustc']"; \
		[ -z "$$llvm_config_cmd" ] || printf "llvm-config = '%s'\n" "$$llvm_config_cmd"; \
		[ -z "$$llvm_ar_cmd" ] || printf "ar = '%s'\n" "$$llvm_ar_cmd"; \
		[ -z "$$llvm_nm_cmd" ] || printf "nm = '%s'\n" "$$llvm_nm_cmd"; \
		[ -z "$$llvm_strip_cmd" ] || printf "strip = '%s'\n" "$$llvm_strip_cmd"; \
		[ -z "$$llvm_objcopy_cmd" ] || printf "objcopy = '%s'\n" "$$llvm_objcopy_cmd"; \
	} > "$(TOOLCHAIN_NATIVE)"; \
	echo "wrote Meson toolchain overlay: $(TOOLCHAIN_NATIVE) ($$toolchain_label, $(COMPILER_CHAIN))"; \
	test -f $(ENVFILE) && . $(ENVFILE) || true; \
	for var in $(MESON_SETUP_LOCAL_UNSET); do unset "$$var"; done; \
	profile_dflags=$$(python3 $(PROFILE_DFLAGS_SCRIPT) $(CONFIG)) || { \
		echo "failed to extract profile -D flags from $(CONFIG)" >&2; exit 2; }; \
	echo "profile -D flags: $$profile_dflags"; \
	$(MESON) setup $(SETUP_ARGS) $$profile_dflags \
		--prefix="$(PREFIX)" --sysconfdir="$(SYSCONFDIR)" \
		"$(BUILDDIR)" "$(TOPSRC)"; \
	$(WRITE_SOURCE_IDENTITY)

build: source-root-check
	@$(call CHECK_LAYOUT,build); \
	test -d "$(BUILDDIR)" || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }; \
	test -f "$(ENVFILE)" && . "$(ENVFILE)" || true; \
	$(ACQUIRE_BUILD_LOCK) \
	$(VERIFY_SOURCE_IDENTITY); \
	build_jobs="$(JOBS)"; \
	case "$(COMPILER_CHAIN):$${CCACHE_PREFIX:-}" in \
		distcc:*|ccache:distcc|ccache:*/distcc) \
			build_jobs="$${GOROROBA_DISTCC_JOBS:-$(DISTCC_JOBS)}" ;; \
		ccache:*) \
			[ -z "$${GOROROBA_DISTCC_JOBS:-}" ] || \
				build_jobs="$$GOROROBA_DISTCC_JOBS" ;; \
	esac; \
	case "$$build_jobs" in \
		''|*[!0-9]*|0) echo "invalid Ninja job count: $$build_jobs" >&2; exit 2 ;; \
	esac; \
	echo "ninja jobs: $$build_jobs ($(COMPILER_CHAIN))"; \
	env $(LOCKED_TOOL_ENV) \
	    $(NINJA) -C "$(BUILDDIR)" -j"$$build_jobs" $(NINJA_ARGS) $(NINJA_TARGETS)

clean:
	@$(ACQUIRE_BUILD_LOCK) \
	$(call CHECK_LAYOUT,clean); \
	rm -rf "$(BUILDDIR)"; \
	echo "cleaned: $(BUILDDIR)"

# Remove the entire repo-local build/ tree (every profile's builddir), not just
# the active PROFILE's.  The tree is gitignored (/build), so this only drops
# regenerable artifacts and never touches the source or an installed PREFIX.
clean-all:
	@$(ACQUIRE_BUILD_LOCK) \
	$(call CHECK_LAYOUT,clean-all); \
	rm -rf "$(BUILD_ROOT)"; \
	echo "cleaned: $(BUILD_ROOT) (all profile builddirs)"

distclean:
	@$(ACQUIRE_BUILD_LOCK) \
	$(call CHECK_LAYOUT,distclean); \
	rm -rf "$(BUILDDIR)"; \
	echo "cleaned: $(BUILDDIR)"; \
	if [ -e "$(PREFIX)" ]; then \
		archive="$(PREFIX).disabled.$$(date -u +%Y%m%dT%H%M%SZ)"; \
		$(SUDO) mv "$(PREFIX)" "$$archive"; \
		echo "archived prefix: $(PREFIX) -> $$archive"; \
	else \
		echo "prefix already absent: $(PREFIX)"; \
	fi

install: source-root-check vostro-prefix-check
	@$(call CHECK_LAYOUT,install); \
	test -d "$(BUILDDIR)" || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }
	@test -f "$(ENVFILE)" && . "$(ENVFILE)" || true; \
	mkdir -p "$(dir $(BUILD_LOCK))"; \
	env $(LOCKED_TOOL_ENV) \
	    $(FLOCK) -x -w $(LOCK_WAIT) "$(BUILD_LOCK)" bash -c 'set -euo pipefail; \
	    python3 "$(SOURCE_ROOT_CONTROL)" verify-identity; \
	    echo "[install] flock acquired; reconfiguring $(BUILDDIR) with --prefix=$(PREFIX)"; \
	    for var in $(MESON_SETUP_LOCAL_UNSET); do unset "$$var"; done; \
	    profile_dflags=$$(python3 $(PROFILE_DFLAGS_SCRIPT) $(CONFIG)) || { \
	      echo "failed to extract profile -D flags from $(CONFIG)" >&2; exit 2; }; \
	    $(MESON) setup $(SETUP_ARGS) $$profile_dflags --prefix="$(PREFIX)" --sysconfdir="$(SYSCONFDIR)" "$(BUILDDIR)" "$(TOPSRC)"; \
	    echo "[install] running meson install --no-rebuild --only-changed -C $(BUILDDIR)"; \
	    $(SUDO) $(MESON) install --no-rebuild --only-changed -C "$(BUILDDIR)"; \
	    : "sudo meson install writes meson-logs/install-log.txt as root; restore"; \
	    : "ownership so a later non-sudo makepkg of the same builddir can rewrite"; \
	    : "it instead of failing package() with a PermissionError under fakeroot"; \
	    $(SUDO) chown -R "$$(id -un):$$(id -gn)" "$(BUILDDIR)/meson-logs"; \
	    echo "[install] complete; releasing flock"'

artifact-check: vostro-prefix-check
	@$(call CHECK_LAYOUT,artifact)
	@test -d "$(PREFIX)" || { echo "prefix missing: $(PREFIX)"; exit 1; }
	find "$(PREFIX)" -maxdepth 5 -type f \
		\( -name "libvulkan_terascale.so" -o -name "libvulkan_r3v.so" -o \
		   -name "r600_dri.so" -o -name "r300_dri.so" -o \
		   -name "libdril_dri.so" -o -name "libgallium-*.so" -o \
		   -name "terascale_icd*.json" -o -name "r3v_icd*.json" \) \
		-printf "%p %TY-%Tm-%Td %TH:%TM:%TS %s bytes\n" | sort
	@stale=$$(find "$(PREFIX)" -maxdepth 6 -type f \
		\( -name "libvulkan_r300.so" -o -name "r300_icd*.json" -o \
		   -name "r300_devenv_icd*.json" \) 2>/dev/null | sort); \
	if [ -n "$$stale" ]; then \
		echo "WARN  stale pre-rename r300 Vulkan ICD artifacts still under $(PREFIX):" >&2; \
		echo "$$stale" | sed 's/^/WARN    /' >&2; \
		echo "WARN  Meson install does not remove absent files; the Vulkan loader still scans icd.d/*.json" >&2; \
	fi

test: source-root-check
	@$(call CHECK_LAYOUT,test); \
	test -d "$(BUILDDIR)" || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }; \
	test -f "$(ENVFILE)" && . "$(ENVFILE)" || true; \
	$(ACQUIRE_BUILD_LOCK) \
	$(VERIFY_SOURCE_IDENTITY); \
	env $(LOCKED_TOOL_ENV) \
	    $(MESON) test -C "$(BUILDDIR)" $(MESON_TEST_ARGS)

build-lease-test:
	@tmpdir="$$(mktemp -d)"; \
	canonical_builddir="$(TOPSRC)/build/.build-lease-test-$$$$"; \
	trap 'rm -rf "$$tmpdir" "$$canonical_builddir"' EXIT HUP INT TERM; \
	lock="$$tmpdir/mesa-build.lock"; \
	mkdir -p "$$tmpdir/build"; \
	assert_locked() { \
		label="$$1"; shift; \
		$(FLOCK) -x "$$lock" sleep 30 & holder="$$!"; \
		attempt=0; \
		while [ "$$attempt" -lt 100 ]; do \
			if ! $(FLOCK) -n "$$lock" true; then break; fi; \
			sleep 0.01; \
			attempt=$$((attempt + 1)); \
		done; \
		if $(FLOCK) -n "$$lock" true; then \
			echo "FAIL  $$label lock holder did not acquire $$lock" >&2; \
			kill "$$holder" 2>/dev/null || true; \
			wait "$$holder" 2>/dev/null || true; \
			exit 1; \
		fi; \
		if "$$@" >/dev/null 2>&1; then status=0; else status="$$?"; fi; \
		kill "$$holder" 2>/dev/null || true; \
		wait "$$holder" 2>/dev/null || true; \
		if [ "$$status" -eq 0 ]; then \
			echo "FAIL  $$label bypassed the held build lease" >&2; \
			exit 1; \
		fi; \
		echo "OK    $$label rejects a held build lease"; \
	}; \
	assert_locked configure $(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		BUILDDIR="$$tmpdir/configure" BUILD_LOCK="$$lock" LOCK_WAIT=0 MESON=true; \
	assert_locked build $(MAKE) --no-print-directory -C "$(CURDIR)" build \
		BUILDDIR="$$tmpdir/build" BUILD_LOCK="$$lock" LOCK_WAIT=0 NINJA=true; \
	assert_locked test $(MAKE) --no-print-directory -C "$(CURDIR)" test \
		BUILDDIR="$$tmpdir/build" BUILD_LOCK="$$lock" LOCK_WAIT=0 MESON=true; \
	assert_locked clean $(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		BUILDDIR="$$tmpdir/build" BUILD_LOCK="$$lock" LOCK_WAIT=0; \
	assert_locked clean-all $(MAKE) --no-print-directory -C "$(CURDIR)" clean-all \
		BUILD_ROOT="$$tmpdir/build-root" BUILD_LOCK="$$lock" LOCK_WAIT=0; \
	assert_locked distclean $(MAKE) --no-print-directory -C "$(CURDIR)" distclean \
		BUILD_ROOT="$$tmpdir" BUILDDIR="$$tmpdir/build" PREFIX="$$tmpdir/prefix" \
		BUILD_LOCK="$$lock" LOCK_WAIT=0; \
	mkdir -p "$$tmpdir/unsafe-sentinel"; \
	if $(MAKE) --no-print-directory -C "$(CURDIR)" clean-all \
		BUILD_ROOT=/ BUILD_LOCK="$$tmpdir/unsafe.lock" LOCK_WAIT=0 \
		>/dev/null 2>&1; then \
		echo "FAIL  clean-all accepted BUILD_ROOT=/" >&2; exit 1; \
	fi; \
	test -d "$$tmpdir/unsafe-sentinel" || { \
		echo "FAIL  clean-all safety check removed the sentinel" >&2; exit 1; \
	}; \
	echo "OK    clean-all rejects BUILD_ROOT=/"; \
	mkdir -p "$$canonical_builddir"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		BUILD_ROOT="$(TOPSRC)/build" BUILDDIR="$$canonical_builddir" \
		BUILD_LOCK="$$tmpdir/canonical.lock" LOCK_WAIT=0 >/dev/null; \
	test ! -e "$$canonical_builddir" || { \
		echo "FAIL  clean rejected the canonical repository build root" >&2; exit 1; \
	}; \
	echo "OK    clean accepts the canonical repository build root"; \
	if $(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		BUILD_ROOT="$(TOPSRC)/.build-lease-source-root-$$$$" \
		BUILDDIR="$(TOPSRC)/.build-lease-source-root-$$$$/probe" \
		BUILD_LOCK="$$tmpdir/source-tree.lock" LOCK_WAIT=0 \
		>/dev/null 2>&1; then \
		echo "FAIL  clean accepted a noncanonical source-tree build root" >&2; exit 1; \
	fi; \
	echo "OK    clean rejects noncanonical source-tree build roots"; \
	unsafe_prefix="/usr/local/.mesa-build-prefix-guard-$$$$"; \
	test ! -e "$$unsafe_prefix" || { \
		echo "FAIL  system-prefix test path already exists: $$unsafe_prefix" >&2; exit 1; \
	}; \
	if prefix_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" distclean \
		BUILD_ROOT="$$tmpdir" BUILDDIR="$$tmpdir/build" \
		PREFIX="$$unsafe_prefix" BUILD_LOCK="$$tmpdir/prefix.lock" \
		LOCK_WAIT=0 SUDO=false 2>&1 )"; then prefix_status=0; \
	else prefix_status="$$?"; fi; \
	if [ "$$prefix_status" -ne 2 ] || \
	   ! printf '%s\n' "$$prefix_output" | \
	      grep -F "refusing unsafe PREFIX: $$unsafe_prefix" >/dev/null; then \
		echo "FAIL  distclean prefix guard returned $$prefix_status" >&2; \
		printf '%s\n' "$$prefix_output" >&2; exit 1; \
	fi; \
	echo "OK    distclean rejects system-prefix descendants"; \
	if install_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" install \
		BUILD_ROOT="$$tmpdir" BUILDDIR="$$tmpdir/build" PREFIX="$$unsafe_prefix" \
		BUILD_LOCK="$$tmpdir/install-prefix.lock" LOCK_WAIT=0 \
		SUDO=false 2>&1 )"; then install_status=0; \
	else install_status="$$?"; fi; \
	if [ "$$install_status" -ne 2 ] || \
	   ! printf '%s\n' "$$install_output" | \
	      grep -F "refusing unsafe PREFIX: $$unsafe_prefix" >/dev/null; then \
		echo "FAIL  install prefix guard returned $$install_status" >&2; \
		printf '%s\n' "$$install_output" >&2; exit 1; \
	fi; \
	echo "OK    install rejects system-prefix descendants"; \
	$(call CHECK_LAYOUT,artifact); \
	echo "OK    prefix guard accepts the canonical profile prefix"; \
	install_plan="$$( $(MAKE) --no-print-directory -n -C "$(CURDIR)" install \
		BUILD_ROOT="$$tmpdir" BUILDDIR="$$tmpdir/build" PREFIX="$$tmpdir/prefix" \
		SYSCONFDIR="$$tmpdir/etc" 2>/dev/null )"; \
	printf '%s\n' "$$install_plan" | grep -F -- "--sysconfdir=\"$$tmpdir/etc\"" >/dev/null || { \
		echo "FAIL  install reconfigure omitted SYSCONFDIR" >&2; exit 1; \
	}; \
	echo "OK    install reconfigure preserves SYSCONFDIR"; \
	distclean_plan="$$( $(MAKE) --no-print-directory -n -C "$(CURDIR)" distclean \
		BUILD_ROOT="$$tmpdir" BUILDDIR="$$tmpdir/build" \
		PREFIX="$$tmpdir/prefix" BUILD_LOCK="$$tmpdir/plan.lock" )"; \
	flock_cmd="$(FLOCK)"; \
	flock_base="$$(basename -- "$$flock_cmd")"; \
	flock_count="$$(printf '%s\n' "$$distclean_plan" | grep -c -- "$$flock_base")"; \
	test "$$flock_count" -eq 1 || { \
		echo "FAIL  distclean plan uses $$flock_count $$flock_base leases" >&2; exit 1; \
	}; \
	echo "OK    distclean holds one build lease"

source-root-selection-test:
	@tmpdir="$$(mktemp -d)"; \
	trap 'rm -rf "$$tmpdir"' EXIT HUP INT TERM; \
	source_root="$$tmpdir/source"; \
	mkdir -p "$$source_root"; \
	git -C "$$source_root" init -q; \
	touch "$$source_root/meson.build" "$$source_root/meson.options"; \
	printf '/build/\n' > "$$source_root/.gitignore"; \
	git -C "$$source_root" add .gitignore meson.build meson.options; \
	git -C "$$source_root" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: add Mesa root markers"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" source-root-check \
		TOPSRC="$$source_root" >/dev/null; \
	echo "OK    external TOPSRC accepts a clean committed Mesa worktree"; \
	nested_root="$$source_root/nested"; \
	mkdir -p "$$nested_root"; \
	touch "$$nested_root/meson.build" "$$nested_root/meson.options"; \
	if nested_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		source-root-check TOPSRC="$$nested_root" 2>&1 )"; then \
		echo "FAIL  source-root-check accepted a nested Git directory" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$nested_output" | \
		grep -F "TOPSRC is not the Git worktree root" >/dev/null || { \
		echo "FAIL  nested TOPSRC returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$nested_output" >&2; exit 1; \
	}; \
	echo "OK    external TOPSRC rejects a nested Git directory"; \
	rm -rf "$$nested_root"; \
	missing_root="$$tmpdir/missing-options"; \
	mkdir -p "$$missing_root"; \
	git -C "$$missing_root" init -q; \
	touch "$$missing_root/meson.build"; \
	git -C "$$missing_root" add meson.build; \
	git -C "$$missing_root" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: omit Mesa options"; \
	if missing_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		source-root-check TOPSRC="$$missing_root" 2>&1 )"; then \
		echo "FAIL  source-root-check accepted a tree without Mesa options" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$missing_output" | \
		grep -F "TOPSRC lacks meson.options or meson_options.txt" >/dev/null || { \
		echo "FAIL  incomplete TOPSRC returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$missing_output" >&2; exit 1; \
	}; \
	echo "OK    external TOPSRC rejects an incomplete Mesa root"; \
	unborn_root="$$tmpdir/unborn"; \
	mkdir -p "$$unborn_root"; \
	git -C "$$unborn_root" init -q; \
	touch "$$unborn_root/meson.build" "$$unborn_root/meson.options"; \
	if unborn_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		source-root-check TOPSRC="$$unborn_root" 2>&1 )"; then \
		echo "FAIL  source-root-check accepted an unborn Git worktree" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$unborn_output" | \
		grep -F "HEAD^{commit}" >/dev/null || { \
		echo "FAIL  unborn TOPSRC returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$unborn_output" >&2; exit 1; \
	}; \
	echo "OK    external TOPSRC rejects an unborn Git worktree"; \
	legacy_root="$$tmpdir/legacy-options"; \
	mkdir -p "$$legacy_root"; \
	git -C "$$legacy_root" init -q; \
	touch "$$legacy_root/meson.build" "$$legacy_root/meson_options.txt"; \
	git -C "$$legacy_root" add meson.build meson_options.txt; \
	git -C "$$legacy_root" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: add legacy Mesa root markers"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" source-root-check \
		TOPSRC="$$legacy_root" >/dev/null; \
	echo "OK    external TOPSRC accepts legacy meson_options.txt"; \
	if injection_output="$$( $(MAKE) --no-print-directory -n -C "$(CURDIR)" \
		configure TOPSRC='source;id;#' 2>&1 )"; then \
		echo "FAIL  source-root-check accepted shell metacharacters" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$injection_output" | \
		grep -F "contains unsupported path characters" >/dev/null || { \
		echo "FAIL  metacharacter TOPSRC returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$injection_output" >&2; exit 1; \
	}; \
	if printf '%s\n' "$$injection_output" | grep -q '^uid='; then \
		echo "FAIL  TOPSRC metacharacters executed a shell command" >&2; \
		exit 1; \
	fi; \
	echo "OK    external TOPSRC rejects shell metacharacters before planning"; \
	if whitespace_output="$$( $(MAKE) --no-print-directory -n -C "$(CURDIR)" \
		configure TOPSRC="$$tmpdir/source root" 2>&1 )"; then \
		echo "FAIL  source-root-check accepted whitespace" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$whitespace_output" | \
		grep -F "contains unsupported path characters" >/dev/null || { \
		echo "FAIL  whitespace TOPSRC returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$whitespace_output" >&2; exit 1; \
	}; \
	echo "OK    external TOPSRC rejects whitespace before planning"; \
	source_build_root="$$source_root/build"; \
	if source_build_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		clean TOPSRC="$$source_root" BUILD_ROOT="$$source_build_root" \
		BUILDDIR="$$source_build_root/probe" \
		BUILD_LOCK="$$tmpdir/source-build.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  clean accepted a build directory inside external TOPSRC" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$source_build_output" | \
		grep -E "refusing (BUILD_ROOT|BUILDDIR) inside external TOPSRC" >/dev/null || { \
		echo "FAIL  source-tree BUILDDIR returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$source_build_output" >&2; exit 1; \
	}; \
	echo "OK    external TOPSRC rejects in-source build directories"; \
	control_build_root="$(CONTROL_ROOT)/build"; \
	if control_build_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		clean TOPSRC="$$source_root" BUILD_ROOT="$$control_build_root" \
		BUILDDIR="$$control_build_root/external-source-probe" \
		BUILD_LOCK="$$tmpdir/control-build.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  clean accepted a build directory in the control worktree" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$control_build_output" | \
		grep -E "refusing (BUILD_ROOT|BUILDDIR) inside the control worktree" >/dev/null || { \
		echo "FAIL  control-worktree BUILDDIR returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$control_build_output" >&2; exit 1; \
	}; \
	echo "OK    external TOPSRC protects the control worktree build tree"; \
	if control_prefix_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		artifact-check TOPSRC="$$source_root" \
		BUILD_ROOT="$$tmpdir/control-prefix-build" \
		BUILDDIR="$$tmpdir/control-prefix-build/probe" \
		PREFIX="$(CONTROL_ROOT)/external-prefix" 2>&1 )"; then \
		echo "FAIL  artifact-check accepted a prefix in the control worktree" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$control_prefix_output" | \
		grep -F "refusing PREFIX inside the control worktree" >/dev/null || { \
		echo "FAIL  control-worktree PREFIX returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$control_prefix_output" >&2; exit 1; \
	}; \
	echo "OK    external TOPSRC protects the control worktree prefix"; \
	safe_build_root="$$tmpdir/safe-build"; \
	outside_root="$$tmpdir/outside"; \
	mkdir -p "$$safe_build_root" "$$outside_root/victim"; \
	touch "$$outside_root/victim/sentinel"; \
	ln -s "$$outside_root" "$$safe_build_root/escape"; \
	if symlink_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		clean TOPSRC="$$source_root" BUILD_ROOT="$$safe_build_root" \
		BUILDDIR="$$safe_build_root/escape/victim" \
		BUILD_LOCK="$$tmpdir/symlink.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  clean accepted a symlink escape" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$symlink_output" | \
		grep -F "refusing BUILDDIR outside BUILD_ROOT" >/dev/null || { \
		echo "FAIL  symlink escape returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$symlink_output" >&2; exit 1; \
	}; \
	test -f "$$outside_root/victim/sentinel" || { \
		echo "FAIL  clean removed the symlink-escape sentinel" >&2; \
		exit 1; \
	}; \
	echo "OK    physical containment rejects symlink escapes"; \
	external_build_root="$$tmpdir/build"; \
	external_builddir="$$external_build_root/probe"; \
	mkdir -p "$$external_builddir"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		TOPSRC="$$source_root" BUILD_ROOT="$$external_build_root" \
		BUILDDIR="$$external_builddir" \
		BUILD_LOCK="$$tmpdir/external-build.lock" LOCK_WAIT=0 >/dev/null; \
	test ! -e "$$external_builddir" || { \
		echo "FAIL  clean left the external build directory in place" >&2; \
		exit 1; \
	}; \
	echo "OK    external TOPSRC accepts an isolated external build root"; \
	mkdir -p "$$source_build_root"; \
	touch "$$source_build_root/sentinel"; \
	if clean_all_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		clean-all TOPSRC="$$source_root" BUILD_ROOT="$$source_build_root" \
		BUILDDIR="$$source_build_root/probe" \
		BUILD_LOCK="$$tmpdir/clean-all.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  clean-all accepted an external TOPSRC" >&2; \
		exit 1; \
	fi; \
	test -f "$$source_build_root/sentinel" || { \
		echo "FAIL  external clean-all removed TOPSRC/build" >&2; \
		exit 1; \
	}; \
	echo "OK    external clean-all stays fail-closed"; \
	configure_plan="$$( $(MAKE) --no-print-directory -n -C "$(CURDIR)" \
		configure TOPSRC="$$source_root" BUILD_ROOT="$$external_build_root" \
		BUILDDIR="$$external_build_root/configure" \
		BUILD_LOCK="$$tmpdir/configure.lock" LOCK_WAIT=0 \
		MESON=meson PROFILE="$(PROFILE)" 2>/dev/null )"; \
	printf '%s\n' "$$configure_plan" | grep -F "meson setup" >/dev/null && \
	printf '%s\n' "$$configure_plan" | grep -F "\"$$source_root\"" >/dev/null || { \
		echo "FAIL  configure does not pass external TOPSRC to Meson" >&2; \
		printf '%s\n' "$$configure_plan" >&2; exit 1; \
	}; \
	echo "OK    configure passes the exact external TOPSRC to Meson"; \
	unbound_build_root="$$tmpdir/unbound-build"; \
	unbound_builddir="$$unbound_build_root/probe"; \
	mkdir -p "$$unbound_builddir"; \
	touch "$$unbound_builddir/sentinel"; \
	if unbound_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$unbound_build_root" \
		BUILDDIR="$$unbound_builddir" PREFIX="$$tmpdir/unbound-prefix" \
		BUILD_LOCK="$$tmpdir/unbound.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true 2>&1 )"; then \
		echo "FAIL  configure adopted a nonempty unbound build directory" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$unbound_output" | \
		grep -F "external build directory lacks a source identity" >/dev/null || { \
		echo "FAIL  unbound build directory returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$unbound_output" >&2; exit 1; \
	}; \
	test -f "$$unbound_builddir/sentinel" || { \
		echo "FAIL  configure mutated the unbound build directory" >&2; \
		exit 1; \
	}; \
	echo "OK    configure rejects a nonempty unbound build directory"; \
	identity_build_root="$$tmpdir/identity-build"; \
	identity_builddir="$$identity_build_root/probe"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$tmpdir/prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true >/dev/null; \
	test -f "$$identity_builddir/.gororoba-source-identity.json" || { \
		echo "FAIL  configure omitted the external source identity" >&2; \
		exit 1; \
	}; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$tmpdir/prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA=true >/dev/null; \
	echo "OK    external build verifies the configure-time identity"; \
	printf '%s\n' "identity drift" >> "$$source_root/meson.build"; \
	git -C "$$source_root" add meson.build; \
	git -C "$$source_root" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: move external source identity"; \
	if drift_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$tmpdir/prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA=true 2>&1 )"; then \
		echo "FAIL  build accepted a moving external source identity" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$drift_output" | \
		grep -F "external source identity drift" >/dev/null || { \
		echo "FAIL  source identity drift returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$drift_output" >&2; exit 1; \
	}; \
	echo "OK    external build rejects source commit drift"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$tmpdir/prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true >/dev/null; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$tmpdir/prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA=true >/dev/null; \
	echo "OK    configure explicitly refreshes source identity"; \
	touch "$$source_root/untracked"; \
	if dirty_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		source-root-check TOPSRC="$$source_root" 2>&1 )"; then \
		echo "FAIL  source-root-check accepted a dirty external worktree" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$dirty_output" | \
		grep -F "external TOPSRC is dirty" >/dev/null || { \
		echo "FAIL  dirty TOPSRC returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$dirty_output" >&2; exit 1; \
	}; \
	rm -f "$$source_root/untracked"; \
	echo "OK    external TOPSRC rejects dirty source state"

build-job-policy-test:
	@tmpdir="$$(mktemp -d)"; \
	trap 'rm -rf "$$tmpdir"' EXIT HUP INT TERM; \
	mkdir -p "$$tmpdir/build"; \
	local_jobs="$$(nproc)"; \
	local_output="$$( $(MAKE) --no-print-directory -s -C "$(CURDIR)" build \
		HOSTENV=job-policy-local COMPILER_CHAIN=direct \
		BUILD_ROOT="$$tmpdir" BUILDDIR="$$tmpdir/build" BUILD_LOCK="$$tmpdir/local.lock" \
		NINJA=echo )"; \
	printf '%s\n' "$$local_output" | grep -F -- "-j$$local_jobs" >/dev/null || { \
		echo "FAIL  local build did not use nproc=$$local_jobs" >&2; \
		printf '%s\n' "$$local_output" >&2; exit 1; \
	}; \
	echo "OK    local build uses nproc=$$local_jobs"; \
	distcc_output="$$( $(MAKE) --no-print-directory -s -C "$(CURDIR)" build \
		HOSTENV=job-policy-local COMPILER_CHAIN=distcc \
		BUILD_ROOT="$$tmpdir" BUILDDIR="$$tmpdir/build" BUILD_LOCK="$$tmpdir/distcc.lock" \
		NINJA=echo )"; \
	printf '%s\n' "$$distcc_output" | grep -F -- "-j$(DISTCC_JOBS)" >/dev/null || { \
		echo "FAIL  distcc build did not use $(DISTCC_JOBS) jobs" >&2; \
		printf '%s\n' "$$distcc_output" >&2; exit 1; \
	}; \
	echo "OK    distcc build uses $(DISTCC_JOBS) jobs"; \
	ccache_distcc_output="$$( CCACHE_PREFIX=/usr/bin/distcc \
		$(MAKE) --no-print-directory -s -C "$(CURDIR)" build \
		HOSTENV=job-policy-local COMPILER_CHAIN=ccache \
		BUILD_ROOT="$$tmpdir" BUILDDIR="$$tmpdir/build" BUILD_LOCK="$$tmpdir/ccache-distcc.lock" \
		NINJA=echo )"; \
	printf '%s\n' "$$ccache_distcc_output" | grep -F -- "-j$(DISTCC_JOBS)" >/dev/null || { \
		echo "FAIL  ccache plus distcc build did not use $(DISTCC_JOBS) jobs" >&2; \
		printf '%s\n' "$$ccache_distcc_output" >&2; exit 1; \
	}; \
	echo "OK    ccache plus distcc build uses $(DISTCC_JOBS) jobs"

define CLEAN_CONFIGURE_BUILD_PAIR
	$(MAKE) clean \
	    PROFILE=$(call PAIR_PROFILE,$1) \
	    HOSTENV=$(call PAIR_HOSTENV,$1) \
	    BUILDDIR=$(call PAIR_BUILDDIR,$1) \
	    PREFIX=$(call PAIR_PREFIX,$1) \
	    INSTALL_NAMESPACE=$(call PAIR_NAMESPACE,$1)
	$(MAKE) configure \
	    PROFILE=$(call PAIR_PROFILE,$1) \
	    HOSTENV=$(call PAIR_HOSTENV,$1) \
	    BUILDDIR=$(call PAIR_BUILDDIR,$1) \
	    PREFIX=$(call PAIR_PREFIX,$1) \
	    INSTALL_NAMESPACE=$(call PAIR_NAMESPACE,$1)
	$(MAKE) build \
	    PROFILE=$(call PAIR_PROFILE,$1) \
	    HOSTENV=$(call PAIR_HOSTENV,$1) \
	    BUILDDIR=$(call PAIR_BUILDDIR,$1) \
	    PREFIX=$(call PAIR_PREFIX,$1) \
	    INSTALL_NAMESPACE=$(call PAIR_NAMESPACE,$1)
endef

define INSTALL_PAIR
	$(MAKE) install \
	    PROFILE=$(call PAIR_PROFILE,$1) \
	    HOSTENV=$(call PAIR_HOSTENV,$1) \
	    BUILDDIR=$(call PAIR_BUILDDIR,$1) \
	    PREFIX=$(call PAIR_PREFIX,$1) \
	    INSTALL_NAMESPACE=$(call PAIR_NAMESPACE,$1)
endef

# Canonical full-feature r300 RELEASE build: maximal GL/GLES surface plus the
# ati_r300 ICD in one artifact (profile 4_).  Installs to the release prefix.
rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba
	$(MAKE) configure PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba
	$(MAKE) build PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba

install-4_r300_full_release_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba

# Canonical full-feature r300 DEBUG build: same surface, assertions live (-O2 -g),
# installs to the -debug prefix (profile 3_).
rebuild-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized
	$(MAKE) configure PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized
	$(MAKE) build PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized

install-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized

# Canonical r600/terakan RELEASE build: maximal terakan stack (r600, zink,
# softpipe, llvmpipe, amd_terascale) plus Rusticl in one artifact (profile 3_).
# Installs to the release prefix.
rebuild-3_terakan_full_release_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean PROFILE=3_terakan_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba
	$(MAKE) configure PROFILE=3_terakan_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba
	$(MAKE) build PROFILE=3_terakan_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba

install-3_terakan_full_release_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=3_terakan_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba

# Canonical r600/terakan DEBUG build: same surface, assertions live (-O2 -g),
# Rusticl live, installs to the -debug prefix (profile 4_).
rebuild-4_terakan_full_debug_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean PROFILE=4_terakan_full_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized
	$(MAKE) configure PROFILE=4_terakan_full_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized
	$(MAKE) build PROFILE=4_terakan_full_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized

install-4_terakan_full_debug_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=4_terakan_full_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized

# r600/terakan RELEASE build without Rusticl: use when bindgen drift makes
# Rusticl unbuildable (profile 5_).  Installs to the release prefix.
rebuild-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba
	$(MAKE) configure PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba
	$(MAKE) build PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba

install-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/local/mesa-26-gororoba

# r600/terakan DEBUG build without Rusticl: use when bindgen drift makes
# Rusticl unbuildable (profile 6_).  Installs to the -debug prefix.
rebuild-6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean PROFILE=6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized
	$(MAKE) configure PROFILE=6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized
	$(MAKE) build PROFILE=6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized

install-6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-optimized

# PRIME r300 driver: ASan+UBSan+extended sanitizers at -O0 (profile 1_).  SCOPED
# install to /opt/mesa-gororoba-debug-asan -- NEVER the system Mesa (an ASan
# libGL/libvulkan as the default Mesa aborts every GL/VK client and can
# black-screen the login).  The prefix must equal the mesa-gororoba-debug-asan
# PKGBUILD _prefix so the repack stages to the right tree; it is reached only
# through mesa-gororoba-debug-asan-run.
rebuild-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean PROFILE=1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-asan
	$(MAKE) configure PROFILE=1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-asan
	$(MAKE) build PROFILE=1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-asan

install-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-asan

# -O0 normal-debug r300 driver, no sanitizer (profile 2_).  SCOPED install to
# /opt/mesa-gororoba-debug-o0 -- NOT the system Mesa; reached through
# mesa-gororoba-debug-o0-run.  The prefix must equal the mesa-gororoba-debug-o0
# PKGBUILD _prefix.
rebuild-2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean PROFILE=2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-o0
	$(MAKE) configure PROFILE=2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-o0
	$(MAKE) build PROFILE=2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-o0

install-2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=/opt/mesa-gororoba-debug-o0

# Coherent GPU debug stack ("if one is updated they are all updated"): rebuild the
# four r300 driver tiers (prime 1_, debug-o0 2_, debug-optimized 3_, release 4_)
# from the current tree so they share the source-of-truth pkgver and stay in
# lockstep.  The per-package PKGBUILDs (mesa-gororoba-debug-asan,
# mesa-gororoba-debug-o0, mesa-gororoba-debug-optimized, mesa-gororoba) then repack
# those builddirs, umr-gororoba repacks its CMake build, and the
# mesa-gororoba-debug-tools meta-package pins them together.  Pacman has no
# atomic "update all"; this target + the meta-package pin are the idiom.  Note: only
# the debug-optimized and release tiers ever become the /usr system Mesa.
rebuild-all-tiers: \
	rebuild-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache \
	rebuild-2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache \
	rebuild-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache \
	rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache
	@echo "rebuilt the four r300 driver tiers (prime + debug-o0 + debug-optimized + release) from the current tree."
	@echo "package: for p in mesa-gororoba-debug-asan mesa-gororoba-debug-o0 mesa-gororoba-debug-optimized mesa-gororoba umr-gororoba mesa-gororoba-debug-tools; do (cd packaging/\$$p && makepkg -f --noconfirm); done"

# mesa-gororoba CachyOS-style /usr drop-in package (r300 GL + llvmpipe +
# ati_r300/r3v Vulkan ICD), built -march=x86-64 so it runs on the K8 Vostro.
# pkg-mesa-gororoba runs makepkg and repo-adds the artifact to a local
# [gororoba] repo; deploy-mesa-gororoba ships it to TARGET_HOST and installs it
# (atomic swap over stock mesa; --overwrite forces any file collision).  Build
# on a fast x86-64 host whose llvm major matches the target's llvm-libs.
# Override the pinned source commit with GOROROBA_COMMIT=<sha>.  Artifacts live
# under ../build/ (the sanctioned build area), not as workspace siblings.
GOROROBA_PKG_SRC   ?= $(CURDIR)/packaging/mesa-gororoba
GOROROBA_PKG_DIR   ?= $(abspath $(CURDIR)/../build/mesa-gororoba-pkg)
GOROROBA_REPO_DIR  ?= $(abspath $(CURDIR)/../build/mesa-gororoba-repo)
GOROROBA_REPO_NAME ?= gororoba
TARGET_HOST        ?= cachyos-vostro1000

pkg-mesa-gororoba:
	mkdir -p "$(GOROROBA_PKG_DIR)/tmp" "$(GOROROBA_REPO_DIR)"
	find "$(GOROROBA_PKG_SRC)" -maxdepth 1 -type f -exec cp -t "$(GOROROBA_PKG_DIR)/" {} +
	[ -z "$(GOROROBA_COMMIT)" ] || sed -i 's/^_commit=.*/_commit=$(GOROROBA_COMMIT)/' "$(GOROROBA_PKG_DIR)/PKGBUILD"
	cd "$(GOROROBA_PKG_DIR)" && TMPDIR="$(GOROROBA_PKG_DIR)/tmp" SRCDEST="$(GOROROBA_PKG_DIR)" BUILDDIR="$(GOROROBA_PKG_DIR)" makepkg -f --noconfirm --skipinteg
	cp "$(GOROROBA_PKG_DIR)"/mesa-gororoba-*.pkg.tar.zst "$(GOROROBA_REPO_DIR)/"
	repo-add "$(GOROROBA_REPO_DIR)/$(GOROROBA_REPO_NAME).db.tar.zst" "$(GOROROBA_REPO_DIR)"/mesa-gororoba-*.pkg.tar.zst
	@echo "packaged + repo-added under $(GOROROBA_REPO_DIR); deploy: make deploy-mesa-gororoba TARGET_HOST=<host>"

deploy-mesa-gororoba:
	@pkg=$$(ls -t "$(GOROROBA_PKG_DIR)"/mesa-gororoba-*.pkg.tar.zst 2>/dev/null | head -1); \
	test -n "$$pkg" || { echo "no package; run 'make pkg-mesa-gororoba' first"; exit 1; }; \
	echo "deploy $$pkg -> $(TARGET_HOST)"; \
	scp "$$pkg" "$(TARGET_HOST):/tmp/" && \
	ssh "$(TARGET_HOST)" "yes | sudo -n pacman -U --overwrite '*' /tmp/$$(basename $$pkg)"
