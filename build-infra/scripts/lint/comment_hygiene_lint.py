#!/usr/bin/env python3
"""Enforce mechanism-first source comments and durable Markdown provenance.

Source comments carry hardware, ABI, specification, ownership, and safety
invariants. Project chronology lives in commit messages and finding records.
Markdown findings may carry chronology, while their load-bearing provenance
uses commit SHAs, bundle directories, finding filenames, source paths, or
branch names.

Usage:
  comment_hygiene_lint.py [--staged]   # lint git-staged changes only
  comment_hygiene_lint.py [paths...]   # lint specific files
  comment_hygiene_lint.py              # lint the whole working tree

Exit codes:
  0  no violations
  1  one or more violations found
  2  invocation error
"""

from __future__ import annotations

import argparse
import ast
import io
import re
import subprocess
import sys
import tokenize
from dataclasses import dataclass
from pathlib import Path

# Pattern definitions.

# Patterns that must NEVER appear in source code comments.
# Each entry: (regex, kind, suggestion)
SOURCE_PATTERNS = [
    (
        re.compile(r"\bsteinmarder task #\d+\b", re.IGNORECASE),
        "task-ref",
        "Move to commit message / PR description.  Reference commit SHA + file:symbol instead.",
    ),
    (
        re.compile(r"\bissue #\d+\b", re.IGNORECASE),
        "issue-ref",
        "Move to commit message / PR description.",
    ),
    (
        re.compile(r"\bmesa PR #\d+\b", re.IGNORECASE),
        "pr-ref",
        "Move to commit message / PR description.",
    ),
    (
        re.compile(r"\bcompanion to PR #\d+\b", re.IGNORECASE),
        "companion-pr",
        "Move to commit message / PR description.",
    ),
    # Catches every shape this project has used: dotted numerics
    # (4.4-style), short alphanumeric suffixes (1A, 1B-alpha,
    # 1B-beta), hyphen-chained sub-task tags (1E-A2-diagnostic,
    # 1E-atomic), and bare-digit forms (0, 1); plus the
    # The numbered deferred-work prefix that systematically
    # slipped past the original regex.  These identifiers rot
    # because the project numbering scheme evolves; describe the
    # durable invariant (what the code does, what file it
    # complements) instead.
    (
        re.compile(
            r"\b(?:TODO-\d+\s+)?Phase\s+\d+(?:[A-Za-z\-_][A-Za-z0-9\-_]*)?\b", re.IGNORECASE
        ),
        "phase-label",
        "Phase labels rot.  Describe the durable invariant (what the code does, "
        "what it complements by file/symbol) instead.  Project chronology belongs "
        "in commit messages and finding-doc bodies, not in source comments.",
    ),
    (
        re.compile(r"\bStep \d+ of Phase\b", re.IGNORECASE),
        "step-of-phase",
        "Move to commit message; the comment should describe the durable invariant.",
    ),
    (
        re.compile(r"\b\(20\d{2}-\d{2}-\d{2}\)"),
        "session-date",
        "Dated parenthetical rots.  Move chronology to commit message.",
    ),
    (
        re.compile(r"\bas of (today|yesterday|now)\b", re.IGNORECASE),
        "deictic-time",
        "Deictic time reference rots when read later.",
    ),
    (
        re.compile(r"\b[CLQI]-20\d{2}-\d{2}-\d{2}-\d+\b"),
        "claim-tag",
        "Date-stamped claim/line-item/question tag rots.  Reference the durable code symbol.",
    ),
    (
        re.compile(r"\bwill be exercised when Phase \d", re.IGNORECASE),
        "cross-phase-breadcrumb",
        "Forward-reference rots; describe what the code does now.",
    ),
    (
        re.compile(r"\bthis chip family\b", re.IGNORECASE),
        "deictic-chip",
        "Use absolute names (Palm/CHIP_PALM/Evergreen) so the comment survives being read from another chip's source file.",
    ),
    (
        re.compile(r"\b(our (driver|GPU)|this GPU|this driver)\b", re.IGNORECASE),
        "deictic-self",
        "Use absolute names (Terakan, Palm, etc.).",
    ),
    # First-person chronology claims rot in source comments.  Distinguish
    # those from technical-state usage about a value already bound or emitted.
    (
        re.compile(
            r"\b(?:(?:we|the (?:driver|code|implementation))\s+(?:previously|currently)\b|"
            r"previously\s+we\b|currently\s+we\b|"
            r"the\s+current\s+(?:implementation|approach|path|behavior|behaviour|way|design)\b|"
            r"the\s+(?:new|old)\s+way\b|"
            r"as\s+of\s+(?:today|yesterday|now)\b)",
            re.IGNORECASE,
        ),
        "deictic-time-2",
        "First-person past/present chronology rots; rewrite as absolute invariant.",
    ),
    (
        re.compile(r"Copyright .* (Eirikr|<operator>|Hilgart|Hinngart)\b"),
        "personal-copyright",
        "Personal-name copyright doesn't belong in source files; use SPDX-License-Identifier: MIT only, or drop the header (LICENSE covers).",
    ),
    (
        re.compile(
            r"^\s*(?:(?:/\*|//|#)\s*[-=*_]{8,}\s*$|"
            r"\*\s*[-=*_]{8,}\s*(?:\*/)?\s*$|"
            r"(?:/\*|//|#)\s*[-=*_]{3,}\s+\S.*\s+[-=*_]{3,}\s*(?:\*/)?\s*$)"
        ),
        "decorative-comment-border",
        "Do not frame source comments with delimiter lines or banner boxes.  Keep the comment body and remove the punctuation border.",
    ),
]

