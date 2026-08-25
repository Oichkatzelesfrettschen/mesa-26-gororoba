# SPDX-License-Identifier: MIT
"""Hold the native ICD's swapchain absence to the tables that say so.

`docs/hardware/r3v-wsi-denominator.md` names the denominator every
WSI-dependent conformance case sits on top of: `KHR_swapchain` appears
in no device extension table, no dEQP binding row, and defines no
`r3v_CreateSwapchainKHR` route in the built driver, so `vkCreateSwapchainKHR`
never reaches this ICD.  A partial edit -- an extension bit flipped on
without the route that would make it true, a binding row added ahead of
the code, or a defined dispatch symbol with no advertised extension --
breaks that denominator silently, because none of the three sources
alone proves the others.  This audit reads all three together and fails
the moment any one of them carries a `KHR_swapchain` signal the other
two do not corroborate.

Usage:
  r3v_native_swapchain_absence_audit.py --device-extensions PATH
      [--binding-tsv PATH] [--library PATH]
  r3v_native_swapchain_absence_audit.py --selftest
Exit 0 when every source agrees the extension and its route are absent.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

EXTENSION_TABLE = "r3v_native_device_extensions_supported"
EXTENSION_NAME = "KHR_swapchain"
BINDING_EXTENSION_NAME = "VK_KHR_swapchain"
ROUTE_SYMBOL = "r3v_CreateSwapchainKHR"


class AuditFailure(Exception):
    """A source names a swapchain signal the other sources do not carry."""


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


def device_extension_advertises_swapchain(source_text):
    """True when the device extension table sets KHR_swapchain true."""
    text = strip_comments(source_text)
    start = text.find(EXTENSION_TABLE)
    if start < 0:
        raise AuditFailure(
            f"the source defines no {EXTENSION_TABLE}, so the advertised "
            f"device surface cannot be read")
    opening = text.find("{", start)
    closing = text.find("}", opening)
    if opening < 0 or closing < 0:
        raise AuditFailure(f"{EXTENSION_TABLE} carries no initializer")
    body = text[opening + 1:closing]
    for match in re.finditer(r"\.\s*([A-Za-z0-9_]+)\s*=\s*([A-Za-z0-9_]+)",
                             body):
        name, value = match.group(1), match.group(2)
        if name != EXTENSION_NAME:
            continue
        if value == "true":
            return True
        if value == "false":
            return False
        raise AuditFailure(
            f"{EXTENSION_TABLE} sets {EXTENSION_NAME} to {value!r}, which "
            f"is neither true nor false, so the advertised surface is "
            f"unread")
    return False


def binding_tsv_names_swapchain(tsv_text):
    """True when any row of the advertised-surface binding ledger names
    VK_KHR_swapchain."""
    for line in tsv_text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split("\t")
        if BINDING_EXTENSION_NAME in fields:
            return True
    return False


def nm_defines_route(nm_output_text):
    """True when nm's full symbol table carries ROUTE_SYMBOL as a defined
    (not undefined, not weak-external) symbol.  An `U` type is a
    reference the linker resolves elsewhere, not a route this driver
    executes; a defined text-section entry (`T`/`t`) is the route."""
    for line in nm_output_text.splitlines():
        fields = line.split()
        if not fields or fields[-1] != ROUTE_SYMBOL:
            continue
        if len(fields) < 2:
            continue
        symbol_type = fields[-2]
        if symbol_type in ("U", "w", "W"):
            continue
        return True
    return False


def audit(device_extensions_text, binding_tsv_text=None, nm_output_text=None):
    advertised = device_extension_advertises_swapchain(device_extensions_text)
    bound = (binding_tsv_names_swapchain(binding_tsv_text)
             if binding_tsv_text is not None else False)
    routed = (nm_defines_route(nm_output_text)
              if nm_output_text is not None else False)

    if not (advertised or bound or routed):
        return "absent"

    missing = []
    if not advertised:
        missing.append(f"{EXTENSION_TABLE} carries no .{EXTENSION_NAME} "
                        f"= true entry")
    if not bound:
        missing.append(f"the advertised-surface binding ledger carries no "
                        f"{BINDING_EXTENSION_NAME} row")
    if not routed:
        missing.append(f"the built library defines no {ROUTE_SYMBOL} "
                        f"route (nm reports it absent or undefined)")
    if missing:
        raise AuditFailure(
            "a swapchain signal appeared with no corresponding route: "
            + "; ".join(missing)
            + ". docs/hardware/r3v-wsi-denominator.md names no driver-"
              "callback route for KHR_swapchain, so every source must "
              "agree it is absent until that document gains one")
    return "advertised"


def run_nm(nm_binary, library_path):
    result = subprocess.run([nm_binary, str(library_path)], check=False,
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise AuditFailure(
            f"{nm_binary} on {library_path} exited {result.returncode}: "
            f"{result.stderr.strip()}")
    return result.stdout


def selftest():
    checks = []

    def check(name, ok):
        checks.append((name, ok))

    def extensions_text(swapchain_value=None):
        entry = (f"      .{EXTENSION_NAME} = {swapchain_value},\n"
                 if swapchain_value is not None else "")
        return ("static const struct vk_device_extension_table\n"
                f"   {EXTENSION_TABLE} = {{\n"
                "      .KHR_get_memory_requirements2 = true,\n"
                + entry +
                "   };\n")

    good_tsv = "extension\tgroup\nVK_KHR_get_memory_requirements2\tfoo\n"
    bad_tsv = good_tsv + f"{BINDING_EXTENSION_NAME}\tbar\n"
    undefined_nm = f"                 U {ROUTE_SYMBOL}\n"
    defined_nm = f"0000000000012340 T {ROUTE_SYMBOL}\n"

    check("reads the current tree as absent",
          audit(extensions_text(), good_tsv, undefined_nm) == "absent")
    check("reads a fully corroborated advertisement as advertised",
          audit(extensions_text("true"), bad_tsv, defined_nm)
          == "advertised")

    def refuses(**kwargs):
        try:
            audit(**kwargs)
        except AuditFailure as exc:
            return str(exc)
        return None

    check("refuses a table edit with no matching route or binding row",
          refuses(device_extensions_text=extensions_text("true"),
                  binding_tsv_text=good_tsv, nm_output_text=undefined_nm)
          is not None)
    check("refuses a binding row with no matching table entry or route",
          refuses(device_extensions_text=extensions_text(),
                  binding_tsv_text=bad_tsv, nm_output_text=undefined_nm)
          is not None)
    check("refuses a defined route with no advertised extension",
          refuses(device_extensions_text=extensions_text(),
                  binding_tsv_text=good_tsv, nm_output_text=defined_nm)
          is not None)
    check("refuses a false entry treated as absent alongside a stray route",
          refuses(device_extensions_text=extensions_text("false"),
                  binding_tsv_text=good_tsv, nm_output_text=defined_nm)
          is not None)
    check("refuses an unreadable literal",
          refuses(device_extensions_text=extensions_text("maybe"),
                  binding_tsv_text=good_tsv, nm_output_text=undefined_nm)
          is not None)
    check("refuses a source with no extension table",
          refuses(device_extensions_text="static void f(void) {}\n")
          is not None)
    check("passes with no binding or nm source supplied and no table entry",
          audit(extensions_text()) == "absent")

    failed = [n for n, ok in checks if not ok]
    for name, ok in checks:
        print(f"  {'ok  ' if ok else 'FAIL'} {name}")
    print(f"{len(checks) - len(failed)}/{len(checks)} checks pass")
    return 1 if failed else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--device-extensions", type=Path)
    parser.add_argument("--binding-tsv", type=Path)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--nm-binary", default="nm")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.device_extensions is None:
        parser.error("--device-extensions or --selftest is required")
    if not args.device_extensions.is_file():
        print(f"r3v-native-swapchain-absence: {args.device_extensions} is "
              f"not a file", file=sys.stderr)
        return 2
    binding_text = None
    if args.binding_tsv is not None:
        if not args.binding_tsv.is_file():
            print(f"r3v-native-swapchain-absence: {args.binding_tsv} is "
                  f"not a file", file=sys.stderr)
            return 2
        binding_text = args.binding_tsv.read_text()
    nm_text = None
    if args.library is not None:
        if not args.library.is_file():
            print(f"r3v-native-swapchain-absence: {args.library} is not a "
                  f"file", file=sys.stderr)
            return 2
        try:
            nm_text = run_nm(args.nm_binary, args.library)
        except AuditFailure as exc:
            print(f"r3v-native-swapchain-absence: {exc}", file=sys.stderr)
            return 1
    try:
        verdict = audit(args.device_extensions.read_text(), binding_text,
                        nm_text)
    except AuditFailure as exc:
        print(f"r3v-native-swapchain-absence: {exc}", file=sys.stderr)
        return 1
    sources = ["device-extension table"]
    if binding_text is not None:
        sources.append("binding ledger")
    if nm_text is not None:
        sources.append("nm symbol table")
    print(f"r3v-native-swapchain-absence: {verdict} across "
          f"{', '.join(sources)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
