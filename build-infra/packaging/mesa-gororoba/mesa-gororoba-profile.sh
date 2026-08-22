# RS482 r300-first loader policy for interactive shells.

export __EGL_VENDOR_LIBRARY_FILENAMES="${__EGL_VENDOR_LIBRARY_FILENAMES:-/usr/share/glvnd/egl_vendor.d/50_mesa.json}"
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/mesa-gororoba/vulkan/icd.d/r3v_icd.x86_64.json}"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-${VK_DRIVER_FILES}}"
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-r300}"
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-0}"
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-r300}"
