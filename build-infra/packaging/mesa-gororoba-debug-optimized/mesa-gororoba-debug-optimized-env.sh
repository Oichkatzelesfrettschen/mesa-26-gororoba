# Source this file to select the side-by-side mesa-26-gororoba debug install
# for the current shell, or use mesa-gororoba-debug-optimized-run to scope it to one
# command.

if [ "${GOROROBA_MESA_PREFIX+x}" = x ]; then
  echo "GOROROBA_MESA_PREFIX was renamed to MESA_INSTALL_PREFIX" >&2
  return 2 2>/dev/null || exit 2
fi

MESA_INSTALL_PREFIX=${MESA_INSTALL_PREFIX:-/opt/mesa-gororoba-debug-optimized}

case ":${PATH:-}:" in
  *":${MESA_INSTALL_PREFIX}/bin:"*) ;;
  *) export PATH="${MESA_INSTALL_PREFIX}/bin${PATH:+:${PATH}}" ;;
esac
case ":${LD_LIBRARY_PATH:-}:" in
  *":${MESA_INSTALL_PREFIX}/lib:"*) ;;
  *) export LD_LIBRARY_PATH="${MESA_INSTALL_PREFIX}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" ;;
esac
case ":${LIBGL_DRIVERS_PATH:-}:" in
  *":${MESA_INSTALL_PREFIX}/lib/dri:"*) ;;
  *) export LIBGL_DRIVERS_PATH="${MESA_INSTALL_PREFIX}/lib/dri${LIBGL_DRIVERS_PATH:+:${LIBGL_DRIVERS_PATH}}" ;;
esac
case ":${LIBVA_DRIVERS_PATH:-}:" in
  *":${MESA_INSTALL_PREFIX}/lib/dri:"*) ;;
  *) export LIBVA_DRIVERS_PATH="${MESA_INSTALL_PREFIX}/lib/dri${LIBVA_DRIVERS_PATH:+:${LIBVA_DRIVERS_PATH}}" ;;
esac
case ":${GBM_BACKENDS_PATH:-}:" in
  *":${MESA_INSTALL_PREFIX}/lib/gbm:"*) ;;
  *) export GBM_BACKENDS_PATH="${MESA_INSTALL_PREFIX}/lib/gbm${GBM_BACKENDS_PATH:+:${GBM_BACKENDS_PATH}}" ;;
esac
case ":${__EGL_VENDOR_LIBRARY_DIRS:-}:" in
  *":${MESA_INSTALL_PREFIX}/share/glvnd/egl_vendor.d:"*) ;;
  *) export __EGL_VENDOR_LIBRARY_DIRS="${MESA_INSTALL_PREFIX}/share/glvnd/egl_vendor.d${__EGL_VENDOR_LIBRARY_DIRS:+:${__EGL_VENDOR_LIBRARY_DIRS}}" ;;
esac
case ":${VK_ADD_LAYER_PATH:-}:" in
  *":${MESA_INSTALL_PREFIX}/share/vulkan/explicit_layer.d:"*) ;;
  *) export VK_ADD_LAYER_PATH="${MESA_INSTALL_PREFIX}/share/vulkan/explicit_layer.d${VK_ADD_LAYER_PATH:+:${VK_ADD_LAYER_PATH}}" ;;
esac
case ":${VK_ADD_IMPLICIT_LAYER_PATH:-}:" in
  *":${MESA_INSTALL_PREFIX}/share/vulkan/implicit_layer.d:"*) ;;
  *) export VK_ADD_IMPLICIT_LAYER_PATH="${MESA_INSTALL_PREFIX}/share/vulkan/implicit_layer.d${VK_ADD_IMPLICIT_LAYER_PATH:+:${VK_ADD_IMPLICIT_LAYER_PATH}}" ;;
esac

export VK_DRIVER_FILES="${VK_DRIVER_FILES:-${MESA_INSTALL_PREFIX}/share/vulkan/icd.d/r3v_icd.x86_64.json}"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-${VK_DRIVER_FILES}}"
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-r300}"
# Pin the Gallium Draw module to its C path: the r300 vertex stage runs
# software TCL, and the in-development software vertex FPU must be the
# code that executes, not the LLVM draw JIT.  Override with
# DRAW_USE_LLVM=1 to compare against the JIT.
export DRAW_USE_LLVM="${DRAW_USE_LLVM:-0}"
