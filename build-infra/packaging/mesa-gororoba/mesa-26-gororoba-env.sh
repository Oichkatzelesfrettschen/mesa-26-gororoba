# Source this file to select the RS482-focused mesa-gororoba loader policy, or
# use mesa-26-gororoba-run to scope it to one command.

GOROROBA_MESA_PREFIX=${GOROROBA_MESA_PREFIX:-/usr}

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

export __EGL_VENDOR_LIBRARY_FILENAMES="${__EGL_VENDOR_LIBRARY_FILENAMES:-${GOROROBA_MESA_PREFIX}/share/glvnd/egl_vendor.d/50_mesa.json}"
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-${GOROROBA_MESA_PREFIX}/share/vulkan/icd.d/r300_icd.x86_64.json}"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-${VK_DRIVER_FILES}}"
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-r300}"
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-0}"
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-r300}"

unset -f mesa_26_gororoba_prepend_path
