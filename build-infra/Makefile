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
#   5_r300_full_release_x86_64v1-gcc-distcc-cache  r300 + zink + ati_r300 GCC diagnostic gate (vostro)
#   3_terakan_full_release_x86_64v1-clang22-distcc-cache  r600 + terascale release + Rusticl (x130e)
#   4_terakan_full_debug_x86_64v1-clang22-distcc-cache    r600 + terascale debug + Rusticl (x130e)
#   5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache  r600 + terascale release, no Rusticl
#   6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache    r600 + terascale debug, no Rusticl
#
# See also:
#   build-infra/README.md                    -- how to use
#   build-infra/configs/*.meson              -- canonical option sets
#   build-infra/env/*.env                    -- per-host toolchain env

override SHELL := /bin/bash
override PYTHONDONTWRITEBYTECODE := 1
export PYTHONDONTWRITEBYTECODE

override BUILD_INFRA_MAKEFILE := $(realpath $(lastword $(MAKEFILE_LIST)))
override BUILD_INFRA_ROOT := $(realpath $(dir $(BUILD_INFRA_MAKEFILE)))
ifneq ($(BUILD_INFRA_MAKEFILE),$(BUILD_INFRA_ROOT)/Makefile)
$(error build-infra Makefile identity is invalid: $(BUILD_INFRA_MAKEFILE))
endif
override CURDIR := $(BUILD_INFRA_ROOT)
# GNU Make evaluates immediate command-line assignments such as :=, ::=, :::=,
# and != before this Makefile loads.  The caller already owns that execution
# boundary.  This guard validates recursive command-line values that reach the
# repository build controls after Make accepts the invocation.
override COMMAND_VARIABLE_WITH_DOLLAR := $(firstword \
	$(foreach variable,$(.VARIABLES),\
		$(if \
			$(and \
				$(filter command line,$(origin $(variable))),\
				$(findstring $$,$(value $(variable)))),\
			$(variable))))
ifneq ($(COMMAND_VARIABLE_WITH_DOLLAR),)
$(error command-line variable $(COMMAND_VARIABLE_WITH_DOLLAR) contains executable dollar syntax)
endif
override LEGACY_NAMESPACE_VARIABLE := $(firstword \
	$(foreach variable,$(.VARIABLES),\
		$(if $(findstring GOROROBA_,$(variable)),$(variable))))
ifneq ($(LEGACY_NAMESPACE_VARIABLE),)
override LEGACY_NAMESPACE_REPLACEMENT := $(if \
	$(filter GOROROBA_MESA_ENV,$(LEGACY_NAMESPACE_VARIABLE)),MESA_ENV_FILE,$(if \
	$(filter GOROROBA_MESA_PREFIX,$(LEGACY_NAMESPACE_VARIABLE)),MESA_INSTALL_PREFIX,$(if \
	$(filter MESA_GOROROBA_DEPLOY_ACCEPTED,$(LEGACY_NAMESPACE_VARIABLE)),MESA_DEPLOY_ACCEPTED,$(subst GOROROBA_,MESA_,$(LEGACY_NAMESPACE_VARIABLE)))))
$(error legacy build variable $(LEGACY_NAMESPACE_VARIABLE) is retired; use $(LEGACY_NAMESPACE_REPLACEMENT))
endif

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
MODE ?=
COMPILER_CHAIN ?= ccache
COMPILER_FAMILY ?= llvm
REPRODUCIBLE_RUN ?= 0
override make_shell_quote = '$(subst ','"'"',$(1))'
override MESA_PROFILE_RAW := $(value PROFILE)
override MESA_HOSTENV_RAW := $(value HOSTENV)
override MESA_INSTALL_NAMESPACE_RAW := $(value INSTALL_NAMESPACE)
override MESA_MODE_RAW := $(value MODE)
override MESA_COMPILER_CHAIN_RAW := $(value COMPILER_CHAIN)
override MESA_COMPILER_FAMILY_RAW := $(value COMPILER_FAMILY)
override MESA_REPRODUCIBLE_RUN_RAW := $(value REPRODUCIBLE_RUN)
override SELECTOR_FIELDS := $(shell \
	MESA_PROFILE_INPUT=$(call make_shell_quote,$(value MESA_PROFILE_RAW)) \
	MESA_HOSTENV_INPUT=$(call make_shell_quote,$(value MESA_HOSTENV_RAW)) \
	MESA_INSTALL_NAMESPACE_INPUT=$(call make_shell_quote,$(value MESA_INSTALL_NAMESPACE_RAW)) \
	MESA_MODE_INPUT=$(call make_shell_quote,$(value MESA_MODE_RAW)) \
	MESA_COMPILER_CHAIN_INPUT=$(call make_shell_quote,$(value MESA_COMPILER_CHAIN_RAW)) \
	MESA_COMPILER_FAMILY_INPUT=$(call make_shell_quote,$(value MESA_COMPILER_FAMILY_RAW)) \
	MESA_REPRODUCIBLE_RUN_INPUT=$(call make_shell_quote,$(value MESA_REPRODUCIBLE_RUN_RAW)) \
	python3 $(call make_shell_quote,$(BUILD_INFRA_ROOT)/scripts/source_root_control.py) \
	validate-selectors)
override SELECTOR_RESOLVE_STATUS := $(.SHELLSTATUS)
ifneq ($(SELECTOR_RESOLVE_STATUS),0)
$(error build selector resolution failed)
endif
ifneq ($(words $(SELECTOR_FIELDS)),7)
$(error build selector control returned an invalid field set)
endif
override PROFILE := $(word 1,$(SELECTOR_FIELDS))
override HOSTENV := $(word 2,$(SELECTOR_FIELDS))
override INSTALL_NAMESPACE := $(word 3,$(SELECTOR_FIELDS))
override MODE_SELECTOR := $(word 4,$(SELECTOR_FIELDS))
ifeq ($(MODE_SELECTOR),default)
override MODE :=
else
override MODE := $(MODE_SELECTOR)
endif
override COMPILER_CHAIN := $(word 5,$(SELECTOR_FIELDS))
override COMPILER_FAMILY := $(word 6,$(SELECTOR_FIELDS))
override REPRODUCIBLE_RUN := $(word 7,$(SELECTOR_FIELDS))

override MAKE_VERSION_SUPPORTED := $(shell \
	MESA_MAKE_VERSION_INPUT=$(call make_shell_quote,$(value MAKE_VERSION)) \
	python3 $(call make_shell_quote,$(BUILD_INFRA_ROOT)/scripts/source_root_control.py) \
	validate-make-version)
ifneq ($(MAKE_VERSION_SUPPORTED),yes)
$(error GNU Make 4.2 or newer is required; found $(MAKE_VERSION))
endif

override CONTROL_ROOT := $(abspath $(CURDIR)/..)
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
override MESA_TOPSRC_INPUT := $(value TOPSRC)
override MESA_BUILD_ROOT_INPUT := $(value BUILD_ROOT)
override MESA_BUILDDIR_INPUT := $(value BUILDDIR)
override MESA_BUILDDIR_LEXICAL_INPUT := $(value BUILDDIR)
override MESA_PREFIX_INPUT := $(value PREFIX)
override MESA_SYSCONFDIR_INPUT := $(value SYSCONFDIR)
override MESA_PROFILE_INPUT := $(value PROFILE)
override MESA_HOSTENV_INPUT := $(value HOSTENV)
override MESA_MODE_INPUT := $(value MODE)
override MESA_COMPILER_CHAIN_INPUT := $(value COMPILER_CHAIN)
override MESA_COMPILER_FAMILY_INPUT := $(value COMPILER_FAMILY)
override MESA_REPRODUCIBLE_RUN_INPUT := $(value REPRODUCIBLE_RUN)
export MESA_TOPSRC_INPUT
export MESA_BUILD_ROOT_INPUT
export MESA_BUILDDIR_INPUT
export MESA_BUILDDIR_LEXICAL_INPUT
export MESA_PREFIX_INPUT
export MESA_SYSCONFDIR_INPUT
export MESA_PROFILE_INPUT
export MESA_HOSTENV_INPUT
export MESA_MODE_INPUT
export MESA_COMPILER_CHAIN_INPUT
export MESA_COMPILER_FAMILY_INPUT
export MESA_REPRODUCIBLE_RUN_INPUT

override SOURCE_ROOT_CONTROL := $(CURDIR)/scripts/source_root_control.py
override MOUNT_BOUNDARY_CALIBRATION := $(CURDIR)/tests/mount_boundary_calibration.py
override REVIEW_THREAD_FRONTIER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-QY6A8/action-frontier.tsv
override REVIEW_THREAD_LEDGER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-QY6A8/resolution-ledger.tsv
override REVIEW_THREAD_EVIDENCE_REFRESH_LEDGER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-QY6A8/evidence-refresh-ledger.tsv
override REVIEW_THREAD_FRONTIER_TOOL := $(CURDIR)/scripts/review_thread_frontier.py
override REVIEW_THREAD_CAPTURE := $(CURDIR)/docs/review-thread-frontiers/merged-thread-frontier-after-QY6A8
override REVIEW_THREAD_CAPTURE_TOOL := $(CURDIR)/scripts/review_thread_batch_capture.py
override REVIEW_THREAD_CLASSIFICATION := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-QY6A8/assessments.tsv
override REVIEW_THREAD_CLASSIFICATION_TOOL := $(CURDIR)/scripts/review_thread_classification.py
override REVIEW_THREAD_PRE_RESOLUTION_FRONTIER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-QY6A8/pre-resolution-frontier.tsv
override REVIEW_THREAD_RESOLUTION_JOURNAL := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-QY6A8/resolution-observation.json
override REVIEW_THREAD_RESOLUTION_TOOL := $(CURDIR)/scripts/review_thread_resolution.py
override REVIEW_THREAD_CORPUS_REVISION := 4b965810d471ad11a2c94369a9f75dd127651c3f
override REVIEW_THREAD_DISPOSITION_REVISION := 660f73b28743b3f032697ae7bdbc980efd3f3020
override REVIEW_THREAD_EVIDENCE_REVISION := 85dafe824576d2d4484ea6ab9141f165d8defee4
override REVIEW_THREAD_CORPUS := $(CURDIR)/docs/review-thread-corpus/unresolved-review-thread-corpus-4b965810d471
override REVIEW_THREAD_CORPUS_ANALYSIS := $(CURDIR)/docs/review-thread-corpus-analysis/unresolved-review-thread-corpus-4b965810d471
override REVIEW_THREAD_CORPUS_TOOL := $(CURDIR)/scripts/review_thread_corpus.py
override REVIEW_THREAD_GROUP_HISTORY_TOOL := $(CURDIR)/scripts/review_thread_group_history.py
override REVIEW_THREAD_CLUSTERS := $(CURDIR)/docs/review-thread-clusters/unresolved-review-thread-corpus-4b965810d471
override REVIEW_THREAD_CLUSTERS_TOOL := $(CURDIR)/scripts/review_thread_clusters.py
override REVIEW_THREAD_TVPLC_FRONTIER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-TvpLc/action-frontier.tsv
override REVIEW_THREAD_TVPLC_LEDGER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-TvpLc/resolution-ledger.tsv
override REVIEW_THREAD_TVPLC_EVIDENCE_REFRESH_LEDGER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-TvpLc/evidence-refresh-ledger.tsv
override REVIEW_THREAD_TVPLC_CAPTURE := $(CURDIR)/docs/review-thread-frontiers/merged-thread-frontier-after-TvpLc
override REVIEW_THREAD_TVPLC_CLASSIFICATION := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-TvpLc/assessments.tsv
override REVIEW_THREAD_TVPLC_PRE_RESOLUTION_FRONTIER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-TvpLc/pre-resolution-frontier.tsv
override REVIEW_THREAD_TVPLC_RESOLUTION_JOURNAL := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-TvpLc/resolution-observation.json
override REVIEW_THREAD_WHUFS_FRONTIER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-WhUFS/action-frontier.tsv
override REVIEW_THREAD_WHUFS_LEDGER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-WhUFS/resolution-ledger.tsv
override REVIEW_THREAD_WHUFS_EVIDENCE_REFRESH_LEDGER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-WhUFS/evidence-refresh-ledger.tsv
override REVIEW_THREAD_WHUFS_CAPTURE := $(CURDIR)/docs/review-thread-frontiers/merged-thread-frontier-after-WhUFS
override REVIEW_THREAD_WHUFS_CLASSIFICATION := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-WhUFS/assessments.tsv
override REVIEW_THREAD_WHUFS_PRE_RESOLUTION_FRONTIER := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-WhUFS/pre-resolution-frontier.tsv
override REVIEW_THREAD_WHUFS_RESOLUTION_JOURNAL := $(CURDIR)/docs/review-thread-classifications/merged-thread-frontier-after-WhUFS/resolution-observation.json
override REVIEW_THREAD_ARCHIVE_FRONTIER := $(CURDIR)/docs/review-thread-frontiers/merged-pr1-pr90/action-frontier.tsv
override REVIEW_THREAD_ARCHIVE_LEDGER := $(CURDIR)/docs/review-thread-frontiers/merged-pr1-pr90/resolution-ledger.tsv
override REVIEW_THREAD_ARCHIVE_COMMIT := d6661faf431e4998f5e7d1879b1d6d6eb3c7bf7c
override SOURCE_ROOT_FIELDS := $(shell \
	MESA_TOPSRC_INPUT=$(call make_shell_quote,$(value MESA_TOPSRC_INPUT)) \
	MESA_BUILD_ROOT_INPUT=$(call make_shell_quote,$(value MESA_BUILD_ROOT_INPUT)) \
	MESA_BUILDDIR_INPUT=$(call make_shell_quote,$(value MESA_BUILDDIR_INPUT)) \
	MESA_PREFIX_INPUT=$(call make_shell_quote,$(value MESA_PREFIX_INPUT)) \
	MESA_SYSCONFDIR_INPUT=$(call make_shell_quote,$(value MESA_SYSCONFDIR_INPUT)) \
	MESA_PROFILE_INPUT=$(call make_shell_quote,$(value MESA_PROFILE_INPUT)) \
	MESA_HOSTENV_INPUT=$(call make_shell_quote,$(value MESA_HOSTENV_INPUT)) \
	MESA_MODE_INPUT=$(call make_shell_quote,$(value MESA_MODE_INPUT)) \
	MESA_COMPILER_CHAIN_INPUT=$(call make_shell_quote,$(value MESA_COMPILER_CHAIN_INPUT)) \
	MESA_COMPILER_FAMILY_INPUT=$(call make_shell_quote,$(value MESA_COMPILER_FAMILY_INPUT)) \
	MESA_REPRODUCIBLE_RUN_INPUT=$(call make_shell_quote,$(value MESA_REPRODUCIBLE_RUN_INPUT)) \
	python3 $(SOURCE_ROOT_CONTROL) resolve-make)
override SOURCE_ROOT_RESOLVE_STATUS := $(.SHELLSTATUS)
ifneq ($(SOURCE_ROOT_RESOLVE_STATUS),0)
$(error source root and build path resolution failed)
endif
ifneq ($(words $(SOURCE_ROOT_FIELDS)),16)
$(error source root control returned an invalid field set)
endif
override TOPSRC := $(word 1,$(SOURCE_ROOT_FIELDS))
override BUILD_ROOT := $(word 2,$(SOURCE_ROOT_FIELDS))
override BUILDDIR := $(word 3,$(SOURCE_ROOT_FIELDS))
override PREFIX := $(word 4,$(SOURCE_ROOT_FIELDS))
override SYSCONFDIR := $(word 5,$(SOURCE_ROOT_FIELDS))
override SOURCE_COMMIT := $(word 6,$(SOURCE_ROOT_FIELDS))
override SOURCE_TREE := $(word 7,$(SOURCE_ROOT_FIELDS))
override CONTROL_COMMIT := $(word 8,$(SOURCE_ROOT_FIELDS))
override CONTROL_TREE := $(word 9,$(SOURCE_ROOT_FIELDS))
override MESA_CONTROL_ROOT_INPUT := $(word 10,$(SOURCE_ROOT_FIELDS))
override MESA_SOURCE_ROOT_ANCHOR := $(word 11,$(SOURCE_ROOT_FIELDS))
override MESA_CONTROL_ROOT_ANCHOR := $(word 12,$(SOURCE_ROOT_FIELDS))
override MESA_BUILD_ROOT_ANCHOR := $(word 13,$(SOURCE_ROOT_FIELDS))
override MESA_BUILDDIR_ANCHOR := $(word 14,$(SOURCE_ROOT_FIELDS))
override MESA_PREFIX_ANCHOR := $(word 15,$(SOURCE_ROOT_FIELDS))
override MESA_SYSCONFDIR_ANCHOR := $(word 16,$(SOURCE_ROOT_FIELDS))
override MESA_SOURCE_COMMIT_CAPTURED := $(SOURCE_COMMIT)
override MESA_SOURCE_TREE_CAPTURED := $(SOURCE_TREE)
override MESA_CONTROL_COMMIT_CAPTURED := $(CONTROL_COMMIT)
override MESA_CONTROL_TREE_CAPTURED := $(CONTROL_TREE)
override MESA_GIT_SHA1_OVERRIDE := $(SOURCE_COMMIT)
export MESA_CONTROL_ROOT_INPUT
export MESA_SOURCE_ROOT_ANCHOR
export MESA_CONTROL_ROOT_ANCHOR
export MESA_BUILD_ROOT_ANCHOR
export MESA_BUILDDIR_ANCHOR
export MESA_PREFIX_ANCHOR
export MESA_SYSCONFDIR_ANCHOR
export MESA_SOURCE_COMMIT_CAPTURED
export MESA_SOURCE_TREE_CAPTURED
export MESA_CONTROL_COMMIT_CAPTURED
export MESA_CONTROL_TREE_CAPTURED
export MESA_GIT_SHA1_OVERRIDE
# Every later Python check consumes the same physical path tuple that recipes
# interpolate.  Retaining the caller's symlink spelling here would let a
# blocked recipe validate a retargeted selector while deleting the path that
# Make captured before it waited on the build lease.
override MESA_TOPSRC_INPUT := $(TOPSRC)
override MESA_BUILD_ROOT_INPUT := $(BUILD_ROOT)
override MESA_BUILDDIR_INPUT := $(BUILDDIR)
override MESA_PREFIX_INPUT := $(PREFIX)
override MESA_SYSCONFDIR_INPUT := $(SYSCONFDIR)
ifeq ($(REPRODUCIBLE_RUN),1)
override REPRODUCIBLE_SOURCE_CHECK_OUTPUT := $(shell \
	python3 $(SOURCE_ROOT_CONTROL) check-reproducible-source)
override REPRODUCIBLE_SOURCE_CHECK_STATUS := $(.SHELLSTATUS)
ifneq ($(REPRODUCIBLE_SOURCE_CHECK_STATUS),0)
$(error reproducible source isolation failed)
endif
endif
# Pair-member descriptor: one space-separated row per build variant.
# Field order: profile hostenv builddir prefix namespace.  Accessors:
# $(call PAIR_PROFILE,LIST), $(call PAIR_HOSTENV,LIST), etc.  Adding a
# new variant means defining one new PAIR_<NAME> list, not five
# parallel _PROFILE/_HOSTENV/_BUILDDIR/_PREFIX/_NAMESPACE variables.
override PAIR_PROFILE   = $(word 1,$1)
override PAIR_HOSTENV   = $(word 2,$1)
override PAIR_BUILDDIR  = $(word 3,$1)
override PAIR_PREFIX    = $(word 4,$1)
override PAIR_NAMESPACE = $(word 5,$1)

ifeq ($(origin JOBS),undefined)
JOBS := $(shell nproc)
endif
ifeq ($(origin DISTCC_JOBS),undefined)
DISTCC_JOBS := 6
endif
# Build serialization lease: held across every Make-managed build-directory
# mutation so concurrent agents cannot configure, build, test, or clean the
# same build domain at once.  Direct Meson and Ninja commands bypass it.
ifeq ($(origin BUILD_LOCK),undefined)
BUILD_LOCK := $(HOME)/.cache/mesa-26-gororoba/mesa-build.lock
endif
ifeq ($(origin LOCK_WAIT),undefined)
LOCK_WAIT := 7200
endif
override BUILD_EXECUTION_FIELDS := $(shell \
	MESA_JOBS_INPUT=$(call make_shell_quote,$(value JOBS)) \
	MESA_DISTCC_JOBS_INPUT=$(call make_shell_quote,$(value DISTCC_JOBS)) \
	MESA_LOCK_WAIT_INPUT=$(call make_shell_quote,$(value LOCK_WAIT)) \
	MESA_BUILD_LOCK_INPUT=$(call make_shell_quote,$(value BUILD_LOCK)) \
	python3 $(SOURCE_ROOT_CONTROL) validate-build-execution)
override BUILD_EXECUTION_RESOLVE_STATUS := $(.SHELLSTATUS)
ifneq ($(BUILD_EXECUTION_RESOLVE_STATUS),0)
$(error build execution control resolution failed)
endif
ifneq ($(words $(BUILD_EXECUTION_FIELDS)),4)
$(error build execution control returned an invalid field set)
endif
override JOBS := $(word 1,$(BUILD_EXECUTION_FIELDS))
override DISTCC_JOBS := $(word 2,$(BUILD_EXECUTION_FIELDS))
override LOCK_WAIT := $(word 3,$(BUILD_EXECUTION_FIELDS))
override BUILD_LOCK := $(word 4,$(BUILD_EXECUTION_FIELDS))
MESON ?= meson
NINJA ?= ninja
SUDO ?= sudo
FLOCK ?= flock
NINJA_TARGETS ?=
REMOVE_TREE ?= rm -rf
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
python3 "$(SOURCE_ROOT_CONTROL)" check-source || exit 2; \
if [ "$(REPRODUCIBLE_RUN)" = "1" ]; then \
  python3 "$(SOURCE_ROOT_CONTROL)" check-reproducible-source || exit 2; \
fi
endef

define CHECK_LAYOUT
python3 "$(SOURCE_ROOT_CONTROL)" check-layout --operation="$1" || exit 2
endef

define VERIFY_CAPTURED_INPUTS
python3 "$(SOURCE_ROOT_CONTROL)" verify-captured-inputs --fields $1 || exit 2
endef

define PREPARE_SOURCE_IDENTITY
python3 "$(SOURCE_ROOT_CONTROL)" prepare-identity || exit 2
endef

define PREPARE_SOURCE_VIEW
python3 "$(SOURCE_ROOT_CONTROL)" prepare-source-view || exit 2
endef

define WRITE_SOURCE_IDENTITY
python3 "$(SOURCE_ROOT_CONTROL)" write-identity || exit 2
endef

define VERIFY_SOURCE_IDENTITY
python3 "$(SOURCE_ROOT_CONTROL)" verify-identity || exit 2
endef

define VERIFY_ARTIFACT_IDENTITY
python3 "$(SOURCE_ROOT_CONTROL)" verify-artifact-identity || exit 2
endef

define VERIFY_DELETE_IDENTITY
python3 "$(SOURCE_ROOT_CONTROL)" verify-delete-identity || exit 2
endef

define VERIFY_DISTCLEAN_IDENTITY
python3 "$(SOURCE_ROOT_CONTROL)" verify-distclean-identity || exit 2
endef

# --- internal paths ---------------------------------------------------------
# The default profile sits at the top of configs/; every other profile (the
# release r300 sibling and the x130e terakan lanes) sits in configs/alternates/.
# Resolve PROFILE against both directories so a bare basename still names a
# profile and the per-profile rebuild-/install- targets need no path prefix.
override CONFIG := $(firstword $(wildcard $(CURDIR)/configs/$(PROFILE).meson $(CURDIR)/configs/alternates/$(PROFILE).meson))
override ENVFILE := $(CURDIR)/env/$(HOSTENV).env
override TOOLCHAIN_NATIVE := $(BUILDDIR)/mesa-toolchain.meson

