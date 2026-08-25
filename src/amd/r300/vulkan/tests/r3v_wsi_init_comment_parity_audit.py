# SPDX-License-Identifier: MIT
"""Hold the three WSI-route comments to the control flow they describe.

`docs/hardware/r3v-wsi-denominator.md`, "Default present route", records
that `r3v_init_wsi` routes through the render-node fd (the DRM/DRI3
path) by default and falls back to the Mesa common WSI software
(xcb-shm CPU) route only when `R3V_WSI_SW` begins with `'1'`.  Three
comment sites--the block above `r3v_init_wsi` itself, the `wsi_device`
field declaration, and the `KHR_surface` extension-table entry--once
stated the opposite: software mode by default, unqualified by the
environment variable that actually selects it.  This check pins that
correction: a comment naming "software mode" or "the lavapipe pattern"
at any of the three sites, with no `R3V_WSI_SW` in that same comment
block, is the same mismatch restated, and the check fails.  The guard
is read from the flagged comment itself, not the file at large, so a
qualified comment elsewhere in the file (an unrelated `getenv` call, a
different comment) does not launder an unqualified one back to a pass;
phrase and guard matching both run on the comment text with its `/*
*/` delimiters, `*` continuation leaders, and line breaks collapsed to
single spaces, so a phrase split across a wrapped comment line still
reads as one phrase.

Usage:
  r3v_wsi_init_comment_parity_audit.py --physical-device C --header H \
      --instance C
  r3v_wsi_init_comment_parity_audit.py --selftest
Exit 0 when every flagged phrase at the three sites sits in a comment
that also names R3V_WSI_SW.
"""

import argparse
import re
import sys
from pathlib import Path

FLAGGED_PHRASES = ("software mode", "lavapipe pattern")
GUARD_STRING = "R3V_WSI_SW"

# Each site names the anchor its preceding comment block must be read
# against, and a human label for the failure message.  The anchor is
# the exact declaration or definition text, not a bare identifier: a
# bare "wsi_device" also matches the field's own name inside the
# corrected inline comment ("vk.wsi_device points here"), which sits
# ahead of the field in the same comment block and would make the
# search land inside the comment it is meant to bound.
SITES = {
    "physical-device": (
        "r3v_init_wsi(struct r3v_physical_device *device)",
        "the r3v_init_wsi definition"),
    "header": (
        "struct wsi_device wsi_device;",
        "the wsi_device field declaration"),
    "instance": (".KHR_surface = true,", "the KHR_surface extension entry"),
}


class AuditFailure(Exception):
    """A flagged phrase sits at a named site without the R3V_WSI_SW guard."""


def preceding_comment(text, anchor):
    """The nearest C comment block that ends before the first occurrence
    of anchor, or the empty string when the anchor or no such comment
    exists."""
    index = text.find(anchor)
    if index < 0:
        return None
    comment_end = text.rfind("*/", 0, index)
    line_comment_end = text.rfind("\n", 0, index)
    if comment_end < 0:
        return ""
    # A block comment reads as adjacent when nothing but whitespace and
    # at most one intervening declaration line separates its close from
    # the anchor; a distant comment belongs to unrelated code above it.
    between = text[comment_end + 2:index]
    if between.count("\n") > 3:
        return ""
    comment_start = text.rfind("/*", 0, comment_end)
    if comment_start < 0:
        return ""
    return text[comment_start:comment_end + 2]


def normalize_comment(comment):
    """Collapse a C comment to one whitespace-normalized line: strip the
    `/* */` delimiters, drop each line's leading `*` continuation
    leader, and join on single spaces.  A phrase or the guard string
    wrapped across a line break in the source reads as contiguous
    text, exactly as a reader parses the comment's prose rather than
    its line breaks."""
    body = comment
    if body.startswith("/*"):
        body = body[2:]
    if body.endswith("*/"):
        body = body[:-2]
    words = []
    for line in body.splitlines():
        stripped = line.strip()
        if stripped.startswith("*"):
            stripped = stripped[1:].strip()
        if stripped:
            words.append(stripped)
    return re.sub(r"\s+", " ", " ".join(words)).strip()


