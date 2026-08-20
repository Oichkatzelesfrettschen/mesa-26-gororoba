# SPDX-License-Identifier: MIT
"""Hold the native ICD's advertised surface to the surface it executes.

The boundaries document's completion criteria require feature and
extension tables generated from implemented behavior, and an advertised
capability with no native route is a promise the driver breaks at the
first call.  A table entry costs one line to add and carries no evidence
with it, so this audit is what makes the criterion checkable: every
device extension the native branch advertises names the native route
that executes it here, and an entry this registry does not name fails.

The registry is the audit's own declaration rather than a second file to
drift from: adding an extension to the driver and not to this list fails,
and adding it to this list states the route that makes it true.

The native feature set is separately held empty.  A feature bit is a
capability claim the core entry points must honor, and the native branch
returns before setting any, so the audit proves that early return still
stands rather than reading the bits it would otherwise have to model.

Usage:
  r3v_native_advertised_surface_audit.py --source PATH
  r3v_native_advertised_surface_audit.py --selftest
Exit 0 when the advertised surface matches the registry.
"""

import argparse
import re
import sys
from pathlib import Path

# Each advertised device extension names the native route that executes
# it.  A row here is a claim the driver keeps, so it carries the
# mechanism rather than the extension's own description.
ADVERTISED_DEVICE_EXTENSIONS = {
    "KHR_get_memory_requirements2":
        "vkGetImageMemoryRequirements2 and vkGetBufferMemoryRequirements2 "
        "resolve through the vk_common *2-form bridges onto the native "
        "one-BO requirement query",
    "KHR_bind_memory2":
        "vkBindImageMemory2 and vkBindBufferMemory2 resolve through the "
        "same bridges onto the native offset-zero binding",
    "KHR_dedicated_allocation":
        "the requirement query carries the required-dedicated signal that "
        "states the image's offset-zero binding to an allocator",
}

NATIVE_GUARD = "#ifdef R3V_NATIVE_BACKEND"
EXTENSION_TABLE = "r3v_native_device_extensions_supported"
FEATURE_FUNCTION = "r3v_physical_device_init_features"


class AuditFailure(Exception):
    """The advertised surface and the registry disagree."""