# Compiler family the toolchain overlay resolves.  llvm selects a coherent
# clang/clang++/llvm-config major and the matching LLVM binutils; gnu selects
# gcc/g++ and leaves Meson on the GNU binutils it finds.  The gnu lane needs a
# profile that declares llvm='disabled', because gcc supplies no llvm-config and
# a mismatched one would configure Mesa against a different toolchain's LLVM.
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
override PROFILE_DFLAGS_SCRIPT := $(CURDIR)/scripts/meson_profile_dflags.py
# -Dwerror=true is always on the CLI so a profile that drops the option, or a
# coredata reset of werror, cannot re-open a warnings-are-nonfatal configure.
# Profile dflags still re-assert every other project option (and werror again).
ifeq ($(TOPSRC),$(CONTROL_ROOT))
override MESON_SOURCE_ROOT := $(TOPSRC)
override EXTERNAL_SOURCE_WRAP_ARG :=
override EXTERNAL_MESON_SETUP_LOCAL_UNSET :=
else
override MESON_SOURCE_ROOT := $(BUILD_ROOT)/.mesa-source-view
override EXTERNAL_SOURCE_WRAP_ARG := --wrap-mode=default
override EXTERNAL_MESON_SETUP_LOCAL_UNSET := MESON_PACKAGE_CACHE_DIR
endif
override SETUP_ARGS := --native-file=$(CONFIG) --native-file=$(TOOLCHAIN_NATIVE) \
	--reconfigure -Dwerror=true $(EXTERNAL_SOURCE_WRAP_ARG)
# MODE=stable pins the same safe -O2 release level through a command-line
# override without enabling LTO.  Default (unset / clean / rebuild) keeps the
# config level.
ifeq ($(MODE),stable)
override SETUP_ARGS += -Doptimization=2 -Db_lto=false
endif
override MESON_SETUP_LOCAL_UNSET := CCACHE_PREFIX DISTCC_HOSTS DISTCC_PUMP \
	INCLUDE_SERVER_PORT INCLUDE_SERVER_ARGS INCLUDE_SERVER_PID \
	$(EXTERNAL_MESON_SETUP_LOCAL_UNSET)
override LOCKED_TOOL_ENV := \
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

override define CREATE_AND_ASSIGN_DISTCC_PROBE_DIRECTORY
probe_dir="$$(mktemp -d)" && [ -n "$$probe_dir" ] && [ -d "$$probe_dir" ]
endef

.PHONY: list source-root-check source-root-control-unit-test remove-identity-v7-build-root \
        naming-policy-test \
        review-thread-frontier-check review-thread-frontier-live-check \
        review-thread-frontier-unit-test review-thread-corpus-check \
        review-thread-corpus-unit-test headless-hardware-gl-runner-test \
        source-root-selection-test distcc-probe-directory-test \
        distcc-gcc-major-probe-test \
        vostro-prefix-check audit configure build clean-registered-build clean clean-all distclean install \
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
        r300-release-profile-contract-test \
        loader-environment-function-scope-test \
        rebuild-all-tiers audit-werror driver-root-audit \
        build-lease-test build-job-policy-test \
        package-mesa deploy-mesa \
        deploy-mesa-unit-test \
        help

# Component-policy gate: every profile config must stay inside its
# allowlist row (build-infra/component-policy/radeon-minimal-allowlist.toml).
.PHONY: profile-audit
profile-audit:
	python3 $(CURDIR)/scripts/profile_audit.py --repo-root $(CURDIR)/..

# Keep the release profile, its canonical documentation, and the named rebuild
# target as one contract.  This is a static gate: it proves test registration
# and execution wiring, while the locked `make test` target supplies the actual
# test verdict after a build.
.PHONY: r300-release-profile-contract-test
r300-release-profile-contract-test:
	@set -euo pipefail; \
	profile='4_r300_full_release_x86_64v1-clang22-distcc-cache'; \
	config="$(CURDIR)/configs/alternates/$$profile.meson"; \
	docs="$(CURDIR)/docs/r300-full-profile-options.md"; \
	readme="$(CURDIR)/README.md"; \
	makefile="$(BUILD_INFRA_MAKEFILE)"; \
	fail() { echo "FAIL  r300 release profile contract: $$*" >&2; exit 1; }; \
	check_contract() { \
		contract_config="$$1"; contract_docs="$$2"; contract_makefile="$$3"; \
		contract_readme="$${4:-$$readme}"; \
		[ -f "$$contract_config" ] || return 1; \
		[ -f "$$contract_docs" ] || return 1; \
		[ -f "$$contract_readme" ] || return 1; \
		option_count="$$(grep -Ec '^[[:space:]]*build-tests[[:space:]]*=' "$$contract_config" || true)"; \
		true_count="$$(grep -Ec '^[[:space:]]*build-tests[[:space:]]*=[[:space:]]*true[[:space:]]*$$' "$$contract_config" || true)"; \
		[ "$$option_count" -eq 1 ] || return 1; \
		[ "$$true_count" -eq 1 ] || return 1; \
		grep -F -- 'build-tests = true' "$$contract_docs" >/dev/null || return 1; \
		grep -F -- 'Those results are build/test evidence and do not establish' "$$contract_docs" >/dev/null || return 1; \
		grep -F -- 'PREFIX="$$MESA_BUILD_ROOT/prefix" REPRODUCIBLE_RUN=1' "$$contract_readme" >/dev/null || return 1; \
		rebuild_body="$$(awk '$$0 == "rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache:" { in_target=1; next } in_target && $$0 !~ /^[[:space:]]/ { exit } in_target { print }' "$$contract_makefile")"; \
		printf '%s\n' "$$rebuild_body" | grep -F -- '[ "$$(REPRODUCIBLE_RUN)" = "1" ]' >/dev/null || return 1; \
		for operation in clean-registered-build configure build test; do \
			printf '%s\n' "$$rebuild_body" | grep -F -- '$$(MAKE) '"$$operation"' PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$$(TOPSRC)" BUILD_ROOT="$$(BUILD_ROOT)"' >/dev/null || return 1; \
		done; \
		build_line="$$(printf '%s\n' "$$rebuild_body" | grep -n -F -- '$$(MAKE) build PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache' | cut -d: -f1 || true)"; \
		test_line="$$(printf '%s\n' "$$rebuild_body" | grep -n -F -- '$$(MAKE) test PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache' | cut -d: -f1 || true)"; \
		[ -n "$$build_line" ] || return 1; \
		[ -n "$$test_line" ] || return 1; \
		[ "$$build_line" -lt "$$test_line" ] || return 1; \
		target_body() { \
			target_name="$$1"; \
			awk -v target="$$target_name:" '$$0 == target { in_target=1; next } in_target && $$0 !~ /^[[:space:]]/ { exit } in_target { print }' "$$contract_makefile"; \
		}; \
		grep -F -- 'override SELECT_PROFILE_PREFIX = $$(if $$(filter 1,$$(REPRODUCIBLE_RUN)),$$(BUILD_ROOT)/prefix,$$1)' "$$contract_makefile" >/dev/null || return 1; \
		check_tier_target() { \
			tier_target="$$1"; tier_profile="$$2"; default_prefix="$$3"; \
			tier_body="$$(target_body "$$tier_target")"; \
			for operation in clean-registered-build configure build; do \
				expected_call='$$(MAKE) '"$$operation"' PROFILE='"$$tier_profile"' HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$$(call SELECT_PROFILE_PREFIX,'"$$default_prefix"')'; \
				printf '%s\n' "$$tier_body" | grep -F -- "$$expected_call" >/dev/null || return 1; \
			done; \
		}; \
		check_tier_target \
			rebuild-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache \
			1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache \
			/opt/mesa-gororoba-debug-asan || return 1; \
		check_tier_target \
			rebuild-2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache \
			2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache \
			/opt/mesa-gororoba-debug-o0 || return 1; \
		check_tier_target \
			rebuild-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache \
			3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache \
			/opt/mesa-gororoba-debug-optimized || return 1; \
		check_release_target() { \
			release_target="$$1"; release_profile="$$2"; release_prefix="$$3"; shift 3; \
			release_body="$$(target_body "$$release_target")"; \
			guard_line="$$(printf '%s\n' "$$release_body" | grep -n -F -- '[ "$$(REPRODUCIBLE_RUN)" = "1" ]' | cut -d: -f1 || true)"; \
			first_operation="$$1"; \
			first_operation_line="$$(printf '%s\n' "$$release_body" | grep -n -F -- '$$(MAKE) '"$$first_operation"' PROFILE='"$$release_profile" | cut -d: -f1 || true)"; \
			[ -n "$$guard_line" ] || return 1; \
			[ -n "$$first_operation_line" ] || return 1; \
			[ "$$guard_line" -lt "$$first_operation_line" ] || return 1; \
			for operation in "$$@"; do \
				expected_call='$$(MAKE) '"$$operation"' PROFILE='"$$release_profile"' HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX='"$$release_prefix"' REPRODUCIBLE_RUN=1 TOPSRC="$$(TOPSRC)" BUILD_ROOT="$$(BUILD_ROOT)"'; \
				printf '%s\n' "$$release_body" | grep -F -- "$$expected_call" >/dev/null || return 1; \
			done; \
		}; \
		check_release_target \
			rebuild-3_terakan_full_release_x86_64v1-clang22-distcc-cache \
			3_terakan_full_release_x86_64v1-clang22-distcc-cache \
			'$$(BUILD_ROOT)/prefix' clean-registered-build configure build || return 1; \
		check_release_target \
			rebuild-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
			5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
			'$$(BUILD_ROOT)/prefix' clean-registered-build configure build || return 1; \
		check_release_target \
			install-4_r300_full_release_x86_64v1-clang22-distcc-cache \
			4_r300_full_release_x86_64v1-clang22-distcc-cache \
			'$$(BUILD_ROOT)/prefix' install || return 1; \
		check_release_target \
			install-3_terakan_full_release_x86_64v1-clang22-distcc-cache \
			3_terakan_full_release_x86_64v1-clang22-distcc-cache \
			'$$(BUILD_ROOT)/prefix' install || return 1; \
		check_release_target \
			install-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
			5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
			'$$(BUILD_ROOT)/prefix' install || return 1; \
		all_tiers_body="$$(awk '$$0 == "rebuild-all-tiers:" { in_target=1; next } in_target && $$0 !~ /^[[:space:]]/ { exit } in_target { print }' "$$contract_makefile")"; \
		all_tiers_guard_line="$$(printf '%s\n' "$$all_tiers_body" | grep -n -F -- '[ "$$(REPRODUCIBLE_RUN)" = "1" ]' | cut -d: -f1 || true)"; \
		all_tiers_first_build_line="$$(printf '%s\n' "$$all_tiers_body" | grep -n -F -- '$$(MAKE) rebuild-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache' | cut -d: -f1 || true)"; \
		[ -n "$$all_tiers_guard_line" ] || return 1; \
		[ -n "$$all_tiers_first_build_line" ] || return 1; \
		[ "$$all_tiers_guard_line" -lt "$$all_tiers_first_build_line" ] || return 1; \
		for tier_target in \
			rebuild-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache \
			rebuild-2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache \
			rebuild-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache \
			rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache; do \
			tier_profile="$${tier_target#rebuild-}"; \
			expected_tier_call='$$(MAKE) '"$$tier_target"' REPRODUCIBLE_RUN=1 TOPSRC="$$(TOPSRC)" BUILD_ROOT="$$(BUILD_ROOT)/'"$$tier_profile"'"'; \
			printf '%s\n' "$$all_tiers_body" | grep -F -- "$$expected_tier_call" >/dev/null || return 1; \
		done; \
		grep -F -- '$$(MESON) test -C "$$(BUILDDIR)"' "$$contract_makefile" >/dev/null || return 1; \
	}; \
	check_contract "$$config" "$$docs" "$$makefile" "$$readme" || fail "canonical files disagree"; \
	echo "OK    r300 release profile contract accepts the canonical files"; \
	tmpdir="$$(mktemp -d)"; \
	trap 'rm -rf -- "$$tmpdir"' EXIT HUP INT TERM; \
	bad_config="$$tmpdir/profile.meson"; \
	sed 's/^build-tests = true$$/build-tests = false/' "$$config" > "$$bad_config"; \
	if check_contract "$$bad_config" "$$docs" "$$makefile"; then \
		fail "known-bad build-tests=false profile passed"; \
	fi; \
	echo "OK    r300 release profile contract rejects build-tests=false"; \
	bad_docs="$$tmpdir/profile.md"; \
	sed '/Those results are build\/test evidence and do not establish/d' "$$docs" > "$$bad_docs"; \
	if check_contract "$$config" "$$bad_docs" "$$makefile"; then \
		fail "known-bad evidence-boundary doc passed"; \
	fi; \
	echo "OK    r300 release profile contract rejects an unbounded evidence doc"; \
	bad_readme="$$tmpdir/README.md"; \
	sed 's#PREFIX="$$MESA_BUILD_ROOT/prefix"#PREFIX=/opt/local/mesa-26-gororoba#' \
		"$$readme" > "$$bad_readme"; \
	if check_contract "$$config" "$$docs" "$$makefile" "$$bad_readme"; then \
		fail "known-bad shared artifact prefix passed"; \
	fi; \
	echo "OK    r300 release profile contract rejects a shared artifact prefix"; \
	bad_makefile="$$tmpdir/Makefile"; \
	grep -F -v -- '$$(MAKE) test PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache' "$$makefile" > "$$bad_makefile"; \
	if check_contract "$$config" "$$docs" "$$bad_makefile"; then \
		fail "known-bad rebuild target without test passed"; \
	fi; \
	echo "OK    r300 release profile contract rejects build-only rebuild wiring"; \
	bad_cleanup_makefile="$$tmpdir/Makefile.cleanup"; \
	sed 's/clean-registered-build PROFILE=/clean PROFILE=/' \
		"$$makefile" > "$$bad_cleanup_makefile"; \
	if check_contract "$$config" "$$docs" "$$bad_cleanup_makefile"; then \
		fail "known-bad release rebuild with ordinary clean passed"; \
	fi; \
	echo "OK    r300 release profile contract rejects ordinary clean in release rebuilds"; \
	bad_prefix_selector_makefile="$$tmpdir/Makefile.prefix-selector"; \
	grep -F -v -- 'override SELECT_PROFILE_PREFIX = ' "$$makefile" > "$$bad_prefix_selector_makefile"; \
	if check_contract "$$config" "$$docs" "$$bad_prefix_selector_makefile"; then \
		fail "known-bad tier targets without the prefix selector passed"; \
	fi; \
	echo "OK    r300 release profile contract rejects a missing tier prefix selector"; \
	bad_prefix_makefile="$$tmpdir/Makefile.prefix"; \
	sed 's#PREFIX=[$$](BUILD_ROOT)/prefix#PREFIX=/opt/local/mesa-26-gororoba#g' \
		"$$makefile" > "$$bad_prefix_makefile"; \
	if check_contract "$$config" "$$docs" "$$bad_prefix_makefile"; then \
		fail "known-bad reproducible targets with a shared prefix passed"; \
	fi; \
	echo "OK    r300 release profile contract rejects shared reproducible prefixes"; \
	bad_tier_roots_makefile="$$tmpdir/Makefile.tier-roots"; \
	sed 's#BUILD_ROOT="[$$](BUILD_ROOT)/[^\"]*"#BUILD_ROOT="$$(BUILD_ROOT)"#g' \
		"$$makefile" > "$$bad_tier_roots_makefile"; \
	if check_contract "$$config" "$$docs" "$$bad_tier_roots_makefile"; then \
		fail "known-bad four-tier target with a shared build root passed"; \
	fi; \
	echo "OK    r300 release profile contract rejects shared four-tier build roots"; \
	remove_target_guard() { \
		mutation_target="$$1"; mutation_output="$$2"; \
		awk -v target="$$mutation_target:" ' \
			$$0 == target { in_target=1 } \
			in_target && !removed && /\[ "\$$\(REPRODUCIBLE_RUN\)" = "1" \]/ { removed=1; next } \
			in_target && $$0 != target && $$0 !~ /^[[:space:]]/ { in_target=0 } \
			{ print } \
			END { if (!removed) exit 2 }' "$$makefile" > "$$mutation_output"; \
	}; \
	for guarded_target in \
		rebuild-3_terakan_full_release_x86_64v1-clang22-distcc-cache \
		rebuild-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache \
		install-4_r300_full_release_x86_64v1-clang22-distcc-cache \
		install-3_terakan_full_release_x86_64v1-clang22-distcc-cache \
		install-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache; do \
		bad_guard_makefile="$$tmpdir/Makefile.$$guarded_target"; \
		remove_target_guard "$$guarded_target" "$$bad_guard_makefile"; \
		if check_contract "$$config" "$$docs" "$$bad_guard_makefile"; then \
			fail "known-bad $$guarded_target without a preflight passed"; \
		fi; \
	done; \
	echo "OK    r300 release profile contract rejects each unguarded release operation"; \
	bad_aggregate_makefile="$$tmpdir/Makefile.aggregate"; \
	grep -F -v -- '[ "$$(REPRODUCIBLE_RUN)" = "1" ]' "$$makefile" > "$$bad_aggregate_makefile"; \
	if check_contract "$$config" "$$docs" "$$bad_aggregate_makefile"; then \
		fail "known-bad aggregate target without a preflight passed"; \
	fi; \
	echo "OK    r300 release profile contract rejects an unguarded aggregate"

source-root-check:
	@$(CHECK_SOURCE_ROOT)
	@echo "OK    source root: $(TOPSRC)"
	@echo "OK    source commit: $(SOURCE_COMMIT)"
	@echo "OK    source tree: $(SOURCE_TREE)"
	@echo "OK    control root: $(CONTROL_ROOT)"
	@echo "OK    control commit: $(CONTROL_COMMIT)"
	@echo "OK    control tree: $(CONTROL_TREE)"
	@echo "OK    reproducible run: $(REPRODUCIBLE_RUN)"

source-root-control-unit-test:
	PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 \
		python3 -m pytest -q \
		$(CURDIR)/tests/test_source_root_control.py \
		$(CURDIR)/tests/test_mount_boundary_calibration.py

remove-identity-v7-build-root: source-root-check
	@[ "$(REPRODUCIBLE_RUN)" = "0" ] || { echo "schema-7 cleanup requires REPRODUCIBLE_RUN=0" >&2; exit 2; }
	@$(ACQUIRE_BUILD_LOCK) \
	$(call VERIFY_CAPTURED_INPUTS,source_root control_root build_root builddir prefix sysconfdir); \
	$(call CHECK_LAYOUT,clean); \
	python3 "$(SOURCE_ROOT_CONTROL)" remove-identity-v7-build-root; \
	echo "removed verified schema-7 build root: $(BUILD_ROOT)"

naming-policy-test:
	python3 $(CURDIR)/scripts/naming_policy.py --self-test
	python3 $(CURDIR)/scripts/naming_policy.py --repo-root $(CURDIR)/..

# Driver-root gate: each AMD GPU family's Vulkan driver keeps its source root
# at the family's vulkan/ directory, and the build graph enters it there.
.PHONY: driver-root-audit
driver-root-audit:
	python3 $(CURDIR)/scripts/driver_root_audit.py --self-test
	python3 $(CURDIR)/scripts/driver_root_audit.py --repo-root $(CURDIR)/..

review-thread-frontier-check:
	python3 $(REVIEW_THREAD_CAPTURE_TOOL) check --input-dir $(REVIEW_THREAD_CAPTURE)
	python3 $(REVIEW_THREAD_CLASSIFICATION_TOOL) check \
		--capture-frontier $(REVIEW_THREAD_CAPTURE)/frontier.tsv \
		--assessments $(REVIEW_THREAD_CLASSIFICATION) \
		--output $(REVIEW_THREAD_FRONTIER) \
		--batch-id merged-review-thread-closure-batch-0004 \
		--batch-size 50
	python3 $(REVIEW_THREAD_RESOLUTION_TOOL) check \
		--frontier $(REVIEW_THREAD_PRE_RESOLUTION_FRONTIER) \
		--journal $(REVIEW_THREAD_RESOLUTION_JOURNAL) \
		--ledger $(REVIEW_THREAD_LEDGER) \
		$(if $(filter 1,$(REVIEW_THREAD_LIVE)),--live,)
	python3 $(REVIEW_THREAD_FRONTIER_TOOL) \
		--frontier $(REVIEW_THREAD_ARCHIVE_FRONTIER) \
		--ledger $(REVIEW_THREAD_ARCHIVE_LEDGER) \
		--batch-size 50 \
		--repo-root $(CURDIR)/.. \
		--main-ref $(REVIEW_THREAD_ARCHIVE_COMMIT) \
		--candidate-ref $(REVIEW_THREAD_ARCHIVE_COMMIT)
	python3 $(REVIEW_THREAD_FRONTIER_TOOL) \
		--frontier $(REVIEW_THREAD_FRONTIER) \
		--ledger $(REVIEW_THREAD_LEDGER) \
		--evidence-refresh-ledger $(REVIEW_THREAD_EVIDENCE_REFRESH_LEDGER) \
		--batch-size 50 \
		--repo-root $(CURDIR)/.. \
		--main-ref origin/main \
		--candidate-ref HEAD \
		$(if $(filter 1,$(REVIEW_THREAD_LIVE)),--live,)
	python3 $(REVIEW_THREAD_CAPTURE_TOOL) check --input-dir $(REVIEW_THREAD_TVPLC_CAPTURE)
	python3 $(REVIEW_THREAD_CLASSIFICATION_TOOL) check \
		--capture-frontier $(REVIEW_THREAD_TVPLC_CAPTURE)/frontier.tsv \
		--assessments $(REVIEW_THREAD_TVPLC_CLASSIFICATION) \
		--output $(REVIEW_THREAD_TVPLC_FRONTIER) \
		--batch-id merged-review-thread-closure-batch-0005 \
		--batch-size 50
	python3 $(REVIEW_THREAD_RESOLUTION_TOOL) check \
		--frontier $(REVIEW_THREAD_TVPLC_PRE_RESOLUTION_FRONTIER) \
		--journal $(REVIEW_THREAD_TVPLC_RESOLUTION_JOURNAL) \
		--ledger $(REVIEW_THREAD_TVPLC_LEDGER) \
		$(if $(filter 1,$(REVIEW_THREAD_LIVE)),--live,)
	python3 $(REVIEW_THREAD_FRONTIER_TOOL) \
		--frontier $(REVIEW_THREAD_TVPLC_FRONTIER) \
		--ledger $(REVIEW_THREAD_TVPLC_LEDGER) \
		--evidence-refresh-ledger $(REVIEW_THREAD_TVPLC_EVIDENCE_REFRESH_LEDGER) \
		--batch-size 50 \
		--repo-root $(CURDIR)/.. \
		--main-ref origin/main \
		--candidate-ref HEAD \
		$(if $(filter 1,$(REVIEW_THREAD_LIVE)),--live,)
	python3 $(REVIEW_THREAD_CAPTURE_TOOL) check --input-dir $(REVIEW_THREAD_WHUFS_CAPTURE)
	python3 $(REVIEW_THREAD_CLASSIFICATION_TOOL) check \
		--capture-frontier $(REVIEW_THREAD_WHUFS_CAPTURE)/frontier.tsv \
		--assessments $(REVIEW_THREAD_WHUFS_CLASSIFICATION) \
		--output $(REVIEW_THREAD_WHUFS_FRONTIER) \
		--batch-id merged-review-thread-closure-batch-0006 \
		--batch-size 50
	python3 $(REVIEW_THREAD_RESOLUTION_TOOL) check \
		--frontier $(REVIEW_THREAD_WHUFS_PRE_RESOLUTION_FRONTIER) \
		--journal $(REVIEW_THREAD_WHUFS_RESOLUTION_JOURNAL) \
		--ledger $(REVIEW_THREAD_WHUFS_LEDGER) \
		$(if $(filter 1,$(REVIEW_THREAD_LIVE)),--live,)
	python3 $(REVIEW_THREAD_FRONTIER_TOOL) \
		--frontier $(REVIEW_THREAD_WHUFS_FRONTIER) \
		--ledger $(REVIEW_THREAD_WHUFS_LEDGER) \
		--evidence-refresh-ledger $(REVIEW_THREAD_WHUFS_EVIDENCE_REFRESH_LEDGER) \
		--batch-size 50 \
		--repo-root $(CURDIR)/.. \
		--main-ref origin/main \
		--candidate-ref HEAD \
		$(if $(filter 1,$(REVIEW_THREAD_LIVE)),--live,)

