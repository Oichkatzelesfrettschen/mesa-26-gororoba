# Copyright 2026 Mesa3D authors
# SPDX-License-Identifier: MIT
"""Validate the single-view view-index state contract in the draw paths.

The multiview lowering reads the driver view-index push constant as
gl_ViewIndex.  Every single-view draw path therefore writes zero after a
multiview path can leave a nonzero index.  The self-test removes each
required reset in turn and requires the contract check to reject every
known-bad mutant.
"""

from pathlib import Path


SOURCE_PATH = Path(__file__).resolve().parents[1] / "terakan_draw.c"
FUNCTION_NAMES = (
    "terakan_CmdDraw",
    "terakan_CmdDrawIndexed",
    "terakan_CmdDrawIndirect",
    "terakan_CmdDrawIndexedIndirect",
)
RESET_CALL = "terakan_set_view_index_push_constant(command_writer, 0);"


class ContractViolationError(AssertionError):
    """The draw-path source contract does not hold."""


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
    opening, closing = function_range(source, function_name)
    return source[opening : closing + 1]


def function_range(source: str, function_name: str) -> tuple[int, int]:
    signature = source.index(function_name + "(")
    opening = source.index("{", signature)
    return opening, matching_brace(source, opening)


def single_view_body(source: str, function_name: str) -> str:
    body = function_body(source, function_name)
    condition = "if (view_mask != 0)"
    condition_index = body.index(condition)
    true_opening = body.index("{", condition_index)
    true_end = matching_brace(body, true_opening)
    else_index = body.index("else", true_end)
    else_opening = body.index("{", else_index)
    return body[else_opening : matching_brace(body, else_opening) + 1]


def verify_contract(source: str) -> None:
    for function_name in FUNCTION_NAMES:
        body = single_view_body(source, function_name)
        reset_count = body.count(RESET_CALL)
        if reset_count != 1:
            raise ContractViolationError(
                f"{function_name} single-view branch has {reset_count} resets"
            )
        if body.index(RESET_CALL) > body.index("terakan_emit_"):
            raise ContractViolationError(
                f"{function_name} emits before restoring view_index"
            )


def remove_one_reset(source: str, function_name: str) -> str:
    opening, closing = function_range(source, function_name)
    body = source[opening : closing + 1]
    reset_index = body.index(RESET_CALL)
    mutant_body = body[:reset_index] + body[reset_index + len(RESET_CALL) :]
    return source[:opening] + mutant_body + source[closing + 1 :]


def main() -> int:
    source = SOURCE_PATH.read_text(encoding="utf-8")
    verify_contract(source)

    for function_name in FUNCTION_NAMES:
        mutant = remove_one_reset(source, function_name)
        try:
            verify_contract(mutant)
        except AssertionError:
            continue
        raise ContractViolationError(
            f"the missing-reset mutant passed for {function_name}"
        )


if __name__ == "__main__":
    raise SystemExit(main())