# Project-specific label/provenance patterns.  OFF by default (opt-in via
# --project-labels).  AGENTS.md forbids these in source comments, but they are
# narrower than the generic rules above and would false-positive on legitimate
# RE prose if always-on, so the strict pre-commit gate stays on the base set
# and callers opt in for an AGENTS.md-grounded source-comment sweep.
PROJECT_LABEL_PATTERNS = [
    (
        re.compile(r"\bM-[A-Z](?:\.\d+)?\b|\bEntry \d+\b"),
        "mission-label",
        "Mission/milestone labels (M-E, M-G.4, Entry 6) rot.  Name the durable "
        "mechanism (identity-map, blend-acc reduction); move chronology to the "
        "commit message or finding-doc.",
    ),
    (
        re.compile(r"\b\d{8}T\d{6}Z\b"),
        "bundle-timestamp",
        "Capture-bundle timestamps are private artifacts.  Cite the mechanism in "
        "source; keep the bundle path in the commit message or finding-doc.",
    ),
    (
        re.compile(r"\bsrc/re/r\d+/"),
        "internal-finding-path",
        "Internal-repo finding/evidence paths are not durable source citations.  "
        "Cite the spec/register/symbol by name; keep the finding path in the "
        "commit message or finding-doc.",
    ),
    (
        re.compile(r"\bmesa #\d+\b", re.IGNORECASE),
        "fork-pr-ref",
        "Fork PR numbers rot in source.  Reference the commit SHA + symbol; move "
        "the PR link to the commit message.",
    ),
]

# Opt-in toggle set by main(); keeps the strict pre-commit gate on the base
# SOURCE_PATTERNS so adding these patterns cannot break unrelated commits.
PROJECT_LABELS_ENABLED = False

# Patterns for markdown finding-docs.  More permissive than source:
# chronology is OK in bodies; what we catch are LLM-readability
# anti-patterns and PR# breadcrumbs.
#
# The full ruleset is documented in
# ~/.claude/projects/.../memory/reference_llm_readable_markdown_standards.md.
MARKDOWN_PATTERNS = [
    (
        re.compile(r"\bPR\s*#\d+\b", re.IGNORECASE),
        "pr-number",
        "Do not use PR numbers as checked-in prose provenance.  Use a commit SHA, "
        "bundle directory, finding filename, source path, or branch name instead.",
    ),
    # Heading depth > H3.  LLMs lose hierarchy past three levels.
    (
        re.compile(r"^#{4,}\s"),
        "heading-too-deep",
        "Heading depth MUST NOT exceed H3.  Use bold or tables for deeper structure.",
    ),
    # Hedge words in rules / reference docs.
    (
        re.compile(
            r"\b(?:might want to consider|we might|perhaps|"
            r"it could be argued|maybe (?:we|should|use)|"
            r"it's worth considering|might be worth)\b",
            re.IGNORECASE,
        ),
        "hedge-word",
        "Imperative voice required: MUST / MUST NOT / SHOULD.  No hedging.",
    ),
    # Position-relative cross-references.
    (
        re.compile(
            r"\b(?:see (?:section\s+)?(?:above|below)|"
            r"as (?:noted|mentioned) (?:above|below)|"
            r"(?:section|paragraph) (?:above|below))\b",
            re.IGNORECASE,
        ),
        "position-relative-ref",
        "Position-relative reference rots when slice-loaded.  Use an explicit "
        "link [label](path/file.md) or name the section by title.",
    ),
    # Decorative banner dividers (===== / ----- runs >= 8 chars, not the
    # markdown 1-3 char setext underline pattern).
    (
        re.compile(r"^(?:={8,}|-{8,}|\*\s\*\s\*|={3,}\s*\*\s*={3,})\s*$"),
        "decorative-divider",
        "Decorative dividers cost tokens with zero semantic value.  Remove.",
    ),
    # Decorative arrows.
    (
        re.compile(r"(==>|-->|<==|<--)"),
        "decorative-arrow",
        "ASCII arrows are decorative; use words ('promotes to', 'derives from').",
    ),
    # Multiple H1s (cheap detection: line beginning with single '#').
    # The actual multi-H1 check happens in lint_markdown_structural.
]