review-thread-frontier-live-check:
	$(MAKE) review-thread-frontier-check REVIEW_THREAD_LIVE=1

review-thread-frontier-unit-test:
	PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 \
		python3 -m pytest -q \
			$(CURDIR)/tests/test_review_thread_frontier.py \
			$(CURDIR)/tests/test_review_thread_batch_capture.py \
			$(CURDIR)/tests/test_review_thread_classification.py \
			$(CURDIR)/tests/test_review_thread_resolution.py

review-thread-corpus-check:
	PYTHONDONTWRITEBYTECODE=1 \
		python3 $(REVIEW_THREAD_CORPUS_TOOL) check \
			--input-dir $(REVIEW_THREAD_CORPUS)
	PYTHONDONTWRITEBYTECODE=1 \
		python3 $(REVIEW_THREAD_GROUP_HISTORY_TOOL) check \
			--corpus-dir $(REVIEW_THREAD_CORPUS) \
			--repo-root $(CURDIR)/.. \
			--revision $(REVIEW_THREAD_DISPOSITION_REVISION) \
			--output-dir $(REVIEW_THREAD_CORPUS_ANALYSIS)
	PYTHONDONTWRITEBYTECODE=1 \
		python3 $(REVIEW_THREAD_CLUSTERS_TOOL) check \
			--corpus-dir $(REVIEW_THREAD_CORPUS) \
			--history-dir $(REVIEW_THREAD_CORPUS_ANALYSIS) \
			--repo-root $(CURDIR)/.. \
			--revision $(REVIEW_THREAD_DISPOSITION_REVISION) \
			--evidence-revision $(REVIEW_THREAD_EVIDENCE_REVISION) \
			--output-dir $(REVIEW_THREAD_CLUSTERS)

review-thread-corpus-unit-test:
	PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 PYTHONDONTWRITEBYTECODE=1 \
		python3 -m pytest -q \
			$(CURDIR)/tests/test_review_thread_corpus.py \
			$(CURDIR)/tests/test_review_thread_group_history.py \
			$(CURDIR)/tests/test_review_thread_clusters.py

headless-hardware-gl-runner-test:
	PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 \
		python3 -m pytest -q \
			$(CURDIR)/tests/test_headless_hardware_gl_runner.py

help:
	@echo "usage: make <target> PROFILE=<profile> [HOSTENV=<env>] [TOPSRC=<mesa-worktree>] [REPRODUCIBLE_RUN=0|1] [BUILDDIR=<path>] [PREFIX=<path>] [SYSCONFDIR=<path>]"
	@echo ""
	@echo "targets:"
	@echo "  configure         meson setup the builddir from configs/[alternates/]\$$(PROFILE).meson"
	@echo "  build             locked ninja -C \$$(BUILDDIR); NINJA_TARGETS=one target, NINJA_ARGS=-k0"
	@echo "  clean-registered-build skip an absent builddir; otherwise run strict clean"
	@echo "  clean             rm -rf \$$(BUILDDIR) (active profile only)"
	@echo "  clean-all         remove the fixed repo-local build/ tree (all profiles)"
	@echo "  distclean         atomically clean + archive \$$(PREFIX) under one lease"
	@echo "  audit             verify host tools, env policy, profile-audit, and profile requirements"
	@echo "  profile-audit     fail when a profile selects drivers outside its allowlist row"
	@echo "  driver-root-audit fail when an AMD Vulkan driver root or its subdir() entry drifts"
	@echo "  review-thread-frontier-check validate finite closure rows and merged evidence"
	@echo "  review-thread-frontier-live-check bind exact GraphQL IDs, URLs, and states"
	@echo "  review-thread-frontier-unit-test calibrate review closure rejection paths"
	@echo "  review-thread-corpus-check replay the complete unresolved-thread corpus and source histories"
	@echo "  review-thread-corpus-unit-test calibrate corpus and history rejection paths"
	@echo "  headless-hardware-gl-runner-test calibrate transient Xorg unit teardown"
	@echo "  source-root-check require TOPSRC to be an exact Mesa Git worktree root"
	@echo "  source-root-control-unit-test exercise path and identity invariants"
	@echo "  remove-identity-v7-build-root remove an exact verified schema-7 build root"
	@echo "  naming-policy-test reject retired build controls and unclear qualification wording"
	@echo "  loader-environment-function-scope-test preserve caller shell functions"
	@echo "  source-root-selection-test calibrate external TOPSRC selection and path guards"
	@echo "  distcc-probe-directory-test calibrates host-audit probe directory creation"
	@echo "  distcc-gcc-major-probe-test calibrates remote C/C++ dispatch and GNU compiler identity"
	@echo "  install           sudo meson install --no-rebuild -C \$$(BUILDDIR)"
	@echo "  artifact-check    list installed Terakan/r600/r3v artifacts under \$$(PREFIX); warn on stale r300 ICDs"
	@echo "  test              locked meson test -C \$$(BUILDDIR); set MESON_TEST_ARGS for one test"
	@echo "  build-lease-test  verify mutation leases and path-safety guards"
	@echo "  build-job-policy-test verify nproc local jobs and the six-job distcc ceiling"
	@echo "  deploy-mesa      install the release package only with MESA_DEPLOY_ACCEPTED=1"
	@echo "  deploy-mesa-unit-test calibrate consent, package selection, and remote argv"
	@echo "  rebuild-<profile> clean + configure + build; r300 release also runs unit tests"
	@echo "  list              show available PROFILEs and HOSTENVs"
	@echo ""
	@echo "profile:   $(PROFILE)"
	@echo "hostenv:   $(HOSTENV)"
	@echo "reproducible run: $(REPRODUCIBLE_RUN)"
	@echo "topsrc:    $(TOPSRC)"
	@echo "builddir:  $(BUILDDIR)"
	@echo "prefix:    $(PREFIX)"
	@echo "compiler chain: $(COMPILER_CHAIN)"

list:
	@echo "available PROFILEs (configs/*.meson + configs/alternates/*.meson):"; \
	for f in configs/*.meson configs/alternates/*.meson; do echo "  $$(basename $$f .meson)"; done
	@echo "available HOSTENVs (env/*.env):"; \
	for f in env/*.env; do echo "  $$(basename $$f .env)"; done

loader-environment-function-scope-test:
	sh $(CURDIR)/packaging/test-loader-environment-function-scope.sh

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

distcc-probe-directory-test:
	@set -euo pipefail; \
	tmpdir="$$(mktemp -d)"; \
	test -n "$$tmpdir" && test -d "$$tmpdir"; \
	trap 'rm -rf -- "$$tmpdir"' EXIT HUP INT TERM; \
	failed_status_path="$$tmpdir/failed-status"; \
	mkdir -- "$$failed_status_path"; \
	mktemp() { printf '%s\n' "$$failed_status_path"; return 1; }; \
	if $(CREATE_AND_ASSIGN_DISTCC_PROBE_DIRECTORY); then \
		echo "FAIL  distcc probe directory accepted a failed mktemp command" >&2; \
		exit 1; \
	fi; \
	test "$$probe_dir" = "$$failed_status_path" || { \
		echo "FAIL  failed mktemp calibration returned the wrong path" >&2; \
		exit 1; \
	}; \
	echo "OK    distcc probe directory rejects a failed mktemp command"; \
	missing_path="$$tmpdir/missing"; \
	mktemp() { printf '%s\n' "$$missing_path"; }; \
	if $(CREATE_AND_ASSIGN_DISTCC_PROBE_DIRECTORY); then \
		echo "FAIL  distcc probe directory accepted a missing path" >&2; \
		exit 1; \
	fi; \
	echo "OK    distcc probe directory rejects a missing path"; \
	created_path="$$tmpdir/created"; \
	mktemp() { mkdir -- "$$created_path"; printf '%s\n' "$$created_path"; }; \
	if $(CREATE_AND_ASSIGN_DISTCC_PROBE_DIRECTORY); then \
		:; \
	else \
		echo "FAIL  distcc probe directory rejected a created path" >&2; \
		exit 1; \
	fi; \
	test "$$probe_dir" = "$$created_path" || { \
		echo "FAIL  successful mktemp calibration returned the wrong path" >&2; \
		exit 1; \
	}; \
	rmdir -- "$$probe_dir"; \
	echo "OK    distcc probe directory accepts a created path"; \
	space_tmpdir="$$tmpdir/TMPDIR with spaces"; \
	mkdir -- "$$space_tmpdir"; \
	mktemp() { command mktemp "$$@"; }; \
	TMPDIR="$$space_tmpdir"; export TMPDIR; \
	if $(CREATE_AND_ASSIGN_DISTCC_PROBE_DIRECTORY); then \
		:; \
	else \
		echo "FAIL  distcc probe directory rejected a TMPDIR path with spaces" >&2; \
		exit 1; \
	fi; \
	case "$$probe_dir" in \
		"$$space_tmpdir"/*) ;; \
		*) echo "FAIL  distcc probe directory ignored TMPDIR with spaces" >&2; \
		   exit 1 ;; \
	esac; \
	rmdir -- "$$probe_dir"; \
	echo "OK    distcc probe directory accepts TMPDIR paths with spaces"

distcc-gcc-major-probe-test:
	@set -euo pipefail; \
	tmpdir="$$(mktemp -d)"; \
	trap 'rm -rf -- "$$tmpdir"' EXIT HUP INT TERM; \
	probe_dir="$$tmpdir/probe"; mkdir -- "$$probe_dir"; \
	distcc_host_name() { \
		host_spec="$$1"; host_spec="$${host_spec%%/*}"; host_spec="$${host_spec%%,*}"; \
		host_spec="$${host_spec#@}"; host_spec="$${host_spec#*@}"; \
		case "$$host_spec" in \
			\[*\]:*) host_spec="$${host_spec%%]*}"; host_spec="$${host_spec#\[}" ;; \
			*:*) host_spec="$${host_spec%%:*}" ;; \
		esac; \
		printf '%s\n' "$$host_spec"; \
	}; \
	distcc_probe_compiler_tool() { \
		compiler_chain="$$1"; compiler_tool="$$2"; \
		case "$$compiler_chain" in \
			ccache) PATH="$${CCACHE_PATH:-$$PATH}" command -v "$$compiler_tool" ;; \
			distcc) printf '%s\n' "$$compiler_tool" ;; \
			*) return 1 ;; \
		esac; \
	}; \
	gcc_object_major() { \
		LC_ALL=C readelf --string-dump=.comment "$$1" 2>/dev/null | \
				sed -n 's/^[[:space:]]*.*GCC: ([^)]*) \([0-9][0-9]*\).*/\1/p' | \
			sed -n '1p'; \
	}; \
	probe_distcc_host() { \
		distcc_spec="$$1"; expected_major="$$2"; compiler_tool="$$3"; \
		compiler_family="$$4"; probe_dir="$$5"; compiler_cxx_tool="$${6:-}"; \
		remote_distcc_host="$$(distcc_host_name "$$distcc_spec")"; \
		if ! getent ahostsv4 "$$remote_distcc_host" >/dev/null 2>&1; then \
			echo "FAIL  distcc volunteer hostname does not resolve: $$distcc_spec" >&2; \
			return 1; \
		fi; \
		if ! rm -f -- "$$probe_dir/probe.c" "$$probe_dir/probe.o" "$$probe_dir/probe.err" || \
			! printf '%s\n' 'int mesa_distcc_probe(void) { return 22; }' > "$$probe_dir/probe.c"; then \
			echo "FAIL  cannot prepare distcc volunteer probe: $$distcc_spec" >&2; \
			return 1; \
		fi; \
		if ! DISTCC_HOSTS="$$distcc_spec" DISTCC_FALLBACK=0 \
			distcc "$$compiler_tool" -Wall -Wextra -Werror \
			-c "$$probe_dir/probe.c" -o "$$probe_dir/probe.o" \
			>/dev/null 2>"$$probe_dir/probe.err"; then \
			echo "FAIL  distcc volunteer dispatch failed: $$distcc_spec ($$compiler_tool)" >&2; \
			sed -n '1,12p' "$$probe_dir/probe.err" >&2; \
			return 1; \
		fi; \
		if [ -z "$$compiler_cxx_tool" ]; then \
			echo "FAIL  distcc volunteer has no verified C++ compiler: $$distcc_spec" >&2; \
			return 1; \
		fi; \
		if [ "$$compiler_family" = gnu ]; then \
			remote_gcc_major="$$(gcc_object_major "$$probe_dir/probe.o")"; \
			if [ -z "$$remote_gcc_major" ]; then \
				echo "FAIL  distcc volunteer object has no GCC identity: $$distcc_spec" >&2; \
				return 1; \
			fi; \
			if [ "$$remote_gcc_major" != "$$expected_major" ]; then \
				echo "FAIL  distcc volunteer GCC major mismatch: $$distcc_spec (client=$$expected_major remote=$$remote_gcc_major)" >&2; \
				return 1; \
			fi; \
		fi; \
		if ! rm -f -- "$$probe_dir/probe.cpp" "$$probe_dir/probe.cpp.o" "$$probe_dir/probe.cpp.err" || \
			! printf '%s\n' 'int mesa_distcc_probe_cpp() { return 23; }' > "$$probe_dir/probe.cpp"; then \
			echo "FAIL  cannot prepare distcc volunteer C++ probe: $$distcc_spec" >&2; \
			return 1; \
		fi; \
		if ! DISTCC_HOSTS="$$distcc_spec" DISTCC_FALLBACK=0 \
			distcc "$$compiler_cxx_tool" -Wall -Wextra -Werror \
			-c "$$probe_dir/probe.cpp" -o "$$probe_dir/probe.cpp.o" \
			>/dev/null 2>"$$probe_dir/probe.cpp.err"; then \
			echo "FAIL  distcc volunteer C++ dispatch failed: $$distcc_spec ($$compiler_cxx_tool)" >&2; \
			sed -n '1,12p' "$$probe_dir/probe.cpp.err" >&2; \
			return 1; \
		fi; \
		if [ "$$compiler_family" = gnu ]; then \
			remote_gxx_major="$$(gcc_object_major "$$probe_dir/probe.cpp.o")"; \
			if [ -z "$$remote_gxx_major" ]; then \
				echo "FAIL  distcc volunteer C++ object has no GCC identity: $$distcc_spec" >&2; \
				return 1; \
			fi; \
			if [ "$$remote_gxx_major" != "$$expected_major" ]; then \
				echo "FAIL  distcc volunteer G++ major mismatch: $$distcc_spec (client=$$expected_major remote=$$remote_gxx_major)" >&2; \
				return 1; \
			fi; \
			echo "OK    distcc volunteer GCC major: $$distcc_spec ($$remote_gcc_major)"; \
			echo "OK    distcc volunteer G++ major: $$distcc_spec ($$remote_gxx_major)"; \
		else \
			echo "OK    distcc volunteer C dispatch: $$distcc_spec ($$compiler_tool)"; \
			echo "OK    distcc volunteer C++ dispatch: $$distcc_spec ($$compiler_cxx_tool)"; \
		fi; \
	}; \
	probe_all_distcc_hosts() { \
		expected_major="$$1"; compiler_tool="$$2"; compiler_family="$$3"; probe_dir="$$4"; compiler_cxx_tool="$${5:-}"; \
		remote_distcc_count=0; remote_distcc_failures=0; \
		for distcc_spec in $${DISTCC_HOSTS:-}; do \
			case "$$distcc_spec" in --*|+*) continue ;; esac; \
			remote_distcc_host="$$(distcc_host_name "$$distcc_spec")"; \
			case "$$remote_distcc_host" in ""|localhost|localhost.localdomain) continue ;; esac; \
			remote_distcc_count=$$((remote_distcc_count + 1)); \
			if ! probe_distcc_host "$$distcc_spec" "$$expected_major" "$$compiler_tool" "$$compiler_family" "$$probe_dir" "$$compiler_cxx_tool"; then \
				remote_distcc_failures=$$((remote_distcc_failures + 1)); \
			fi; \
		done; \
		[ "$$remote_distcc_count" -gt 0 ] && [ "$$remote_distcc_failures" -eq 0 ]; \
	}; \
	getent() { return 0; }; \
	fake_probe_spec=""; \
	distcc() { \
		fake_probe_spec="$${DISTCC_HOSTS:-}"; output=""; \
		printf '%s\n' "$$1" >> "$$tmpdir/probe-tools"; \
		while [ "$$#" -gt 0 ]; do \
			if [ "$$1" = -o ]; then shift; output="$$1"; fi; \
			shift; \
		done; \
		[ -n "$$output" ] || return 1; \
		: > "$$output"; \
	}; \
	readelf() { \
		case "$$fake_probe_spec" in \
			volunteer-gxx-mismatch/1) \
				case "$$2" in *probe.cpp.o) printf '%s\n' 'String dump of section .comment: GCC: (GNU) 15.4.0' ;; \
				*) printf '%s\n' 'String dump of section .comment: GCC: (GNU) 16.1.1' ;; esac ;; \
			volunteer-a/1|volunteer-b/1) printf '%s\n' 'String dump of section .comment: GCC: (GNU) 16.1.1' ;; \
			volunteer-vendor/1) printf '%s\n' 'String dump of section .comment: GCC: (Ubuntu 16.1.1-1ubuntu1) 16.1.1' ;; \
			volunteer-mismatch/1) printf '%s\n' 'String dump of section .comment: GCC: (GNU) 15.4.0' ;; \
			volunteer-missing/1|*) return 1 ;; \
			esac; \
	}; \
	test "$$(distcc_host_name 'USER@volunteer-a:3632/4,lzo')" = volunteer-a || { \
		echo "FAIL  distcc host parser did not isolate a volunteer hostname" >&2; exit 1; \
	}; \
	test "$$(distcc_host_name 'localhost,lzo/2')" = localhost || { \
		echo "FAIL  distcc host parser did not preserve localhost classification" >&2; exit 1; \
	}; \
	test "$$(distcc_host_name '@localhost/2')" = localhost || { \
		echo "FAIL  distcc host parser did not classify SSH localhost as local" >&2; exit 1; \
	}; \
	echo "OK    distcc host parser handles user, port, and option syntax"; \
	client_compiler_dir="$$tmpdir/client-compilers"; mkdir -- "$$client_compiler_dir"; \
	: > "$$client_compiler_dir/clang-22"; : > "$$client_compiler_dir/clang++-22"; \
	chmod +x "$$client_compiler_dir/clang-22" "$$client_compiler_dir/clang++-22"; \
	CCACHE_PATH="$$client_compiler_dir"; export CCACHE_PATH; \
	resolved_clang="$$(distcc_probe_compiler_tool ccache clang-22)"; \
	resolved_clangxx="$$(distcc_probe_compiler_tool ccache clang++-22)"; \
	test "$$resolved_clang" = "$$client_compiler_dir/clang-22" && \
		test "$$resolved_clangxx" = "$$client_compiler_dir/clang++-22" || { \
		echo "FAIL  ccache distcc probes did not resolve compiler paths through CCACHE_PATH" >&2; exit 1; \
	}; \
	test "$$(distcc_probe_compiler_tool distcc clang-22)" = clang-22 || { \
		echo "FAIL  direct distcc probe did not preserve the versioned compiler name" >&2; exit 1; \
	}; \
	echo "OK    distcc probes mirror ccache and direct compiler forms"; \
	: > "$$tmpdir/probe-tools"; \
	DISTCC_HOSTS='+zeroconf @localhost/2 localhost/2 volunteer-a/1'; \
	if ! probe_all_distcc_hosts 22 "$$resolved_clang" llvm "$$probe_dir" "$$resolved_clangxx"; then \
		echo "FAIL  LLVM C and C++ volunteer probes were rejected" >&2; exit 1; \
	fi; \
	test "$$remote_distcc_count" -eq 1 || { \
		echo "FAIL  discovery or SSH-local tokens were counted as remote volunteers" >&2; exit 1; \
	}; \
	printf '%s\n%s\n' "$$resolved_clang" "$$resolved_clangxx" | \
		cmp -s - "$$tmpdir/probe-tools" || { \
		echo "FAIL  LLVM volunteer probes did not dispatch the exact C and C++ compiler forms" >&2; exit 1; \
	}; \
	echo "OK    LLVM probes skip discovery/local tokens and dispatch exact C/C++ compiler forms"; \
	DISTCC_HOSTS='volunteer-a/1'; \
	if probe_all_distcc_hosts 22 "$$resolved_clang" llvm "$$probe_dir"; then \
		echo "FAIL  LLVM volunteer passed without a C++ compiler probe" >&2; exit 1; \
	fi; \
	test "$$remote_distcc_count" -eq 1 && test "$$remote_distcc_failures" -eq 1 || { \
		echo "FAIL  missing LLVM C++ compiler did not fail its volunteer" >&2; exit 1; \
	}; \
	echo "OK    missing LLVM C++ compiler fails closed"; \
	: > "$$tmpdir/probe-tools"; \
	DISTCC_HOSTS='localhost/2 volunteer-a/1 volunteer-vendor/1 volunteer-b/1'; \
	if ! probe_all_distcc_hosts 16 gcc gnu "$$probe_dir" g++; then \
		echo "FAIL  matching GCC majors for every volunteer were rejected" >&2; exit 1; \
	fi; \
	test "$$remote_distcc_count" -eq 3 || { echo "FAIL  expected three remote volunteer probes" >&2; exit 1; }; \
	echo "OK    matching GCC and G++ major identities pass for every remote volunteer"; \
	DISTCC_HOSTS='volunteer-a/1 volunteer-mismatch/1'; \
	if probe_all_distcc_hosts 16 gcc gnu "$$probe_dir" g++; then \
		echo "FAIL  mismatched remote GCC major passed" >&2; exit 1; \
	fi; \
	test "$$remote_distcc_count" -eq 2 && test "$$remote_distcc_failures" -eq 1 || { \
		echo "FAIL  mismatched volunteer did not produce one failed probe" >&2; exit 1; \
	}; \
	echo "OK    mismatched GCC major is rejected"; \
	DISTCC_HOSTS='volunteer-gxx-mismatch/1'; \
	if probe_all_distcc_hosts 16 gcc gnu "$$probe_dir" g++; then \
		echo "FAIL  mismatched remote G++ major passed" >&2; exit 1; \
	fi; \
	test "$$remote_distcc_count" -eq 1 && test "$$remote_distcc_failures" -eq 1 || { \
		echo "FAIL  mismatched G++ major did not produce one failed probe" >&2; exit 1; \
	}; \
	echo "OK    mismatched G++ major is rejected"; \
	DISTCC_HOSTS='volunteer-missing/1'; \
	if probe_all_distcc_hosts 16 gcc gnu "$$probe_dir" g++; then \
		echo "FAIL  missing remote GCC identity passed" >&2; exit 1; \
	fi; \
	test "$$remote_distcc_count" -eq 1 && test "$$remote_distcc_failures" -eq 1 || { \
		echo "FAIL  missing volunteer identity did not fail closed" >&2; exit 1; \
	}; \
	echo "OK    missing GCC identity fails closed"

