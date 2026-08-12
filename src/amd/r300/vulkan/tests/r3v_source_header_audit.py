# SPDX-License-Identifier: MIT
"""Source-header audit for the R3V lane.

A header carries the license grant and the file's own description.  Three
rules fix that content, and each one fails closed.

An SPDX-License-Identifier line states the grant, so every C, header, and
Python file under the audited root carries one within its opening lines.

A copyright line asserts that a named holder holds the copyright, so it
appears only for a holder who does.  The names a template supplies -- the
local git identity, a model's default, an invented project collective --
assert authorship that does not exist, and absent attribution is the correct
state for a file with no such holder.  REAL_COPYRIGHT_HOLDERS is the reviewed
list of holders whose lines this tree preserves verbatim.  The list names the
Mesa3D authorship line used by the route-policy files; a file arriving with
another upstream header joins the list by review.

AI participation rides in commit trailers (Assisted-by, Generated-by), so a
header naming a model or a generation tool states it in the wrong artifact.

Modes:
  <root>...        audit every C, header, and Python file under each root
  --fixture NAME   audit one synthetic file carrying the NAME defect, which
                   calibrates each rule against a header known to violate it
  --selftest       every fixture, plus the clean header each one mutates
"""

import re
import sys
import tempfile
from pathlib import Path

SUFFIXES = (".c", ".h", ".py")

# Lines a header may carry that name a holder.  Each entry is a holder this
# tree preserves verbatim because the project attribution is reviewed for
# files in this lane.
REAL_COPYRIGHT_HOLDERS: tuple[str, ...] = ("Mesa3D authors",)


def normalize_holder(holder: str) -> str:
    """Return the exact holder name after header punctuation and dates."""
    value = " ".join(holder.split())
    value = re.sub(r"^\(c\)\s*", "", value, flags=re.IGNORECASE)
    value = re.sub(
        r"^(?:\d{4}(?:\s*-\s*\d{4})?(?:\s*,\s*\d{4})?\s*)+", "", value)
    return value.casefold()


REAL_COPYRIGHT_HOLDER_KEYS = frozenset(
    normalize_holder(holder) for holder in REAL_COPYRIGHT_HOLDERS)

# The opening lines a header rule reads.  A license grant and a copyright line
# sit at the top of a file; a match further down is prose or test data.
HEADER_LINES = 8

COPYRIGHT = re.compile(r"^[^A-Za-z0-9]*Copyright\b(?P<holder>.*)$",
                       re.IGNORECASE)

# Disclosure that belongs in a commit trailer.  The pattern matches the tool
# naming that reaches a header, not the words themselves: "generated from
# vk.xml by <script>" states a build mechanism and stays.
AI_TOOL = (r"(?:claude|chatgpt|codex|copilot|gemini|mistral|ollama|"
           r"deepseek|gpt(?:[-\w.]*)|coderabbit|an?\s+(?:AI|LLM))")
AI_DISCLOSURE = re.compile(
    r"(?:\b(?:LLM[- ]assisted|AI[- ]assisted)\b"
    rf"|\bgenerated\s+by\s+{AI_TOOL}\b"
    rf"|\b(?:assisted|generated)-by:(?:\s*$|\s+{AI_TOOL}\b|"
    r"\s*\*/\s*$))",
    re.IGNORECASE)

SPDX_IDENTIFIER = re.compile(
    r"^\s*(?:"
    r"(?:#|//)\s*SPDX-License-Identifier:\s*"
    r"(?P<line_value>\S(?:.*\S)?)"
    r"|(?:/\*|\*)\s*SPDX-License-Identifier:\s*"
    r"(?P<block_value>[^*/]*?)(?:\s*\*/)?"
    r")\s*$")


def has_spdx_identifier(line: str) -> bool:
    """Return whether a line carries an SPDX tag with a nonempty value."""
    match = SPDX_IDENTIFIER.search(line)
    if match is None:
        return False
    value = match.group("line_value") or match.group("block_value")
    return bool(value and value.strip())


def audit_file(path: Path):
    """Return the defects in one file's header."""
    try:
        head = path.read_text(errors="replace").splitlines()[:HEADER_LINES]
    except OSError as exc:
        return [f"{path}: unreadable: {exc}"]

    defects = []
    if not any(has_spdx_identifier(line) for line in head):
        defects.append(f"{path}: no SPDX-License-Identifier in the first "
                       f"{HEADER_LINES} lines")
    for line in head:
        match = COPYRIGHT.match(line.strip())
        if match is not None:
            holder = match.group("holder").strip()
            if normalize_holder(holder) not in REAL_COPYRIGHT_HOLDER_KEYS:
                defects.append(f"{path}: copyright line names an unreviewed "
                               f"holder: {line.strip()}")
        if AI_DISCLOSURE.search(line):
            defects.append(f"{path}: AI disclosure belongs in a commit "
                           f"trailer: {line.strip()}")
    return defects


def audit_roots(roots):
    """Audit every source file under each root, newest defect list first."""
    defects = []
    audited = 0
    for root in roots:
        root = Path(root)
        files = ([root] if root.is_file() else
                 sorted(p for p in root.rglob("*") if p.suffix in SUFFIXES))
        for path in files:
            audited += 1
            defects.extend(audit_file(path))
    return audited, defects


# A clean header and isolated mutations of it, each violating one predicate.
# The audit runs over each written file, so a predicate that stopped reading
# its line fails its own calibration.
CLEAN_HEADER = """\
/* SPDX-License-Identifier: MIT */

/* A description of the mechanism this file carries. */
"""