def site_violations(text, filename_label):
    violations = []
    for site, (anchor, description) in SITES.items():
        comment = preceding_comment(text, anchor)
        if not comment:
            continue
        normalized = normalize_comment(comment)
        lowered = normalized.lower()
        hit = next((p for p in FLAGGED_PHRASES if p in lowered), None)
        if hit is None:
            continue
        if GUARD_STRING not in normalized:
            violations.append(
                f"{filename_label}: the comment above {description} "
                f"names {hit!r} and that same comment names no "
                f"{GUARD_STRING}, so the comment restates the "
                f"software-mode-by-default mismatch this check pins "
                f"closed")
    return violations


def audit(sources):
    """sources is a sequence of (filename_label, text) pairs."""
    violations = []
    for filename_label, text in sources:
        violations.extend(site_violations(text, filename_label))
    if violations:
        raise AuditFailure("; ".join(violations))
    return True


def selftest():
    checks = []

    def check(name, ok):
        checks.append((name, ok))

    corrected_physical_device = (
        "/* sw_device follows R3V_WSI_SW.  A value beginning with '1' "
        "sets sw_device true, the Mesa common WSI software mode (the "
        "lavapipe pattern); every other value takes the DRM/DRI3 route. "
        "*/\n"
        "static VkResult\n"
        "r3v_init_wsi(struct r3v_physical_device *device)\n"
        "{\n"
        "   const char *wsi_sw_env = getenv(\"R3V_WSI_SW\");\n"
        "}\n")
    corrected_header = (
        "   /* GPU-resident present by default, the DRM/DRI3 path; "
        "R3V_WSI_SW=1 switches it to the Mesa common WSI software mode "
        "(the lavapipe pattern). vk.wsi_device points here after "
        "r3v_init_wsi. */\n"
        "   struct wsi_device wsi_device;\n")
    corrected_instance = (
        "  /* The VK_KHR_surface family backs presentation through Mesa's "
        "common WSI; r3v_init_wsi routes the DRM/DRI3 path by default, "
        "R3V_WSI_SW=1 switches it to software mode. */\n"
        "  .KHR_surface = true,\n")

    check("passes the corrected three-site tree",
          audit([("r3v_physical_device.c", corrected_physical_device),
                 ("r3v_physical_device.h", corrected_header),
                 ("r3v_instance.c", corrected_instance)]))

    def refuses(sources, fragment):
        try:
            audit(sources)
        except AuditFailure as exc:
            return fragment in str(exc)
        return False

    reintroduced_physical_device = (
        "/* Mesa common WSI in software mode, the lavapipe pattern: "
        "sw_device makes the swapchain allocate CPU-reachable images and "
        "present through the xcb-shm path. */\n"
        "static VkResult\n"
        "r3v_init_wsi(struct r3v_physical_device *device)\n"
        "{\n"
        "}\n")
    check("refuses the reintroduced physical-device comment",
          refuses([("r3v_physical_device.c",
                    reintroduced_physical_device)],
                  "r3v_init_wsi definition"))

    reintroduced_header = (
        "   /* Mesa common WSI, software mode (the lavapipe pattern): "
        "sw_device makes the swapchain allocate CPU-reachable images. "
        "*/\n"
        "   struct wsi_device wsi_device;\n")
    check("refuses the reintroduced header comment",
          refuses([("r3v_physical_device.h", reintroduced_header)],
                  "wsi_device field declaration"))

    reintroduced_instance = (
        "  /* backs presentation through Mesa's common WSI in software "
        "mode ... X11 surfaces only; presentation runs the xcb-shm CPU "
        "path, no DRM modifiers or dma-buf involved. */\n"
        "  .KHR_surface = true,\n")
    check("refuses the reintroduced instance comment",
          refuses([("r3v_instance.c", reintroduced_instance)],
                  "KHR_surface extension entry"))

    guarded_same_comment = (
        "/* Mesa common WSI in software mode, the lavapipe pattern, "
        "selected by R3V_WSI_SW: sw_device makes the swapchain "
        "allocate CPU-reachable images and present through the "
        "xcb-shm path. */\n"
        "static VkResult\n"
        "r3v_init_wsi(struct r3v_physical_device *device)\n"
        "{\n"
        "}\n")
    check("passes a comment naming the phrase with the guard in the "
          "same comment",
          audit([("r3v_physical_device.c", guarded_same_comment)]))

    check("refuses the guard sitting in an unrelated comment elsewhere "
          "in the file rather than the flagged comment itself",
          refuses([("r3v_physical_device.c",
                    reintroduced_physical_device
                    + "\n/* R3V_WSI_SW selects the fallback. */\n")],
                  "r3v_init_wsi definition"))

    # Known-bad mutation A: a guard occurring anywhere else in the
    # file--a getenv("R3V_WSI_SW") call inside r3v_init_wsi's own
    # body, an unrelated comment--does not qualify a flagged comment;
    # only R3V_WSI_SW inside the flagged comment itself qualifies it.
    mutation_a_file_scoped_guard_elsewhere = (
        "/* Mesa common WSI in software mode, the lavapipe pattern: "
        "sw_device makes the swapchain allocate CPU-reachable images "
        "and present through the xcb-shm path. */\n"
        "static VkResult\n"
        "r3v_init_wsi(struct r3v_physical_device *device)\n"
        "{\n"
        "   const char *wsi_sw_env = getenv(\"R3V_WSI_SW\");\n"
        "}\n")
    check("refuses mutation A: R3V_WSI_SW present only in the function "
          "body, not the flagged comment",
          refuses([("r3v_physical_device.c",
                    mutation_a_file_scoped_guard_elsewhere)],
                  "r3v_init_wsi definition"))

    # Known-bad mutation B: wrapping a flagged phrase across a comment
    # continuation line ("software" / "* mode") still reads as the
    # phrase once normalize_comment joins the lines; the comment
    # carries no R3V_WSI_SW at all, so the wrap is the only thing
    # standing between this fixture and a correct refusal.
    mutation_b_phrase_wrapped_across_lines = (
        "  /* backs presentation through Mesa's common WSI in software\n"
        "   * mode ... X11 surfaces only; presentation runs through the\n"
        "   * xcb-shm CPU path, no DRM modifiers or dma-buf involved, the\n"
        "   * lavapipe\n"
        "   * pattern this driver borrows the copy step from. */\n"
        "  .KHR_surface = true,\n")
    check("refuses mutation B: both flagged phrases wrapped across a "
          "comment continuation line",
          refuses([("r3v_instance.c",
                    mutation_b_phrase_wrapped_across_lines)],
                  "KHR_surface extension entry"))

    check("passes a file with neither anchor present",
          audit([("unrelated.c", "/* software mode, lavapipe pattern */\n"
                                  "static void unrelated(void) {}\n")]))

    check("passes a distant unrelated comment above the anchor",
          audit([("r3v_physical_device.c",
                  "/* software mode, lavapipe pattern, about something "
                  "else entirely */\n"
                  "\n\n\n\n\n"
                  "static VkResult\n"
                  "r3v_init_wsi(struct r3v_physical_device *device)\n"
                  "{\n}\n")]))

    check("collects violations across multiple files",
          refuses([("r3v_physical_device.c", reintroduced_physical_device),
                   ("r3v_physical_device.h", reintroduced_header)],
                  "wsi_device field declaration"))

    failed = [n for n, ok in checks if not ok]
    for name, ok in checks:
        print(f"  {'ok  ' if ok else 'FAIL'} {name}")
    print(f"{len(checks) - len(failed)}/{len(checks)} checks pass")
    return 1 if failed else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--physical-device", type=Path)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--instance", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    paths = {
        "physical-device": args.physical_device,
        "header": args.header,
        "instance": args.instance,
    }
    missing = [name for name, path in paths.items() if path is None]
    if missing:
        parser.error("--selftest or all of --physical-device, --header, "
                     "--instance is required")
    sources = []
    for name, path in paths.items():
        if not path.is_file():
            print(f"r3v-wsi-init-comment-parity: {path} is not a file",
                  file=sys.stderr)
            return 2
        sources.append((str(path), path.read_text()))
    try:
        audit(sources)
    except AuditFailure as exc:
        print(f"r3v-wsi-init-comment-parity: {exc}", file=sys.stderr)
        return 1
    print(f"r3v-wsi-init-comment-parity: {GUARD_STRING} names every "
          f"software-mode or lavapipe-pattern comment across "
          f"{len(sources)} sources")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