audit: source-root-check source-root-control-unit-test naming-policy-test \
       headless-hardware-gl-runner-test \
       review-thread-frontier-check review-thread-frontier-unit-test \
       review-thread-corpus-check review-thread-corpus-unit-test \
       loader-environment-function-scope-test \
       source-root-selection-test \
       distcc-probe-directory-test \
       distcc-gcc-major-probe-test \
       vostro-prefix-check audit-werror profile-audit \
       r300-release-profile-contract-test driver-root-audit
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
	distcc_probe_compiler_tool() { \
		compiler_chain="$$1"; compiler_tool="$$2"; \
		case "$$compiler_chain" in \
			ccache) PATH="$${CCACHE_PATH:-$$PATH}" command -v "$$compiler_tool" ;; \
			distcc) printf '%s\n' "$$compiler_tool" ;; \
			*) return 1 ;; \
		esac; \
	}; \
	gcc_version_major() { \
		"$$1" -dumpversion 2>/dev/null | sed 's/[.].*//;q'; \
	}; \
	distcc_host_name() { \
		host_spec="$$1"; host_spec="$${host_spec%%/*}"; host_spec="$${host_spec%%,*}"; \
		host_spec="$${host_spec#@}"; host_spec="$${host_spec#*@}"; \
		case "$$host_spec" in \
			\[*\]:*) host_spec="$${host_spec%%]*}"; host_spec="$${host_spec#\[}" ;; \
			*:*) host_spec="$${host_spec%%:*}" ;; \
		esac; \
		printf '%s\n' "$$host_spec"; \
	}; \
	gcc_object_major() { \
		LC_ALL=C readelf --string-dump=.comment "$$1" 2>/dev/null | \
				sed -n 's/^[[:space:]]*.*GCC: ([^)]*) \([0-9][0-9]*\).*/\1/p' | \
			sed -n '1p'; \
	}; \
	probe_distcc_host() { \
		distcc_spec="$$1"; expected_major="$$2"; compiler_tool="$$3"; \
		compiler_family="$$4"; probe_dir="$$5"; compiler_cxx_tool="$${6:-}"; \
		remote_distcc_host="$$(distcc_host_name "$$distcc_spec")"; \
		if ! getent ahostsv4 "$$remote_distcc_host" >/dev/null 2>&1; then \
			echo "FAIL  distcc volunteer hostname does not resolve: $$distcc_spec" >&2; \
			return 1; \
		fi; \
		if ! rm -f -- "$$probe_dir/probe.c" "$$probe_dir/probe.o" "$$probe_dir/probe.err" || \
			! printf '%s\n' 'int mesa_distcc_probe(void) { return 22; }' > "$$probe_dir/probe.c"; then \
			echo "FAIL  cannot prepare distcc volunteer probe: $$distcc_spec" >&2; \
			return 1; \
		fi; \
		if ! DISTCC_HOSTS="$$distcc_spec" DISTCC_FALLBACK=0 \
			distcc "$$compiler_tool" -Wall -Wextra -Werror \
			-c "$$probe_dir/probe.c" -o "$$probe_dir/probe.o" \
			>/dev/null 2>"$$probe_dir/probe.err"; then \
			echo "FAIL  distcc volunteer dispatch failed: $$distcc_spec ($$compiler_tool)" >&2; \
			sed -n '1,12p' "$$probe_dir/probe.err" >&2; \
			return 1; \
		fi; \
		if [ -z "$$compiler_cxx_tool" ]; then \
			echo "FAIL  distcc volunteer has no verified C++ compiler: $$distcc_spec" >&2; \
			return 1; \
		fi; \
		if [ "$$compiler_family" = gnu ]; then \
			remote_gcc_major="$$(gcc_object_major "$$probe_dir/probe.o")"; \
			if [ -z "$$remote_gcc_major" ]; then \
				echo "FAIL  distcc volunteer object has no GCC identity: $$distcc_spec" >&2; \
				return 1; \
			fi; \
			if [ "$$remote_gcc_major" != "$$expected_major" ]; then \
				echo "FAIL  distcc volunteer GCC major mismatch: $$distcc_spec (client=$$expected_major remote=$$remote_gcc_major)" >&2; \
				return 1; \
			fi; \
		fi; \
		if ! rm -f -- "$$probe_dir/probe.cpp" "$$probe_dir/probe.cpp.o" "$$probe_dir/probe.cpp.err" || \
			! printf '%s\n' 'int mesa_distcc_probe_cpp() { return 23; }' > "$$probe_dir/probe.cpp"; then \
			echo "FAIL  cannot prepare distcc volunteer C++ probe: $$distcc_spec" >&2; \
			return 1; \
		fi; \
		if ! DISTCC_HOSTS="$$distcc_spec" DISTCC_FALLBACK=0 \
			distcc "$$compiler_cxx_tool" -Wall -Wextra -Werror \
			-c "$$probe_dir/probe.cpp" -o "$$probe_dir/probe.cpp.o" \
			>/dev/null 2>"$$probe_dir/probe.cpp.err"; then \
			echo "FAIL  distcc volunteer C++ dispatch failed: $$distcc_spec ($$compiler_cxx_tool)" >&2; \
			sed -n '1,12p' "$$probe_dir/probe.cpp.err" >&2; \
			return 1; \
		fi; \
		if [ "$$compiler_family" = gnu ]; then \
			remote_gxx_major="$$(gcc_object_major "$$probe_dir/probe.cpp.o")"; \
			if [ -z "$$remote_gxx_major" ]; then \
				echo "FAIL  distcc volunteer C++ object has no GCC identity: $$distcc_spec" >&2; \
				return 1; \
			fi; \
			if [ "$$remote_gxx_major" != "$$expected_major" ]; then \
				echo "FAIL  distcc volunteer G++ major mismatch: $$distcc_spec (client=$$expected_major remote=$$remote_gxx_major)" >&2; \
				return 1; \
			fi; \
			ok "distcc volunteer GCC major: $$distcc_spec ($$remote_gcc_major)"; \
			ok "distcc volunteer G++ major: $$distcc_spec ($$remote_gxx_major)"; \
		else \
			ok "distcc volunteer C dispatch: $$distcc_spec ($$compiler_tool)"; \
			ok "distcc volunteer C++ dispatch: $$distcc_spec ($$compiler_cxx_tool)"; \
		fi; \
	}; \
	find_versioned_gcc_tool() { \
		tool="$$1"; version="$$2"; \
		for candidate in "$$tool-$$version" "$$tool$$version"; do \
			if command -v "$$candidate" >/dev/null 2>&1 && \
			   [ "$$(gcc_version_major "$$candidate")" = "$$version" ]; then \
				printf '%s\n' "$$candidate"; return 0; \
			fi; \
		done; \
		return 1; \
	}; \
	find_audit_gcc_tool() { \
		tool="$$1"; requested="$$2"; \
		if [ -n "$$requested" ]; then \
			find_versioned_gcc_tool "$$tool" "$$requested"; \
		else \
			command -v "$$tool" >/dev/null 2>&1 || return 1; \
			printf '%s\n' "$$tool"; \
		fi; \
	}; \
	need_gnu_family() { \
		grep -Eq "^[[:space:]]*llvm[[:space:]]*=[[:space:]]*'disabled'" "$(CONFIG)" || { \
			fail "COMPILER_FAMILY=gnu requires a profile with llvm='disabled'; $(CONFIG) enables LLVM"; \
			return; \
		}; \
		requested_gcc="$${MESA_GCC_VERSION:-}"; \
		audit_gcc_tool="$$(find_audit_gcc_tool gcc "$$requested_gcc")" || { \
			if [ -n "$$requested_gcc" ]; then \
				fail "requested GCC major $$requested_gcc lacks matching gcc-$$requested_gcc"; \
			else \
				fail "missing gcc on PATH"; \
			fi; \
			return; \
		}; \
		audit_gxx_tool="$$(find_audit_gcc_tool g++ "$$requested_gcc")" || { \
			if [ -n "$$requested_gcc" ]; then \
				fail "requested GCC major $$requested_gcc lacks matching g++-$$requested_gcc"; \
			else \
				fail "missing g++ on PATH"; \
			fi; \
			return; \
		}; \
		gcc_major="$$(gcc_version_major "$$audit_gcc_tool")"; \
		gxx_major="$$(gcc_version_major "$$audit_gxx_tool")"; \
		if [ -z "$$gcc_major" ] || [ "$$gcc_major" != "$$gxx_major" ]; then \
			fail "GCC compiler pair has mismatched majors: gcc=$${gcc_major:-unresolved}, g++=$${gxx_major:-unresolved}"; \
			return; \
		fi; \
		audit_compiler_tool="$$audit_gcc_tool"; \
		audit_compiler_cxx_tool="$$audit_gxx_tool"; \
		audit_compiler_major="$$gcc_major"; \
		if [ -n "$$requested_gcc" ]; then \
			ok "GNU family $$gcc_major with pinned gcc/g++"; \
		else \
			ok "GNU family $$gcc_major with PATH gcc/g++"; \
		fi; \
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
		requested="$${MESA_LLVM_VERSION:-}"; selected=""; \
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
			audit_compiler_tool="$$(find_audit_compiler_tool clang "$$selected")"; \
			audit_compiler_cxx_tool="$$(find_audit_compiler_tool clang++ "$$selected")"; \
			audit_compiler_major="$$selected"; \
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
	case "$(COMPILER_FAMILY)" in \
		llvm) need_llvm_family "$$needs_llvm_config" ;; \
		gnu) need_gnu_family ;; \
		*) fail "unknown COMPILER_FAMILY=$(COMPILER_FAMILY)" ;; \
	esac; \
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
		if [ "$${MESA_DISTCC_JOBS:-}" = "$(DISTCC_JOBS)" ]; then \
			ok "distcc aggregate build ceiling is $(DISTCC_JOBS) jobs"; \
		else \
			fail "distcc aggregate build ceiling is $${MESA_DISTCC_JOBS:-unset}; expected $(DISTCC_JOBS)"; \
		fi; \
		distcc_slots="$$(printf '%s\n' "$${DISTCC_HOSTS:-}" | grep -oE '/[0-9]+' | tr -d / | awk '{sum += $$1} END {print sum + 0}')"; \
		if [ "$$distcc_slots" = "$(DISTCC_JOBS)" ]; then \
			ok "distcc host allocation totals $(DISTCC_JOBS) slots"; \
		else \
			fail "distcc host allocation totals $$distcc_slots slots; expected $(DISTCC_JOBS)"; \
		fi; \
		if [ "$(COMPILER_FAMILY)" = gnu ]; then need_command readelf; fi; \
		if $(CREATE_AND_ASSIGN_DISTCC_PROBE_DIRECTORY); then \
			audit_distcc_compiler_tool="$$(distcc_probe_compiler_tool "$(COMPILER_CHAIN)" "$${audit_compiler_tool:-}" || true)"; \
			audit_distcc_compiler_cxx_tool="$$(distcc_probe_compiler_tool "$(COMPILER_CHAIN)" "$${audit_compiler_cxx_tool:-}" || true)"; \
			remote_distcc_count=0; remote_distcc_failures=0; \
			for distcc_spec in $${DISTCC_HOSTS:-}; do \
				case "$$distcc_spec" in --*|+*) continue ;; esac; \
				remote_distcc_host="$$(distcc_host_name "$$distcc_spec")"; \
				case "$$remote_distcc_host" in ""|localhost|localhost.localdomain) continue ;; esac; \
				remote_distcc_count=$$((remote_distcc_count + 1)); \
				if [ -n "$$audit_distcc_compiler_tool" ] && \
				   [ -n "$$audit_distcc_compiler_cxx_tool" ] && \
				   probe_distcc_host "$$distcc_spec" "$${audit_compiler_major:-}" "$$audit_distcc_compiler_tool" "$(COMPILER_FAMILY)" "$$probe_dir" "$$audit_distcc_compiler_cxx_tool"; then \
					:; \
				else \
					remote_distcc_failures=$$((remote_distcc_failures + 1)); \
				fi; \
			done; \
			if [ "$$remote_distcc_count" -eq 0 ]; then \
				fail "distcc host allocation has no remote compiler"; \
			elif [ "$$remote_distcc_failures" -gt 0 ]; then \
				fail "distcc volunteer verification failed for $$remote_distcc_failures of $$remote_distcc_count remote compiler(s)"; \
			else \
				ok "distcc volunteer verification passed for $$remote_distcc_count remote compiler(s)"; \
			fi; \
			if ! rm -f -- "$$probe_dir/probe.c" "$$probe_dir/probe.o" "$$probe_dir/probe.err" \
				"$$probe_dir/probe.cpp" "$$probe_dir/probe.cpp.o" "$$probe_dir/probe.cpp.err"; then \
				fail "cannot remove distcc remote probe files: $$probe_dir"; \
			elif ! rmdir -- "$$probe_dir"; then \
				fail "cannot remove distcc remote probe directory: $$probe_dir"; \
			fi; \
		else \
			fail "cannot create distcc remote probe directory"; \
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
		echo "  meson ninja-build clang binutils pkg-config ccache distcc sccache"; \
	echo "  libdrm-dev libxcb-dri3-dev libxcb-present-dev libxshmfence-dev"; \
	echo "  libx11-xcb-dev libxrandr-dev libxcb-randr0-dev libxcb-sync-dev"; \
	echo "  libxcb-xfixes0-dev libwayland-dev wayland-protocols python3-mako"; \
	echo "  python3-ply python3-packaging libvulkan-dev glslang-tools"; \
	if [ "$$rusticl_enabled" -eq 1 ]; then echo "  rust-bindgen llvm-dev libclang-dev"; fi; \
	if [ "$$failures" -gt 0 ]; then echo; echo "Terakan host audit failed: $$failures failure(s), $$warnings warning(s)." >&2; exit 1; fi; \
	echo; echo "Terakan host audit passed with $$warnings warning(s)."

configure: source-root-check vostro-prefix-check
	@case "$(COMPILER_FAMILY)" in \
		llvm|gnu) ;; \
		*) echo "unknown COMPILER_FAMILY=$(COMPILER_FAMILY); expected llvm or gnu" >&2; exit 2 ;; \
	esac; \
	$(call CHECK_LAYOUT,configure); \
	$(ACQUIRE_BUILD_LOCK) \
	$(call VERIFY_CAPTURED_INPUTS,source_root control_root build_root builddir prefix sysconfdir); \
	$(PREPARE_SOURCE_IDENTITY); \
	$(PREPARE_SOURCE_VIEW); \
	mkdir -p "$(dir $(BUILDDIR))"; \
	test -f "$(ENVFILE)" && . "$(ENVFILE)" || true; \
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
	gcc_version_major() { \
		"$$1" -dumpversion 2>/dev/null | sed 's/[.].*//;q'; \
	}; \
	find_versioned_gcc_tool() { \
		tool="$$1"; version="$$2"; \
		for candidate in "$$tool-$$version" "$$tool$$version"; do \
			if command -v "$$candidate" >/dev/null 2>&1 && \
			   [ "$$(gcc_version_major "$$candidate")" = "$$version" ]; then \
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
		requested_gcc="$${MESA_GCC_VERSION:-}"; \
		if [ -n "$$requested_gcc" ]; then \
			cc_cmd="$$(find_versioned_gcc_tool gcc "$$requested_gcc")" || { \
				echo "requested GCC major $$requested_gcc lacks gcc-$$requested_gcc" >&2; exit 1; }; \
			cxx_cmd="$$(find_versioned_gcc_tool g++ "$$requested_gcc")" || { \
				echo "requested GCC major $$requested_gcc lacks g++-$$requested_gcc" >&2; exit 1; }; \
		else \
			command -v gcc >/dev/null 2>&1 || { echo "no gcc on PATH" >&2; exit 1; }; \
			command -v g++ >/dev/null 2>&1 || { echo "no g++ on PATH" >&2; exit 1; }; \
			cc_cmd=gcc; cxx_cmd=g++; \
		fi; \
		gcc_major="$$(gcc_version_major "$$cc_cmd")"; \
		gxx_major="$$(gcc_version_major "$$cxx_cmd")"; \
		if [ -z "$$gcc_major" ] || [ -z "$$gxx_major" ] || [ "$$gcc_major" != "$$gxx_major" ]; then \
			echo "mismatched GCC compiler majors: gcc=$${gcc_major:-unresolved}, g++=$${gxx_major:-unresolved}" >&2; exit 1; \
		fi; \
		if [ -n "$$requested_gcc" ] && [ "$$gcc_major" != "$$requested_gcc" ]; then \
			echo "requested GCC major $$requested_gcc resolved to gcc major $$gcc_major" >&2; exit 1; \
		fi; \
		cc_direct_cmd="$$(command -v "$$cc_cmd")"; \
		cxx_direct_cmd="$$(command -v "$$cxx_cmd")"; \
		llvm_config_cmd=""; llvm_ar_cmd=""; llvm_nm_cmd=""; \
		llvm_strip_cmd=""; llvm_objcopy_cmd=""; \
		toolchain_label="GCC $$("$$cc_cmd" -dumpversion | sed 's/[.].*//')"; \
	else \
	if grep -Eq "^[[:space:]]*llvm[[:space:]]*=[[:space:]]*'disabled'" "$(CONFIG)"; then needs_llvm_config=0; else needs_llvm_config=1; fi; \
	requested="$${MESA_LLVM_VERSION:-}"; \
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
	test -f "$(ENVFILE)" && . "$(ENVFILE)" || true; \
	for var in $(MESON_SETUP_LOCAL_UNSET); do unset "$$var"; done; \
	profile_dflags=$$(python3 "$(PROFILE_DFLAGS_SCRIPT)" "$(CONFIG)") || { \
		echo "failed to extract profile -D flags from $(CONFIG)" >&2; exit 2; }; \
	echo "profile -D flags: $$profile_dflags"; \
	$(MESON) setup $(SETUP_ARGS) $$profile_dflags \
		--prefix="$(PREFIX)" --sysconfdir="$${MESA_SYSCONFDIR_INPUT}" \
		"$(BUILDDIR)" "$(MESON_SOURCE_ROOT)" || { \
			echo "Meson setup failed; source identity remains provisional" >&2; \
			exit 2; \
		}; \
	$(WRITE_SOURCE_IDENTITY)

build: source-root-check
	@$(call CHECK_LAYOUT,build); \
	test -d "$(BUILDDIR)" || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }; \
	test -f "$(ENVFILE)" && . "$(ENVFILE)" || true; \
	$(ACQUIRE_BUILD_LOCK) \
	$(call VERIFY_CAPTURED_INPUTS,source_root control_root build_root builddir prefix sysconfdir); \
	$(call CHECK_LAYOUT,build); \
	$(VERIFY_SOURCE_IDENTITY); \
	build_jobs="$(JOBS)"; \
	case "$(COMPILER_CHAIN):$${CCACHE_PREFIX:-}" in \
		distcc:*|ccache:distcc|ccache:*/distcc) \
			build_jobs="$${MESA_DISTCC_JOBS:-$(DISTCC_JOBS)}" ;; \
		ccache:*) \
			[ -z "$${MESA_DISTCC_JOBS:-}" ] || \
				build_jobs="$$MESA_DISTCC_JOBS" ;; \
	esac; \
	case "$$build_jobs" in \
		''|*[!0-9]*|0) echo "invalid Ninja job count: $$build_jobs" >&2; exit 2 ;; \
	esac; \
	echo "ninja jobs: $$build_jobs ($(COMPILER_CHAIN))"; \
	env $(LOCKED_TOOL_ENV) \
	    $(NINJA) -C "$(BUILDDIR)" -j"$$build_jobs" $(NINJA_ARGS) $(NINJA_TARGETS); \
	build_status=$$?; \
	$(VERIFY_SOURCE_IDENTITY); \
	exit "$$build_status"

clean-registered-build:
	@selected_builddir_state="$$(python3 "$(SOURCE_ROOT_CONTROL)" selected-builddir-state)" || exit 2; \
	case "$$selected_builddir_state" in \
		present) $(MAKE) --no-print-directory clean ;; \
		absent) echo "no registered build exists: $(BUILDDIR)" ;; \
		*) echo "invalid selected build-directory state: $$selected_builddir_state" >&2; exit 2 ;; \
	esac

clean:
	@$(ACQUIRE_BUILD_LOCK) \
	$(call VERIFY_CAPTURED_INPUTS,source_root control_root build_root builddir prefix sysconfdir); \
	$(call CHECK_LAYOUT,clean); \
	$(VERIFY_DELETE_IDENTITY); \
	$(REMOVE_TREE) "$(BUILDDIR)" || { \
		echo "failed to remove build directory: $(BUILDDIR)" >&2; exit 2; \
	}; \
	{ [ ! -e "$(BUILDDIR)" ] && [ ! -L "$(BUILDDIR)" ]; } || { \
		echo "build directory remains after removal: $(BUILDDIR)" >&2; exit 2; \
	}; \
	echo "cleaned: $(BUILDDIR)"

# Remove the entire repo-local build/ tree (every profile's builddir), not just
# the active PROFILE's.  The tree is gitignored (/build), so this only drops
# regenerable artifacts and never touches the source or an installed PREFIX.
clean-all:
	@$(ACQUIRE_BUILD_LOCK) \
	$(call VERIFY_CAPTURED_INPUTS,source_root control_root build_root); \
	$(call CHECK_LAYOUT,clean-all); \
	$(REMOVE_TREE) "$(BUILD_ROOT)" || { \
		echo "failed to remove build root: $(BUILD_ROOT)" >&2; exit 2; \
	}; \
	{ [ ! -e "$(BUILD_ROOT)" ] && [ ! -L "$(BUILD_ROOT)" ]; } || { \
		echo "build root remains after removal: $(BUILD_ROOT)" >&2; exit 2; \
	}; \
	echo "cleaned: $(BUILD_ROOT) (all profile builddirs)"

distclean:
	@$(ACQUIRE_BUILD_LOCK) \
	$(call VERIFY_CAPTURED_INPUTS,source_root control_root build_root builddir prefix sysconfdir); \
	$(call CHECK_LAYOUT,distclean); \
	$(VERIFY_DISTCLEAN_IDENTITY); \
	$(REMOVE_TREE) "$(BUILDDIR)" || { \
		echo "failed to remove build directory: $(BUILDDIR)" >&2; exit 2; \
	}; \
	{ [ ! -e "$(BUILDDIR)" ] && [ ! -L "$(BUILDDIR)" ]; } || { \
		echo "build directory remains after removal: $(BUILDDIR)" >&2; exit 2; \
	}; \
	echo "cleaned: $(BUILDDIR)"; \
	if [ -e "$(PREFIX)" ] || [ -L "$(PREFIX)" ]; then \
		archive="$(PREFIX).disabled.$$(date -u +%Y%m%dT%H%M%SZ).$$$$"; \
		{ [ ! -e "$$archive" ] && [ ! -L "$$archive" ]; } || { \
			echo "archive target already exists: $$archive" >&2; exit 2; \
		}; \
		$(SUDO) mv "$(PREFIX)" "$$archive" || { \
			echo "failed to archive prefix: $(PREFIX)" >&2; exit 2; \
		}; \
		{ [ ! -e "$(PREFIX)" ] && [ ! -L "$(PREFIX)" ]; } && \
		{ [ -e "$$archive" ] || [ -L "$$archive" ]; } || { \
			echo "prefix archival postcondition failed: $(PREFIX)" >&2; exit 2; \
		}; \
		echo "archived prefix: $(PREFIX) -> $$archive"; \
	else \
		echo "prefix already absent: $(PREFIX)"; \
	fi

