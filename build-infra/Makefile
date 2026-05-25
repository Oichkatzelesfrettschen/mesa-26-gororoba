# build-infra/Makefile -- mesa-26-gororoba build entry point
#
# Consolidates the 9 legacy build-*/ dirs into a single infra that
# selects a PROFILE -> meson config file -> builddir under
# ../../build/mesa-<profile>/.
#
# Usage:
#   make configure PROFILE=terakan-full      # first time, or after config edits
#   make build     PROFILE=terakan-full      # incremental ninja
#   make rebuild-terakan-full                # clean + build (common loop)
#   make clean     PROFILE=terakan-full      # drop the builddir only
#   make distclean PROFILE=terakan-full      # drop builddir + archive install prefix
#   make rebuild-terakan-distcc-no-rusticl-ccache-no-pump
#                                                # warm/incremental: ccache + distcc
#   make rebuild-terakan-distcc-no-rusticl-pump
#                                                # cold/clean: distcc-pump + direct clang
#   make install   PROFILE=terakan-full      # meson install --no-rebuild
#   make audit     PROFILE=terakan-distcc-no-rusticl HOSTENV=btver1-ccache-no-pump
#   make list                                # show available PROFILEs
#
# See also:
#   build-infra/README.md                    -- how to use
#   build-infra/configs/*.meson              -- canonical option sets
#   build-infra/env/*.env                    -- per-host toolchain env
#   ../../steinmarder/docs/workspace/mesa-fork-synthesis.md

SHELL := /bin/bash

# --- configurable parameters -------------------------------------------------
PROFILE  ?= terakan-full
HOSTENV  ?= btver1
BUILDDIR ?= $(abspath $(CURDIR)/../../build/mesa-$(PROFILE))
INSTALL_NAMESPACE ?= mesa
# Keep profile lanes isolated by default because meson install does not remove
# artifacts that are absent from the current profile.  Override PREFIX=... on
# the make line when intentionally installing into a shared active tree.
PREFIX   ?= /usr/local/$(INSTALL_NAMESPACE)-$(PROFILE)
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

TERAKAN_PAIR_RELEASE := \
    terakan-distcc-no-rusticl \
    btver1-ccache-no-pump \
    $(abspath $(CURDIR)/../../build/mesa-terakan-distcc-no-rusticl-ccache-no-pump) \
    /usr/local/mesa-26-gororoba \
    mesa-26-gororoba

TERAKAN_PAIR_DEBUG := \
    terakan-distcc-no-rusticl-debug \
    btver1-ccache-no-pump \
    $(abspath $(CURDIR)/../../build/mesa-terakan-distcc-no-rusticl-debug-ccache-no-pump) \
    /usr/local/mesa-26-gororoba-debug \
    mesa-26-gororoba-debug
ifneq (,$(filter $(HOSTENV),btver1 btver1-ccache-no-pump))
JOBS     ?= 8
else ifneq (,$(filter $(HOSTENV),vostro1000-k8-ccache-no-pump))
JOBS     ?= 16
else ifneq (,$(filter $(HOSTENV),vostro1000-k8-distcc))
JOBS     ?= 24
else
JOBS     ?= $(shell nproc)
endif
# Build serialization lock: held during ninja and meson install to prevent
# concurrent builds from different agents stomping each other.  Any process
# that respects this lock will serialize automatically.
BUILD_LOCK ?= $(HOME)/.cache/mesa-26-gororoba/mesa-build.lock

# Generated source/header targets to prebuild outside distcc-pump.  Keep this
# to known compiler, Vulkan, and Terascale codegen surfaces so the helper does
# not accidentally run unrelated custom commands.
GENERATED_TARGET_REGEX ?= ^(src/git_sha1[.]h|src/(compiler|vulkan|amd/terascale)/.*|src/util/(driconf_static[.]h|shader_stats[.]h|format_srgb[.]c|format/.*))$$

