# SPDX-License-Identifier: MIT
"""Validate the Terakan soft-fp64 build and lowering contract.

The Terakan soft-fp64 library requires glslangValidator at build time.  The
Evergreen lowering mask combines the NIR full-software path with the four
algebraic operations that require explicit lowering options.  The parser
compares exact dependency and mask terms.  The calibrated mutants reject a
missing or weakened dependency gate, each missing lowering bit, a changed mask
operator, and a direct mask bypass.
"""

from pathlib import Path


ROOT_MESON_PATH = Path(__file__).resolve().parents[5] / "meson.build"
VULKAN_MESON_PATH = Path(__file__).resolve().parents[1] / "meson.build"
PHYSICAL_DEVICE_PATH = (
    Path(__file__).resolve().parents[1] / "terakan_physical_device.c"
)
MASK_NAME = "evergreen_fp64_lowering_options"
MASK_BITS = (
    "nir_lower_fp64_full_software",
    "nir_lower_dceil",
    "nir_lower_drcp",
    "nir_lower_dsqrt",
    "nir_lower_drsq",
)
GLSLANG_REQUIRED_TERMS = (
    "with_vulkan_overlay_layer",
    "with_aco_tests",
    "with_bvh",
    "with_amd_terascale_vk",
)


class ContractViolationError(AssertionError):
    """The Terakan soft-fp64 source contract does not hold."""


def delimited_block(source: str, start_anchor: str, end_anchor: str) -> str:
    """Return the source block between two stable mechanism anchors."""

    start = source.index(start_anchor)
    end = source.index(end_anchor, start)
    return source[start:end]


def split_top_level_or(expression: str) -> tuple[str, ...]:
    """Return exact terms separated by top-level ``or`` operators."""

    terms = []
    term_start = 0
    parenthesis_depth = 0
    index = 0
    while index < len(expression):
        character = expression[index]
        if character == "(":
            parenthesis_depth += 1
        elif character == ")":
            parenthesis_depth -= 1
            if parenthesis_depth < 0:
                raise ContractViolationError("glslang predicate has unmatched ')'")
        elif parenthesis_depth == 0 and expression.startswith(" or ", index):
            terms.append(expression[term_start:index].strip())
            index += len(" or ")
            term_start = index
            continue
        index += 1
    if parenthesis_depth != 0:
        raise ContractViolationError("glslang predicate has unmatched '('")
    terms.append(expression[term_start:].strip())
    return tuple(terms)


def verify_contract(root_meson: str, vulkan_meson: str, physical_device: str) -> None:
    """Verify the glslang dependency and complete Evergreen lowering mask."""

    glslang_find = delimited_block(
        root_meson,
        "prog_glslang = find_program(",
        "\n\nif prog_glslang.found()",
    )
    required_line = next(
        line for line in glslang_find.splitlines() if "required :" in line
    )
    required_expression = required_line.split(":", 1)[1].strip()
    if split_top_level_or(required_expression) != GLSLANG_REQUIRED_TERMS:
        raise ContractViolationError("Terakan requires glslangValidator")

    softfp64_target = delimited_block(
        vulkan_meson,
        "terakan_float64_spv_h = custom_target(",
        "\n)\n\nlibvulkan_terascale = shared_library(",
    )
    for token in (
        "input : [glsl2spirv, float64_glsl_file]",
        "prog_glslang,",
        "glslang_depfile,",
    ):
        if token not in softfp64_target:
            raise ContractViolationError(
                f"softfp64 custom target misses {token}"
            )

    library_target = delimited_block(
        vulkan_meson,
        "libvulkan_terascale = shared_library(",
        "\n)\n\nicd_lib_path =",
    )
    if "terakan_float64_spv_h," not in library_target:
        raise ContractViolationError("library omits the softfp64 generated header")

    mask_start = physical_device.index(
        f"const nir_lower_doubles_options {MASK_NAME} ="
    )
    mask_end = physical_device.index(";", mask_start)
    mask_definition = physical_device[mask_start:mask_end]
    mask_rhs = mask_definition.split("=", 1)[1].strip()
    mask_terms = tuple(term.strip() for term in mask_rhs.split("|"))
    if (
        len(mask_terms) != len(MASK_BITS)
        or len(set(mask_terms)) != len(MASK_BITS)
        or set(mask_terms) != set(MASK_BITS)
    ):
        raise ContractViolationError("softfp64 mask terms are incomplete or changed")

    options_start = physical_device.index(".lower_doubles_options =")
    options_end = physical_device.index(",", options_start)
    options_assignment = physical_device[options_start:options_end]
    if f": {MASK_NAME}" not in options_assignment:
        raise ContractViolationError("non-r9xx options bypass the named mask")

    assertion_start = physical_device.index("assert(!features.shaderFloat64 ||")
    assertion_end = physical_device.index(");", assertion_start)
    assertion = physical_device[assertion_start:assertion_end]
    if MASK_NAME not in assertion:
        raise ContractViolationError("shaderFloat64 assertion bypasses the named mask")

    if physical_device.count(MASK_NAME) != 3:
        raise ContractViolationError("softfp64 mask has an unshared use")


