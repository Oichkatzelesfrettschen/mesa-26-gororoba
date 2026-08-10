# Copyright 2026 Mesa3D authors
# SPDX-License-Identifier: MIT
"""Validate the Terakan soft-fp64 construction and teardown contract.

The physical-device initializer creates the soft-fp64 mutex before fallible
ISA, Vulkan, and WSI initialization.  Every failure edge reaches the shared
mutex cleanup, and the build and runtime comments name the first lazy-library
call.  The mutant checks reject missing cleanup, a direct calloc return, and
either stale lazy-trigger description.
"""

from pathlib import Path


SOURCE_PATH = Path(__file__).resolve().parents[1] / "terakan_physical_device.c"
MESON_PATH = Path(__file__).resolve().parents[1] / "meson.build"
SOFTFP64_PATH = Path(__file__).resolve().parents[1] / "terakan_softfp64.c"


class ContractViolationError(AssertionError):
    """The soft-fp64 lifetime source contract does not hold."""


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


def block_body(source: str, anchor: str) -> str:
    anchor_index = source.index(anchor)
    opening = source.index("{", anchor_index)
    return source[opening : matching_brace(source, opening) + 1]


def verify_contract(source: str, meson: str, softfp64: str) -> None:
    init = function_body(source, "terakan_physical_device_init")
    mutex_init = "simple_mtx_init(&device->softfp64_mutex, mtx_plain);"
    mutex_destroy = "simple_mtx_destroy(&device->softfp64_mutex);"
    if init.count(mutex_init) != 1:
        raise ContractViolationError("initializer has one softfp64 mutex init")
    if init.count(mutex_destroy) != 1:
        raise ContractViolationError("initializer has one softfp64 mutex cleanup")

    failure_edges = (
        ("if (device->isa == NULL)", "goto fail_mutex;"),
        ("if (r600_isa_init(", "goto fail_isa;"),
        ("result = vk_physical_device_init(", "goto fail_isa;"),
        ("result = terakan_wsi_init(", "goto fail_device;"),
    )
    for anchor, edge in failure_edges:
        if edge not in block_body(init, anchor):
            raise ContractViolationError(f"{anchor} does not reach {edge}")

    fail_device = init.index("fail_device:")
    fail_isa = init.index("fail_isa:", fail_device)
    fail_mutex = init.index("fail_mutex:", fail_isa)
    if any(token in init[fail_device:fail_isa] for token in ("goto ", "return ")):
        raise ContractViolationError("fail_device does not fall through to fail_isa")
    if any(token in init[fail_isa:fail_mutex] for token in ("goto ", "return ")):
        raise ContractViolationError("fail_isa does not fall through to fail_mutex")
    if "vk_physical_device_finish(&device->vk);" not in init[fail_device:fail_isa]:
        raise ContractViolationError("WSI failure finishes the Vulkan device")
    isa_cleanup = init[fail_isa:fail_mutex]
    if "if (device->isa != NULL)" not in isa_cleanup:
        raise ContractViolationError("ISA cleanup guards the allocation failure")
    if "r600_isa_destroy(device->isa);" not in isa_cleanup:
        raise ContractViolationError("ISA cleanup destroys initialized state")
    if mutex_destroy not in init[fail_mutex:]:
        raise ContractViolationError("all initializer failures destroy the mutex")

    lazy_trigger = "terakan_physical_device_get_softfp64()"
    if lazy_trigger not in source:
        raise ContractViolationError("physical-device comment names the lazy call")
    if "first fp64 shader" in source:
        raise ContractViolationError("physical-device comment uses the actual trigger")
    if lazy_trigger not in meson or "lazily" not in meson:
        raise ContractViolationError("Meson comment names lazy softfp64 import")
    if "at\n# device init" in meson or "at device init" in meson:
        raise ContractViolationError("Meson comment does not claim device-init import")
    if "terakan_physical_device_get_softfp64(" not in softfp64:
        raise ContractViolationError("runtime provider exposes the named lazy call")


def remove_mutex_cleanup(source: str) -> str:
    return source.replace(
        "fail_mutex:\n   simple_mtx_destroy(&device->softfp64_mutex);",
        "fail_mutex:",
        1,
    )


def restore_calloc_direct_return(source: str) -> str:
    return source.replace(
        "result = vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);\n      goto fail_mutex;",
        "return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);",
        1,
    )


def restore_stale_physical_comment(source: str) -> str:
    return source.replace(
        "terakan_physical_device_get_softfp64(), not here.",
        "the first fp64 shader, not here.",
        1,
    )


def restore_stale_meson_comment(meson: str) -> str:
    return meson.replace(
        "converted to a cached NIR\n# library lazily by terakan_physical_device_get_softfp64()",
        "converted via spirv_to_nir at device init",
        1,
    )


def main() -> int:
    source = SOURCE_PATH.read_text(encoding="utf-8")
    meson = MESON_PATH.read_text(encoding="utf-8")
    softfp64 = SOFTFP64_PATH.read_text(encoding="utf-8")
    verify_contract(source, meson, softfp64)

    mutants = (
        ("missing mutex cleanup", remove_mutex_cleanup(source), meson),
        ("calloc direct return", restore_calloc_direct_return(source), meson),
        (
            "stale physical-device trigger",
            restore_stale_physical_comment(source),
            meson,
        ),
        ("stale Meson trigger", source, restore_stale_meson_comment(meson)),
    )
    for name, mutant_source, mutant_meson in mutants:
        try:
            verify_contract(mutant_source, mutant_meson, softfp64)
        except ContractViolationError:
            continue
        raise ContractViolationError(f"the {name} mutant passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