# --- internal paths ---------------------------------------------------------
TOPSRC   := $(abspath $(CURDIR)/..)
CONFIG   := $(CURDIR)/configs/$(PROFILE).meson
ENVFILE  := $(CURDIR)/env/$(HOSTENV).env
TOOLCHAIN_NATIVE := $(BUILDDIR)/gororoba-toolchain.meson
ifeq ($(PROFILE),terakan-distcc-no-rusticl-pump)
COMPILER_CHAIN ?= distcc
else
COMPILER_CHAIN ?= ccache
endif

ifeq (,$(wildcard $(CONFIG)))
$(error unknown PROFILE='$(PROFILE)'; see `make list`)
endif

# `meson setup --native-file` loads option defaults from CONFIG.  The
# generated toolchain overlay keeps compiler package suffixes host-local
# instead of committing one LLVM major into every native file.
SETUP_ARGS := --native-file=$(CONFIG) --native-file=$(TOOLCHAIN_NATIVE) --reconfigure
MESON_SETUP_LOCAL_UNSET := CCACHE_PREFIX DISTCC_HOSTS DISTCC_PUMP INCLUDE_SERVER_PORT INCLUDE_SERVER_ARGS INCLUDE_SERVER_PID

.PHONY: list audit configure build build-generated build-pump clean distclean install \
        artifact-check test \
        rebuild-terakan-full rebuild-terakan-distcc rebuild-terakan-distcc-no-rusticl \
        rebuild-terakan-distcc-no-rusticl-ccache-no-pump \
        rebuild-terakan-distcc-no-rusticl-ccache-no-pump-pair \
        install-terakan-distcc-no-rusticl-ccache-no-pump-pair \
        rebuild-terakan-distcc-no-rusticl-debug-ccache-no-pump \
        rebuild-terakan-distcc-no-rusticl-pump \
        rebuild-terakan-minimal \
        rebuild-base-debug rebuild-terakan-release rebuild-terakan-profile \
        rebuild-r300-vostro-k8-release rebuild-r300-vostro-k8-debug \
        install-r300-vostro-k8-release install-r300-vostro-k8-debug \
        rebuild-r300vk-vostro-k8-release rebuild-r300vk-vostro-k8-debug \
        install-r300vk-vostro-k8-release install-r300vk-vostro-k8-debug \
        help

help:
	@echo "usage: make <target> PROFILE=<profile> [HOSTENV=<env>] [BUILDDIR=<path>] [PREFIX=<path>]"
	@echo ""
	@echo "targets:"
	@echo "  configure         meson setup the builddir from configs/\$$(PROFILE).meson"
	@echo "  build             ninja -C \$$(BUILDDIR)"
	@echo "  clean             rm -rf \$$(BUILDDIR)"
	@echo "  distclean         clean + archive \$$(PREFIX) aside"
	@echo "  audit             verify host tools, env policy, and profile requirements"
	@echo "  install           sudo meson install --no-rebuild -C \$$(BUILDDIR)"
	@echo "  artifact-check    list installed Terakan/r600 artifacts under \$$(PREFIX)"
	@echo "  test              ninja -C \$$(BUILDDIR) test"
	@echo "  rebuild-<profile> clean + configure + build for that profile"
	@echo "  rebuild-terakan-distcc-no-rusticl-ccache-no-pump"
	@echo "                    warm/incremental no-rusticl lane: ccache -> distcc -> clang"
	@echo "  rebuild-terakan-distcc-no-rusticl-pump"
	@echo "                    cold/clean no-rusticl lane: distcc-pump -> distcc -> clang"
	@echo "  list              show available PROFILEs and HOSTENVs"
	@echo ""
	@echo "profile:   $(PROFILE)"
	@echo "hostenv:   $(HOSTENV)"
	@echo "builddir:  $(BUILDDIR)"
	@echo "prefix:    $(PREFIX)"
	@echo "compiler chain: $(COMPILER_CHAIN)"