install: source-root-check vostro-prefix-check
	@$(call CHECK_LAYOUT,install); \
	test -d "$(BUILDDIR)" || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }
	@test -f "$(ENVFILE)" && . "$(ENVFILE)" || true; \
	mkdir -p "$(dir $(BUILD_LOCK))"; \
	env $(LOCKED_TOOL_ENV) MESA_SYSCONFDIR_INPUT="$(SYSCONFDIR)" \
	    $(FLOCK) -x -w $(LOCK_WAIT) "$(BUILD_LOCK)" bash -c 'set -euo pipefail; \
	    python3 "$(SOURCE_ROOT_CONTROL)" verify-captured-inputs \
	      --fields source_root control_root build_root builddir prefix sysconfdir; \
	    python3 "$(SOURCE_ROOT_CONTROL)" check-layout --operation=install; \
	    python3 "$(SOURCE_ROOT_CONTROL)" verify-identity; \
	    python3 "$(SOURCE_ROOT_CONTROL)" prepare-identity; \
	    python3 "$(SOURCE_ROOT_CONTROL)" prepare-source-view; \
	    echo "[install] flock acquired; reconfiguring $(BUILDDIR) with --prefix=$(PREFIX)"; \
	    for var in $(MESON_SETUP_LOCAL_UNSET); do unset "$$var"; done; \
	    profile_dflags=$$(python3 "$(PROFILE_DFLAGS_SCRIPT)" "$(CONFIG)") || { \
	      echo "failed to extract profile -D flags from $(CONFIG)" >&2; exit 2; }; \
	    $(MESON) setup $(SETUP_ARGS) $$profile_dflags --prefix="$(PREFIX)" \
	      --sysconfdir="$$MESA_SYSCONFDIR_INPUT" "$(BUILDDIR)" "$(MESON_SOURCE_ROOT)" || { \
	      echo "Meson setup failed; source identity remains provisional" >&2; \
	      exit 2; \
	    }; \
	    echo "[install] running meson install --no-rebuild --only-changed -C $(BUILDDIR)"; \
	    $(SUDO) $(MESON) install --no-rebuild --only-changed -C "$(BUILDDIR)" || { \
	      echo "Meson install failed; source identity remains provisional" >&2; \
	      exit 2; \
	    }; \
	    : "sudo meson install writes meson-logs/install-log.txt as root; restore"; \
	    : "ownership so a later non-sudo makepkg of the same builddir can rewrite"; \
	    : "it instead of failing package() with a PermissionError under fakeroot"; \
	    $(SUDO) chown -R "$$(id -un):$$(id -gn)" "$(BUILDDIR)/meson-logs" || { \
	      echo "Meson install log ownership restore failed; source identity remains provisional" >&2; \
	      exit 2; \
	    }; \
	    python3 "$(SOURCE_ROOT_CONTROL)" write-identity; \
	    python3 "$(SOURCE_ROOT_CONTROL)" verify-identity; \
	    echo "[install] complete; releasing flock"'

artifact-check: source-root-check vostro-prefix-check
	@set -euo pipefail; \
	$(call CHECK_LAYOUT,artifact); \
	$(ACQUIRE_BUILD_LOCK) \
	$(call VERIFY_CAPTURED_INPUTS,source_root control_root build_root builddir prefix sysconfdir); \
	$(call CHECK_LAYOUT,artifact); \
	$(VERIFY_ARTIFACT_IDENTITY); \
	test -d "$(PREFIX)" || { echo "prefix missing: $(PREFIX)"; exit 1; }; \
	if ! artifact_rows="$$(find "$(PREFIX)" -maxdepth 5 -type f \
		\( -name "libvulkan_terascale.so" -o -name "libvulkan_r3v.so" -o \
		   -name "r600_dri.so" -o -name "r300_dri.so" -o \
		   -name "libdril_dri.so" -o -name "libgallium-*.so" -o \
		   -name "terascale_icd*.json" -o -name "r3v_icd*.json" \) \
		-printf "%p %TY-%Tm-%Td %TH:%TM:%TS %s bytes\n" 2>&1)"; then \
		echo "FAIL  artifact scan could not read PREFIX: $(PREFIX)" >&2; \
		printf '%s\n' "$$artifact_rows" >&2; \
		exit 2; \
	fi; \
	printf '%s\n' "$$artifact_rows" | sort

test: source-root-check
	@$(call CHECK_LAYOUT,test); \
	test -d "$(BUILDDIR)" || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }; \
	test -f "$(ENVFILE)" && . "$(ENVFILE)" || true; \
	$(ACQUIRE_BUILD_LOCK) \
	$(call VERIFY_CAPTURED_INPUTS,source_root control_root build_root builddir prefix sysconfdir); \
	$(call CHECK_LAYOUT,test); \
	$(VERIFY_SOURCE_IDENTITY); \
	env $(LOCKED_TOOL_ENV) \
	    $(MESON) test -C "$(BUILDDIR)" $(MESON_TEST_ARGS); \
	test_status=$$?; \
	$(VERIFY_SOURCE_IDENTITY); \
	exit "$$test_status"

