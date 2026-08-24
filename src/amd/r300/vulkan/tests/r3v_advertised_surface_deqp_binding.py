# SPDX-License-Identifier: MIT
"""Bind every advertised extension, granted feature bit, receipt-bound
limit, and format-feature row to its implementation test and its exact
dEQP group.

Each advertised item is a claim the conformance run measures, so it
names the registered Mesa test that exercises the executing route and
the dEQP-VK group whose mustpass cases judge it.  The judging group is
exact: a version-gated group above the pinned apiVersion (vulkan1p2 and
later) judges nothing on a 1.0 device, and a group shallower than a
test family (dEQP-VK.api) is an ancestor, so both refuse.  A group whose
cases fail today stays bound; the ledger records the judge, and the
non-pass ledger records the deviation.  The advertised set is
derived from the driver sources (extension tables, granted feature bits,
the limit-receipt registry, the format-property switch, the apiVersion
pin, and the queue-family shape); a derived item with no ledger row
fails, a ledger row nothing derives fails, a row naming an unregistered
test fails, and with R3V_DEQP_MUSTPASS_DIR naming a CTS mustpass
directory a group matching no mustpass case fails.  Without the corpus
the group check reports itself as not run and the remaining clauses
still decide.

Usage:
  ... --ledger TSV --physical-device C --instance C --private-header H \
      --intro-tests meson-info/intro-tests.json
  ... --selftest
"""

import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import r3v_native_advertised_surface_audit as surface  # noqa: E402
import r3v_vulkan10_requirement_inventory as inventory  # noqa: E402

HEADER = ["kind", "name", "implementation_test", "deqp_group"]
MUSTPASS_ENV = "R3V_DEQP_MUSTPASS_DIR"


class BindingFailure(Exception):
    pass


FEATURE_BIT_RE = re.compile(r"VK_FORMAT_FEATURE_2_\w+_BIT")
CASE_BLOCK_RE = re.compile(
    r"((?:case\s+VK_FORMAT_\w+\s*:\s*)+)(.*?)"
    r"(?=case\s+VK_FORMAT_\w+\s*:|default\s*:|\Z)", re.S)


def advertised_formats(text):
    """Every (format, feature bit) pair r3v_get_format_properties grants.

    A grant is scoped to the exact bit, not the format alone: two case
    labels sharing one block share every bit that block assigns across
    linearTilingFeatures and bufferFeatures, and a block that widens or
    narrows its assigned bits changes the derived pair set even when its
    format labels do not, so a feature-bit change needs a ledger row of
    its own rather than riding an unrelated format's existing row.
    """
    body = re.search(r"r3v_get_format_properties\s*\([^{]*\{(.*?)\n\}",
                     text, re.S)
    if body is None:
        raise BindingFailure("no r3v_get_format_properties body")
    stripped = surface.strip_comments(body.group(1))
    pairs = set()
    for labels_text, block in CASE_BLOCK_RE.findall(stripped):
        formats = re.findall(r"VK_FORMAT_\w+", labels_text)
        bits = set(FEATURE_BIT_RE.findall(block))
        for fmt in formats:
            for bit in bits:
                pairs.add(f"{fmt}:{bit}")
    if not pairs:
        raise BindingFailure(
            "r3v_get_format_properties grants no (format, feature bit) pair")
    return sorted(pairs)


def derive(pdev_text, instance_text, private_text):
    items = set()
    api = inventory.parse_api_version(private_text)
    items.add(("api_version", f"{api[0]}.{api[1]}"))
    items.add(("queue_family", inventory.parse_queue_family(pdev_text)))
    for f in inventory.parse_granted_features(pdev_text):
        items.add(("feature", f))
    for e in inventory.parse_struct_table(instance_text,
                                         inventory.INSTANCE_TABLE):
        items.add(("instance_extension", e))
    for e in inventory.parse_struct_table(pdev_text,
                                         inventory.DEVICE_TABLE):
        items.add(("device_extension", e))
    for name in surface.LIMIT_RECEIPTS:
        items.add(("limit", name))
    for fmt_bit in advertised_formats(pdev_text):
        items.add(("format_feature", fmt_bit))
    return items


def read_ledger(path):
    lines = Path(path).read_text().splitlines()
    if not lines or lines[0].split("\t") != HEADER:
        raise BindingFailure(f"{path} header is not {HEADER}")
    rows = []
    for n, line in enumerate(lines[1:], start=2):
        fields = line.split("\t")
        if len(fields) != 4 or any(not f for f in fields):
            raise BindingFailure(f"{path}:{n} is not a four-field row")
        rows.append(tuple(fields))
    return rows


SELF_TESTS = {"r3v-advertised-surface-deqp-binding",
              "r3v-advertised-surface-deqp-binding-selftest"}
VERSION_GATED_GROUP = re.compile(r"\.vulkan1p[1-9]")
MIN_GROUP_DEPTH = 2


def registered_tests(intro_tests_path):
    data = json.loads(Path(intro_tests_path).read_text())
    names = {t["name"] for t in data} - SELF_TESTS
    if not names:
        raise BindingFailure(f"{intro_tests_path} lists no tests")
    return names


def mustpass_cases(mustpass_dir):
    cases = set()
    for p in Path(mustpass_dir).rglob("*.txt"):
        for line in p.read_text().splitlines():
            if line.startswith("dEQP-VK."):
                cases.add(line.strip())
    if not cases:
        raise BindingFailure(f"{mustpass_dir} carries no dEQP-VK cases")
    return cases