list:
	@echo "available PROFILEs (configs/*.meson):"; \
	for f in configs/*.meson; do echo "  $$(basename $$f .meson)"; done
	@echo "available HOSTENVs (env/*.env):"; \
	for f in env/*.env; do echo "  $$(basename $$f .env)"; done

audit:
	@failures=0; warnings=0; \
	fail() { echo "FAIL  $$*" >&2; failures=$$((failures + 1)); }; \
	warn() { echo "WARN  $$*" >&2; warnings=$$((warnings + 1)); }; \
	ok() { echo "OK    $$*"; }; \
	need_command() { if command -v "$$1" >/dev/null 2>&1; then ok "command $$1"; else fail "missing command $$1"; fi; }; \
	need_file() { if [ -e "$$1" ]; then ok "path $$1"; else fail "missing path $$1"; fi; }; \
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
	llvm_has_core() { \
		version="$$1"; \
		need_config="$${2:-1}"; \
		find_llvm_tool clang "$$version" >/dev/null 2>&1 && \
		find_llvm_tool clang++ "$$version" >/dev/null 2>&1 && \
		{ [ "$$need_config" -eq 0 ] || find_llvm_tool llvm-config "$$version" >/dev/null 2>&1; }; \
	}; \
	need_llvm_family() { \
		need_config="$${1:-1}"; \
		requested="$${MESA_LLVM_VERSION:-$${GOROROBA_LLVM_VERSION:-}}"; selected=""; \
		if [ -n "$$requested" ]; then \
			if llvm_has_core "$$requested" "$$need_config"; then selected="$$requested"; \
			else \
				if [ "$$need_config" = "1" ]; then \
					fail "requested LLVM major $$requested lacks required clang/clang++/llvm-config tools on PATH"; \
				else \
					fail "requested LLVM major $$requested lacks required clang/clang++ tools on PATH"; \
				fi; \
				return; \
			fi; \
		else \
			for version in 21 22 20 19 18 17 16 15 14 13 12 11 10; do \
				if llvm_has_core "$$version" "$$need_config"; then selected="$$version"; break; fi; \
			done; \
			version=30; \
			while [ -z "$$selected" ] && [ "$$version" -ge 10 ]; do \
				if llvm_has_core "$$version" "$$need_config"; then selected="$$version"; break; fi; \
				version=$$((version - 1)); \
			done; \
		fi; \
		if [ -n "$$selected" ]; then \
			if [ "$$need_config" -eq 1 ]; then ok "LLVM family $$selected with clang/clang++/llvm-config"; else ok "LLVM compiler family $$selected"; fi; \
		else fail "missing coherent LLVM compiler family"; fi; \
	}; \
	need_pkg_config() { if pkg-config --exists "$$1" 2>/dev/null; then ok "pkg-config $$1"; else fail "missing pkg-config dependency $$1"; fi; }; \
	echo "Terakan host audit"; \
	echo "profile: $(PROFILE)"; \
	echo "hostenv: $(HOSTENV)"; \
	echo "topsrc:  $(TOPSRC)"; echo; \
	need_file "$(CONFIG)"; need_file "$(ENVFILE)"; \
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
	else ok "distcc not in active compiler chain; skipping host policy check"; fi; \
	if [ "$(HOSTENV)" = "btver1-ccache-no-pump" ]; then \
		if grep -Eq 'export CCACHE_PREFIX=.*distcc' "$(ENVFILE)"; then ok "warm lane defaults CCACHE_PREFIX to distcc"; else fail "warm lane must default CCACHE_PREFIX to distcc"; fi; \
		if grep -q 'unset DISTCC_PUMP' "$(ENVFILE)"; then ok "warm lane clears distcc-pump state"; else fail "warm lane must clear distcc-pump state"; fi; \
	fi; \
	if [ "$(HOSTENV)" = "btver1-distcc-pump" ]; then \
		if grep -q 'unset CCACHE_PREFIX' "$(ENVFILE)"; then ok "pump lane removes ccache prefix"; else fail "pump lane must unset CCACHE_PREFIX"; fi; \
		if grep -q ',cpp' "$(ENVFILE)"; then ok "pump lane derives cpp host options"; else fail "pump lane must derive ,cpp host options"; fi; \
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

configure:
	@mkdir -p $(dir $(BUILDDIR))
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
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
	has_core() { \
		version="$$1"; \
		need_config="$${2:-1}"; \
		find_tool clang "$$version" >/dev/null 2>&1 && \
		find_tool clang++ "$$version" >/dev/null 2>&1 && \
		{ [ "$$need_config" -eq 0 ] || find_tool llvm-config "$$version" >/dev/null 2>&1; }; \
	}; \
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
		for candidate_version in 21 22 20 19 18 17 16 15 14 13 12 11 10; do \
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
	clang_cmd="$$(find_tool clang "$$llvm_version")"; \
	clangxx_cmd="$$(find_tool clang++ "$$llvm_version")"; \
	llvm_config_cmd=""; \
	if [ "$$needs_llvm_config" -eq 1 ]; then llvm_config_cmd="$$(find_tool llvm-config "$$llvm_version")"; fi; \
	llvm_ar_cmd="$$(find_tool llvm-ar "$$llvm_version" || true)"; \
	llvm_nm_cmd="$$(find_tool llvm-nm "$$llvm_version" || true)"; \
	llvm_strip_cmd="$$(find_tool llvm-strip "$$llvm_version" || true)"; \
	llvm_objcopy_cmd="$$(find_tool llvm-objcopy "$$llvm_version" || true)"; \
	mkdir -p "$$(dirname "$(TOOLCHAIN_NATIVE)")"; \
	{ \
		echo "# Generated by build-infra/Makefile."; \
		echo "# Do not edit; rerun make configure."; \
		echo; \
		echo "[binaries]"; \
		case "$(COMPILER_CHAIN)" in \
			ccache) \
				printf "c = ['ccache', '%s']\n" "$$clang_cmd"; \
				printf "cpp = ['ccache', '%s']\n" "$$clangxx_cmd"; \
				;; \
			distcc) \
				printf "c = ['distcc', '%s']\n" "$$clang_cmd"; \
				printf "cpp = ['distcc', '%s']\n" "$$clangxx_cmd"; \
				;; \
			direct) \
				printf "c = '%s'\n" "$$clang_cmd"; \
				printf "cpp = '%s'\n" "$$clangxx_cmd"; \
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
	echo "wrote Meson toolchain overlay: $(TOOLCHAIN_NATIVE) (LLVM $$llvm_version, $(COMPILER_CHAIN))"
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
	for var in $(MESON_SETUP_LOCAL_UNSET); do unset "$$var"; done; \
	meson setup $(SETUP_ARGS) --prefix=$(PREFIX) $(BUILDDIR) $(TOPSRC)

build:
	@test -d $(BUILDDIR) || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
	mkdir -p "$(dir $(BUILD_LOCK))"; \
	flock -x -w 7200 $(BUILD_LOCK) ninja -C $(BUILDDIR) -j$(JOBS)

build-generated:
	@test -d $(BUILDDIR) || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
	mapfile -t targets < <(ninja -C $(BUILDDIR) -t targets all | awk -v re='$(GENERATED_TARGET_REGEX)' '$$2 == "CUSTOM_COMMAND" { sub(/:$$/, "", $$1); if ($$1 ~ re) print $$1 }'); \
	if [ "$${#targets[@]}" -gt 0 ]; then \
		echo "prebuilding $${#targets[@]} selected generated Meson/Ninja targets outside distcc-pump"; \
		ninja -C $(BUILDDIR) -j$(JOBS) "$${targets[@]}"; \
	else \
		echo "no selected generated targets found"; \
	fi

build-pump:
	@test -d $(BUILDDIR) || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
	distcc-pump ninja -C $(BUILDDIR) -j$(JOBS)

clean:
	rm -rf $(BUILDDIR)
	@echo "cleaned: $(BUILDDIR)"

distclean: clean
	@if [ -e "$(PREFIX)" ]; then \
		archive="$(PREFIX).disabled.$$(date -u +%Y%m%dT%H%M%SZ)"; \
		sudo mv "$(PREFIX)" "$$archive"; \
		echo "archived prefix: $(PREFIX) -> $$archive"; \
	else \
		echo "prefix already absent: $(PREFIX)"; \
	fi

install:
	@test -d $(BUILDDIR) || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
	mkdir -p "$(dir $(BUILD_LOCK))"; \
	flock -x -w 7200 $(BUILD_LOCK) bash -c 'set -euo pipefail; \
	    echo "[install] flock acquired; reconfiguring $(BUILDDIR) with --prefix=$(PREFIX)"; \
	    for var in $(MESON_SETUP_LOCAL_UNSET); do unset "$$var"; done; \
	    meson setup $(SETUP_ARGS) --prefix=$(PREFIX) $(BUILDDIR) $(TOPSRC); \
	    echo "[install] running meson install --no-rebuild -C $(BUILDDIR)"; \
	    sudo meson install --no-rebuild -C $(BUILDDIR); \
	    echo "[install] complete; releasing flock"'

artifact-check:
	@test -d $(PREFIX) || { echo "prefix missing: $(PREFIX)"; exit 1; }
	find $(PREFIX) -maxdepth 5 -type f \
		\( -name "libvulkan_terascale.so" -o -name "r600_dri.so" -o \
		   -name "libgallium-*.so" -o -name "terascale_icd*.json" \) \
		-printf "%p %TY-%Tm-%Td %TH:%TM:%TS %s bytes\n" | sort

test:
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
	ninja -C $(BUILDDIR) test

# Pair-member loops.  Each takes one PAIR_<NAME> list and threads
# PROFILE + HOSTENV + BUILDDIR + PREFIX + INSTALL_NAMESPACE through
# to the sub-make.  Threading PREFIX through configure (not just
# install) ensures meson-private/install.dat is generated at the
# correct prefix on the first configure, removing the prior
# "configure to default prefix, then retarget at install" hazard
# where install.dat retained the configure-time prefix and the
# binaries landed under /usr/local/<NAMESPACE>-<PROFILE>/ instead
# of the requested PREFIX.
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

# common single-command loops (clean + configure + build)
rebuild-terakan-full:
	$(MAKE) clean PROFILE=terakan-full
	$(MAKE) configure PROFILE=terakan-full
	$(MAKE) build     PROFILE=terakan-full

rebuild-terakan-distcc:
	$(MAKE) clean PROFILE=terakan-distcc
	$(MAKE) configure PROFILE=terakan-distcc
	$(MAKE) build     PROFILE=terakan-distcc

# Same as terakan-distcc but with gallium-rusticl=false.  Use this
# when the rusticl rust frontend fails to build due to bindgen drift
# (see docs/workspace/rusticl-bindgen-drift.md in steinmarder).
rebuild-terakan-distcc-no-rusticl:
	$(MAKE) clean PROFILE=terakan-distcc-no-rusticl
	$(MAKE) configure PROFILE=terakan-distcc-no-rusticl
	$(MAKE) build     PROFILE=terakan-distcc-no-rusticl

# Future clean-rebuild lane for x130e.  This intentionally does NOT use
# distcc-pump, and it writes to a distinct builddir so it cannot disturb an
# active mesa-terakan-distcc-no-rusticl build.  `clean` removes only BUILDDIR;
# ccache and sccache live under ~/.cache via env/btver1-ccache-no-pump.env.
rebuild-terakan-distcc-no-rusticl-ccache-no-pump:
	$(call CLEAN_CONFIGURE_BUILD_PAIR,$(TERAKAN_PAIR_RELEASE))

# Pair target: build BOTH the release-default canonical AND the
# debug variant in the same invocation.  Release goes to
# /usr/local/mesa-26-gororoba/ (timing-sensitive probe target),
# debug goes to /usr/local/mesa-26-gororoba-debug/ (interactive
# bringup target).  Both builds use the same ccache + distcc
# fabric so ccache hits across them give near-incremental
# wall-clock on the second pass.  Installs run only after both build
# trees succeed so the active release/debug pair does not get half
# updated by a late debug failure.
rebuild-terakan-distcc-no-rusticl-ccache-no-pump-pair:
	$(MAKE) rebuild-terakan-distcc-no-rusticl-ccache-no-pump
	$(MAKE) rebuild-terakan-distcc-no-rusticl-debug-ccache-no-pump
	$(call INSTALL_PAIR,$(TERAKAN_PAIR_RELEASE))
	$(call INSTALL_PAIR,$(TERAKAN_PAIR_DEBUG))

rebuild-terakan-distcc-no-rusticl-debug-ccache-no-pump:
	$(call CLEAN_CONFIGURE_BUILD_PAIR,$(TERAKAN_PAIR_DEBUG))

# Install-only convenience targets.  Use these after the pair build
# trees are already populated (e.g., when fixing the install routing
# without redoing the 60-minute compile pass).
install-terakan-distcc-no-rusticl-ccache-no-pump-pair:
	$(call INSTALL_PAIR,$(TERAKAN_PAIR_RELEASE))
	$(call INSTALL_PAIR,$(TERAKAN_PAIR_DEBUG))

# Cold clean lane for x130e.  Configure and generated Meson/Ninja outputs are
# completed before the include server starts; the heavy C/C++ compile then runs
# under distcc-pump with direct clang.  This avoids generated-header changes
# invalidating pump's include snapshot.
rebuild-terakan-distcc-no-rusticl-pump:
	$(MAKE) clean PROFILE=terakan-distcc-no-rusticl-pump HOSTENV=btver1-distcc-pump BUILDDIR=$(abspath $(CURDIR)/../../build/mesa-terakan-distcc-no-rusticl-pump)
	$(MAKE) configure PROFILE=terakan-distcc-no-rusticl-pump HOSTENV=btver1-distcc-pump BUILDDIR=$(abspath $(CURDIR)/../../build/mesa-terakan-distcc-no-rusticl-pump)
	$(MAKE) build-generated PROFILE=terakan-distcc-no-rusticl-pump HOSTENV=btver1-distcc-pump BUILDDIR=$(abspath $(CURDIR)/../../build/mesa-terakan-distcc-no-rusticl-pump)
	$(MAKE) build-pump PROFILE=terakan-distcc-no-rusticl-pump HOSTENV=btver1-distcc-pump BUILDDIR=$(abspath $(CURDIR)/../../build/mesa-terakan-distcc-no-rusticl-pump)

rebuild-terakan-minimal:
	$(MAKE) clean PROFILE=terakan-minimal
	$(MAKE) configure PROFILE=terakan-minimal
	$(MAKE) build     PROFILE=terakan-minimal

rebuild-base-debug:
	$(MAKE) clean PROFILE=base-debug
	$(MAKE) configure PROFILE=base-debug
	$(MAKE) build     PROFILE=base-debug

# forward-looking variants (configs to be added as they are validated)
rebuild-terakan-release:
	$(MAKE) clean PROFILE=terakan-release
	$(MAKE) configure PROFILE=terakan-release
	$(MAKE) build     PROFILE=terakan-release

rebuild-terakan-profile:
	$(MAKE) clean PROFILE=terakan-profile
	$(MAKE) configure PROFILE=terakan-profile
	$(MAKE) build     PROFILE=terakan-profile

# Canonical Vostro r300 release build: prefix /usr/local/mesa-26-gororoba
rebuild-r300-vostro-k8-release:
	$(MAKE) -j1 clean PROFILE=r300-vostro-k8-release HOSTENV=vostro1000-k8-distcc COMPILER_CHAIN=distcc PREFIX=/usr/local/mesa-26-gororoba
	$(MAKE) -j1 configure PROFILE=r300-vostro-k8-release HOSTENV=vostro1000-k8-distcc COMPILER_CHAIN=distcc PREFIX=/usr/local/mesa-26-gororoba
	$(MAKE) build PROFILE=r300-vostro-k8-release HOSTENV=vostro1000-k8-distcc COMPILER_CHAIN=distcc PREFIX=/usr/local/mesa-26-gororoba

install-r300-vostro-k8-release:
	$(MAKE) install PROFILE=r300-vostro-k8-release HOSTENV=vostro1000-k8-distcc COMPILER_CHAIN=distcc PREFIX=/usr/local/mesa-26-gororoba

# Canonical Vostro r300 debugoptimized build: prefix /usr/local/mesa-26-gororoba-debug
rebuild-r300-vostro-k8-debug:
	$(MAKE) -j1 clean PROFILE=r300-vostro-k8-debug HOSTENV=vostro1000-k8-distcc COMPILER_CHAIN=distcc PREFIX=/usr/local/mesa-26-gororoba-debug
	$(MAKE) -j1 configure PROFILE=r300-vostro-k8-debug HOSTENV=vostro1000-k8-distcc COMPILER_CHAIN=distcc PREFIX=/usr/local/mesa-26-gororoba-debug
	$(MAKE) build PROFILE=r300-vostro-k8-debug HOSTENV=vostro1000-k8-distcc COMPILER_CHAIN=distcc PREFIX=/usr/local/mesa-26-gororoba-debug

install-r300-vostro-k8-debug:
	$(MAKE) install PROFILE=r300-vostro-k8-debug HOSTENV=vostro1000-k8-distcc COMPILER_CHAIN=distcc PREFIX=/usr/local/mesa-26-gororoba-debug

# Canonical Vostro r300vk release build: prefix /usr/local/mesa-26-gororoba
rebuild-r300vk-vostro-k8-release:
	$(MAKE) -j1 clean PROFILE=r300vk-vostro-k8-release HOSTENV=vostro1000-k8-ccache-no-pump COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba
	$(MAKE) -j1 configure PROFILE=r300vk-vostro-k8-release HOSTENV=vostro1000-k8-ccache-no-pump COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba
	$(MAKE) build PROFILE=r300vk-vostro-k8-release HOSTENV=vostro1000-k8-ccache-no-pump COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba

install-r300vk-vostro-k8-release:
	$(MAKE) install PROFILE=r300vk-vostro-k8-release HOSTENV=vostro1000-k8-ccache-no-pump COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba

# Canonical Vostro r300vk debugoptimized build: prefix /usr/local/mesa-26-gororoba-debug
rebuild-r300vk-vostro-k8-debug:
	$(MAKE) -j1 clean PROFILE=r300vk-vostro-k8-debug HOSTENV=vostro1000-k8-ccache-no-pump COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba-debug
	$(MAKE) -j1 configure PROFILE=r300vk-vostro-k8-debug HOSTENV=vostro1000-k8-ccache-no-pump COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba-debug
	$(MAKE) build PROFILE=r300vk-vostro-k8-debug HOSTENV=vostro1000-k8-ccache-no-pump COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba-debug

install-r300vk-vostro-k8-debug:
	$(MAKE) install PROFILE=r300vk-vostro-k8-debug HOSTENV=vostro1000-k8-ccache-no-pump COMPILER_CHAIN=ccache PREFIX=/usr/local/mesa-26-gororoba-debug
