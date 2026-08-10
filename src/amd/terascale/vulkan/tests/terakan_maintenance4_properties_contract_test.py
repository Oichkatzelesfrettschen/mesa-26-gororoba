# Copyright 2026 Mesa3D authors
# SPDX-License-Identifier: MIT
"""Validate the Terakan maintenance4 buffer-size property contract.

The capability table assigns maxBufferSize from the winsys maximum allocation
size before it advertises VK_KHR_maintenance4.  The DRM Radeon and WDDM
initializers provide page-aligned UINT32_MAX limits.  The calibrated mutants
reject missing, zero-valued, wrong-source, and wrong-order assignments.
"""

from pathlib import Path


SOURCE_PATH = Path(__file__).resolve().parents[1] / "terakan_physical_device.c"
DRM_SOURCE_PATH = (
    Path(__file__).resolve().parents[1]
    / "winsys/drm_radeon/terakan_physical_device_drm_radeon.c"
)
WDDM_SOURCE_PATH = (
    Path(__file__).resolve().parents[1]
    / "winsys/wddm/terakan_physical_device_wddm.c"
)


class ContractViolationError(AssertionError):
    """The maintenance4 property source contract does not hold."""


def matching_brace(source: str, opening: int) -> int:
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise ContractViolationError("unbalanced source braces")


def function_body(source: str, function_name: str) -> str:
    signature = source.index(function_name + "(")
    opening = source.index("{", signature)
    return source[opening : matching_brace(source, opening) + 1]


def verify_contract(source: str, drm_source: str, wddm_source: str) -> None:
    capabilities = function_body(source, "terakan_physical_device_get_capabilities")
    maintenance4_start = capabilities.index(
        "/* VK_KHR_maintenance4 (#414, Vulkan 1.3)."
    )
    maintenance4_end = capabilities.index(
        "/* VK_EXT_non_seamless_cube_map", maintenance4_start
    )
    maintenance4 = capabilities[maintenance4_start:maintenance4_end]
    assignment = "properties_out->maxBufferSize = max_memory_allocation_size;"
    storage_range = "properties_out->maxStorageBufferRange ="
    extension = "extensions_out->KHR_maintenance4 = true;"
    feature = "features_out->maintenance4 = true;"

    if maintenance4.count(assignment) != 1:
        raise ContractViolationError("maintenance4 assigns one nonzero buffer limit")
    if capabilities.index(storage_range) > capabilities.index(assignment):
        raise ContractViolationError(
            "maintenance4 reports a limit after maxStorageBufferRange"
        )
    if extension not in maintenance4 or feature not in maintenance4:
        raise ContractViolationError(
            "maintenance4 advertises its extension and feature"
        )
    if maintenance4.index(assignment) > maintenance4.index(extension):
        raise ContractViolationError(
            "maintenance4 assigns the limit before advertisement"
        )
    if maintenance4.index(assignment) > maintenance4.index(feature):
        raise ContractViolationError(
            "maintenance4 assigns the limit before feature advertisement"
        )
    if "UINT32_MAX & ~(page_size - 1)" not in drm_source:
        raise ContractViolationError("DRM Radeon passes a page-aligned 32-bit limit")
    if "UINT32_MAX & ~(system_info.dwAllocationGranularity - 1)" not in wddm_source:
        raise ContractViolationError("WDDM passes a page-aligned 32-bit limit")


def remove_max_buffer_size_assignment(source: str) -> str:
    return source.replace(
        "   properties_out->maxBufferSize = max_memory_allocation_size;\n", "", 1
    )


def zero_max_buffer_size_assignment(source: str) -> str:
    return source.replace(
        "properties_out->maxBufferSize = max_memory_allocation_size;",
        "properties_out->maxBufferSize = 0;",
        1,
    )


def use_max_memory_property(source: str) -> str:
    return source.replace(
        "properties_out->maxBufferSize = max_memory_allocation_size;",
        "properties_out->maxBufferSize = properties_out->maxMemoryAllocationSize;",
        1,
    )


def move_max_buffer_size_before_storage_range(source: str) -> str:
    assignment = "   properties_out->maxBufferSize = max_memory_allocation_size;\n"
    storage_range = (
        "   properties_out->maxStorageBufferRange = "
        "~(((uint32_t)1 << tile_pipe_interleave_bytes_log2) - 1);\n"
    )
    source_without_assignment = source.replace(assignment, "", 1)
    return source_without_assignment.replace(
        storage_range, assignment + storage_range, 1
    )


def main() -> int:
    source = SOURCE_PATH.read_text(encoding="utf-8")
    drm_source = DRM_SOURCE_PATH.read_text(encoding="utf-8")
    wddm_source = WDDM_SOURCE_PATH.read_text(encoding="utf-8")
    verify_contract(source, drm_source, wddm_source)

    for name, mutant_source in (
        ("missing maxBufferSize assignment", remove_max_buffer_size_assignment(source)),
        ("zero maxBufferSize assignment", zero_max_buffer_size_assignment(source)),
        ("wrong maxBufferSize source", use_max_memory_property(source)),
        (
            "maxBufferSize before maxStorageBufferRange",
            move_max_buffer_size_before_storage_range(source),
        ),
    ):
        try:
            verify_contract(mutant_source, drm_source, wddm_source)
        except ContractViolationError:
            continue
        raise ContractViolationError(f"the {name} mutant passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