build-lease-test:
	@tmpdir="$$(python3 "$(SOURCE_ROOT_CONTROL)" \
		create-test-directory --label=build-lease)"; \
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
	printf '%s\n' "$$install_plan" | \
		grep -F -- "MESA_SYSCONFDIR_INPUT=\"$$tmpdir/etc\"" >/dev/null || { \
			echo "FAIL  install omitted canonical SYSCONFDIR transport" >&2; \
			exit 1; \
		}; \
	printf '%s\n' "$$install_plan" | \
		grep -F -- '--sysconfdir="$$MESA_SYSCONFDIR_INPUT"' >/dev/null || { \
			echo "FAIL  install interpolated SYSCONFDIR into locked program text" >&2; \
			exit 1; \
		}; \
	echo "OK    install transports canonical SYSCONFDIR outside locked program text"; \
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
	@set -euo pipefail; \
	while IFS= read -r environment_name; do \
		case "$$environment_name" in \
			GIT_*) unset "$$environment_name" ;; \
		esac; \
	done < <(compgen -e); \
	export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null \
		GIT_CONFIG_NOSYSTEM=1 GIT_NO_REPLACE_OBJECTS=1 \
		GIT_TERMINAL_PROMPT=0; \
	tmpdir="$$(python3 "$(SOURCE_ROOT_CONTROL)" \
		create-test-directory --label=source-root)"; \
	trap 'rm -rf "$$tmpdir"' EXIT HUP INT TERM; \
	if legacy_make_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		MAKE_VERSION=4.1 source-root-check 2>&1 )"; then \
		echo "FAIL  source-root control accepted GNU Make 4.1" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$legacy_make_output" | \
		grep -F "GNU Make 4.2 or newer is required" >/dev/null || { \
		echo "FAIL  old GNU Make returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$legacy_make_output" >&2; exit 1; \
	}; \
	echo "OK    source-root control declares the GNU Make 4.2 floor"; \
	make_version_marker="$$tmpdir/make-version-executed"; \
	hostile_make_version='4"; touch '"$$make_version_marker"'; #.4'; \
	if make_version_output="$$( $(MAKE) --no-print-directory \
		-C "$(CURDIR)" help "MAKE_VERSION=$$hostile_make_version" 2>&1 )"; then \
		echo "FAIL  MAKE_VERSION accepted shell metacharacters" >&2; \
		exit 1; \
	fi; \
	test ! -e "$$make_version_marker" || { \
		echo "FAIL  MAKE_VERSION entered shell program text" >&2; exit 1; \
	}; \
	printf '%s\n' "$$make_version_output" | \
		grep -F "GNU Make version is invalid" >/dev/null || { \
			echo "FAIL  MAKE_VERSION metacharacters returned the wrong diagnostic" >&2; \
			printf '%s\n' "$$make_version_output" >&2; exit 1; \
		}; \
	echo "OK    MAKE_VERSION comparison treats caller text as data"; \
	if lower_bound_make="$$(command -v make-4.2 2>/dev/null)"; then \
		lower_bound_output="$$( "$$lower_bound_make" --no-print-directory \
			-C "$(CURDIR)" help TOPSRC="$(CONTROL_ROOT)" \
			BUILD_ROOT="$$tmpdir/make42-build" \
			BUILDDIR="$$tmpdir/make42-build/probe" \
			PREFIX="$$tmpdir/make42-build/prefix" 2>&1 )" || { \
			echo "FAIL  GNU Make 4.2 could not transport path inputs" >&2; \
			printf '%s\n' "$$lower_bound_output" >&2; exit 1; \
		}; \
		printf '%s\n' "$$lower_bound_output" | \
			grep -F "topsrc:    $(CONTROL_ROOT)" >/dev/null || { \
			echo "FAIL  GNU Make 4.2 returned the wrong source root" >&2; \
			printf '%s\n' "$$lower_bound_output" >&2; exit 1; \
		}; \
		echo "OK    GNU Make 4.2 transports parse-time path inputs"; \
	else \
		echo "SKIP  GNU Make 4.2 executable is unavailable"; \
	fi; \
	source_root="$$tmpdir/source"; \
	mkdir -p "$$source_root"; \
	git -C "$$source_root" init -q; \
	touch "$$source_root/meson.build" "$$source_root/meson.options" \
		"$$source_root/source_view_bytecode_probe.py"; \
	printf '/build/\n' > "$$source_root/.gitignore"; \
	git -C "$$source_root" add .gitignore meson.build meson.options \
		source_view_bytecode_probe.py; \
	git -C "$$source_root" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: add Mesa root markers"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" source-root-check \
		TOPSRC="$$source_root" >/dev/null; \
	echo "OK    external TOPSRC accepts a clean committed Mesa worktree"; \
	source_commit="$$(git -C "$$source_root" rev-parse HEAD)"; \
	source_tree="$$(git -C "$$source_root" rev-parse HEAD^{tree})"; \
	control_commit="$$(git -C "$(CONTROL_ROOT)" rev-parse HEAD)"; \
	control_tree="$$(git -C "$(CONTROL_ROOT)" rev-parse HEAD^{tree})"; \
	resolver_override_output="$$( $(MAKE) --no-print-directory \
		-C "$(CURDIR)" source-root-check TOPSRC="$$source_root" \
		CURDIR=/tmp BUILD_INFRA_ROOT=/tmp BUILD_INFRA_MAKEFILE=/tmp \
		CONTROL_ROOT=/ SOURCE_ROOT_CONTROL=/bin/false \
		MOUNT_BOUNDARY_CALIBRATION=/bin/false \
		SOURCE_ROOT_FIELDS='a b c d e f g h i j k l m n o p' \
		SOURCE_ROOT_RESOLVE_STATUS=1 SOURCE_COMMIT=forged \
		SOURCE_TREE=forged CONTROL_COMMIT=forged CONTROL_TREE=forged \
		MESA_TOPSRC_INPUT=/ MESA_BUILD_ROOT_INPUT=/ \
		MESA_BUILDDIR_INPUT=/ MESA_PREFIX_INPUT=/ \
		MESA_SYSCONFDIR_INPUT=/ \
		MESA_PROFILE_INPUT=forged \
		MESA_HOSTENV_INPUT=forged \
		MESA_PROFILE_RAW=forged MESA_HOSTENV_RAW=forged \
		MESA_INSTALL_NAMESPACE_RAW=forged MESA_MODE_RAW=forged \
		MESA_COMPILER_CHAIN_RAW=forged \
		MESA_COMPILER_FAMILY_RAW=forged \
		MESA_REPRODUCIBLE_RUN_RAW=forged \
		MODE_SELECTOR=forged \
		SELECTOR_FIELDS='forged forged forged forged forged forged forged' \
		SELECTOR_RESOLVE_STATUS=1 COMMAND_VARIABLE_WITH_DOLLAR=forged \
		MESA_CONTROL_ROOT_INPUT=/ \
		MESA_SOURCE_ROOT_ANCHOR=forged \
		MESA_CONTROL_ROOT_ANCHOR=forged \
		MESA_BUILD_ROOT_ANCHOR=forged \
		MESA_BUILDDIR_ANCHOR=forged \
		MESA_PREFIX_ANCHOR=forged \
		MESA_SYSCONFDIR_ANCHOR=forged \
		MESA_SOURCE_COMMIT_CAPTURED=forged \
		MESA_SOURCE_TREE_CAPTURED=forged \
		MESA_CONTROL_COMMIT_CAPTURED=forged \
		MESA_CONTROL_TREE_CAPTURED=forged \
		make_shell_quote=forged 2>&1 \
	)" || { \
		echo "FAIL  command-line proof inputs replaced resolver authority" >&2; \
		printf '%s\n' "$$resolver_override_output" >&2; exit 1; \
	}; \
	for expected_line in \
		"OK    source root: $$source_root" \
		"OK    source commit: $$source_commit" \
		"OK    source tree: $$source_tree" \
		"OK    control root: $(CONTROL_ROOT)" \
		"OK    control commit: $$control_commit" \
		"OK    control tree: $$control_tree"; do \
		printf '%s\n' "$$resolver_override_output" | \
			grep -F "$$expected_line" >/dev/null || { \
				echo "FAIL  resolver authority returned the wrong identity" >&2; \
				printf '%s\n' "$$resolver_override_output" >&2; exit 1; \
			}; \
	done; \
	echo "OK    command-line variables cannot forge resolver-owned inputs"; \
	selector_marker="$$tmpdir/selector-executed"; \
	make_selector_input='$$(shell touch '"$$selector_marker"')'; \
	for selector_name in PROFILE HOSTENV INSTALL_NAMESPACE MODE \
		COMPILER_CHAIN COMPILER_FAMILY REPRODUCIBLE_RUN; do \
		if selector_output="$$( $(MAKE) --no-print-directory \
			-C "$(CURDIR)" help \
			"$$selector_name=$$make_selector_input" 2>&1 )"; then \
			echo "FAIL  $$selector_name accepted a Make function" >&2; \
			exit 1; \
		fi; \
		test ! -e "$$selector_marker" || { \
			echo "FAIL  $$selector_name executed a Make function" >&2; \
			exit 1; \
		}; \
		printf '%s\n' "$$selector_output" | \
			grep -F "contains executable dollar syntax" >/dev/null || { \
				echo "FAIL  $$selector_name returned the wrong diagnostic" >&2; \
				printf '%s\n' "$$selector_output" >&2; exit 1; \
			}; \
	done; \
	hostenv_marker="$$tmpdir/hostenv-injected"; \
	hostenv_injection='missing; touch '"$$hostenv_marker"'; #'; \
	if hostenv_output="$$( $(MAKE) --no-print-directory \
		-C "$(CURDIR)" configure \
		BUILD_ROOT="$$tmpdir/selector-build" \
		BUILDDIR="$$tmpdir/selector-build/probe" \
		PREFIX="$$tmpdir/selector-build/prefix" \
		HOSTENV="$$hostenv_injection" MESON=true 2>&1 )"; then \
		echo "FAIL  HOSTENV accepted shell metacharacters" >&2; \
		exit 1; \
	fi; \
	test ! -e "$$hostenv_marker" || { \
		echo "FAIL  HOSTENV executed shell metacharacters" >&2; exit 1; \
	}; \
	printf '%s\n' "$$hostenv_output" | \
		grep -F "contains an invalid leaf identifier" >/dev/null || { \
			echo "FAIL  HOSTENV metacharacters returned the wrong diagnostic" >&2; \
			printf '%s\n' "$$hostenv_output" >&2; exit 1; \
		}; \
	echo "OK    recursive build selectors reject executable syntax"; \
	sysconfdir_marker="$$tmpdir/sysconfdir-executed"; \
	sysconfdir_injection='"; touch '"$$sysconfdir_marker"'; #'; \
	if sysconfdir_output="$$( $(MAKE) --no-print-directory \
		-C "$(CURDIR)" install \
		BUILD_ROOT="$$tmpdir/sysconfdir-build" \
		BUILDDIR="$$tmpdir/sysconfdir-build/probe" \
		PREFIX="$$tmpdir/sysconfdir-build/prefix" \
		SYSCONFDIR="$$sysconfdir_injection" \
		MESON=true SUDO=true 2>&1 )"; then \
		echo "FAIL  SYSCONFDIR accepted shell metacharacters" >&2; \
		exit 1; \
	fi; \
	test ! -e "$$sysconfdir_marker" || { \
		echo "FAIL  SYSCONFDIR entered locked shell program text" >&2; exit 1; \
	}; \
	printf '%s\n' "$$sysconfdir_output" | \
		grep -F "MESA_SYSCONFDIR_INPUT contains unsupported path characters" \
		>/dev/null || { \
			echo "FAIL  SYSCONFDIR metacharacters returned the wrong diagnostic" >&2; \
			printf '%s\n' "$$sysconfdir_output" >&2; exit 1; \
		}; \
	echo "OK    SYSCONFDIR rejects shell metacharacters before recipe planning"; \
	execution_control_marker="$$tmpdir/build-execution-control-executed"; \
	execution_control_injection='1"; touch '"$$execution_control_marker"'; #'; \
	for control_name in JOBS DISTCC_JOBS LOCK_WAIT; do \
		if control_output="$$( $(MAKE) --no-print-directory \
			-C "$(CURDIR)" help \
			"$$control_name=$$execution_control_injection" 2>&1 )"; then \
			echo "FAIL  $$control_name accepted shell metacharacters" >&2; \
			exit 1; \
		fi; \
		test ! -e "$$execution_control_marker" || { \
			echo "FAIL  $$control_name entered shell program text" >&2; exit 1; \
		}; \
		printf '%s\n' "$$control_output" | \
			grep -F "MESA_$${control_name}_INPUT must be a decimal integer" \
			>/dev/null || { \
				echo "FAIL  $$control_name returned the wrong diagnostic" >&2; \
				printf '%s\n' "$$control_output" >&2; exit 1; \
			}; \
	done; \
	build_lock_injection='"; touch '"$$execution_control_marker"'; #'; \
	if build_lock_output="$$( $(MAKE) --no-print-directory \
		-C "$(CURDIR)" help BUILD_LOCK="$$build_lock_injection" 2>&1 )"; then \
		echo "FAIL  BUILD_LOCK accepted shell metacharacters" >&2; \
		exit 1; \
	fi; \
	test ! -e "$$execution_control_marker" || { \
		echo "FAIL  BUILD_LOCK entered shell program text" >&2; exit 1; \
	}; \
	printf '%s\n' "$$build_lock_output" | \
		grep -F "MESA_BUILD_LOCK_INPUT contains unsupported path characters" \
		>/dev/null || { \
			echo "FAIL  BUILD_LOCK metacharacters returned the wrong diagnostic" >&2; \
			printf '%s\n' "$$build_lock_output" >&2; exit 1; \
		}; \
	echo "OK    build execution controls reject shell metacharacters"; \
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
	command_marker="$$tmpdir/command-substitution-executed"; \
	command_input='$$(touch '"$$command_marker"')'; \
	if command_output="$$( $(MAKE) --no-print-directory -n -C "$(CURDIR)" \
		configure TOPSRC="$$command_input" 2>&1 )"; then \
		echo "FAIL  source-root-check accepted command substitution" >&2; \
		exit 1; \
	fi; \
	test ! -e "$$command_marker" || { \
		echo "FAIL  TOPSRC command substitution executed" >&2; exit 1; \
	}; \
	printf '%s\n' "$$command_output" | \
		grep -F "contains executable dollar syntax" >/dev/null || { \
		echo "FAIL  command-substitution TOPSRC returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$command_output" >&2; exit 1; \
	}; \
	echo "OK    parse-time transport preserves command substitution as data"; \
	quote_input="source'quote"; \
	if quote_output="$$( $(MAKE) --no-print-directory -n -C "$(CURDIR)" \
		configure TOPSRC="$$quote_input" 2>&1 )"; then \
		echo "FAIL  source-root-check accepted a single quote" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$quote_output" | \
		grep -F "contains unsupported path characters" >/dev/null || { \
		echo "FAIL  single-quote TOPSRC returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$quote_output" >&2; exit 1; \
	}; \
	echo "OK    parse-time transport rejects single quotes as data"; \
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
	unsafe_physical="$$tmpdir/source'physical"; \
	unsafe_link="$$tmpdir/safe-source-link"; \
	mkdir -p "$$unsafe_physical"; \
	git -C "$$unsafe_physical" init -q; \
	touch "$$unsafe_physical/meson.build" "$$unsafe_physical/meson.options"; \
	git -C "$$unsafe_physical" add meson.build meson.options; \
	git -C "$$unsafe_physical" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: add unsafe physical root"; \
	ln -s "$$unsafe_physical" "$$unsafe_link"; \
	if resolved_output="$$( $(MAKE) --no-print-directory -n -C "$(CURDIR)" \
		configure TOPSRC="$$unsafe_link" 2>&1 )"; then \
		echo "FAIL  source-root-check accepted an unsafe resolved path" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$resolved_output" | \
		grep -F "resolves to a path with unsupported characters" >/dev/null || { \
		echo "FAIL  unsafe resolved TOPSRC returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$resolved_output" >&2; exit 1; \
	}; \
	echo "OK    external TOPSRC rejects unsafe physical symlink targets"; \
	source_build_root="$$source_root/build"; \
	if source_build_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		clean TOPSRC="$$source_root" BUILD_ROOT="$$source_build_root" \
		BUILDDIR="$$source_build_root/probe" \
		BUILD_LOCK="$$tmpdir/source-build.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  clean accepted a build directory inside external TOPSRC" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$source_build_output" | \
		grep -E "refusing (BUILD_ROOT|BUILDDIR) inside external TOPSRC|refusing BUILD_ROOT inside a Git worktree" >/dev/null || { \
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
	peer_root="$$tmpdir/peer-repository"; \
	mkdir -p "$$peer_root"; \
	git -C "$$peer_root" init -q; \
	touch "$$peer_root/peer-sentinel"; \
	if peer_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		TOPSRC="$$source_root" BUILD_ROOT="$$peer_root/build-output" \
		BUILDDIR="$$peer_root/build-output/probe" \
		PREFIX="$$peer_root/build-output/prefix" \
		BUILD_LOCK="$$tmpdir/peer.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  clean accepted a sibling Git worktree" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$peer_output" | \
		grep -F "refusing BUILD_ROOT inside a Git worktree" >/dev/null || { \
		echo "FAIL  sibling Git worktree returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$peer_output" >&2; exit 1; \
	}; \
	test -f "$$peer_root/peer-sentinel" || { \
		echo "FAIL  clean changed the sibling Git worktree" >&2; exit 1; \
	}; \
	echo "OK    destructive build roots exclude sibling Git worktrees"; \
	bare_repository="$$tmpdir/retained-bare-repository.git"; \
	git init --bare -q "$$bare_repository"; \
	touch "$$bare_repository/retained-sentinel"; \
	if bare_repository_output="$$( $(MAKE) --no-print-directory \
		-C "$(CURDIR)" clean TOPSRC="$(CONTROL_ROOT)" \
		BUILD_ROOT="$$bare_repository" \
		BUILDDIR="$$bare_repository/objects/probe" \
		PREFIX="$$bare_repository/prefix" \
		BUILD_LOCK="$$tmpdir/bare-repository.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  clean accepted a build directory inside a bare Git repository" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$bare_repository_output" | \
		grep -F "refusing BUILDDIR inside a Git directory" >/dev/null || { \
			echo "FAIL  bare Git repository returned the wrong diagnostic" >&2; \
			printf '%s\n' "$$bare_repository_output" >&2; exit 1; \
		}; \
	test -f "$$bare_repository/retained-sentinel" || { \
		echo "FAIL  clean changed the bare Git repository" >&2; exit 1; \
	}; \
	echo "OK    destructive targets preserve bare Git repositories"; \
	nested_repository_root="$$tmpdir/nested-repository-holder"; \
	nested_repository_builddir="$$nested_repository_root/probe"; \
	nested_repository="$$nested_repository_builddir/retained-repository"; \
	mkdir -p "$$nested_repository"; \
	git -C "$$nested_repository" init -q; \
	touch "$$nested_repository/retained-sentinel"; \
	if nested_repository_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		clean TOPSRC="$(CONTROL_ROOT)" \
		BUILD_ROOT="$$nested_repository_root" \
		BUILDDIR="$$nested_repository_builddir" \
		PREFIX="$$nested_repository_root/prefix" \
		BUILD_LOCK="$$tmpdir/nested-repository.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  clean accepted a nested Git worktree" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$nested_repository_output" | \
		grep -F "refusing BUILDDIR containing a Git repository marker" >/dev/null || { \
			echo "FAIL  nested Git worktree returned the wrong diagnostic" >&2; \
			printf '%s\n' "$$nested_repository_output" >&2; exit 1; \
		}; \
	test -f "$$nested_repository/retained-sentinel" || { \
		echo "FAIL  clean deleted the nested Git worktree" >&2; exit 1; \
	}; \
	echo "OK    destructive targets preserve nested Git worktrees"; \
	if command -v unshare >/dev/null 2>&1 && \
		command -v mount >/dev/null 2>&1 && \
		unshare --user --map-root-user --mount true >/dev/null 2>&1; then \
		caller_mount_namespace="$$(readlink /proc/self/ns/mnt)"; \
		if [ -e /opt/local ]; then \
			opt_local_before="present:$$(stat -c '%d:%i:%f:%u:%g' /opt/local)"; \
		else \
			opt_local_before=absent; \
		fi; \
		mount_boundary_output="$$( \
			MESA_MOUNT_BOUNDARY_CALIBRATION=1 \
			MESA_CALLER_MOUNT_NAMESPACE="$$caller_mount_namespace" \
			unshare --user --map-root-user --mount bash -c '\
				set -euo pipefail; \
				mount --make-rprivate /; \
				mount -t tmpfs -o mode=0700 tmpfs /tmp; \
				mount -t tmpfs -o mode=0755 tmpfs /opt; \
				mkdir -p /opt/local; \
				exec python3 "$$1" --control-root "$$2"' \
				_ "$(MOUNT_BOUNDARY_CALIBRATION)" "$(CONTROL_ROOT)" \
				2>&1 \
		)" || { \
			echo "FAIL  private mount-boundary calibration failed" >&2; \
			printf '%s\n' "$$mount_boundary_output" >&2; exit 1; \
		}; \
		if [ -e /opt/local ]; then \
			opt_local_after="present:$$(stat -c '%d:%i:%f:%u:%g' /opt/local)"; \
		else \
			opt_local_after=absent; \
		fi; \
		test "$$opt_local_before" = "$$opt_local_after" || { \
			echo "FAIL  private mount calibration changed host /opt/local" >&2; \
			exit 1; \
		}; \
		printf '%s\n' "$$mount_boundary_output"; \
	else \
		echo "SKIP  private user/mount namespace is unavailable"; \
	fi; \
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
	system_prefix_build_root="$$tmpdir/system-prefix-build"; \
	for system_prefix in / /boot /dev /etc /home /proc /root /run /sys /tmp /var; do \
		if env \
			MESA_TOPSRC_INPUT="$(CONTROL_ROOT)" \
			MESA_BUILD_ROOT_INPUT="$$system_prefix_build_root" \
			MESA_BUILDDIR_INPUT="$$system_prefix_build_root/probe" \
			MESA_PREFIX_INPUT="$$system_prefix" \
			MESA_PROFILE_INPUT="$(PROFILE)" \
			python3 "$(SOURCE_ROOT_CONTROL)" \
				check-layout --operation=distclean >/dev/null 2>&1; then \
			echo "FAIL  distclean layout accepted system prefix $$system_prefix" >&2; \
			exit 1; \
		fi; \
	done; \
	echo "OK    control-source distclean rejects system top-level prefixes"; \
	for system_build_root in / /boot /dev /etc /home /proc /root /run /sys /tmp /var /var/lib /var/tmp; do \
		if env \
			MESA_TOPSRC_INPUT="$(CONTROL_ROOT)" \
			MESA_BUILD_ROOT_INPUT="$$system_build_root" \
			MESA_BUILDDIR_INPUT="$$system_build_root/probe" \
			MESA_PREFIX_INPUT="/opt/local/mesa-$(PROFILE)" \
			MESA_PROFILE_INPUT="$(PROFILE)" \
			python3 "$(SOURCE_ROOT_CONTROL)" \
				check-layout --operation=distclean >/dev/null 2>&1; then \
			echo "FAIL  distclean layout accepted system build root $$system_build_root" >&2; \
			exit 1; \
		fi; \
	done; \
	echo "OK    destructive operations reject system build roots"; \
	for hostile_home in / /var/lib; do \
		if HOME="$$hostile_home" env \
			MESA_TOPSRC_INPUT="$(CONTROL_ROOT)" \
			MESA_BUILD_ROOT_INPUT="$$hostile_home/source-root-review" \
			MESA_BUILDDIR_INPUT="$$hostile_home/source-root-review/probe" \
			MESA_PREFIX_INPUT="/opt/local/mesa-$(PROFILE)" \
			MESA_PROFILE_INPUT="$(PROFILE)" \
			python3 "$(SOURCE_ROOT_CONTROL)" \
				check-layout --operation=clean >/dev/null 2>&1; then \
			echo "FAIL  HOME=$$hostile_home admitted a destructive root" >&2; \
			exit 1; \
		fi; \
	done; \
	echo "OK    build namespace ownership ignores caller-controlled HOME"; \
	remove_failure_root="$$tmpdir/remove-failure"; \
	remove_failure_builddir="$$remove_failure_root/probe"; \
	mkdir -p "$$remove_failure_builddir"; \
	touch "$$remove_failure_builddir/sentinel"; \
	if remove_failure_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		TOPSRC="$(CONTROL_ROOT)" BUILD_ROOT="$$remove_failure_root" \
		BUILDDIR="$$remove_failure_builddir" \
		PREFIX="$$remove_failure_root/prefix" \
		BUILD_LOCK="$$tmpdir/remove-failure.lock" LOCK_WAIT=0 \
		REMOVE_TREE=false 2>&1 )"; then \
		echo "FAIL  clean reported success after its remove command failed" >&2; \
		exit 1; \
	fi; \
	test -f "$$remove_failure_builddir/sentinel" || { \
		echo "FAIL  remove-failure calibration changed its sentinel" >&2; \
		exit 1; \
	}; \
	if printf '%s\n' "$$remove_failure_output" | grep -F "cleaned:" >/dev/null; then \
		echo "FAIL  clean printed success after remove failure" >&2; \
		exit 1; \
	fi; \
	echo "OK    clean propagates remove failure"; \
	if clean_all_failure_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" clean-all \
		TOPSRC="$(CONTROL_ROOT)" BUILD_ROOT="$(CONTROL_ROOT)/build" \
		BUILD_LOCK="$$tmpdir/clean-all-failure.lock" LOCK_WAIT=0 \
		REMOVE_TREE=false 2>&1 )"; then \
		echo "FAIL  clean-all reported success after its remove command failed" >&2; \
		exit 1; \
	fi; \
	if printf '%s\n' "$$clean_all_failure_output" | grep -F "cleaned:" >/dev/null; then \
		echo "FAIL  clean-all printed success after remove failure" >&2; \
		exit 1; \
	fi; \
	echo "OK    clean-all propagates remove failure"; \
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
	if empty_clean_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		TOPSRC="$$source_root" BUILD_ROOT="$$external_build_root" \
		BUILDDIR="$$external_builddir" \
		BUILD_LOCK="$$tmpdir/external-build.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  external clean adopted an empty unbound build directory" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$empty_clean_output" | \
		grep -F "external build root lacks source identity" >/dev/null || { \
		echo "FAIL  empty unbound clean returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$empty_clean_output" >&2; exit 1; \
	}; \
	rmdir "$$external_builddir"; \
	echo "OK    external clean rejects an existing unbound build directory"; \
	if absent_clean_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		TOPSRC="$$source_root" BUILD_ROOT="$$external_build_root" \
		BUILDDIR="$$external_builddir" \
		BUILD_LOCK="$$tmpdir/external-build.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  external clean adopted an absent unbound build directory" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$absent_clean_output" | \
		grep -F "external build root lacks source identity" >/dev/null || { \
		echo "FAIL  absent unbound clean returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$absent_clean_output" >&2; exit 1; \
	}; \
	echo "OK    external clean rejects an absent unbound build directory"; \
	registered_clean_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		clean-registered-build \
		TOPSRC="$$source_root" BUILD_ROOT="$$external_build_root" \
		BUILDDIR="$$external_builddir" \
		BUILD_LOCK="$$tmpdir/external-build.lock" LOCK_WAIT=0 2>&1 )"; \
	printf '%s\n' "$$registered_clean_output" | \
		grep -Fx "no registered build exists: $$external_builddir" >/dev/null || { \
		echo "FAIL  registered clean returned the wrong absent-build diagnostic" >&2; \
		printf '%s\n' "$$registered_clean_output" >&2; exit 1; \
	}; \
	{ [ ! -e "$$external_builddir" ] && [ ! -L "$$external_builddir" ]; } || { \
		echo "FAIL  registered clean created an absent build directory" >&2; \
		exit 1; \
	}; \
	echo "OK    registered clean leaves an absent build directory untouched"; \
	ln -s "$$tmpdir/missing-build-target" "$$external_builddir"; \
	if broken_link_clean_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		clean-registered-build \
		TOPSRC="$$source_root" BUILD_ROOT="$$external_build_root" \
		BUILDDIR="$$external_builddir" \
		BUILD_LOCK="$$tmpdir/external-build.lock" LOCK_WAIT=0 2>&1 )"; then \
		echo "FAIL  registered clean treated a broken symlink as an absent build" >&2; \
		exit 1; \
	fi; \
	if printf '%s\n' "$$broken_link_clean_output" | \
		grep -F "no registered build exists" >/dev/null; then \
		echo "FAIL  registered clean skipped a broken build-directory symlink" >&2; \
		exit 1; \
	fi; \
	test -L "$$external_builddir" || { \
		echo "FAIL  registered clean removed an unbound build-directory symlink" >&2; \
		exit 1; \
	}; \
	echo "OK    registered clean delegates build-directory symlinks to strict clean"; \
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
		PREFIX="$$external_build_root/prefix" \
		BUILD_LOCK="$$tmpdir/configure.lock" LOCK_WAIT=0 \
		MESON=meson PROFILE="$(PROFILE)" 2>/dev/null )"; \
	external_source_view="$$external_build_root/.mesa-source-view"; \
	printf '%s\n' "$$configure_plan" | grep -F "meson setup" >/dev/null && \
	printf '%s\n' "$$configure_plan" | grep -F "\"$$external_source_view\"" >/dev/null && \
	printf '%s\n' "$$configure_plan" | grep -F -- "--wrap-mode=default" >/dev/null && \
	printf '%s\n' "$$configure_plan" | grep -F "MESON_PACKAGE_CACHE_DIR" >/dev/null || { \
		echo "FAIL  configure does not pass the derived source view to Meson" >&2; \
		printf '%s\n' "$$configure_plan" >&2; exit 1; \
	}; \
	if printf '%s\n' "$$configure_plan" | grep -F -- "--wrap-mode=nodownload" >/dev/null; then \
		echo "FAIL  external source view still disables required wrap population" >&2; \
		exit 1; \
	fi; \
	echo "OK    external configure plans the derived source view"; \
	control_plan_build_root="$$tmpdir/control-plan-build"; \
	control_configure_plan="$$( $(MAKE) --no-print-directory -n -C "$(CURDIR)" \
		configure TOPSRC="$(CONTROL_ROOT)" BUILD_ROOT="$$control_plan_build_root" \
		BUILDDIR="$$control_plan_build_root/configure" \
		PREFIX="$$control_plan_build_root/prefix" \
		BUILD_LOCK="$$tmpdir/control-plan.lock" LOCK_WAIT=0 \
		MESON=meson PROFILE="$(PROFILE)" 2>/dev/null )"; \
	printf '%s\n' "$$control_configure_plan" | grep -F "meson setup" >/dev/null && \
	printf '%s\n' "$$control_configure_plan" | grep -F "\"$(CONTROL_ROOT)\"" >/dev/null || { \
		echo "FAIL  control configure does not pass the control source root to Meson" >&2; \
		printf '%s\n' "$$control_configure_plan" >&2; exit 1; \
	}; \
	if printf '%s\n' "$$control_configure_plan" | \
		grep -F ".mesa-source-view" >/dev/null; then \
		echo "FAIL  control configure planned an external source view" >&2; \
		exit 1; \
	fi; \
	if printf '%s\n' "$$control_configure_plan" | \
		grep -F -- "--wrap-mode=default" >/dev/null; then \
		echo "FAIL  control configure changed its wrap policy" >&2; \
		exit 1; \
	fi; \
	if printf '%s\n' "$$control_configure_plan" | \
		grep -F "MESON_PACKAGE_CACHE_DIR" >/dev/null; then \
		echo "FAIL  control configure changed its package-cache environment" >&2; \
		exit 1; \
	fi; \
	echo "OK    control configure retains the original source path"; \
	wrap_archive_root="$$tmpdir/wrap-archive-root"; \
	wrap_archive="$$tmpdir/Vulkan-Profiles-fixture.tar"; \
	mkdir -p "$$wrap_archive_root/Vulkan-Profiles-source"; \
	printf '%s\n' "project('Vulkan-Profiles-fixture')" > \
		"$$wrap_archive_root/Vulkan-Profiles-source/meson.build"; \
	tar --format=ustar --sort=name --mtime=@0 --owner=0 --group=0 \
		--numeric-owner -cf "$$wrap_archive" -C "$$wrap_archive_root" \
		Vulkan-Profiles-source; \
	wrap_archive_hash="$$(sha256sum "$$wrap_archive" | awk '{print $$1}')"; \
	wrap_source_root="$$tmpdir/wrap-source"; \
	mkdir -p "$$wrap_source_root/subprojects"; \
	git -C "$$wrap_source_root" init -q; \
	printf '%s\n' "project('source-view-wrap-fixture')" > \
		"$$wrap_source_root/meson.build"; \
	touch "$$wrap_source_root/meson.options"; \
	{ \
		printf '%s\n' "[wrap-file]"; \
		printf '%s\n' "directory = Vulkan-Profiles-source"; \
		printf '%s\n' "source_url = file://$$wrap_archive"; \
		printf '%s\n' "source_filename = Vulkan-Profiles-fixture.tar"; \
		printf '%s\n' "source_hash = $$wrap_archive_hash"; \
	} > "$$wrap_source_root/subprojects/Vulkan-Profiles.wrap"; \
	git -C "$$wrap_source_root" add meson.build meson.options subprojects; \
	git -C "$$wrap_source_root" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: add hashed Vulkan-Profiles fixture"; \
	synthetic_meson="$$tmpdir/synthetic-meson"; \
	{ \
		printf '%s\n' '#!/bin/sh' 'set -eu' \
			'for argument do source_view=$$argument; done' \
			'wrap=$$source_view/subprojects/Vulkan-Profiles.wrap' \
			'directory=$$(sed -n "s/^directory = //p" "$$wrap")' \
			'source_url=$$(sed -n "s/^source_url = //p" "$$wrap")' \
			'source_hash=$$(sed -n "s/^source_hash = //p" "$$wrap")' \
			'case "$$source_url" in file://*) archive=$${source_url#file://} ;; *) exit 2 ;; esac' \
			'printf "%s  %s\n" "$$source_hash" "$$archive" | sha256sum -c - >/dev/null' \
			'tar -xf "$$archive" -C "$$source_view/subprojects"' \
			'test -f "$$source_view/subprojects/$$directory/meson.build"'; \
	} > "$$synthetic_meson"; \
	chmod 700 "$$synthetic_meson"; \
	wrap_build_root="$$tmpdir/wrap-build"; \
	wrap_builddir="$$wrap_build_root/probe"; \
	wrap_prefix="$$wrap_build_root/prefix"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$wrap_source_root" BUILD_ROOT="$$wrap_build_root" \
		BUILDDIR="$$wrap_builddir" PREFIX="$$wrap_prefix" \
		BUILD_LOCK="$$tmpdir/wrap.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 \
		MESON="$$synthetic_meson" >/dev/null; \
	wrap_source_view="$$wrap_build_root/.mesa-source-view"; \
	test -f "$$wrap_source_view/subprojects/Vulkan-Profiles-source/meson.build" || { \
		echo "FAIL  hashed Vulkan-Profiles fixture was not populated in the source view" >&2; \
		exit 1; \
	}; \
	test -z "$$(git -C "$$wrap_source_root" status --porcelain=v1 \
		--untracked-files=all)" || { \
		echo "FAIL  wrap preparation mutated the original external TOPSRC" >&2; \
		exit 1; \
	}; \
	grep -F "\"source_view\": \"$$wrap_source_view\"" \
		"$$wrap_build_root/.mesa-external-source-identity.json" >/dev/null && \
	grep -E '"source_view_digest": "sha256:[0-9a-f]{64}"' \
		"$$wrap_build_root/.mesa-external-source-identity.json" >/dev/null || { \
		echo "FAIL  final identity omitted the source-view path or digest" >&2; \
		exit 1; \
	}; \
	echo "OK    hashed Vulkan-Profiles fixture populates only the derived source view"; \
	wrap_population_file="$$wrap_source_view/subprojects/Vulkan-Profiles-source/meson.build"; \
	printf '%s\n' "source-view drift" >> "$$wrap_population_file"; \
	touch "$$wrap_builddir/drift-sentinel"; \
	for drift_target in build test install artifact-check clean; do \
		if drift_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
			"$$drift_target" TOPSRC="$$wrap_source_root" \
			BUILD_ROOT="$$wrap_build_root" BUILDDIR="$$wrap_builddir" \
			PREFIX="$$wrap_prefix" BUILD_LOCK="$$tmpdir/wrap.lock" \
			LOCK_WAIT=0 COMPILER_CHAIN=direct NINJA=true MESON=true \
			SUDO=true 2>&1 )"; then \
			echo "FAIL  $$drift_target accepted source-view drift" >&2; \
			exit 1; \
		fi; \
		printf '%s\n' "$$drift_output" | \
			grep -F "source view content drift" >/dev/null || { \
			echo "FAIL  $$drift_target returned the wrong source-view diagnostic" >&2; \
			printf '%s\n' "$$drift_output" >&2; exit 1; \
		}; \
	done; \
	test -f "$$wrap_builddir/drift-sentinel" || { \
		echo "FAIL  drift-rejected clean mutated the bound build directory" >&2; \
		exit 1; \
	}; \
	printf '%s\n' "project('Vulkan-Profiles-fixture')" > "$$wrap_population_file"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$wrap_source_root" BUILD_ROOT="$$wrap_build_root" \
		BUILDDIR="$$wrap_builddir" PREFIX="$$wrap_prefix" \
		BUILD_LOCK="$$tmpdir/wrap.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA=true >/dev/null; \
	echo "OK    every finalized consumer rejects source-view content drift"; \
	sed -i '/^source_hash = /d' \
		"$$wrap_source_root/subprojects/Vulkan-Profiles.wrap"; \
	git -C "$$wrap_source_root" add subprojects/Vulkan-Profiles.wrap; \
	git -C "$$wrap_source_root" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: remove wrap source hash"; \
	unhashed_build_root="$$tmpdir/unhashed-wrap-build"; \
	if unhashed_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$wrap_source_root" BUILD_ROOT="$$unhashed_build_root" \
		BUILDDIR="$$unhashed_build_root/probe" \
		PREFIX="$$unhashed_build_root/prefix" \
		BUILD_LOCK="$$tmpdir/unhashed-wrap.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 \
		MESON="$$synthetic_meson" 2>&1 )"; then \
		echo "FAIL  configure admitted an unhashed wrap-file download" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$unhashed_output" | \
		grep -F "wrap-file source_hash must be SHA-256" >/dev/null || { \
		echo "FAIL  unhashed wrap returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$unhashed_output" >&2; exit 1; \
	}; \
	test ! -e "$$unhashed_build_root/.mesa-source-view" || { \
		echo "FAIL  unhashed wrap published a derived source view" >&2; \
		exit 1; \
	}; \
	echo "OK    external configure rejects an unhashed wrap-file download"; \
	default_prefix_build_root="$$tmpdir/default-prefix-build"; \
	if default_prefix_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$default_prefix_build_root" \
		BUILDDIR="$$default_prefix_build_root/probe" \
		BUILD_LOCK="$$tmpdir/default-prefix.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true 2>&1 )"; then \
		echo "FAIL  external configure accepted the shared default prefix" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$default_prefix_output" | \
		grep -F "external PREFIX must be a direct child of BUILD_ROOT" >/dev/null || { \
		echo "FAIL  shared default prefix returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$default_prefix_output" >&2; exit 1; \
	}; \
	echo "OK    external configure rejects the shared default prefix"; \
	unbound_build_root="$$tmpdir/unbound-build"; \
	unbound_builddir="$$unbound_build_root/probe"; \
	mkdir -p "$$unbound_builddir"; \
	touch "$$unbound_builddir/sentinel"; \
	if unbound_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$unbound_build_root" \
		BUILDDIR="$$unbound_builddir" PREFIX="$$unbound_build_root/prefix" \
		BUILD_LOCK="$$tmpdir/unbound.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true 2>&1 )"; then \
		echo "FAIL  configure adopted a nonempty unbound build directory" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$unbound_output" | \
		grep -F "external build root lacks a source identity" >/dev/null || { \
		echo "FAIL  unbound build directory returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$unbound_output" >&2; exit 1; \
	}; \
	test -f "$$unbound_builddir/sentinel" || { \
		echo "FAIL  configure mutated the unbound build directory" >&2; \
		exit 1; \
	}; \
	echo "OK    configure rejects a nonempty unbound build directory"; \
	failed_config_root="$$tmpdir/failed-config"; \
	failed_config_builddir="$$failed_config_root/probe"; \
	failed_config_prefix="$$failed_config_root/prefix"; \
	if failed_config_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$failed_config_root" \
		BUILDDIR="$$failed_config_builddir" PREFIX="$$failed_config_prefix" \
		BUILD_LOCK="$$tmpdir/failed-config.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=false 2>&1 )"; then \
		echo "FAIL  configure finalized identity after Meson failure" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$failed_config_output" | \
		grep -F "Meson setup failed; source identity remains provisional" >/dev/null || { \
		echo "FAIL  Meson failure returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$failed_config_output" >&2; exit 1; \
	}; \
	test -f "$$failed_config_root/.mesa-external-source-identity.json" || { \
		echo "FAIL  failed configure omitted its provisional root identity" >&2; \
		exit 1; \
	}; \
	grep -F '"state": "provisional"' \
		"$$failed_config_root/.mesa-external-source-identity.json" \
		>/dev/null || { \
		echo "FAIL  failed configure did not mark its root provisional" >&2; \
		exit 1; \
	}; \
	test ! -e "$$failed_config_builddir/.mesa-source-identity.json" || { \
		echo "FAIL  failed configure wrote a finalized build identity" >&2; \
		exit 1; \
	}; \
	test ! -d "$$failed_config_builddir/meson-private" || { \
		echo "FAIL  false Meson command produced meson-private" >&2; \
		exit 1; \
	}; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		TOPSRC="$$source_root" BUILD_ROOT="$$failed_config_root" \
		BUILDDIR="$$failed_config_builddir" PREFIX="$$failed_config_prefix" \
		BUILD_LOCK="$$tmpdir/failed-config.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct >/dev/null; \
	test ! -e "$$failed_config_builddir" || { \
		echo "FAIL  provisional identity did not admit interrupted-build cleanup" >&2; \
		exit 1; \
	}; \
	echo "OK    Meson failure leaves a provisional cleanup identity"; \
	transaction_source="$$tmpdir/transaction-source"; \
	transaction_build_root="$$tmpdir/transaction-build"; \
	transaction_builddir="$$transaction_build_root/probe"; \
	transaction_prefix="$$transaction_build_root/prefix"; \
	mkdir -p "$$transaction_source"; \
	git -C "$$transaction_source" init -q; \
	touch "$$transaction_source/meson.build" "$$transaction_source/meson.options"; \
	git -C "$$transaction_source" add meson.build meson.options; \
	git -C "$$transaction_source" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: prepare source identity"; \
	env \
		MESA_TOPSRC_INPUT="$$transaction_source" \
		MESA_BUILD_ROOT_INPUT="$$transaction_build_root" \
		MESA_BUILDDIR_INPUT="$$transaction_builddir" \
		MESA_PREFIX_INPUT="$$transaction_prefix" \
		MESA_PROFILE_INPUT="$(PROFILE)" \
		python3 "$(SOURCE_ROOT_CONTROL)" prepare-identity; \
	printf '%s\n' "source drift" >> "$$transaction_source/meson.build"; \
	git -C "$$transaction_source" add meson.build; \
	git -C "$$transaction_source" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: drift prepared source identity"; \
	if transaction_output="$$( env \
		MESA_TOPSRC_INPUT="$$transaction_source" \
		MESA_BUILD_ROOT_INPUT="$$transaction_build_root" \
		MESA_BUILDDIR_INPUT="$$transaction_builddir" \
		MESA_PREFIX_INPUT="$$transaction_prefix" \
		MESA_PROFILE_INPUT="$(PROFILE)" \
		python3 "$(SOURCE_ROOT_CONTROL)" write-identity 2>&1 )"; then \
		echo "FAIL  identity finalization accepted a different source tuple" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$transaction_output" | \
		grep -F "external source identity drift" >/dev/null || { \
		echo "FAIL  source transaction drift returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$transaction_output" >&2; exit 1; \
	}; \
	test ! -e "$$transaction_builddir/.mesa-source-identity.json" || { \
		echo "FAIL  source transaction drift wrote a finalized identity" >&2; \
		exit 1; \
	}; \
	echo "OK    identity finalization preserves the prepared source tuple"; \
	identity_build_root="$$tmpdir/identity-build"; \
	identity_builddir="$$identity_build_root/probe"; \
	identity_prefix="$$identity_build_root/prefix"; \
	internal_path_marker="$$tmpdir/internal-path-executed"; \
	forged_envfile="$$tmpdir/forged.env"; \
	forged_toolchain="$$tmpdir/outside/forged-toolchain.meson"; \
	mkdir -p "$$(dirname "$$forged_toolchain")"; \
	printf 'touch %s\n' "$$internal_path_marker" > "$$forged_envfile"; \
	forged_setup_args='; touch '"$$internal_path_marker"'; #'; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true \
		CONFIG=/bin/false ENVFILE="$$forged_envfile" \
		PROFILE_DFLAGS_SCRIPT=/bin/false \
		TOOLCHAIN_NATIVE="$$forged_toolchain" \
		SETUP_ARGS="$$forged_setup_args" SHELL=/bin/false \
		MESON_SETUP_LOCAL_UNSET="x; touch $$internal_path_marker; #" \
		>/dev/null; \
	test ! -e "$$internal_path_marker" || { \
		echo "FAIL  configure executed a forged internal selector" >&2; \
		exit 1; \
	}; \
	test ! -e "$$forged_toolchain" || { \
		echo "FAIL  configure wrote a forged toolchain path" >&2; \
		exit 1; \
	}; \
	test -f "$$identity_builddir/mesa-toolchain.meson" || { \
		echo "FAIL  configure omitted the selected build toolchain" >&2; \
		exit 1; \
	}; \
	echo "OK    derived build controls override command-line forgeries"; \
	test -f "$$identity_builddir/.mesa-source-identity.json" || { \
		echo "FAIL  configure omitted the external source identity" >&2; \
		exit 1; \
	}; \
	test -f "$$identity_build_root/.mesa-external-source-identity.json" || { \
		echo "FAIL  configure omitted the external build-root identity" >&2; \
		exit 1; \
	}; \
	bytecode_control="$$tmpdir/source-view-bytecode-control"; \
	mkdir -p "$$bytecode_control"; \
	touch "$$bytecode_control/source_view_bytecode_probe.py"; \
	env -u PYTHONDONTWRITEBYTECODE PYTHONPATH="$$bytecode_control" \
		python3 -c 'import source_view_bytecode_probe'; \
	find "$$bytecode_control" -path '*/__pycache__/*.pyc' -print -quit | \
		grep -q . || { \
		echo "FAIL  Python bytecode control did not create a cache file" >&2; \
		exit 1; \
	}; \
	bytecode_ninja="$$tmpdir/source-view-bytecode-ninja"; \
	{ \
		printf '%s\n' '#!/bin/sh' 'set -eu' \
			'test "$${PYTHONDONTWRITEBYTECODE:-}" = 1' \
			'test "$$MESA_GIT_SHA1_OVERRIDE" = "$$MESA_SOURCE_COMMIT_CAPTURED"' \
			'source_view="$$MESA_BUILD_ROOT_INPUT/.mesa-source-view"' \
			'PYTHONPATH="$$source_view" python3 -c "import source_view_bytecode_probe"'; \
	} > "$$bytecode_ninja"; \
	chmod 700 "$$bytecode_ninja"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA="$$bytecode_ninja" \
		PYTHONDONTWRITEBYTECODE=0 MESA_GIT_SHA1_OVERRIDE=forged >/dev/null; \
	if find "$$identity_build_root/.mesa-source-view" \
		\( -name __pycache__ -o -name '*.pyc' -o -name '*.pyo' \) \
		-print -quit | grep -q .; then \
		echo "FAIL  build tools wrote Python bytecode into the source view" >&2; \
		exit 1; \
	fi; \
	echo "OK    build tools inherit source identity without writing Python bytecode"; \
	for selector_case in \
		'profile:MESA_PROFILE_INPUT:selector-profile-drift' \
		'hostenv:MESA_HOSTENV_INPUT:selector-hostenv-drift' \
		'mode:MESA_MODE_INPUT:stable' \
		'compiler_chain:MESA_COMPILER_CHAIN_INPUT:ccache' \
		'compiler_family:MESA_COMPILER_FAMILY_INPUT:gnu'; do \
		IFS=: read -r selector_field selector_variable selector_value \
			<<< "$$selector_case"; \
		if selector_drift_output="$$( env \
			MESA_TOPSRC_INPUT="$$source_root" \
			MESA_BUILD_ROOT_INPUT="$$identity_build_root" \
			MESA_BUILDDIR_INPUT="$$identity_builddir" \
			MESA_PREFIX_INPUT="$$identity_prefix" \
			MESA_SYSCONFDIR_INPUT="$(SYSCONFDIR)" \
			MESA_PROFILE_INPUT="$(PROFILE)" \
			MESA_HOSTENV_INPUT="$(HOSTENV)" \
			MESA_MODE_INPUT="$(MODE)" \
			MESA_COMPILER_CHAIN_INPUT=direct \
			MESA_COMPILER_FAMILY_INPUT=llvm \
			MESA_REPRODUCIBLE_RUN_INPUT=0 \
			"$$selector_variable=$$selector_value" \
			python3 "$(SOURCE_ROOT_CONTROL)" verify-identity 2>&1 )"; then \
			echo "FAIL  identity accepted $$selector_field drift" >&2; \
			exit 1; \
		fi; \
		printf '%s\n' "$$selector_drift_output" | \
			grep -F "external source identity drift: $$selector_field" \
			>/dev/null || { \
			echo "FAIL  $$selector_field drift returned the wrong diagnostic" >&2; \
			printf '%s\n' "$$selector_drift_output" >&2; exit 1; \
		}; \
	done; \
	echo "OK    external identities bind every build-policy selector"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA=true SHELL=/bin/false \
		LOCKED_TOOL_ENV="; touch $$internal_path_marker; #" >/dev/null; \
	test ! -e "$$internal_path_marker" || { \
		echo "FAIL  build executed a forged locked environment" >&2; \
		exit 1; \
	}; \
	echo "OK    external build verifies the configure-time identity"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA=true \
		CURDIR=/tmp BUILD_INFRA_ROOT=/tmp BUILD_INFRA_MAKEFILE=/tmp \
		CONTROL_ROOT=/ SOURCE_ROOT_CONTROL=/bin/false \
		SOURCE_ROOT_FIELDS='a b c d e f g h i j k l m n o p' \
		SOURCE_ROOT_RESOLVE_STATUS=1 SOURCE_COMMIT=forged \
		SOURCE_TREE=forged CONTROL_COMMIT=forged CONTROL_TREE=forged \
		MESA_TOPSRC_INPUT=/ MESA_BUILD_ROOT_INPUT=/ \
		MESA_BUILDDIR_INPUT=/ MESA_PREFIX_INPUT=/ \
		MESA_SYSCONFDIR_INPUT=/ \
		MESA_PROFILE_INPUT=forged \
		MESA_HOSTENV_INPUT=forged \
		MESA_PROFILE_RAW=forged MESA_HOSTENV_RAW=forged \
		MESA_INSTALL_NAMESPACE_RAW=forged MESA_MODE_RAW=forged \
		MESA_COMPILER_CHAIN_RAW=forged \
		MESA_COMPILER_FAMILY_RAW=forged \
		MESA_REPRODUCIBLE_RUN_RAW=forged \
		MODE_SELECTOR=forged \
		SELECTOR_FIELDS='forged forged forged forged forged forged forged' \
		SELECTOR_RESOLVE_STATUS=1 COMMAND_VARIABLE_WITH_DOLLAR=forged \
		MESA_CONTROL_ROOT_INPUT=/ \
		MESA_SOURCE_ROOT_ANCHOR=forged \
		MESA_CONTROL_ROOT_ANCHOR=forged \
		MESA_BUILD_ROOT_ANCHOR=forged \
		MESA_BUILDDIR_ANCHOR=forged \
		MESA_PREFIX_ANCHOR=forged \
		MESA_SYSCONFDIR_ANCHOR=forged \
		MESA_SOURCE_COMMIT_CAPTURED=forged \
		MESA_SOURCE_TREE_CAPTURED=forged \
		MESA_CONTROL_COMMIT_CAPTURED=forged \
		MESA_CONTROL_TREE_CAPTURED=forged >/dev/null; \
	echo "OK    resolver-owned proof inputs override command-line forgeries"; \
	if install_failure_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" install \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESON=false SUDO=true 2>&1 )"; then \
		echo "FAIL  install finalized identity after Meson failure" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$install_failure_output" | \
		grep -F "Meson setup failed; source identity remains provisional" \
		>/dev/null || { \
		echo "FAIL  install Meson failure returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$install_failure_output" >&2; exit 1; \
	}; \
	grep -F '"state": "provisional"' \
		"$$identity_build_root/.mesa-external-source-identity.json" \
		>/dev/null || { \
		echo "FAIL  failed install left the root identity final" >&2; \
		exit 1; \
	}; \
	if install_stale_build="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA=true 2>&1 )"; then \
		echo "FAIL  build accepted state after failed install setup" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$install_stale_build" | \
		grep -F "expected final" >/dev/null || { \
		echo "FAIL  failed-install build returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$install_stale_build" >&2; exit 1; \
	}; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true >/dev/null; \
	post_setup_failure_meson="$$tmpdir/post-setup-failure-meson"; \
	{ \
		printf '%s\n' '#!/bin/sh' 'set -eu' \
			'case "$$1" in' \
			'setup) exit 0 ;;' \
			'install) : > "$$0.install-invoked"; exit 73 ;;' \
			'*) exit 64 ;;' \
			'esac'; \
	} > "$$post_setup_failure_meson"; \
	chmod 700 "$$post_setup_failure_meson"; \
	if post_setup_failure_output="$$( $(MAKE) --no-print-directory \
		-C "$(CURDIR)" install \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESON="$$post_setup_failure_meson" \
		SUDO=env 2>&1 )"; then \
		echo "FAIL  install finalized identity after post-setup failure" >&2; \
		exit 1; \
	fi; \
	test -f "$$post_setup_failure_meson.install-invoked" || { \
		echo "FAIL  post-setup install fixture did not reach installation" >&2; \
		exit 1; \
	}; \
	printf '%s\n' "$$post_setup_failure_output" | \
		grep -F "Meson install failed; source identity remains provisional" \
		>/dev/null || { \
		echo "FAIL  post-setup install failure returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$post_setup_failure_output" >&2; exit 1; \
	}; \
	grep -F '"state": "provisional"' \
		"$$identity_build_root/.mesa-external-source-identity.json" \
		>/dev/null || { \
		echo "FAIL  post-setup install failure left the root identity final" >&2; \
		exit 1; \
	}; \
	mkdir -p "$$identity_prefix"; \
	touch "$$identity_prefix/libgallium-partial-install.so"; \
	if partial_artifact_output="$$( $(MAKE) --no-print-directory \
		-C "$(CURDIR)" artifact-check \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct 2>&1 )"; then \
		echo "FAIL  artifact-check accepted a partial failed install" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$partial_artifact_output" | \
		grep -F "expected final" >/dev/null || { \
		echo "FAIL  partial install returned the wrong artifact diagnostic" >&2; \
		printf '%s\n' "$$partial_artifact_output" >&2; exit 1; \
	}; \
	echo "OK    post-setup install failure keeps identities provisional"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true >/dev/null; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" install \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESON=true SUDO=true >/dev/null; \
	cmp -s "$$identity_build_root/.mesa-external-source-identity.json" \
		"$$identity_builddir/.mesa-source-identity.json" || { \
		echo "FAIL  successful install wrote mismatched transactions" >&2; \
		exit 1; \
	}; \
	echo "OK    successful install finalizes one source transaction"; \
	mkdir -p "$$identity_prefix"; \
	touch "$$identity_prefix/libgallium-source-root-test.so"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" artifact-check \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct >/dev/null; \
	if artifact_drift_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		artifact-check TOPSRC="$$legacy_root" \
		BUILD_ROOT="$$identity_build_root" BUILDDIR="$$identity_builddir" \
		PREFIX="$$identity_prefix" BUILD_LOCK="$$tmpdir/identity.lock" \
		LOCK_WAIT=0 COMPILER_CHAIN=direct 2>&1 )"; then \
		echo "FAIL  artifact-check accepted another source identity" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$artifact_drift_output" | \
		grep -F "external source identity drift" >/dev/null || { \
		echo "FAIL  artifact source drift returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$artifact_drift_output" >&2; exit 1; \
	}; \
	echo "OK    artifact reporting binds the selected source tuple"; \
	if reconfigure_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=false 2>&1 )"; then \
		echo "FAIL  failed reconfigure reported success" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$reconfigure_output" | \
		grep -F "Meson setup failed; source identity remains provisional" \
		>/dev/null || { \
		echo "FAIL  failed reconfigure returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$reconfigure_output" >&2; exit 1; \
	}; \
	grep -F '"state": "provisional"' \
		"$$identity_build_root/.mesa-external-source-identity.json" \
		>/dev/null || { \
		echo "FAIL  failed reconfigure left the root identity final" >&2; \
		exit 1; \
	}; \
	if stale_build_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA=true 2>&1 )"; then \
		echo "FAIL  build accepted a final identity under a provisional root" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$stale_build_output" | \
		grep -F "expected final" >/dev/null || { \
		echo "FAIL  stale build returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$stale_build_output" >&2; exit 1; \
	}; \
	if stale_test_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" test \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESON=true 2>&1 )"; then \
		echo "FAIL  test accepted a final identity under a provisional root" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$stale_test_output" | \
		grep -F "expected final" >/dev/null || { \
		echo "FAIL  stale test returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$stale_test_output" >&2; exit 1; \
	}; \
	if stale_install_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" install \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESON=true SUDO=true 2>&1 )"; then \
		echo "FAIL  install accepted a final identity under a provisional root" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$stale_install_output" | \
		grep -F "expected final" >/dev/null || { \
		echo "FAIL  stale install returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$stale_install_output" >&2; exit 1; \
	}; \
	if stale_distclean_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		distclean TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct SUDO= 2>&1 )"; then \
		echo "FAIL  distclean accepted a stale final build identity" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$stale_distclean_output" | \
		grep -F "expected final" >/dev/null || { \
		echo "FAIL  stale distclean returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$stale_distclean_output" >&2; exit 1; \
	}; \
	echo "OK    failed reconfigure revokes every finalized-identity consumer"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true >/dev/null; \
	grep -F '"state": "final"' \
		"$$identity_build_root/.mesa-external-source-identity.json" \
		>/dev/null || { \
		echo "FAIL  successful reconfigure did not finalize its root" >&2; \
		exit 1; \
	}; \
	echo "OK    successful reconfigure restores one final transaction"; \
	if prefix_drift_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" \
		PREFIX="$$identity_build_root/alternate-prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA=true 2>&1 )"; then \
		echo "FAIL  build accepted a prefix outside its source identity" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$prefix_drift_output" | \
		grep -F "external source identity drift: prefix" >/dev/null || { \
		echo "FAIL  prefix identity drift returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$prefix_drift_output" >&2; exit 1; \
	}; \
	echo "OK    external build rejects prefix identity drift"; \
	alternate_prefix="$$identity_build_root/alternate-prefix"; \
	mkdir -p "$$alternate_prefix"; \
	touch "$$alternate_prefix/sentinel"; \
	if distclean_prefix_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" distclean \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$alternate_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct SUDO= 2>&1 )"; then \
		echo "FAIL  distclean accepted a prefix outside its source identity" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$distclean_prefix_output" | \
		grep -F "external source identity drift: prefix" >/dev/null || { \
		echo "FAIL  distclean prefix drift returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$distclean_prefix_output" >&2; exit 1; \
	}; \
	test -d "$$identity_builddir" || { \
		echo "FAIL  distclean deleted the source-bound build directory" >&2; \
		exit 1; \
	}; \
	test -f "$$alternate_prefix/sentinel" || { \
		echo "FAIL  distclean moved the unbound alternate prefix" >&2; \
		exit 1; \
	}; \
	echo "OK    external distclean rejects unbound prefix mutation"; \
	remove_distclean_root="$$tmpdir/remove-distclean"; \
	remove_distclean_builddir="$$remove_distclean_root/probe"; \
	remove_distclean_prefix="$$remove_distclean_root/prefix"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$remove_distclean_root" \
		BUILDDIR="$$remove_distclean_builddir" PREFIX="$$remove_distclean_prefix" \
		BUILD_LOCK="$$tmpdir/remove-distclean.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true >/dev/null; \
	mkdir -p "$$remove_distclean_prefix"; \
	touch "$$remove_distclean_prefix/sentinel"; \
	if remove_distclean_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" distclean \
		TOPSRC="$$source_root" BUILD_ROOT="$$remove_distclean_root" \
		BUILDDIR="$$remove_distclean_builddir" PREFIX="$$remove_distclean_prefix" \
		BUILD_LOCK="$$tmpdir/remove-distclean.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct REMOVE_TREE=false SUDO= 2>&1 )"; then \
		echo "FAIL  distclean reported success after remove failure" >&2; \
		exit 1; \
	fi; \
	test -d "$$remove_distclean_builddir" && \
	test -f "$$remove_distclean_prefix/sentinel" || { \
		echo "FAIL  remove-failure distclean changed a protected target" >&2; \
		exit 1; \
	}; \
	if printf '%s\n' "$$remove_distclean_output" | grep -E "cleaned:|archived prefix:" >/dev/null; then \
		echo "FAIL  distclean printed success after remove failure" >&2; \
		exit 1; \
	fi; \
	echo "OK    distclean propagates remove failure before prefix archival"; \
	archive_failure_root="$$tmpdir/archive-failure"; \
	archive_failure_builddir="$$archive_failure_root/probe"; \
	archive_failure_prefix="$$archive_failure_root/prefix"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$archive_failure_root" \
		BUILDDIR="$$archive_failure_builddir" PREFIX="$$archive_failure_prefix" \
		BUILD_LOCK="$$tmpdir/archive-failure.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true >/dev/null; \
	mkdir -p "$$archive_failure_prefix"; \
	touch "$$archive_failure_prefix/sentinel"; \
	if archive_failure_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" distclean \
		TOPSRC="$$source_root" BUILD_ROOT="$$archive_failure_root" \
		BUILDDIR="$$archive_failure_builddir" PREFIX="$$archive_failure_prefix" \
		BUILD_LOCK="$$tmpdir/archive-failure.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct SUDO=false 2>&1 )"; then \
		echo "FAIL  distclean reported success after archive failure" >&2; \
		exit 1; \
	fi; \
	test ! -e "$$archive_failure_builddir" && \
	test -f "$$archive_failure_prefix/sentinel" || { \
		echo "FAIL  archive-failure distclean violated its ordered effects" >&2; \
		exit 1; \
	}; \
	if printf '%s\n' "$$archive_failure_output" | grep -F "archived prefix:" >/dev/null; then \
		echo "FAIL  distclean printed archival success after mv failure" >&2; \
		exit 1; \
	fi; \
	echo "OK    distclean propagates prefix archival failure"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" distclean \
		TOPSRC="$$source_root" BUILD_ROOT="$$archive_failure_root" \
		BUILDDIR="$$archive_failure_builddir" PREFIX="$$archive_failure_prefix" \
		BUILD_LOCK="$$tmpdir/archive-failure.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct SUDO= \
		>/dev/null; \
	test ! -e "$$archive_failure_prefix" || { \
		echo "FAIL  distclean retry left the bound prefix" >&2; exit 1; \
	}; \
	set -- "$$archive_failure_prefix".disabled.*; \
	test "$$#" -eq 1 && test -f "$$1/sentinel" || { \
		echo "FAIL  distclean retry did not retain one archived prefix" >&2; \
		exit 1; \
	}; \
	echo "OK    distclean resumes after prefix archival failure"; \
	clean_distclean_root="$$tmpdir/clean-distclean"; \
	clean_distclean_builddir="$$clean_distclean_root/probe"; \
	clean_distclean_prefix="$$clean_distclean_root/prefix"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$clean_distclean_root" \
		BUILDDIR="$$clean_distclean_builddir" PREFIX="$$clean_distclean_prefix" \
		BUILD_LOCK="$$tmpdir/clean-distclean.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true >/dev/null; \
	mkdir -p "$$clean_distclean_prefix"; \
	touch "$$clean_distclean_prefix/sentinel"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		TOPSRC="$$source_root" BUILD_ROOT="$$clean_distclean_root" \
		BUILDDIR="$$clean_distclean_builddir" PREFIX="$$clean_distclean_prefix" \
		BUILD_LOCK="$$tmpdir/clean-distclean.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct >/dev/null; \
	test ! -e "$$clean_distclean_builddir" && \
	test -f "$$clean_distclean_prefix/sentinel" || { \
		echo "FAIL  clean-then-distclean setup lost a protected target" >&2; \
		exit 1; \
	}; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" artifact-check \
		TOPSRC="$$source_root" BUILD_ROOT="$$clean_distclean_root" \
		BUILDDIR="$$clean_distclean_builddir" PREFIX="$$clean_distclean_prefix" \
		BUILD_LOCK="$$tmpdir/clean-distclean.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct >/dev/null; \
	echo "OK    artifact reporting uses final root identity after clean"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" distclean \
		TOPSRC="$$source_root" BUILD_ROOT="$$clean_distclean_root" \
		BUILDDIR="$$clean_distclean_builddir" PREFIX="$$clean_distclean_prefix" \
		BUILD_LOCK="$$tmpdir/clean-distclean.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct SUDO= \
		>/dev/null; \
	test ! -e "$$clean_distclean_prefix" || { \
		echo "FAIL  distclean left the bound prefix after clean" >&2; \
		exit 1; \
	}; \
	set -- "$$clean_distclean_prefix".disabled.*; \
	test "$$#" -eq 1 && test -f "$$1/sentinel" || { \
		echo "FAIL  distclean did not retain one archived prefix after clean" >&2; \
		exit 1; \
	}; \
	echo "OK    clean then distclean archives the source-bound prefix"; \
	touch "$$identity_builddir/ownership-sentinel"; \
	if clean_owner_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" clean \
		TOPSRC="$$legacy_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct 2>&1 )"; then \
		echo "FAIL  clean deleted a build directory owned by another source" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$clean_owner_output" | \
		grep -F "external source identity drift" >/dev/null || { \
		echo "FAIL  clean source ownership returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$clean_owner_output" >&2; exit 1; \
	}; \
	test -f "$$identity_builddir/ownership-sentinel" || { \
		echo "FAIL  clean removed the source-owned build directory" >&2; \
		exit 1; \
	}; \
	echo "OK    external clean verifies build-directory ownership"; \
	mkdir -p "$$identity_prefix"; \
	touch "$$identity_prefix/ownership-sentinel"; \
	retarget_root="$$tmpdir/retarget-root"; \
	retarget_builddir="$$retarget_root/probe"; \
	retarget_prefix="$$retarget_root/prefix"; \
	retarget_original="$$tmpdir/retarget-original"; \
	retarget_victim="$$tmpdir/retarget-victim"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$retarget_root" \
		BUILDDIR="$$retarget_builddir" PREFIX="$$retarget_prefix" \
		BUILD_LOCK="$$tmpdir/retarget-configure.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true >/dev/null; \
	mkdir -p "$$retarget_prefix" \
		"$$retarget_victim/probe" "$$retarget_victim/prefix"; \
	touch "$$retarget_builddir/original-sentinel" \
		"$$retarget_prefix/original-sentinel" \
		"$$retarget_victim/probe/victim-sentinel" \
		"$$retarget_victim/prefix/victim-sentinel"; \
	retarget_flock="$$tmpdir/retarget-flock"; \
	retarget_ready="$$tmpdir/retarget.ready"; \
	retarget_release="$$tmpdir/retarget.release"; \
	retarget_output="$$tmpdir/retarget.output"; \
	printf '%s\n' \
		'#!/bin/sh' \
		': > "$$MESA_RETARGET_READY"' \
		'while [ ! -e "$$MESA_RETARGET_RELEASE" ]; do sleep 0.01; done' \
		'exec flock "$$@"' > "$$retarget_flock"; \
	chmod +x "$$retarget_flock"; \
	MESA_RETARGET_READY="$$retarget_ready" \
	MESA_RETARGET_RELEASE="$$retarget_release" \
		$(MAKE) --no-print-directory -C "$(CURDIR)" distclean \
			TOPSRC="$$source_root" BUILD_ROOT="$$retarget_root" \
			BUILDDIR="$$retarget_builddir" PREFIX="$$retarget_prefix" \
			BUILD_LOCK="$$tmpdir/retarget.lock" LOCK_WAIT=5 \
			COMPILER_CHAIN=direct FLOCK="$$retarget_flock" SUDO= \
			> "$$retarget_output" 2>&1 & \
	retarget_pid=$$!; \
	retarget_wait=0; \
	while [ ! -e "$$retarget_ready" ] && [ "$$retarget_wait" -lt 500 ]; do \
		sleep 0.01; retarget_wait=$$((retarget_wait + 1)); \
	done; \
	if [ ! -e "$$retarget_ready" ]; then \
		touch "$$retarget_release"; wait "$$retarget_pid" 2>/dev/null || true; \
		echo "FAIL  retarget mutant never reached the build lease" >&2; \
		exit 1; \
	fi; \
	mv "$$retarget_root" "$$retarget_original"; \
	ln -s "$$retarget_victim" "$$retarget_root"; \
	touch "$$retarget_release"; \
	if wait "$$retarget_pid"; then \
		echo "FAIL  distclean accepted a replaced build root" >&2; \
		exit 1; \
	fi; \
	grep -E "selected path (target|identity) changed" \
		"$$retarget_output" >/dev/null || { \
		echo "FAIL  replaced build root returned the wrong diagnostic" >&2; \
		cat "$$retarget_output" >&2; exit 1; \
	}; \
	test -f "$$retarget_original/probe/original-sentinel" && \
	test -f "$$retarget_original/prefix/original-sentinel" || { \
		echo "FAIL  distclean changed the originally selected root" >&2; \
		exit 1; \
	}; \
	test -f "$$retarget_victim/probe/victim-sentinel" && \
	test -f "$$retarget_victim/prefix/victim-sentinel" || { \
		echo "FAIL  distclean changed the replacement root" >&2; \
		exit 1; \
	}; \
	unlink "$$retarget_root"; \
	mv "$$retarget_original" "$$retarget_root"; \
	test -f "$$identity_builddir/ownership-sentinel" && \
	test -f "$$identity_prefix/ownership-sentinel" || { \
		echo "FAIL  retarget mutant changed the bound identity paths" >&2; \
		exit 1; \
	}; \
	echo "OK    destructive operations reject build-root replacement at the lease"; \
	revision_source="$$tmpdir/revision-lease-source"; \
	mkdir -p "$$revision_source"; \
	git -C "$$revision_source" init -q; \
	touch "$$revision_source/meson.build" "$$revision_source/meson.options"; \
	printf '%s\n' "revision-a" > "$$revision_source/revision-marker"; \
	git -C "$$revision_source" add meson.build meson.options revision-marker; \
	git -C "$$revision_source" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: add source revision A"; \
	revision_a="$$(git -C "$$revision_source" rev-parse HEAD)"; \
	printf '%s\n' "revision-b" > "$$revision_source/revision-marker"; \
	git -C "$$revision_source" add revision-marker; \
	git -C "$$revision_source" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: add source revision B"; \
	revision_b="$$(git -C "$$revision_source" rev-parse HEAD)"; \
	revision_b_tree="$$(git -C "$$revision_source" rev-parse HEAD^{tree})"; \
	git -C "$$revision_source" switch --detach -q "$$revision_a"; \
	revision_build_root="$$tmpdir/revision-lease-build"; \
	revision_ready="$$tmpdir/revision.ready"; \
	revision_release="$$tmpdir/revision.release"; \
	revision_output="$$tmpdir/revision.output"; \
	MESA_RETARGET_READY="$$revision_ready" \
	MESA_RETARGET_RELEASE="$$revision_release" \
		$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
			TOPSRC="$$revision_source" BUILD_ROOT="$$revision_build_root" \
			BUILDDIR="$$revision_build_root/probe" \
			PREFIX="$$revision_build_root/prefix" \
			BUILD_LOCK="$$tmpdir/revision.lock" LOCK_WAIT=5 \
			FLOCK="$$retarget_flock" \
			SOURCE_COMMIT="$$revision_b" SOURCE_TREE="$$revision_b_tree" \
			MESA_SOURCE_COMMIT_CAPTURED="$$revision_b" \
			MESA_SOURCE_TREE_CAPTURED="$$revision_b_tree" \
			COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true \
			> "$$revision_output" 2>&1 & \
	revision_pid=$$!; \
	revision_wait=0; \
	while [ ! -e "$$revision_ready" ] && [ "$$revision_wait" -lt 500 ]; do \
		sleep 0.01; revision_wait=$$((revision_wait + 1)); \
	done; \
	if [ ! -e "$$revision_ready" ]; then \
		touch "$$revision_release"; wait "$$revision_pid" 2>/dev/null || true; \
		echo "FAIL  revision mutant never reached the build lease" >&2; \
		exit 1; \
	fi; \
	git -C "$$revision_source" switch --detach -q "$$revision_b"; \
	touch "$$revision_release"; \
	if wait "$$revision_pid"; then \
		echo "FAIL  configure accepted a source revision change at the lease" >&2; \
		exit 1; \
	fi; \
	grep -F "selected revision changed while waiting for the build lease: source_commit" \
		"$$revision_output" >/dev/null || { \
			echo "FAIL  source revision change returned the wrong diagnostic" >&2; \
			cat "$$revision_output" >&2; exit 1; \
		}; \
	test ! -e "$$revision_build_root" || { \
		echo "FAIL  source revision change mutated the build root" >&2; exit 1; \
	}; \
	echo "OK    build lease binds the selected source commit and tree"; \
	if shared_prefix_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$legacy_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_build_root/legacy-probe" \
		PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true 2>&1 )"; then \
		echo "FAIL  configure shared one prefix across source revisions" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$shared_prefix_output" | \
		grep -F "external source identity drift" >/dev/null || { \
		echo "FAIL  shared-prefix source conflict returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$shared_prefix_output" >&2; exit 1; \
	}; \
	echo "OK    external build root rejects a second source revision"; \
	printf '%s\n' "identity drift" >> "$$source_root/meson.build"; \
	git -C "$$source_root" add meson.build; \
	git -C "$$source_root" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: move external source identity"; \
	if drift_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
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
	if refresh_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$identity_build_root" \
		BUILDDIR="$$identity_builddir" PREFIX="$$identity_prefix" \
		BUILD_LOCK="$$tmpdir/identity.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true 2>&1 )"; then \
		echo "FAIL  configure refreshed a source-bound build root in place" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$refresh_output" | \
		grep -F "external source identity drift" >/dev/null || { \
		echo "FAIL  in-place source refresh returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$refresh_output" >&2; exit 1; \
	}; \
	refreshed_build_root="$$tmpdir/refreshed-build"; \
	refreshed_builddir="$$refreshed_build_root/probe"; \
	refreshed_prefix="$$refreshed_build_root/prefix"; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" configure \
		TOPSRC="$$source_root" BUILD_ROOT="$$refreshed_build_root" \
		BUILDDIR="$$refreshed_builddir" PREFIX="$$refreshed_prefix" \
		BUILD_LOCK="$$tmpdir/refreshed.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct MESA_LLVM_VERSION=22 MESON=true >/dev/null; \
	$(MAKE) --no-print-directory -C "$(CURDIR)" build \
		TOPSRC="$$source_root" BUILD_ROOT="$$refreshed_build_root" \
		BUILDDIR="$$refreshed_builddir" PREFIX="$$refreshed_prefix" \
		BUILD_LOCK="$$tmpdir/refreshed.lock" LOCK_WAIT=0 \
		COMPILER_CHAIN=direct NINJA=true >/dev/null; \
	echo "OK    source revision changes require a fresh isolated build root"; \
	git -C "$$source_root" update-index --assume-unchanged meson.build; \
	printf '%s\n' "assume-unchanged drift" >> "$$source_root/meson.build"; \
	if assume_output="$$( $(MAKE) --no-print-directory -C "$(CURDIR)" \
		source-root-check TOPSRC="$$source_root" 2>&1 )"; then \
		echo "FAIL  source-root-check accepted assume-unchanged drift" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$assume_output" | \
		grep -F "external TOPSRC is dirty" >/dev/null || { \
		echo "FAIL  assume-unchanged drift returned the wrong diagnostic" >&2; \
		printf '%s\n' "$$assume_output" >&2; exit 1; \
	}; \
	git -C "$$source_root" show HEAD:meson.build > "$$source_root/meson.build"; \
	git -C "$$source_root" update-index --no-assume-unchanged meson.build; \
	echo "OK    external TOPSRC detects assume-unchanged drift"; \
	printf '/build/\n/subprojects/\n' > "$$source_root/.gitignore"; \
	git -C "$$source_root" add .gitignore; \
	git -C "$$source_root" \
		-c user.name=source-root-test -c user.email=source-root-test.invalid \
		commit -qm "test: ignore Meson subproject sources"; \
	mkdir -p "$$source_root/subprojects/retained-source"; \
	touch "$$source_root/subprojects/retained-source/meson.build"; \
	if ignored_subproject_output="$$( $(MAKE) --no-print-directory \
		-C "$(CURDIR)" source-root-check TOPSRC="$$source_root" 2>&1 )"; then \
		echo "FAIL  source-root-check accepted ignored subproject sources" >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "$$ignored_subproject_output" | \
		grep -F "external TOPSRC is dirty" >/dev/null || { \
			echo "FAIL  ignored subproject returned the wrong diagnostic" >&2; \
			printf '%s\n' "$$ignored_subproject_output" >&2; exit 1; \
		}; \
	rm -rf "$$source_root/subprojects"; \
	echo "OK    external TOPSRC rejects ignored subproject sources"; \
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
	@tmpdir="$$(python3 "$(SOURCE_ROOT_CONTROL)" \
		create-test-directory --label=build-jobs)"; \
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

