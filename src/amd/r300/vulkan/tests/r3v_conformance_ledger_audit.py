# SPDX-License-Identifier: MIT
"""Hold every numeric claim in the non-pass ledger to a named authority.

r3v_conformance_nonpass_ledger.tsv classifies dEQP non-pass rows, and its
authority text carries numbers -- limits, case counts, descriptor bounds
-- that the conformance frontier and the status document cite.  A number
typed by hand drifts from the source it describes (R3V_MAX_RENDER_EXTENT
read 64 in the ledger while the source advertised 256), so this audit
requires each integer in a blocking row's authority text to carry one
of three authorities in the registry below:

  source_query   a macro or literal in a source file whose current value
                 equals the number (checked here);
  receipt        a retained bundle or host-model measurement the number
                 came from, named so the frontier can re-measure it;
  prose          a non-quantitative count (thread counts in a race
                 description) that no source or receipt pins.

A ledger integer absent from the registry fails, and a source_query whose
source no longer yields the number fails.

Usage:
  r3v_conformance_ledger_audit.py --ledger PATH --source-root DIR
  r3v_conformance_ledger_audit.py --selftest
"""

import argparse
import csv
import re
import sys
from pathlib import Path

INTEGER = re.compile(r"(?<![\w.:/-])(\d{1,7})(?![\w.:/-]|\s*(?:-|\.\d))")

# (class, number) -> (kind, locator).  A source_query locator is
# "file::regex" where the regex's first group must equal the number.
CLAIMS = {
    ("limit_below_core_minimum", "2048"): (
        "source_query",
        r"src/amd/r300/vulkan/r3v_private.h::"
        r"#define R3V_R3XX_MAX_TEXTURE_DIMENSION (\d+)u"),
    ("limit_below_core_minimum", "256"): (
        "source_query",
        r"src/amd/r300/vulkan/r3v_private.h::"
        r"#define R3V_MAX_RENDER_EXTENT (\d+)u"),
    ("limit_below_core_minimum", "2560"): (
        "source_query",
        r"src/amd/r300/common/r300_chip_identity.c::"
        r"\.render_span_max = (\d+),"),
    ("limit_below_core_minimum", "4096"): (
        "receipt",
        "docs/hardware/rs482-2048-4096-virtualization.md composed surface"),
    ("pipeline_barrier_executing_route_gap", "24"): (
        "receipt", "r3v-sampled-rung-conformance-movement-host-model"),
    ("pipeline_barrier_executing_route_gap", "28"): (
        "receipt", "r3v-sampled-rung-conformance-movement-host-model"),
    ("pipeline_barrier_executing_route_gap", "52"): (
        "receipt", "r3v-sampled-rung-conformance-movement-host-model"),
    ("sampling_fragment_route_absent", "0"): ("prose", "binding index zero"),
    ("loader_instance_extension_enumeration_race", "1"): (
        "prose", "thread count in the race description"),
    ("loader_instance_extension_enumeration_race", "2"): (
        "prose", "thread count in the race description"),
    ("descriptor_outside_executed_subset", "1024"): (
        "source_query",
        r"src/amd/r300/vulkan/r3v_native.h::"
        r"#define R3V_NATIVE_DESCRIPTOR_COUNT_MAX (\d+)u?"),
    ("descriptor_outside_executed_subset", "65536"): (
        "source_query",
        r"src/amd/r300/vulkan/r3v_native.h::"
        r"#define R3V_NATIVE_DESCRIPTOR_POOL_SET_MAX (\d+)u?"),
}
KINDS = frozenset({"source_query", "receipt", "prose"})


class AuditFailure(Exception):
    """A ledger number carries no authority or its source disagrees."""


def integers(text):
    return sorted(set(INTEGER.findall(text)))


def audit(rows, source_root):
    seen = set()
    for row in rows:
        if row["disposition"] != "blocks":
            continue
        for number in integers(row["authority"]):
            key = (row["class"], number)
            seen.add(key)
            claim = CLAIMS.get(key)
            if claim is None:
                raise AuditFailure(
                    f"ledger row {row['class']} carries the number {number} "
                    f"with no registered authority; add a source_query, "
                    f"receipt, or prose row to CLAIMS")
            kind, locator = claim
            if kind not in KINDS:
                raise AuditFailure(f"{key}: unknown authority kind {kind}")
            if kind == "source_query":
                path, _, pattern = locator.partition("::")
                text = (source_root / path).read_text(encoding="utf-8")
                match = re.search(pattern, text)
                if match is None:
                    raise AuditFailure(
                        f"{key}: source query {locator!r} matches nothing")
                if match.group(1) != number:
                    raise AuditFailure(
                        f"ledger row {row['class']} says {number} where "
                        f"{path} yields {match.group(1)}; regenerate the "
                        f"ledger text from the source")
    stale = set(CLAIMS) - seen
    if stale:
        raise AuditFailure(
            f"registry rows name numbers the ledger no longer carries: "
            f"{sorted(stale)}")


def read_ledger(path):
    with open(path, newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def selftest():
    rows = [{"class": "limit_below_core_minimum", "disposition": "blocks",
             "authority": "R3V_MAX_RENDER_EXTENT 256"}]
    good = {("limit_below_core_minimum", "256"): CLAIMS[
        ("limit_below_core_minimum", "256")]}
    saved = dict(CLAIMS)
    try:
        CLAIMS.clear()
        CLAIMS.update(good)
        root = Path(__file__).resolve().parents[5]
        audit(rows, root)
        bad_rows = [{"class": "limit_below_core_minimum",
                     "disposition": "blocks",
                     "authority": "R3V_MAX_RENDER_EXTENT 64"}]
        CLAIMS.clear()
        CLAIMS[("limit_below_core_minimum", "64")] = (
            "source_query",
            r"src/amd/r300/vulkan/r3v_private.h::"
            r"#define R3V_MAX_RENDER_EXTENT (\d+)u")
        try:
            audit(bad_rows, root)
        except AuditFailure:
            pass
        else:
            raise AuditFailure("selftest admitted a stale source number")
        CLAIMS.clear()
        try:
            audit(rows, root)
        except AuditFailure:
            pass
        else:
            raise AuditFailure("selftest admitted a bare number")
    finally:
        CLAIMS.clear()
        CLAIMS.update(saved)
    print("r3v_conformance_ledger_audit: selftest OK")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ledger", type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.selftest:
            return selftest()
        if args.ledger is None or args.source_root is None:
            parser.error("--ledger and --source-root are required")
        rows = read_ledger(args.ledger)
        audit(rows, args.source_root.resolve())
        print(f"r3v_conformance_ledger_audit: {len(rows)} rows, every "
              f"blocking number carries an authority")
        return 0
    except (AuditFailure, OSError) as error:
        print(f"r3v_conformance_ledger_audit: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
