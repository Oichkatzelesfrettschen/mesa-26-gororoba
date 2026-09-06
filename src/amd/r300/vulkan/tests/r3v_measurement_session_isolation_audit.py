#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The measurement session opens once, and nothing else can reopen it.

Two rules, both in source.  r3v_native_device_refresh_delivery_gates
re-reads the environment and runs many times over one device, so its body
names no measurement-session state; the name invites the reuse, and this
refuses it.  And each entry point the allowance chain rests on carries one call site
across the driver: r3v_measurement_session_init, which is the memset that
clears a spent budget, r3v_measurement_session_open, the
r3v_measurement_declaration_open that calls it, and the
vk_device_memory_create that every stamped allocation passes through.  A
second caller would open a second allowance for one device, or publish a
VkDeviceMemory with no generation on it.
"""

import argparse
import pathlib
import re
import sys

FORBIDDEN = re.compile(r"measurement_session|r3v_measurement_")

# Every entry point that can give a device an allowance it did not have.
# session_init is the memset that clears a spent budget, session_open
# installs one, and declaration_open is the loader that calls it.
ALLOWANCE_ENTRY_POINTS = (
    "r3v_measurement_session_init",
    "r3v_measurement_session_open",
    "r3v_measurement_declaration_open",
    # The VkDeviceMemory wrapper's one constructor.  r3v_AllocateMemory
    # stamps the allocation generation right after this call and before
    # publishing the handle, so a second construction site would publish a
    # wrapper carrying generation zero -- and two such wrappers would be
    # indistinguishable to a binding that compares handle and generation.
    "vk_device_memory_create",
    # The campaign's accounting, each at exactly one production site: the
    # fill route binds a case, the queue spends one execution immediately
    # before the ioctl, and the first submission of an active session
    # takes the durable claim.  A second site for any of them would be a
    # second place the budget, the identity, and the arm's ownership
    # could disagree, which is the defect the pair exists to catch.
    "r3v_measurement_session_bind",
    "r3v_measurement_session_consume",
    "r3v_measurement_claim_acquire",
)


def blank_noncode(text):
    """The text with comment and literal contents replaced by spaces.

    A brace inside a comment or a string literal is not C structure, and
    counting one would end the inspected body early and let a forbidden
    call through.  Newlines survive so offsets and line numbers hold.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        two = text[i : i + 2]
        if two == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(ch if ch == "\n" else " "
                               for ch in text[i:end]))
            i = end
        elif two == "//":
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif c in "\"'":
            quote = c
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " "
                               for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def body_of(text, function):
    """The brace-balanced body of a function defined at column zero."""
    code = blank_noncode(text)
    start = code.find("\n" + function + "(")
    if start < 0:
        return None
    open_brace = code.find("{", start)
    if open_brace < 0:
        return None
    depth = 0
    for i in range(open_brace, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace : i + 1]
    return None


def call_sites(paths, symbol):
    """Files carrying a call to `symbol`, and how many calls each carries.

    Comments and literals are blanked first, so a symbol named in prose is
    not a call.  A definition is what is skipped, and it is recognized by
    what precedes the name: a return type on its own line above, or a
    storage class and a return type on the same line.  A call wrapped for
    the column limit starts its own line with the name, so the earlier
    rule -- any name at the start of a line is a definition -- counted a
    wrapped call as zero.
    """
    pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(symbol) + r"\s*\(")
    found = {}
    for path in paths:
        code = blank_noncode(pathlib.Path(path).read_text(encoding="utf-8"))
        count = 0
        for m in pattern.finditer(code):
            line_start = code.rfind("\n", 0, m.start()) + 1
            prefix = code[line_start : m.start()].strip()
            if prefix == "":
                # The name opens a line.  A definition has its return type
                # alone on the line above and nothing before that on this
                # one; a wrapped call has an unclosed expression above it,
                # which ends in an operator, a comma, or an open paren.
                above = code.rfind("\n", 0, line_start - 1) + 1
                previous = code[above : line_start - 1].strip()
                if previous and previous[-1] not in "=,(&|!?+-*/%<>:":
                    continue
            count += 1
        if count:
            found[str(path)] = count
    return found