override SELECT_PROFILE_PREFIX = $(if $(filter 1,$(REPRODUCIBLE_RUN)),$(BUILD_ROOT)/prefix,$1)

# Canonical full-feature r300 RELEASE build: maximal GL/GLES surface plus the
# ati_r300 ICD in one artifact (profile 4_).  The registered in-tree unit tests
# run after the build through the locked `make test` target; their result is
# build/test evidence, not hardware or CTS/Piglit/dEQP evidence.
rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache:
	@[ "$(REPRODUCIBLE_RUN)" = "1" ] || { echo "release qualification requires REPRODUCIBLE_RUN=1 with distinct detached control and TOPSRC worktrees" >&2; exit 2; }
	$(MAKE) clean-registered-build PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"
	$(MAKE) configure PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"
	$(MAKE) build PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"
	$(MAKE) test PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"

install-4_r300_full_release_x86_64v1-clang22-distcc-cache:
	@[ "$(REPRODUCIBLE_RUN)" = "1" ] || { echo "release qualification requires REPRODUCIBLE_RUN=1 with distinct detached control and TOPSRC worktrees" >&2; exit 2; }
	$(MAKE) install PROFILE=4_r300_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"

# Canonical full-feature r300 DEBUG build: same surface, assertions live (-O2 -g),
# installs to the -debug prefix (profile 3_).
rebuild-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean-registered-build PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)
	$(MAKE) configure PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)
	$(MAKE) build PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)