def remove_terakan_gate(root_meson: str) -> str:
    """Return a source mutant that makes the Terakan dependency optional."""

    return root_meson.replace(" or with_amd_terascale_vk", "", 1)


def weaken_terakan_gate(root_meson: str) -> str:
    """Return a source mutant that combines the Terakan gate with ``and``."""

    return root_meson.replace(
        " or with_amd_terascale_vk", " and with_amd_terascale_vk", 1
    )


def remove_mask_bit(physical_device: str, bit: str) -> str:
    """Return a source mutant that removes one lowering bit."""

    mask_start = physical_device.index(
        f"const nir_lower_doubles_options {MASK_NAME} ="
    )
    mask_end = physical_device.index(";", mask_start)
    mask_definition = physical_device[mask_start:mask_end]
    mutated_definition = mask_definition.replace(bit, "", 1)
    return physical_device[:mask_start] + mutated_definition + physical_device[mask_end:]


def use_and_for_mask_bits(physical_device: str) -> str:
    """Return a source mutant that changes one mask operator to ``&``."""

    return physical_device.replace(
        "nir_lower_fp64_full_software | nir_lower_dceil",
        "nir_lower_fp64_full_software & nir_lower_dceil",
        1,
    )


def bypass_named_mask(physical_device: str) -> str:
    """Return a source mutant that bypasses the shared Evergreen mask."""

    return physical_device.replace(
        f": {MASK_NAME},", ": nir_lower_fp64_full_software,", 1
    )


def main() -> int:
    root_meson = ROOT_MESON_PATH.read_text(encoding="utf-8")
    vulkan_meson = VULKAN_MESON_PATH.read_text(encoding="utf-8")
    physical_device = PHYSICAL_DEVICE_PATH.read_text(encoding="utf-8")
    verify_contract(root_meson, vulkan_meson, physical_device)

    mutants = [
        (
            "missing Terakan glslang gate",
            remove_terakan_gate(root_meson),
            vulkan_meson,
            physical_device,
        ),
        (
            "weakened Terakan glslang gate",
            weaken_terakan_gate(root_meson),
            vulkan_meson,
            physical_device,
        ),
        *[
            (
                f"missing {bit}",
                root_meson,
                vulkan_meson,
                remove_mask_bit(physical_device, bit),
            )
            for bit in MASK_BITS
        ],
        (
            "mask AND operator",
            root_meson,
            vulkan_meson,
            use_and_for_mask_bits(physical_device),
        ),
        (
            "named mask bypass",
            root_meson,
            vulkan_meson,
            bypass_named_mask(physical_device),
        ),
    ]
    for name, mutant_root, mutant_vulkan, mutant_physical in mutants:
        try:
            verify_contract(mutant_root, mutant_vulkan, mutant_physical)
        except ContractViolationError:
            continue
        raise ContractViolationError(f"the {name} mutant passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
