# SPDX-License-Identifier: MIT
"""Validate the Terakan maintenance4 buffer-size property contract.

The capability table assigns maxBufferSize from the winsys maximum allocation
size before it advertises VK_KHR_maintenance4.  The DRM Radeon and WDDM
initializers provide page-aligned UINT32_MAX limits.  The calibrated mutants
reject missing, zero-valued, wrong-source, and wrong-provider assignments.
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


def matching_parenthesis(source: str, opening: int) -> int:
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "(":
            depth += 1
        elif source[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    raise ContractViolationError("unbalanced call parentheses")


def call_arguments(source: str, function_name: str) -> list[str]:
    signature = source.index(function_name + "(")
    opening = source.index("(", signature)
    arguments = source[opening + 1 : matching_parenthesis(source, opening)]
    result: list[str] = []
    argument_start = 0
    depth = 0
    for index, character in enumerate(arguments):
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        elif character == "," and depth == 0:
            result.append(arguments[argument_start:index].strip())
            argument_start = index + 1
    result.append(arguments[argument_start:].strip())
    return result


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
    extension = "extensions_out->KHR_maintenance4 = true;"
    feature = "features_out->maintenance4 = true;"

    if maintenance4.count(assignment) != 1:
        raise ContractViolationError("maintenance4 assigns one nonzero buffer limit")
    max_memory_property = (
        "properties_out->maxMemoryAllocationSize = max_memory_allocation_size;"
    )
    if max_memory_property not in capabilities:
        raise ContractViolationError(
            "the allocation limit populates maxMemoryAllocationSize"
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
    drm_arguments = call_arguments(drm_source, "terakan_physical_device_init")
    if len(drm_arguments) < 10 or drm_arguments[8:10] != [
        "UINT32_MAX & ~(page_size - 1)",
        "page_size",
    ]:
        raise ContractViolationError("DRM Radeon passes the aligned limit to init")
    wddm_arguments = call_arguments(wddm_source, "terakan_physical_device_init")
    if len(wddm_arguments) < 10 or wddm_arguments[8:10] != [
        "UINT32_MAX & ~(system_info.dwAllocationGranularity - 1)",
        "system_info.dwAllocationGranularity",
    ]:
        raise ContractViolationError("WDDM passes the aligned limit to init")


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


def use_drm_page_size_for_max_allocation(source: str) -> str:
    return source.replace(
        "UINT32_MAX & ~(page_size - 1), page_size, &tiling_info",
        "page_size, page_size, &tiling_info",
        1,
    )


def use_wddm_granularity_for_max_allocation(source: str) -> str:
    return source.replace(
        "UINT32_MAX & ~(system_info.dwAllocationGranularity - 1),\n"
        "      system_info.dwAllocationGranularity, &tiling_info",
        "system_info.dwAllocationGranularity,\n"
        "      system_info.dwAllocationGranularity, &tiling_info",
        1,
    )


def main() -> int:
    source = SOURCE_PATH.read_text(encoding="utf-8")
    drm_source = DRM_SOURCE_PATH.read_text(encoding="utf-8")
    wddm_source = WDDM_SOURCE_PATH.read_text(encoding="utf-8")
    verify_contract(source, drm_source, wddm_source)

    for name, mutant_source, mutant_drm_source, mutant_wddm_source in (
        (
            "missing maxBufferSize assignment",
            remove_max_buffer_size_assignment(source),
            drm_source,
            wddm_source,
        ),
        (
            "zero maxBufferSize assignment",
            zero_max_buffer_size_assignment(source),
            drm_source,
            wddm_source,
        ),
        (
            "wrong maxBufferSize source",
            use_max_memory_property(source),
            drm_source,
            wddm_source,
        ),
        (
            "DRM wrong max allocation argument",
            source,
            use_drm_page_size_for_max_allocation(drm_source),
            wddm_source,
        ),
        (
            "WDDM wrong max allocation argument",
            source,
            drm_source,
            use_wddm_granularity_for_max_allocation(wddm_source),
        ),
    ):
        try:
            verify_contract(
                mutant_source, mutant_drm_source, mutant_wddm_source
            )
        except ContractViolationError:
            continue
        raise ContractViolationError(f"the {name} mutant passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
