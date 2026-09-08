# Select the stock package's experimental R3V ICD for one command.
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/mesa-gororoba/vulkan/icd.d/r3v_icd.x86_64.json}"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-${VK_DRIVER_FILES}}"
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-r300}"
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-r300}"
export DRAW_USE_LLVM="${DRAW_USE_LLVM:-0}"
