# SPDX-License-Identifier: MIT
# makepkg supplies callback variables and consumes the package metadata arrays.
# shellcheck shell=bash disable=SC2034,SC2154
# Shared package ownership and Make-driven staging for stock Mesa variants.

arch=('x86_64')
url='https://github.com/Oichkatzelesfrettschen/mesa-26-gororoba'
license=('MIT')
depends=(expat glibc libdrm libelf libgcc libglvnd libstdc++ libx11 libxcb libxext
         libxdamage libxshmfence libxxf86vm libva lm_sensors spirv-tools
         wayland zlib zstd vulkan-icd-loader)
makedepends=(clang llvm meson ninja glslang python-mako python-ply wayland-protocols
             libxrandr xorgproto libxml2 lld git make ccache sccache python-pytest)
if [[ $_variant != release ]]; then
  depends+=(libunwind valgrind)
fi
provides=("mesa=${epoch}:${pkgver}-${pkgrel}" "libva-mesa-driver=${epoch}:${pkgver}-${pkgrel}"
          "mesa-libgl=${epoch}:${pkgver}-${pkgrel}"
          "vulkan-mesa-implicit-layers=${epoch}:${pkgver}-${pkgrel}"
          "vulkan-mesa-device-select=${epoch}:${pkgver}-${pkgrel}"
          libva-driver opengl-driver vulkan-driver "vulkan-radeon=${epoch}:${pkgver}-${pkgrel}")
conflicts=(mesa vulkan-mesa-implicit-layers vulkan-mesa-device-select vulkan-radeon
           'libva-mesa-driver<1:24.2.7-1' 'mesa-libgl<17.0.1-2'
           mesa-gororoba-debug mesa-gororoba-debug-asan)
for _other in mesa-gororoba mesa-gororoba-debug-optimized mesa-gororoba-debug-o0; do
  [[ $_other == "$pkgname" ]] || conflicts+=("$_other")
done
unset _other
replaces=(mesa vulkan-mesa-implicit-layers vulkan-mesa-device-select vulkan-radeon
          'libva-mesa-driver<1:24.2.7-1' 'mesa-libgl<17.0.1-2')
options=('!lto' '!strip')
install=mesa-gororoba.install

_package_paths() {
  _source_root=${MESA_PACKAGE_SRCROOT:-${srcdir}/mesa-source}
  _control_root=${MESA_PACKAGE_CONTROL_ROOT:-${_source_root}}
  local fetched_commit selected_commit
  fetched_commit=$(git -c core.fsmonitor=false -C "${srcdir}/mesa-source" rev-parse HEAD) || return 1
  selected_commit=$(git -c core.fsmonitor=false -C "${_source_root}" rev-parse HEAD) || return 1
  if [[ $fetched_commit != "$selected_commit" ]]; then
    echo 'Mesa package source differs from the fetched recipe revision; pin MESA_COMMIT to the qualified source.' >&2
    return 1
  fi
  if [[ ${1:-} == from-receipt ]]; then
    _build_root=$(cat "${srcdir}/mesa-package-build-root") || return 1
  else
    _build_root=${MESA_PACKAGE_BUILD_ROOT:-/var/tmp/mesa-26-gororoba-$(id -u)/package-${pkgname}-${selected_commit}}
  fi
  _builddir=${MESA_PACKAGE_BUILDDIR:-${_build_root}/mesa-${_profile}}
  _stage=${_build_root}/package-root
  _selected_commit=$selected_commit
}

_package_make() {
  make -C "${_control_root}/build-infra" "$@" \
    PROFILE="${_profile}" PREFIX=/usr TOPSRC="${_source_root}" \
    BUILD_ROOT="${_build_root}" BUILDDIR="${_builddir}" \
    REPRODUCIBLE_RUN="${MESA_PACKAGE_REPRODUCIBLE_RUN:-0}"
}

build() {
  _package_paths || return 1
  if [[ -n ${MESA_PACKAGE_BUILDDIR:-} ]]; then
    # Repacking preserves the qualified build's configuration and object files.
    _package_make source-root-check || return 1
  else
    _package_make configure || return 1
    _package_make build || return 1
  fi
}

check() {
  _package_paths || return 1
  local vulkan_dir=${_source_root}/src/amd/r300/vulkan
  python3 "${_control_root}/build-infra/scripts/mesa_package_layout.py" config \
    --builddir "${_builddir}" || return 1
  if [[ -n ${MESA_PACKAGE_BUILDDIR:-} ]]; then
    _package_make test MESON_TEST_ARGS=--no-rebuild || return 1
  else
    _package_make test || return 1
  fi
  python3 "${vulkan_dir}/tests/r3v_native_advertised_surface_audit.py" --selftest || return 1
  python3 "${vulkan_dir}/tests/r3v_native_advertised_surface_audit.py" \
    --source "${vulkan_dir}/r3v_physical_device.c" || return 1
  python3 "${vulkan_dir}/tests/r3v_qualification_inventory.py" --selftest || return 1
  python3 "${vulkan_dir}/tests/r3v_qualification_inventory.py" "${_builddir}" --require-tests || return 1
  # Staging runs before fakeroot so build ownership checks use the real account.
  if [[ -e ${_stage}/usr/share/mesa-gororoba/build-identity.json ]]; then
    _package_make clean-package-stage || return 1
  fi
  _package_make stage-package || return 1
  printf '%s\n' "${_build_root}" > "${srcdir}/mesa-package-build-root"
}

package() {
  _package_paths from-receipt || return 1
  python3 "${_control_root}/build-infra/scripts/mesa_package_layout.py" verify \
    --builddir "${_builddir}" --stage "${_stage}" --profile "${_profile}" --source-commit "${_selected_commit}" || return 1
  cp -a "${_stage}/." "${pkgdir}/" || return 1
  install -Dm755 "${srcdir}/mesa-gororoba-run" "${pkgdir}/usr/bin/mesa-gororoba-run"
  install -Dm644 "${srcdir}/mesa-gororoba-env.sh" \
    "${pkgdir}/etc/mesa-gororoba/mesa-gororoba-env.sh"
  install -Dm644 "${srcdir}/90-mesa-gororoba-r300.conf" \
    "${pkgdir}/usr/lib/environment.d/90-mesa-gororoba-r300.conf"
  install -Dm644 "${srcdir}/20-rs482-modesetting-glamor.conf" \
    "${pkgdir}/usr/share/mesa-gororoba/xorg/20-rs482-modesetting-glamor.conf"
  install -Dm644 "${_source_root}/docs/license.rst" \
    "${pkgdir}/usr/share/licenses/${pkgname}/license.rst"
}
