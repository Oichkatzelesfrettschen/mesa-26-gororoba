# SPDX-License-Identifier: MIT
"""Validate the view-index state contract in the draw paths.

The multiview lowering reads the driver view-index push constant as
gl_ViewIndex.  Every single-view draw path therefore writes zero after a
multiview path can leave a nonzero index.  Each multiview draw path writes the
active index, while the setter dirties the push constant only when that value
changes.  The self-test removes each required update and mutates the setter
guard to require the contract check to reject every known-bad form.
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
UPDATE_CALL = "terakan_set_view_index_push_constant(command_writer, view_idx);"
UNCHANGED_GUARD = "if (*view_index_constant == view_index)"
VIEW_INDEX_ASSIGNMENT = "*view_index_constant = view_index;"
VIEW_INDEX_DIRTY_BIT = "BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_VIEW_INDEX)"


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


def multiview_body(source: str, function_name: str) -> str:
    body = function_body(source, function_name)
    condition_index = body.index("if (view_mask != 0)")
    opening = body.index("{", condition_index)
    return body[opening : matching_brace(body, opening) + 1]


def verify_contract(source: str) -> None:
    setter = function_body(source, "terakan_set_view_index_push_constant")
    try:
        guard_index = setter.index(UNCHANGED_GUARD)
        assignment_index = setter.index(VIEW_INDEX_ASSIGNMENT)
        dirty_index = setter.index(VIEW_INDEX_DIRTY_BIT)
    except ValueError as error:
        raise ContractViolationError(
            "view-index setter lacks value-change dirtying"
        ) from error

    guard_opening = setter.index("{", guard_index)
    guard_body = setter[guard_opening : matching_brace(setter, guard_opening) + 1]
    if guard_body.count("return;") != 1:
        raise ContractViolationError(
            "unchanged view index does not return before dirtying"
        )
    if not guard_index < assignment_index < dirty_index:
        raise ContractViolationError(
            "view index is not compared before assignment and dirtying"
        )

    for function_name in FUNCTION_NAMES:
        single_view = single_view_body(source, function_name)
        reset_count = single_view.count(RESET_CALL)
        if reset_count != 1:
            raise ContractViolationError(
                f"{function_name} single-view branch has {reset_count} resets"
            )
        if single_view.index(RESET_CALL) > single_view.index("terakan_emit_"):
            raise ContractViolationError(
                f"{function_name} emits before restoring view_index"
            )

        multiview = multiview_body(source, function_name)
        update_count = multiview.count(UPDATE_CALL)
        if update_count != 1:
            raise ContractViolationError(
                f"{function_name} multiview branch has {update_count} updates"
            )
        if multiview.index(UPDATE_CALL) > multiview.index("terakan_emit_"):
            raise ContractViolationError(
                f"{function_name} emits before updating view_index"
            )


def remove_one_call(source: str, function_name: str, call: str) -> str:
    opening, closing = function_range(source, function_name)
    body = source[opening : closing + 1]
    call_index = body.index(call)
    mutant_body = body[:call_index] + body[call_index + len(call) :]
    return source[:opening] + mutant_body + source[closing + 1 :]


def main() -> int:
    source = SOURCE_PATH.read_text(encoding="utf-8")
    verify_contract(source)

    for function_name in FUNCTION_NAMES:
        for call in (RESET_CALL, UPDATE_CALL):
            mutant = remove_one_call(source, function_name, call)
            try:
                verify_contract(mutant)
            except AssertionError:
                continue
            raise ContractViolationError(
                f"the missing-call mutant passed for {function_name}: {call}"
            )

    for old, new in (
        (UNCHANGED_GUARD, "if (*view_index_constant != view_index)"),
        (UNCHANGED_GUARD, "if (false)"),
    ):
        assert old in source
        mutant = source.replace(old, new, 1)
        try:
            verify_contract(mutant)
        except AssertionError:
            continue
        raise ContractViolationError(f"the setter-guard mutant passed: {new}")


if __name__ == "__main__":
    raise SystemExit(main())