def group_matches(group, cases):
    prefix = group + "."
    return group in cases or any(c.startswith(prefix) for c in cases)


def check(rows, derived, tests, cases):
    ledger_items = {(r[0], r[1]) for r in rows}
    missing = sorted(derived - ledger_items)
    if missing:
        raise BindingFailure(
            "advertised without a binding row: " +
            ", ".join(f"{k}:{n}" for k, n in missing))
    stale = sorted(ledger_items - derived)
    if stale:
        raise BindingFailure(
            "ledger rows nothing advertises: " +
            ", ".join(f"{k}:{n}" for k, n in stale))
    for kind, name, test, group in rows:
        if test not in tests:
            raise BindingFailure(
                f"{kind}:{name} names implementation test {test}, which "
                "is not registered")
        if not re.fullmatch(r"dEQP-VK(\.[A-Za-z0-9_]+)+", group):
            raise BindingFailure(
                f"{kind}:{name} names {group!r}, which is not a dEQP-VK "
                "group path")
        if VERSION_GATED_GROUP.search(group):
            raise BindingFailure(
                f"{kind}:{name} names {group}, a group gated above the "
                "pinned apiVersion 1.0, which judges nothing on this device")
        if group.count(".") < MIN_GROUP_DEPTH:
            raise BindingFailure(
                f"{kind}:{name} names {group}, an ancestor group rather "
                "than the judging group")
        if cases is not None and not group_matches(group, cases):
            raise BindingFailure(
                f"{kind}:{name} names {group}, which matches no mustpass "
                "case")


def run(args):
    pdev = Path(args.physical_device).read_text()
    inst = Path(args.instance).read_text()
    priv = Path(args.private_header).read_text()
    derived = derive(pdev, inst, priv)
    rows = read_ledger(args.ledger)
    tests = registered_tests(args.intro_tests)
    mustpass = os.environ.get(MUSTPASS_ENV)
    cases = mustpass_cases(mustpass) if mustpass else None
    check(rows, derived, tests, cases)
    corpus = (f"{len(cases)} mustpass cases" if cases else
              f"mustpass group check not run ({MUSTPASS_ENV} unset)")
    print(f"binding holds: {len(rows)} rows, {len(tests)} registered "
          f"tests, {corpus}")


def selftest():
    derived = {("feature", "robustBufferAccess"),
               ("device_extension", "VK_KHR_bind_memory2")}
    rows = [("feature", "robustBufferAccess", "t-robust",
             "dEQP-VK.robustness.buffer_access"),
            ("device_extension", "VK_KHR_bind_memory2", "t-bind",
             "dEQP-VK.memory.binding")]
    tests = {"t-robust", "t-bind"}
    cases = {"dEQP-VK.robustness.buffer_access.x",
             "dEQP-VK.memory.binding.y"}
    check(rows, derived, tests, cases)

    def expect(fragment, **kw):
        a = dict(rows=rows, derived=derived, tests=tests, cases=cases)
        a.update(kw)
        try:
            check(a["rows"], a["derived"], a["tests"], a["cases"])
        except BindingFailure as e:
            if fragment in str(e):
                return
            raise SystemExit(f"selftest: expected {fragment!r}: {e}")
        raise SystemExit(f"selftest: fixture for {fragment!r} passed")

    expect("without a binding row",
           derived=derived | {("limit", "maxImageDimension2D")})
    expect("nothing advertises", rows=rows + [
        ("limit", "maxViewportDimensions[0]", "t-bind", "dEQP-VK.x")])
    expect("is not registered", tests={"t-robust"})
    expect("matches no mustpass case",
           cases={"dEQP-VK.robustness.buffer_access.x"})
    expect("not a dEQP-VK group path", rows=[rows[0], (
        "device_extension", "VK_KHR_bind_memory2", "t-bind",
        "memory.binding")])
    expect("gated above the pinned apiVersion", rows=[rows[0], (
        "device_extension", "VK_KHR_bind_memory2", "t-bind",
        "dEQP-VK.api.info.vulkan1p2_limits_validation")])
    expect("an ancestor group", rows=[rows[0], (
        "device_extension", "VK_KHR_bind_memory2", "t-bind",
        "dEQP-VK.memory")], cases=cases | {"dEQP-VK.memory.x"})
    with tempfile.TemporaryDirectory() as d:
        Path(d, "api.txt").write_text("dEQP-VK.api.smoke.triangle\n")
        got = mustpass_cases(d)
        assert got == {"dEQP-VK.api.smoke.triangle"}, got
    print("selftest: known-good binding passes; missing-row, stale-row, "
          "unregistered-test, group-without-case, malformed-group, "
          "version-gated-group, and ancestor-group fixtures each fail by "
          "their clause")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--ledger")
    p.add_argument("--physical-device")
    p.add_argument("--instance")
    p.add_argument("--private-header")
    p.add_argument("--intro-tests")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args()
    if args.selftest:
        selftest()
        return
    for f in ("ledger", "physical_device", "instance", "private_header",
              "intro_tests"):
        if getattr(args, f) is None:
            p.error(f"--{f.replace('_', '-')} is required")
    try:
        run(args)
    except (BindingFailure, inventory.InventoryFailure,
            surface.AuditFailure) as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
