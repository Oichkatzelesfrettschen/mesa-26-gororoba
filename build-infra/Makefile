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
PREFIX   ?= /usr/local/mesa-$(PROFILE)
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

.PHONY: list configure build clean distclean install test \
        rebuild-terakan-full rebuild-terakan-distcc rebuild-terakan-distcc-no-rusticl \
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
	ninja -C $(BUILDDIR) -j$(JOBS)

clean:
	rm -rf $(BUILDDIR)
	@echo "cleaned: $(BUILDDIR)"

distclean: clean
	sudo rm -rf $(PREFIX) || rm -rf $(PREFIX)
	@echo "distcleaned prefix: $(PREFIX)"

install:
	sudo ninja -C $(BUILDDIR) install

test:
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
