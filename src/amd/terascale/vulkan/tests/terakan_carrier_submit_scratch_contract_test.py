# SPDX-License-Identifier: MIT
"""Validate carrier submit scratch ownership and loop reuse.

The queue owns one carrier-combined-IB scratch allocation, reuses it for every
carrier IB, and releases it during queue teardown.  Mutants restore the
loop-local stack allocation and remove queue teardown to calibrate the source
contract against both failure modes.
"""

from pathlib import Path


SOURCE_PATH = Path(__file__).resolve().parents[1] / "terakan_queue.c"


class ContractViolationError(AssertionError):
    """The carrier submit scratch source contract does not hold."""


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


def carrier_submit_branch(submit_body: str) -> str:
    anchor = "if (unlikely(command_buffer_indirect_buffer->carrier_lists != NULL))"
    branch_start = submit_body.index(anchor)
    opening = submit_body.index("{", branch_start)
    return submit_body[opening : matching_brace(submit_body, opening) + 1]


def verify_contract(source: str) -> None:
    submit = function_body(source, "terakan_queue_submit")
    carrier_branch = carrier_submit_branch(submit)
    if carrier_branch.count("terakan_carrier_submit_scratch_ensure(") != 1:
        raise ContractViolationError("carrier submit branch has one scratch ensure")
    if "queue->carrier_submit_scratch.dwords" not in carrier_branch:
        raise ContractViolationError("carrier submit branch consumes queue scratch")
    if "alloca(carrier_aligned_size" in carrier_branch:
        raise ContractViolationError("carrier submit branch retains loop-local alloca")
    if "vk_free(&device->vk.alloc, carrier_ib" in carrier_branch:
        raise ContractViolationError("carrier submit branch frees scratch per IB")

    create = function_body(source, "terakan_queue_create")
    if create.count("terakan_carrier_submit_scratch_init(") != 1:
        raise ContractViolationError("queue create initializes carrier scratch once")
    destroy = function_body(source, "terakan_queue_destroy")
    if destroy.count("terakan_carrier_submit_scratch_finish(") != 1:
        raise ContractViolationError("queue destroy releases carrier scratch once")


def restore_loop_alloca(source: str) -> str:
    ensure_start = source.index("if (!terakan_carrier_submit_scratch_ensure(")
    ensure_end = source.index("uint32_t * const carrier_ib", ensure_start)
    carrier_declaration_end = source.index(";", ensure_end) + 1
    replacement = (
        "uint32_t * const carrier_ib = "
        "alloca(carrier_aligned_size * sizeof(*carrier_ib));"
    )
    return source[:ensure_start] + replacement + source[carrier_declaration_end:]


def remove_teardown(source: str) -> str:
    teardown_start = source.index("terakan_carrier_submit_scratch_finish(")
    teardown_end = source.index(";", teardown_start) + 1
    return source[:teardown_start] + source[teardown_end:]


def main() -> int:
    source = SOURCE_PATH.read_text(encoding="utf-8")
    verify_contract(source)

    mutants = (
        ("loop-local alloca", restore_loop_alloca(source)),
        ("missing teardown", remove_teardown(source)),
    )
    for name, mutant in mutants:
        try:
            verify_contract(mutant)
        except ContractViolationError:
            continue
        raise ContractViolationError(f"the {name} mutant passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