def lint_markdown_structural(path: Path, text: str) -> list[Violation]:
    """Whole-file structural checks for markdown.

    Catches multi-H1 and missing-frontmatter conditions that can't be
    expressed as a per-line regex.  Frontmatter is required only on
    files inside known programmatic-load directories (memory entries
    and finding docs).
    """
    violations: list[Violation] = []
    lines = text.splitlines()

    h1_count = 0
    h1_first_line = None
    in_fenced_block = False
    for line_no, line in enumerate(lines, start=1):
        if line.lstrip().startswith("```"):
            in_fenced_block = not in_fenced_block
            continue
        if in_fenced_block:
            continue
        if line.startswith("# ") and not line.startswith("# !"):
            h1_count += 1
            if h1_count == 1:
                h1_first_line = line_no
            elif h1_count == 2:
                violations.append(
                    Violation(
                        path=path,
                        line_no=line_no,
                        line_text=line.rstrip(),
                        kind="multi-h1",
                        suggestion=(
                            "Exactly one H1 per file (line {} also).  "
                            "Demote subsequent H1s to H2.".format(h1_first_line)
                        ),
                    )
                )

    path_str = str(path)
    needs_frontmatter = (
        "/memory/" in path_str
        or "/findings/active/" in path_str
        or "/findings/concluded/" in path_str
    )
    if needs_frontmatter:
        has_frontmatter = (
            len(lines) >= 1
            and lines[0].strip() == "---"
            and any(ln.strip() == "---" for ln in lines[1:50])
        )
        if not has_frontmatter:
            violations.append(
                Violation(
                    path=path,
                    line_no=1,
                    line_text=lines[0][:120] if lines else "",
                    kind="missing-frontmatter",
                    suggestion=(
                        "Files under memory/ or findings/{active,concluded}/ "
                        "MUST start with a YAML frontmatter block (--- name: "
                        "... description: ... metadata.type: ... ---)."
                    ),
                )
            )

    return violations


# Helpers.


@dataclass
class Violation:
    path: Path
    line_no: int
    line_text: str
    kind: str
    suggestion: str


def is_source_file(path: Path) -> bool:
    suffixes = {
        ".c",
        ".h",
        ".cpp",
        ".cc",
        ".cxx",
        ".hpp",
        ".py",
        ".rs",
        ".sh",
        ".bash",
        ".zsh",
        ".go",
        ".rb",
        ".pl",
        ".lua",
    }
    return path.suffix in suffixes


def is_markdown_file(path: Path) -> bool:
    return path.suffix == ".md"


def is_retained_evidence_source(path: Path) -> bool:
    """Source files copied into a retained-evidence bundle are preserved
    verbatim, not live source.  A directory rename moves them and would
    otherwise drag their pre-existing capture-time comments through the
    strict source gate.  Evidence under results/ or staged_sync/ is held to
    bundle-integrity rules (manifests, hashes), not live-source comment
    hygiene, so exempt source files living inside those trees."""
    path_text = path.as_posix().lstrip("./")
    parts = set(path.parts)
    retained = (
        "results" in parts
        or "staged_sync" in parts
        or path_text.startswith("docs/external_sources/")
        or path_text.startswith("src/re/r600/external/")
    )
    return retained and is_source_file(path)


def is_generated_artifact(text: str) -> bool:
    """Generated files are checked by their generator, not by prose lint."""
    header = "\n".join(text.splitlines()[:5])
    return "GENERATED BY " in header and "DO NOT EDIT" in header


