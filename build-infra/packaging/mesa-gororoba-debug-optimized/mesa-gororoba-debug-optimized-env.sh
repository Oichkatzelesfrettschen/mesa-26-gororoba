# Source this file to select the side-by-side mesa-26-gororoba debug install
# for the current shell, or use mesa-gororoba-debug-optimized-run to scope it to one
# command.

GOROROBA_MESA_PREFIX=${GOROROBA_MESA_PREFIX:-/opt/mesa-gororoba-debug-optimized}

mesa_26_gororoba_prepend_path() {
  var_name=$1
  path_value=$2
  eval "old_value=\${$var_name:-}"
  case ":$old_value:" in
    *":$path_value:"*) ;;
    ::) eval "export $var_name=\$path_value" ;;
    *) eval "export $var_name=\$path_value:\$old_value" ;;
  esac
}

mesa_26_gororoba_prepend_path PATH "${GOROROBA_MESA_PREFIX}/bin"
mesa_26_gororoba_prepend_path LD_LIBRARY_PATH "${GOROROBA_MESA_PREFIX}/lib"
mesa_26_gororoba_prepend_path LIBGL_DRIVERS_PATH "${GOROROBA_MESA_PREFIX}/lib/dri"
mesa_26_gororoba_prepend_path LIBVA_DRIVERS_PATH "${GOROROBA_MESA_PREFIX}/lib/dri"
mesa_26_gororoba_prepend_path GBM_BACKENDS_PATH "${GOROROBA_MESA_PREFIX}/lib/gbm"
mesa_26_gororoba_prepend_path __EGL_VENDOR_LIBRARY_DIRS "${GOROROBA_MESA_PREFIX}/share/glvnd/egl_vendor.d"
mesa_26_gororoba_prepend_path VK_ADD_LAYER_PATH "${GOROROBA_MESA_PREFIX}/share/vulkan/explicit_layer.d"
mesa_26_gororoba_prepend_path VK_ADD_IMPLICIT_LAYER_PATH "${GOROROBA_MESA_PREFIX}/share/vulkan/implicit_layer.d"

export VK_DRIVER_FILES="${VK_DRIVER_FILES:-${GOROROBA_MESA_PREFIX}/share/vulkan/icd.d/r3v_icd.x86_64.json}"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-${VK_DRIVER_FILES}}"
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-r300}"
# Pin the Gallium Draw module to its C path: the r300 vertex stage runs
# software TCL, and the in-development software vertex FPU must be the
# code that executes, not the LLVM draw JIT.  Override with
# DRAW_USE_LLVM=1 to compare against the JIT.
export DRAW_USE_LLVM="${DRAW_USE_LLVM:-0}"

unset -f mesa_26_gororoba_prepend_path
