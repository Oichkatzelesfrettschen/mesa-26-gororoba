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
#   make distclean PROFILE=terakan-full      # drop builddir + install prefix
#   make rebuild-terakan-distcc-no-rusticl-ccache-no-pump
#                                                # warm/incremental: ccache + distcc
#   make rebuild-terakan-distcc-no-rusticl-pump
#                                                # cold/clean: distcc-pump + direct clang
#   make install   PROFILE=terakan-full PREFIX=/usr/local/mesa-terakan
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
# Canonical single install prefix for all profiles.  Build-time variants
# (rusticl on/off, debug/release, pump/no-pump) are selected via PROFILE
# and live in separate BUILDDIRs, but the active installed copy lives at
# a single canonical path.  Override with PREFIX=... on the make line for
# parallel installs.
PREFIX   ?= /usr/local/mesa-26-gororoba
JOBS     ?= $(shell nproc)

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

.PHONY: list configure build build-generated build-pump clean distclean install test \
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
	@echo "  distclean         clean + rm -rf \$$(PREFIX)"
	@echo "  install           sudo ninja -C \$$(BUILDDIR) install"
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
	mapfile -t targets < <(ninja -C $(BUILDDIR) -t targets all | awk '$$2 == "CUSTOM_COMMAND" { sub(/:$$/, "", $$1); if ($$1 !~ /^meson-internal__/ && $$1 !~ /\.so$$/) print $$1 }'); \
	if [ "$${#targets[@]}" -gt 0 ]; then \
		echo "prebuilding $${#targets[@]} generated Meson/Ninja targets outside distcc-pump"; \
		ninja -C $(BUILDDIR) -j$(JOBS) "$${targets[@]}"; \
	else \
		echo "no generated targets found"; \
	fi

build-pump:
	@test -d $(BUILDDIR) || { echo "configure first: make configure PROFILE=$(PROFILE)"; exit 1; }
	@test -f $(ENVFILE) && . $(ENVFILE) || true; \
	distcc-pump ninja -C $(BUILDDIR) -j$(JOBS)

clean:
	rm -rf $(BUILDDIR)
	@echo "cleaned: $(BUILDDIR)"

distclean: clean
	sudo rm -rf $(PREFIX) || rm -rf $(PREFIX)
	@echo "distcleaned prefix: $(PREFIX)"

install:
	sudo ninja -C $(BUILDDIR) install

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