def strip_comments(text):
    """Remove C comments so a commented-out entry reads as absent."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def advertised_extensions(source_text):
    """The extensions the native device table sets true."""
    text = strip_comments(source_text)
    start = text.find(EXTENSION_TABLE)
    if start < 0:
        raise AuditFailure(
            f"the source defines no {EXTENSION_TABLE}, so the advertised "
            f"surface cannot be read")
    opening = text.find("{", start)
    closing = text.find("}", opening)
    if opening < 0 or closing < 0:
        raise AuditFailure(f"{EXTENSION_TABLE} carries no initializer")
    body = text[opening + 1:closing]
    advertised = {}
    for match in re.finditer(r"\.\s*([A-Za-z0-9_]+)\s*=\s*([A-Za-z0-9_]+)",
                             body):
        name, value = match.group(1), match.group(2)
        if value == "true":
            advertised[name] = True
        elif value == "false":
            advertised[name] = False
        else:
            raise AuditFailure(
                f"{EXTENSION_TABLE} sets {name} to {value!r}, which is "
                f"neither true nor false, so what it advertises is unread")
    return {name for name, on in advertised.items() if on}


def native_features_empty(source_text):
    """True when the native branch returns before setting any feature."""
    text = strip_comments(source_text)
    start = text.find(FEATURE_FUNCTION)
    if start < 0:
        raise AuditFailure(
            f"the source defines no {FEATURE_FUNCTION}, so the native "
            f"feature set cannot be read")
    body_start = text.find("{", start)
    if body_start < 0:
        raise AuditFailure(f"{FEATURE_FUNCTION} carries no body")
    guard = text.find(NATIVE_GUARD, body_start)
    if guard < 0:
        raise AuditFailure(
            f"{FEATURE_FUNCTION} carries no {NATIVE_GUARD} branch, so the "
            f"native build's feature set is whatever the Gallium body sets")
    endif = text.find("#endif", guard)
    if endif < 0:
        raise AuditFailure(f"the {NATIVE_GUARD} branch never closes")
    branch = text[guard + len(NATIVE_GUARD):endif]
    # A feature assignment inside the branch means the native build
    # advertises that capability; the branch may only return.
    assignment = re.search(r"features\s*->\s*([A-Za-z0-9_]+)\s*=", branch)
    if assignment:
        raise AuditFailure(
            f"the native branch of {FEATURE_FUNCTION} sets "
            f"features->{assignment.group(1)}, so the native build "
            f"advertises a feature; the registry holds the native feature "
            f"set empty")
    if not re.search(r"\breturn\b", branch):
        raise AuditFailure(
            f"the native branch of {FEATURE_FUNCTION} does not return, so "
            f"the Gallium body below it sets the native feature set")
    return True


def audit(source_text):
    advertised = advertised_extensions(source_text)
    registered = set(ADVERTISED_DEVICE_EXTENSIONS)
    unregistered = sorted(advertised - registered)
    if unregistered:
        raise AuditFailure(
            "the native build advertises "
            + ", ".join(unregistered)
            + " with no route named for it; name the native route in "
              "ADVERTISED_DEVICE_EXTENSIONS or stop advertising it")
    withdrawn = sorted(registered - advertised)
    if withdrawn:
        raise AuditFailure(
            "the registry names " + ", ".join(withdrawn)
            + " which the native build no longer advertises; a registry "
              "naming a route the driver dropped overstates the surface")
    native_features_empty(source_text)
    return sorted(advertised)


def selftest():
    checks = []

    def check(name, ok):
        checks.append((name, ok))

    def good(extensions=None, feature_branch="   return;\n"):
        entries = extensions if extensions is not None else [
            f"      .{name} = true," for name in ADVERTISED_DEVICE_EXTENSIONS]
        return ("static const struct vk_device_extension_table\n"
                f"   {EXTENSION_TABLE} = {{\n"
                + "\n".join(entries) + "\n   };\n"
                "\nstatic void\n"
                f"{FEATURE_FUNCTION}(struct vk_features *features)\n"
                "{\n   memset(features, 0, sizeof(*features));\n"
                f"{NATIVE_GUARD}\n{feature_branch}#endif\n"
                "   features->robustBufferAccess = true;\n}\n")

    def refuses(text, fragment):
        try:
            audit(text)
        except AuditFailure as exc:
            return fragment in str(exc)
        return False

    check("admits the registered surface",
          audit(good()) == sorted(ADVERTISED_DEVICE_EXTENSIONS))

    # Known-bad: one extension added to the driver alone.
    entries = [f"      .{name} = true," for name in ADVERTISED_DEVICE_EXTENSIONS]
    entries.append("      .KHR_swapchain = true,")
    check("refuses an extension with no route named",
          refuses(good(entries), "KHR_swapchain"))

    # Known-bad: an extension dropped from the driver alone.
    entries = [f"      .{name} = true,"
               for name in list(ADVERTISED_DEVICE_EXTENSIONS)[:-1]]
    check("refuses a registry naming a dropped extension",
          refuses(good(entries), "no longer advertises"))

    # Known-good: an entry set false is not advertised, so it needs no row.
    entries = [f"      .{name} = true," for name in ADVERTISED_DEVICE_EXTENSIONS]
    entries.append("      .KHR_swapchain = false,")
    check("reads a false entry as unadvertised",
          audit(good(entries)) == sorted(ADVERTISED_DEVICE_EXTENSIONS))

    # Known-good: a commented-out entry is absent, not advertised.
    entries = [f"      .{name} = true," for name in ADVERTISED_DEVICE_EXTENSIONS]
    entries.append("      /* .KHR_swapchain = true, */")
    check("reads a commented entry as absent",
          audit(good(entries)) == sorted(ADVERTISED_DEVICE_EXTENSIONS))

    # Known-bad: a feature set inside the native branch.
    check("refuses a feature set in the native branch",
          refuses(good(feature_branch="   features->logicOp = true;\n"
                                      "   return;\n"),
                  "advertises a feature"))

    # Known-bad: the native branch falling through to the Gallium body.
    check("refuses a native branch that does not return",
          refuses(good(feature_branch="   (void)features;\n"),
                  "does not return"))

    # Known-bad: a value the audit cannot read leaves the surface unread.
    entries = [f"      .{name} = true," for name in ADVERTISED_DEVICE_EXTENSIONS]
    entries.append("      .KHR_swapchain = with_swapchain,")
    check("refuses an entry whose value is not a literal",
          refuses(good(entries), "neither true nor false"))

    # Known-bad: fail closed on missing input rather than passing an
    # audit that read nothing.
    check("refuses a source with no extension table",
          refuses("static void f(void) {}\n", "defines no"))
    check("refuses a source with no feature function",
          refuses("static const struct vk_device_extension_table\n"
                  f"   {EXTENSION_TABLE} = {{\n"
                  + "\n".join(f"      .{n} = true,"
                              for n in ADVERTISED_DEVICE_EXTENSIONS)
                  + "\n   };\n", "defines no"))

    failed = [n for n, ok in checks if not ok]
    for name, ok in checks:
        print(f"  {'ok  ' if ok else 'FAIL'} {name}")
    print(f"{len(checks) - len(failed)}/{len(checks)} checks pass")
    return 1 if failed else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.source is None:
        parser.error("--source or --selftest is required")
    if not args.source.is_file():
        print(f"r3v-native-advertised-surface: {args.source} is not a file",
              file=sys.stderr)
        return 2
    try:
        advertised = audit(args.source.read_text())
    except AuditFailure as exc:
        print(f"r3v-native-advertised-surface: {exc}", file=sys.stderr)
        return 1
    for name in advertised:
        print(f"  {name}: {ADVERTISED_DEVICE_EXTENSIONS[name]}")
    print(f"native device extensions advertised: {len(advertised)}; "
          f"native feature set empty")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
