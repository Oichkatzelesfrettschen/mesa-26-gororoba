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
ifneq (,$(filter $(HOSTENV),btver1 btver1-ccache-no-pump))
JOBS     ?= 8
else
JOBS     ?= $(shell nproc)
endif
# Generated source/header targets to prebuild outside distcc-pump.  Keep this
# to known compiler, Vulkan, and Terascale codegen surfaces so the helper does
# not accidentally run unrelated custom commands.
GENERATED_TARGET_REGEX ?= ^(src/git_sha1[.]h|src/(compiler|vulkan|amd/terascale)/.*|src/util/(driconf_static[.]h|shader_stats[.]h|format_srgb[.]c|format/.*))$$

# --- internal paths ---------------------------------------------------------
TOPSRC   := $(abspath $(CURDIR)/..)
CONFIG   := $(CURDIR)/configs/$(PROFILE).meson
ENVFILE  := $(CURDIR)/env/$(HOSTENV).env

ifeq (,$(wildcard $(CONFIG)))
$(error unknown PROFILE='$(PROFILE)'; see `make list`)
endif

# `meson setup --native-file` loads option defaults from CONFIG.  We also
# source ENVFILE so CC, CXX, PKG_CONFIG_PATH, CFLAGS etc. are consistent
# across hosts.
SETUP_ARGS := --native-file=$(CONFIG) --reconfigure

.PHONY: list audit configure build build-generated build-pump clean distclean install \
        artifact-check test \
        rebuild-terakan-full rebuild-terakan-distcc rebuild-terakan-distcc-no-rusticl \
        rebuild-terakan-distcc-no-rusticl-ccache-no-pump \
        rebuild-terakan-distcc-no-rusticl-pump \
        rebuild-terakan-minimal \
        rebuild-base-debug rebuild-terakan-release rebuild-terakan-profile \
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
	@echo "                    warm/incremental no-rusticl lane: ccache -> distcc -> clang-21"
	@echo "  rebuild-terakan-distcc-no-rusticl-pump"
	@echo "                    cold/clean no-rusticl lane: distcc-pump -> distcc -> clang-21"
	@echo "  list              show available PROFILEs and HOSTENVs"
	@echo ""
	@echo "profile:   $(PROFILE)"
	@echo "hostenv:   $(HOSTENV)"
	@echo "builddir:  $(BUILDDIR)"
	@echo "prefix:    $(PREFIX)"

list:
	@echo "available PROFILEs (configs/*.meson):"; \
	for f in configs/*.meson; do echo "  $$(basename $$f .meson)"; done
	@echo "available HOSTENVs (env/*.env):"; \
	for f in env/*.env; do echo "  $$(basename $$f .env)"; done

audit:
	./scripts/terakan-host-audit.sh --profile $(PROFILE) --hostenv $(HOSTENV)

configure:
	@mkdir -p $(dir $(BUILDDIR))
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
	meson setup $(SETUP_ARGS) --prefix=$(PREFIX) $(BUILDDIR) $(TOPSRC)

build:
	@test -d $(BUILDDIR) || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
	ninja -C $(BUILDDIR) -j$(JOBS)

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
	@cur_prefix=$$(meson configure $(BUILDDIR) 2>/dev/null | awk "/^  prefix /{print \$\$2}" | head -1); \
	if [ "$$cur_prefix" != "$(PREFIX)" ]; then \
		echo "[install] builddir prefix $$cur_prefix != requested $(PREFIX); reconfiguring"; \
		meson configure --prefix=$(PREFIX) $(BUILDDIR); \
	fi
	sudo meson install --no-rebuild -C $(BUILDDIR)

artifact-check:
	@test -d $(PREFIX) || { echo "prefix missing: $(PREFIX)"; exit 1; }
	find $(PREFIX) -maxdepth 5 -type f \
		\( -name "libvulkan_terascale.so" -o -name "r600_dri.so" -o \
		   -name "libgallium-*.so" -o -name "terascale_icd*.json" \) \
		-printf "%p %TY-%Tm-%Td %TH:%TM:%TS %s bytes\n" | sort

test:
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
	ninja -C $(BUILDDIR) test

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
	$(MAKE) clean PROFILE=terakan-distcc-no-rusticl HOSTENV=btver1-ccache-no-pump BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump
	$(MAKE) configure PROFILE=terakan-distcc-no-rusticl HOSTENV=btver1-ccache-no-pump BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump
	$(MAKE) build PROFILE=terakan-distcc-no-rusticl HOSTENV=btver1-ccache-no-pump BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-ccache-no-pump

# Cold clean lane for x130e.  Configure and generated Meson/Ninja outputs are
# completed before the include server starts; the heavy C/C++ compile then runs
# under distcc-pump with direct clang-21.  This avoids generated-header changes
# invalidating pump's include snapshot.
rebuild-terakan-distcc-no-rusticl-pump:
	$(MAKE) clean PROFILE=terakan-distcc-no-rusticl-pump HOSTENV=btver1-distcc-pump BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-pump
	$(MAKE) configure PROFILE=terakan-distcc-no-rusticl-pump HOSTENV=btver1-distcc-pump BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-pump
	$(MAKE) build-generated PROFILE=terakan-distcc-no-rusticl-pump HOSTENV=btver1-distcc-pump BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-pump
	$(MAKE) build-pump PROFILE=terakan-distcc-no-rusticl-pump HOSTENV=btver1-distcc-pump BUILDDIR=/home/eirikr/workspaces/mesa/build/mesa-terakan-distcc-no-rusticl-pump

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