install-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)

# Canonical r600/terakan RELEASE build: maximal terakan stack (r600, zink,
# softpipe, llvmpipe, amd_terascale) plus Rusticl in one artifact (profile 3_).
# Installs to the release prefix.
rebuild-3_terakan_full_release_x86_64v1-clang22-distcc-cache:
	@[ "$(REPRODUCIBLE_RUN)" = "1" ] || { echo "release qualification requires REPRODUCIBLE_RUN=1 with distinct detached control and TOPSRC worktrees" >&2; exit 2; }
	$(MAKE) clean-registered-build PROFILE=3_terakan_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"
	$(MAKE) configure PROFILE=3_terakan_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"
	$(MAKE) build PROFILE=3_terakan_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"

install-3_terakan_full_release_x86_64v1-clang22-distcc-cache:
	@[ "$(REPRODUCIBLE_RUN)" = "1" ] || { echo "release qualification requires REPRODUCIBLE_RUN=1 with distinct detached control and TOPSRC worktrees" >&2; exit 2; }
	$(MAKE) install PROFILE=3_terakan_full_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"

# Canonical r600/terakan DEBUG build: same surface, assertions live (-O2 -g),
# Rusticl live, installs to the -debug prefix (profile 4_).
rebuild-4_terakan_full_debug_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean-registered-build PROFILE=4_terakan_full_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)
	$(MAKE) configure PROFILE=4_terakan_full_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)
	$(MAKE) build PROFILE=4_terakan_full_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)

install-4_terakan_full_debug_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=4_terakan_full_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)

# r600/terakan RELEASE build without Rusticl: use when bindgen drift makes
# Rusticl unbuildable (profile 5_).  Installs to the release prefix.
rebuild-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache:
	@[ "$(REPRODUCIBLE_RUN)" = "1" ] || { echo "release qualification requires REPRODUCIBLE_RUN=1 with distinct detached control and TOPSRC worktrees" >&2; exit 2; }
	$(MAKE) clean-registered-build PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"
	$(MAKE) configure PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"
	$(MAKE) build PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"

install-5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache:
	@[ "$(REPRODUCIBLE_RUN)" = "1" ] || { echo "release qualification requires REPRODUCIBLE_RUN=1 with distinct detached control and TOPSRC worktrees" >&2; exit 2; }
	$(MAKE) install PROFILE=5_terakan_norusticl_release_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(BUILD_ROOT)/prefix REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)"

# r600/terakan DEBUG build without Rusticl: use when bindgen drift makes
# Rusticl unbuildable (profile 6_).  Installs to the -debug prefix.
rebuild-6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean-registered-build PROFILE=6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)
	$(MAKE) configure PROFILE=6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)
	$(MAKE) build PROFILE=6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)

install-6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=6_terakan_norusticl_debug_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-optimized)

# PRIME r300 driver: ASan+UBSan+extended sanitizers at -O0 (profile 1_).  SCOPED
# install to /opt/mesa-gororoba-debug-asan -- NEVER the system Mesa (an ASan
# libGL/libvulkan as the default Mesa aborts every GL/VK client and can
# black-screen the login).  The prefix must equal the mesa-gororoba-debug-asan
# PKGBUILD _prefix so the repack stages to the right tree; it is reached only
# through mesa-gororoba-debug-asan-run.
rebuild-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean-registered-build PROFILE=1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-asan)
	$(MAKE) configure PROFILE=1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-asan)
	$(MAKE) build PROFILE=1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-asan)

install-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-asan)

# -O0 normal-debug r300 driver, no sanitizer (profile 2_).  SCOPED install to
# /opt/mesa-gororoba-debug-o0 -- NOT the system Mesa; reached through
# mesa-gororoba-debug-o0-run.  The prefix must equal the mesa-gororoba-debug-o0
# PKGBUILD _prefix.
rebuild-2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache:
	$(MAKE) clean-registered-build PROFILE=2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-o0)
	$(MAKE) configure PROFILE=2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-o0)
	$(MAKE) build PROFILE=2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-o0)

install-2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache:
	$(MAKE) install PROFILE=2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache HOSTENV=vostro1000-x86-64-v1-clang22-ccache-distcc COMPILER_CHAIN=ccache PREFIX=$(call SELECT_PROFILE_PREFIX,/opt/mesa-gororoba-debug-o0)

# Coherent GPU debug stack ("if one is updated they are all updated"): rebuild the
# four r300 driver tiers (prime 1_, debug-o0 2_, debug-optimized 3_, release 4_)
# from the current tree so they share the source-of-truth pkgver and stay in
# lockstep.  The per-package PKGBUILDs (mesa-gororoba-debug-asan,
# mesa-gororoba-debug-o0, mesa-gororoba-debug-optimized, mesa-gororoba) then repack
# those builddirs, umr-gororoba repacks its CMake build, and the
# mesa-gororoba-debug-tools meta-package pins them together.  Pacman has no
# atomic "update all"; this target + the meta-package pin are the idiom.  Note: only
# the debug-optimized and release tiers ever become the /usr system Mesa.
rebuild-all-tiers:
	@[ "$(REPRODUCIBLE_RUN)" = "1" ] || { echo "four-tier qualification requires REPRODUCIBLE_RUN=1 with distinct detached control and TOPSRC worktrees" >&2; exit 2; }
	$(MAKE) rebuild-1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)/1_r300_full_debug_asan_o0_x86_64v1-clang22-distcc-cache"
	$(MAKE) rebuild-2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)/2_r300_full_debug_o0_x86_64v1-clang22-distcc-cache"
	$(MAKE) rebuild-3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)/3_r300_full_debug_optimized_x86_64v1-clang22-distcc-cache"
	$(MAKE) rebuild-4_r300_full_release_x86_64v1-clang22-distcc-cache REPRODUCIBLE_RUN=1 TOPSRC="$(TOPSRC)" BUILD_ROOT="$(BUILD_ROOT)/4_r300_full_release_x86_64v1-clang22-distcc-cache"
	@echo "rebuilt the four r300 driver tiers (prime + debug-o0 + debug-optimized + release) from the current tree."
	@echo "package: for p in mesa-gororoba-debug-asan mesa-gororoba-debug-o0 mesa-gororoba-debug-optimized mesa-gororoba umr-gororoba mesa-gororoba-debug-tools; do (cd packaging/\$$p && makepkg -f --noconfirm); done"

# mesa-gororoba CachyOS-style /usr drop-in package (r300 GL + llvmpipe +
# ati_r300/r3v Vulkan ICD), built -march=x86-64 so it runs on the K8 Vostro.
# package-mesa runs makepkg and repo-adds the artifact to a local
# The pacman repository named by MESA_REPO_NAME contains the release package;
# deploy-mesa ships it to TARGET_HOST and asks pacman
# to execute the package-declared replacement transaction.  Build
# on a fast x86-64 host whose llvm major matches the target's llvm-libs.
# Override the pinned source commit with MESA_COMMIT=<sha>.  Artifacts live
# under ../build/ (the sanctioned build area), not as workspace siblings.
MESA_PKG_SRC   ?= $(CURDIR)/packaging/mesa-gororoba
MESA_PKG_DIR   ?= $(abspath $(CURDIR)/../build/mesa-package)
MESA_REPO_DIR  ?= $(abspath $(CURDIR)/../build/mesa-package-repository)
MESA_REPO_NAME ?= gororoba
TARGET_HOST        ?= cachyos-vostro1000
MESA_DEPLOY_ACCEPTED ?= 0

package-mesa:
	mkdir -p "$(MESA_PKG_DIR)/tmp" "$(MESA_REPO_DIR)"
	find "$(MESA_PKG_SRC)" -maxdepth 1 -type f -exec cp -t "$(MESA_PKG_DIR)/" {} +
	[ -z "$(MESA_COMMIT)" ] || sed -i 's/^_commit=.*/_commit=$(MESA_COMMIT)/' "$(MESA_PKG_DIR)/PKGBUILD"
	cd "$(MESA_PKG_DIR)" && TMPDIR="$(MESA_PKG_DIR)/tmp" SRCDEST="$(MESA_PKG_DIR)" BUILDDIR="$(MESA_PKG_DIR)" makepkg -f --noconfirm --skipinteg
	cp "$(MESA_PKG_DIR)"/mesa-gororoba-*.pkg.tar.zst "$(MESA_REPO_DIR)/"
	repo-add "$(MESA_REPO_DIR)/$(MESA_REPO_NAME).db.tar.zst" "$(MESA_REPO_DIR)"/mesa-gororoba-*.pkg.tar.zst
	@echo "packaged + repo-added under $(MESA_REPO_DIR); deploy: make deploy-mesa TARGET_HOST=<host>"

deploy-mesa:
	python3 $(CURDIR)/scripts/deploy_mesa.py \
		--package-dir "$(MESA_PKG_DIR)" \
		--target-host "$(TARGET_HOST)" \
		--accepted "$(MESA_DEPLOY_ACCEPTED)"

deploy-mesa-unit-test:
	PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 \
		python3 -m pytest -q $(CURDIR)/tests/test_deploy_mesa.py