def in_comment_context(line: str, suffix: str) -> bool:
    """Cheap heuristic: is this line plausibly inside a comment?

    Real comment parsing would need a proper lexer per language.  For
    the noise-reduction lint, we just look for the comment lead-ins.
    Markdown files have no comment context (the whole file is text).
    """
    line_stripped = line.strip()
    if suffix in {".c", ".h", ".cpp", ".cc", ".cxx", ".hpp"}:
        # C-style: //, /*, *, line containing /*...*/
        return (
            line_stripped.startswith("//")
            or line_stripped.startswith("/*")
            or line_stripped.startswith("*")
            or "//" in line
            or "/*" in line
        )
    if suffix in {".sh", ".bash", ".zsh", ".rs", ".rb", ".pl"}:
        return line_stripped.startswith("#") or "//" in line
    if suffix == ".lua":
        return line_stripped.startswith("--")
    return False


def python_comment_segments(text: str) -> tuple[list[tuple[int, str]], tuple[int, str] | None]:
    """Return exact Python hash-comment and docstring source segments."""

    try:
        tree = ast.parse(text)
        tokens = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except (IndentationError, SyntaxError, ValueError, tokenize.TokenError) as exc:
        return [], (getattr(exc, "lineno", None) or 1, str(exc))

    segments = [
        (token.start[0], token.string) for token in tokens if token.type == tokenize.COMMENT
    ]
    docstring_owners = (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)
    for owner in ast.walk(tree):
        if not isinstance(owner, docstring_owners) or not owner.body:
            continue
        statement = owner.body[0]
        if not (
            isinstance(statement, ast.Expr)
            and isinstance(statement.value, ast.Constant)
            and isinstance(statement.value.value, str)
        ):
            continue
        source = ast.get_source_segment(text, statement.value)
        if source is None:
            return [], (statement.value.lineno, "AST docstring lacks a source segment")
        segments.extend(
            (statement.value.lineno + offset, line)
            for offset, line in enumerate(source.splitlines())
        )
    return sorted(segments), None


def preserved_license_header_lines(text: str) -> set[int]:
    """Line numbers of a file's leading upstream license header.

    A header carrying someone else's copyright is preserved verbatim
    across moves and refactors, so its framing is that author's and not
    a style choice this tree makes.  The block is the file's opening
    comment, and it qualifies only when it names a copyright holder; a
    plain SPDX line frames nothing and needs no exemption.  Only the
    decorative-border rule reads this: a header that carries chronology
    or a personal-name copyright is still reported.
    """
    lines = text.splitlines()
    index = 0
    while index < len(lines) and not lines[index].strip():
        index += 1
    if index >= len(lines) or not lines[index].lstrip().startswith("/*"):
        return set()
    header: set[int] = set()
    carries_copyright = False
    for offset in range(index, len(lines)):
        header.add(offset + 1)
        if "Copyright" in lines[offset]:
            carries_copyright = True
        if "*/" in lines[offset]:
            return header if carries_copyright else set()
    return set()


def lint_source_text(path: Path, text: str, patterns) -> list[Violation]:
    if path.suffix == ".py":
        segments, parse_error = python_comment_segments(text)
        if parse_error is not None:
            line_no, detail = parse_error
            return [
                Violation(
                    path=path,
                    line_no=line_no,
                    line_text=detail,
                    kind="python-comment-parse",
                    suggestion="Python comment classification requires valid syntax.",
                )
            ]
    else:
        segments = [
            (line_no, line)
            for line_no, line in enumerate(text.splitlines(), start=1)
            if in_comment_context(line, path.suffix)
        ]

    license_header = preserved_license_header_lines(text)

    violations: list[Violation] = []
    for line_no, line in segments:
        for pattern, kind, suggestion in patterns:
            if pattern.search(line):
                if (kind == "decorative-comment-border"
                        and line_no in license_header):
                    continue
                violations.append(
                    Violation(
                        path=path,
                        line_no=line_no,
                        line_text=line.rstrip(),
                        kind=kind,
                        suggestion=suggestion,
                    )
                )
                break
    return violations