SELF_MUTATED_HEADER = "".join(
    Path(__file__).read_text().splitlines(keepends=True)[1:])


FIXTURES = {
    # The prose mentions SPDX without carrying the tag syntax.
    "missing-spdx": CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* SPDX-License-Identifier is required. */"),
    # The auditor's own docstring repeats the SPDX name, so deleting its first
    # line calibrates the opening-line boundary as well as the tag syntax.
    "missing-spdx-self-mutation": SELF_MUTATED_HEADER,
    "malformed-spdx": CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* SPDX-License-Identifier: */"),
    "spdx-colon-prose": CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* The SPDX-License-Identifier: field is required */"),
    "spdx-trailing-prose": CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* SPDX-License-Identifier: MIT */ trailing prose"),
    "invented-copyright": CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* SPDX-License-Identifier: MIT\n"
        " * Copyright (c) 2026 the example project\n */"),
    "unreviewed-copyright-suffix": CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* SPDX-License-Identifier: MIT\n"
        " * Copyright (c) 2026 Mesa3D authors and Example Corp\n */"),
    "ai-disclosure": CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* SPDX-License-Identifier: MIT\n"
        " * Generated by CodeRabbit\n */"),
    "ai-assisted-by-trailer": CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* SPDX-License-Identifier: MIT\n"
        " * Assisted-by: Claude\n */"),
    "ai-generated-by-trailer": CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* SPDX-License-Identifier: MIT\n"
        " * Generated-by: ChatGPT Codex (5.x)\n */"),
    "ai-empty-trailer": CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* SPDX-License-Identifier: MIT\n"
        " * Generated-by:\n */"),
}

FIXTURE_PREDICATES = {
    "missing-spdx": "no SPDX-License-Identifier",
    "missing-spdx-self-mutation": "no SPDX-License-Identifier",
    "malformed-spdx": "no SPDX-License-Identifier",
    "spdx-colon-prose": "no SPDX-License-Identifier",
    "spdx-trailing-prose": "no SPDX-License-Identifier",
    "invented-copyright": "copyright line names an unreviewed holder",
    "unreviewed-copyright-suffix": "copyright line names an unreviewed holder",
    "ai-disclosure": "AI disclosure belongs in a commit trailer",
    "ai-assisted-by-trailer": "AI disclosure belongs in a commit trailer",
    "ai-generated-by-trailer": "AI disclosure belongs in a commit trailer",
    "ai-empty-trailer": "AI disclosure belongs in a commit trailer",
}


def fixture_defects(contents: str) -> list[str]:
    """Audit fixture contents without exposing the temporary path."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "fixture.c"
        path.write_text(contents)
        return [defect.replace(f"{path}:", "fixture.c:", 1)
                for defect in audit_file(path)]


def run_fixture(name, contents=None):
    """Audit one synthetic header and report its defects."""
    defects = fixture_defects(FIXTURES[name] if contents is None else contents)
    print(f"header fixture {name}: {len(defects)} defects")
    for defect in defects:
        print(f"  {defect}")
    return defects


def selftest():
    clean_defects = run_fixture("clean", CLEAN_HEADER)
    if clean_defects:
        raise AssertionError(("clean", clean_defects))
    reviewed_header = CLEAN_HEADER.replace(
        "/* SPDX-License-Identifier: MIT */",
        "/* SPDX-License-Identifier: MIT\n"
        " * Copyright (c) 2026   Mesa3D authors\n */")
    reviewed_defects = fixture_defects(reviewed_header)
    if reviewed_defects:
        raise AssertionError(("reviewed-copyright", reviewed_defects))
    for name in sorted(FIXTURES):
        defects = run_fixture(name)
        expected = FIXTURE_PREDICATES[name]
        if len(defects) != 1 or expected not in defects[0]:
            raise AssertionError((name, defects))
    print(f"r3v_source_header_audit selftest: {len(FIXTURES)} isolated "
          "defect legs and two clean legs OK")
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["--selftest"]:
        return selftest()
    if argv[:1] == ["--fixture"]:
        if argv[1] == "missing-root":
            # The dropped-subtree calibration supplies enough clean files to
            # clear the aggregate floor.  The absent root therefore remains
            # the sole refusal when the per-root existence check is active.
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp) / "source-root"
                root.mkdir()
                for index in range(20):
                    (root / f"fixture_{index}.c").write_text(CLEAN_HEADER)
                return main([str(root), str(Path(tmp) /
                                             "does-not-exist-root")])
        defects = run_fixture(argv[1],
                              CLEAN_HEADER if argv[1] == "clean" else None)
        return 1 if defects else 0

    # A root that resolves to nothing reports a clean audit it never ran,
    # so every requested root must exist and contribute at least one file
    # before the aggregate verdict counts.
    missing = [root for root in argv
               if not Path(root).exists() or
               (Path(root).is_dir() and
                not any(p.suffix in SUFFIXES for p in Path(root).rglob("*")))]
    if missing:
        for root in missing:
            print(f"model failure: root {root} is absent or contributes "
                  f"no source files")
        return 2

    audited, defects = audit_roots(argv)
    for defect in defects:
        print(defect)
    print(f"source headers: {audited} audited, {len(defects)} defects")
    if audited < 20:
        print(f"model failure: {audited} files audited, too few for the "
              f"R3V lane; the roots did not resolve")
        return 2
    return 1 if defects else 0


if __name__ == "__main__":
    sys.exit(main())
