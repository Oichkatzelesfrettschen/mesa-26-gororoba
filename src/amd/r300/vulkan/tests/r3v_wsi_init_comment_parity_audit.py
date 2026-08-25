# SPDX-License-Identifier: MIT
"""Hold the three WSI-route comments to the control flow they describe.

`docs/hardware/r3v-wsi-denominator.md`, "Default present route", records
that `r3v_init_wsi` routes through the render-node fd (the DRM/DRI3
path) by default and falls back to the Mesa common WSI software
(xcb-shm CPU) route only when `R3V_WSI_SW` begins with `'1'`.  Three
comment sites -- the block above `r3v_init_wsi` itself, the `wsi_device`
field declaration, and the `KHR_surface` extension-table entry -- once
stated the opposite: software mode by default, unqualified by the
environment variable that actually selects it.  This check pins that
correction: a comment naming "software mode" or "the lavapipe pattern"
at any of the three sites, in a file that nowhere names `R3V_WSI_SW`,
is the same mismatch restated, and the check fails.

Usage:
  r3v_wsi_init_comment_parity_audit.py --physical-device C --header H \
      --instance C
  r3v_wsi_init_comment_parity_audit.py --selftest
Exit 0 when every flagged phrase at the three sites sits in a file that
also names R3V_WSI_SW.
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


def site_violations(text, filename_label):
    violations = []
    has_guard = GUARD_STRING in text
    for site, (anchor, description) in SITES.items():
        comment = preceding_comment(text, anchor)
        if not comment:
            continue
        lowered = comment.lower()
        hit = next((p for p in FLAGGED_PHRASES if p in lowered), None)
        if hit is None:
            continue
        if not has_guard:
            violations.append(
                f"{filename_label}: the comment above {description} "
                f"names {hit!r} and the file names no {GUARD_STRING}, so "
                f"the comment restates the software-mode-by-default "
                f"mismatch this check pins closed")
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

    check("passes a file naming the phrase with the guard present",
          audit([("r3v_physical_device.c",
                  reintroduced_physical_device
                  + "\n/* R3V_WSI_SW selects the fallback. */\n")]))

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