def lint_file(path: Path) -> list[Violation]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except (OSError, UnicodeDecodeError):
        return []
    if is_generated_artifact(text):
        return []
    if is_retained_evidence_source(path):
        return []
    violations: list[Violation] = []
    is_md = is_markdown_file(path)
    is_src = is_source_file(path)
    if is_md:
        patterns = MARKDOWN_PATTERNS
    elif is_src:
        patterns = SOURCE_PATTERNS + (PROJECT_LABEL_PATTERNS if PROJECT_LABELS_ENABLED else [])
    else:
        patterns = []
    if not patterns:
        return []
    if is_src:
        return lint_source_text(path, text, patterns)
    in_fenced_block = False
    in_html_comment = False
    for line_no, line in enumerate(text.splitlines(), start=1):
        # Markdown: an HTML comment (the SPDX header block) opens with
        # <!-- and closes with -->; the closing token is comment syntax,
        # not a decorative arrow, so the block's lines carry no rule.
        if is_md:
            stripped = line.strip()
            if in_html_comment:
                if "-->" in stripped:
                    in_html_comment = False
                continue
            if stripped.startswith("<!--"):
                if "-->" not in stripped[4:]:
                    in_html_comment = True
                continue
        # Markdown: track fenced code blocks so we don't flag fixture
        # content (example snippets, sample outputs).  Only opening fences
        # need a language tag; closing fences are plain ``` by markdown syntax.
        if is_md and line.lstrip().startswith("```"):
            if not in_fenced_block and line.strip() == "```":
                violations.append(
                    Violation(
                        path=path,
                        line_no=line_no,
                        line_text=line.rstrip(),
                        kind="bare-code-fence",
                        suggestion=(
                            "Code fences MUST include a language tag (```bash, "
                            "```c, ```python, ```json, ```text, etc.).  Bare "
                            "``` blocks lose parser hinting."
                        ),
                    )
                )
            in_fenced_block = not in_fenced_block
            continue
        if is_md and in_fenced_block:
            continue
        for pattern, kind, suggestion in patterns:
            m = pattern.search(line)
            if m:
                violations.append(
                    Violation(
                        path=path,
                        line_no=line_no,
                        line_text=line.rstrip(),
                        kind=kind,
                        suggestion=suggestion,
                    )
                )
                break
    if is_md:
        violations.extend(lint_markdown_structural(path, text))
    return violations


