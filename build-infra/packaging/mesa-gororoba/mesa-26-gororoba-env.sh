# Source this file to select the side-by-side mesa-26-gororoba install for the
# current shell, or use mesa-26-gororoba-run to scope it to one command.

GOROROBA_MESA_PREFIX=${GOROROBA_MESA_PREFIX:-/opt/mesa-26-gororoba}

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

export VK_DRIVER_FILES="${VK_DRIVER_FILES:-${GOROROBA_MESA_PREFIX}/share/vulkan/icd.d/r300_icd.x86_64.json}"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-${VK_DRIVER_FILES}}"
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-r300}"

unset -f mesa_26_gororoba_prepend_path