def audit(text):
    """The forbidden names the gate refresh body carries."""
    body = body_of(text, "r3v_native_device_refresh_delivery_gates")
    if body is None:
        return ["r3v_native_device_refresh_delivery_gates is not defined here"]
    return sorted({m.group(0) for m in FORBIDDEN.finditer(body)})


def self_test():
    """Calibrated on a body that opens a session and one that does not."""
    good = """
void
r3v_native_device_refresh_delivery_gates(struct r3v_native_device *device)
{
   device->r2vb_delivery_gate = exact_gate("A");
}
"""
    bad = """
void
r3v_native_device_refresh_delivery_gates(struct r3v_native_device *device)
{
   r3v_measurement_session_init(&device->measurement_session);
}
"""
    # A brace inside a comment or a literal is not the body's end, so a
    # call after one is still inspected.
    hidden = """
void
r3v_native_device_refresh_delivery_gates(struct r3v_native_device *device)
{
   /* a closing brace } in prose */
   const char *s = "}";
   char c = '}';
   // and one } in a line comment
   r3v_measurement_session_init(&device->measurement_session);
}
"""
    assert audit(good) == [], audit(good)
    assert audit(bad) == ["measurement_session", "r3v_measurement_"], audit(bad)
    assert audit(hidden) == ["measurement_session", "r3v_measurement_"], \
        audit(hidden)
    assert audit("void other(void) {}")

    import tempfile

    with tempfile.TemporaryDirectory() as d:
        one = pathlib.Path(d) / "one.c"
        one.write_text("""
/* r3v_measurement_declaration_open( in prose */
static int caller(void) { return r3v_measurement_declaration_open(a); }
enum x
r3v_measurement_declaration_open(int a)
{
   return 0;
}
""", encoding="utf-8")
        # A call wrapped for the column limit opens its own line.  Counting
        # it as a definition would hide a second caller behind a line break.
        wrapped = pathlib.Path(d) / "wrapped.c"
        wrapped.write_text("""
static int g(void)
{
   const int result =
      r3v_measurement_declaration_open(&session, path,
                                       &deployment, &reason);
   return result;
}
""", encoding="utf-8")
        two = pathlib.Path(d) / "two.c"
        two.write_text(
            "int f(void) { return r3v_measurement_declaration_open(b); }\n",
            encoding="utf-8")
        sites = call_sites([one], "r3v_measurement_declaration_open")
        assert sum(sites.values()) == 1, sites
        sites = call_sites([wrapped], "r3v_measurement_declaration_open")
        assert sum(sites.values()) == 1, sites
        sites = call_sites([one, wrapped, two],
                           "r3v_measurement_declaration_open")
        assert sum(sites.values()) == 3, sites


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument(
        "--single-caller", metavar="DIR", action="append", default=[],
        help="a directory whose C sources are searched for calls to each "
             "allowance-opening entry point; exactly one call site is "
             "required for each, across all of them")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("r3v measurement session isolation: self-test passed")
        return 0
    if args.source is None:
        parser.error("a source file or --self-test is required")
    findings = audit(pathlib.Path(args.source).read_text(encoding="utf-8"))
    if findings:
        for f in findings:
            print(f"r3v_native_device_refresh_delivery_gates names {f}",
                  file=sys.stderr)
        return 1
    print("r3v measurement session isolation: the gate refresh opens nothing")

    if args.single_caller:
        sources = sorted(
            p for d in args.single_caller
            for p in pathlib.Path(d).rglob("*.c")
            if "tests" not in p.parts)
        failed = False
        for symbol in ALLOWANCE_ENTRY_POINTS:
            sites = call_sites(sources, symbol)
            total = sum(sites.values())
            if total != 1:
                print(f"{symbol} has {total} call sites, not one: {sites}",
                      file=sys.stderr)
                failed = True
                continue
            print("r3v measurement session isolation: "
                  f"{symbol} is called at one site, {next(iter(sites))}")
        if failed:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