def run_self_test() -> int:
    failures: list[str] = []
    bad_cases = {
        "module": '"""Contract.\nPhase 4 configures our driver.\n"""\n',
        "class": (
            'class Example:\n    """Contract.\n' '    Phase 4 configures our driver.\n    """\n'
        ),
        "function": (
            'def example():\n    """Contract.\n' '    Phase 4 configures our driver.\n    """\n'
        ),
    }
    for name, text in bad_cases.items():
        violations = lint_source_text(Path(f"{name}.py"), text, SOURCE_PATTERNS)
        if not any(violation.kind == "phase-label" for violation in violations):
            failures.append(f"interior {name} docstring escaped detection")

    assigned_data = 'payload = """Contract.\nPhase 4 configures our driver.\n"""\n'
    if lint_source_text(Path("assigned.py"), assigned_data, SOURCE_PATTERNS):
        failures.append("assigned triple-quoted data was classified as a docstring")

    hash_comment = 'payload = "Phase 4 data"  # Phase 4 configures our driver\n'
    hash_violations = lint_source_text(Path("hash.py"), hash_comment, SOURCE_PATTERNS)
    if not any(violation.kind == "phase-label" for violation in hash_violations):
        failures.append("ordinary Python hash comment escaped detection")

    # The border rule yields to a preserved upstream license header and
    # nowhere else, so each arm moves exactly one fact.
    upstream_header = (
        "/*****************************************\n"
        " * Copyright 2011 Advanced Micro Devices, Inc.\n"
        " * SPDX-License-Identifier: MIT\n"
        " *****************************************/\n"
        "\nvoid f(void) {}\n"
    )
    if lint_source_text(Path("upstream.c"), upstream_header, SOURCE_PATTERNS):
        failures.append("preserved upstream license header was reported")

    unowned_header = upstream_header.replace(
        " * Copyright 2011 Advanced Micro Devices, Inc.\n", "")
    if not any(violation.kind == "decorative-comment-border"
               for violation in lint_source_text(
                   Path("unowned.c"), unowned_header, SOURCE_PATTERNS)):
        failures.append("a leading border naming no copyright holder escaped")

    body_border = (
        "/*\n * Copyright 2011 Advanced Micro Devices, Inc.\n */\n"
        "\n/*\n * ==========================================\n"
        " * Section\n */\nvoid f(void) {}\n"
    )
    if not any(violation.kind == "decorative-comment-border"
               for violation in lint_source_text(
                   Path("body.c"), body_border, SOURCE_PATTERNS)):
        failures.append("a border below the license header escaped")

    # The exemption covers the border rule on that line and leaves every
    # later rule reading it.  The project-label rules run after the border
    # rule, so a preserved header whose own border line also carries a
    # capture-bundle timestamp still reports the timestamp.
    labeled_border = upstream_header.replace(
        "/*****************************************\n",
        "/***** 20260801T120000Z *****\n")
    if not any(violation.kind == "bundle-timestamp"
               for violation in lint_source_text(
                   Path("labeled.c"), labeled_border,
                   SOURCE_PATTERNS + PROJECT_LABEL_PATTERNS)):
        failures.append("a timestamp on an exempt border line escaped")

    dated_header = upstream_header.replace(
        " * SPDX-License-Identifier: MIT\n",
        " * SPDX-License-Identifier: MIT\n * Phase 4 configures our driver.\n")
    if not any(violation.kind == "phase-label"
               for violation in lint_source_text(
                   Path("dated.c"), dated_header, SOURCE_PATTERNS)):
        failures.append("the header exemption reached a rule other than the border")

    invalid = lint_source_text(Path("invalid.py"), 'payload = """unterminated\n', SOURCE_PATTERNS)
    if not any(violation.kind == "python-comment-parse" for violation in invalid):
        failures.append("invalid Python syntax did not fail closed")

    if failures:
        print("comment-hygiene self-test: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("comment-hygiene self-test: PASS")
    return 0


def git_staged_paths() -> list[Path]:
    out = subprocess.check_output(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
        text=True,
    )
    return [Path(p) for p in out.splitlines() if p.strip()]


def git_tracked_paths() -> list[Path]:
    out = subprocess.check_output(["git", "ls-files"], text=True)
    return [Path(p) for p in out.splitlines() if p.strip()]


def walk_paths(roots: list[Path]) -> list[Path]:
    files: list[Path] = []
    for r in roots:
        if r.is_file():
            files.append(r)
        elif r.is_dir():
            for sub in r.rglob("*"):
                if sub.is_file() and (is_source_file(sub) or is_markdown_file(sub)):
                    files.append(sub)
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--staged", action="store_true", help="lint only git-staged files")
    parser.add_argument("--strict", action="store_true", help="exit 1 on any violation")
    parser.add_argument("--self-test", action="store_true", help="calibrate Python comment parsing")
    parser.add_argument(
        "--project-labels",
        action="store_true",
        help="also flag project mission labels (M-E/Entry N), capture-bundle "
        "timestamps, internal src/re finding paths, and 'mesa #N' fork PR refs "
        "in source comments (opt-in; the strict pre-commit gate leaves this off)",
    )
    parser.add_argument("paths", nargs="*", type=Path, help="files or directories to lint")
    args = parser.parse_args()
    global PROJECT_LABELS_ENABLED
    PROJECT_LABELS_ENABLED = args.project_labels

    if args.self_test:
        return run_self_test()

    if args.staged and args.paths:
        print("error: --staged and explicit paths are mutually exclusive", file=sys.stderr)
        return 2

    if args.staged:
        paths = git_staged_paths()
    elif args.paths:
        paths = walk_paths(args.paths)
    else:
        paths = git_tracked_paths()

    all_violations: list[Violation] = []
    for p in paths:
        if not p.exists():
            continue
        all_violations.extend(lint_file(p))

    if not all_violations:
        print("comment-hygiene: clean (no violations across {} files)".format(len(paths)))
        return 0

    by_kind: dict[str, int] = {}
    for v in all_violations:
        by_kind[v.kind] = by_kind.get(v.kind, 0) + 1

    print(
        "comment-hygiene: {} violations across {} files".format(
            len(all_violations), len({v.path for v in all_violations})
        )
    )
    print()
    print("Rule: AGENTS.md 'Comments, prose, and safety' and 'Comments, commits, and Markdown'")
    print("      source comments must not carry project-management chronology,")
    print("      deictic references, personal-copyright names, or decorative borders.")
    print()
    print("Violations by kind:")
    for kind, n in sorted(by_kind.items(), key=lambda kv: -kv[1]):
        print("  {:24s}  {}".format(kind, n))
    print()
    print("Details (first 50):")
    for v in all_violations[:50]:
        print("  {}:{}  [{}]".format(v.path, v.line_no, v.kind))
        print("    {}".format(v.line_text[:120]))
        print("    suggest: {}".format(v.suggestion))
    if len(all_violations) > 50:
        print("  ... ({} more)".format(len(all_violations) - 50))

    return 1 if (args.strict or args.staged) else 0


if __name__ == "__main__":
    sys.exit(main())
