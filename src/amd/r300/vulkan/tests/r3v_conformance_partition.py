#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exact-cover partition of the pinned dEQP-VK mustpass corpus into
conformance slices.

The corpus is every ``dEQP-VK.*`` line under the mustpass directory, read
recursively and deduplicated; a line repeated across files is a corpus
defect and refuses.  A partition table (``order slice groups hazard
required_evidence``) assigns a case to a slice when the case equals one
of the slice's groups or continues it with a dot.  An exhaustive
partition proves exact cover: every corpus case belongs to one slice and
to one slice alone, and the slice counts sum to the unique corpus count.
A pilot partition (``--kind pilot``) proves the same disjointness but
admits uncovered cases, so its receipts name a pilot slice run and never
the corpus.

Every slice's caselist is generated here, in sorted order, with its count
and SHA-256 recorded beside the corpus digest and the partition table
digest in ``partition_manifest.json``.  The ``unknown`` hazard blocks
execution: the manifest marks such a slice ``executable: false`` and the
runner refuses its caselist, so an unclassified case never rides a
``none`` slice onto the target.

Subcommands: ``generate`` writes the caselists and manifest; ``check``
proves the partition against the corpus without writing caselists;
``selftest`` calibrates every refusal on synthetic corpora.
"""

import argparse
import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

HEADER = ["order", "slice", "groups", "hazard", "required_evidence"]
HAZARDS = {"none", "submission", "display", "unknown"}
EVIDENCE = {"host-model", "silicon"}
KINDS = {"pilot", "exhaustive"}
MANIFEST_VERSION = 1


class PartitionRefusal(Exception):
    pass


def sha256_lines(lines):
    return hashlib.sha256(("\n".join(lines) + "\n").encode()).hexdigest()


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_corpus(mustpass_dir):
    """Every dEQP-VK case under the directory, once; a case listed twice
    across files refuses because the corpus denominator would be
    ambiguous."""
    root = Path(mustpass_dir)
    files = sorted(p for p in root.rglob("*.txt") if p.is_file())
    if not files:
        raise PartitionRefusal(f"{mustpass_dir} holds no mustpass files")
    seen = {}
    for f in files:
        for line in f.read_text().splitlines():
            c = line.strip()
            if not c.startswith("dEQP-VK."):
                continue
            if c in seen:
                raise PartitionRefusal(
                    f"{c} is listed in {seen[c]} and {f.relative_to(root)}")
            seen[c] = f.relative_to(root)
    if not seen:
        raise PartitionRefusal(f"{mustpass_dir} lists no dEQP-VK case")
    return sorted(seen), [str(f.relative_to(root)) for f in files]


def load_partition(path):
    lines = Path(path).read_text().splitlines()
    if not lines or lines[0].split("\t") != HEADER:
        raise PartitionRefusal(f"{path} header is not {HEADER}")
    rows = []
    names = set()
    for n, line in enumerate(lines[1:], start=2):
        f = line.split("\t")
        if len(f) != 5 or not all(f):
            raise PartitionRefusal(f"{path}:{n} is not a five-field row")
        order, name, groups, hazard, evidence = f
        if name in names:
            raise PartitionRefusal(f"{path}:{n}: slice {name} repeats")
        names.add(name)
        if hazard not in HAZARDS:
            raise PartitionRefusal(f"{path}:{n}: hazard {hazard!r} unknown")
        if evidence not in EVIDENCE:
            raise PartitionRefusal(f"{path}:{n}: evidence {evidence!r} "
                                   "unknown")
        if hazard != "none" and evidence != "silicon":
            raise PartitionRefusal(f"{path}:{n}: a hazardous slice requires "
                                   "silicon evidence")
        glist = groups.split(" ")
        for g in glist:
            if not g.startswith("dEQP-VK.") or g.endswith(".") or ".." in g:
                raise PartitionRefusal(f"{path}:{n}: {g!r} is not a group")
        rows.append({"order": int(order), "slice": name, "groups": glist,
                     "hazard": hazard, "required_evidence": evidence})
    if [r["order"] for r in rows] != list(range(1, len(rows) + 1)):
        raise PartitionRefusal(f"{path}: slice order is not 1..N")
    return rows


def group_index(rows):
    index = {}
    for r in rows:
        for g in r["groups"]:
            index.setdefault(g, []).append((r["slice"], g))
    return index


def matching_groups(case, index):
    """Every (slice, group) pair claiming the case; the case matches a
    group when it equals the group or continues it with a dot, so a
    group is a namespace prefix and never a substring.  The lookup walks
    the case's own dotted prefixes, so its cost is the case depth."""
    hits = []
    parts = case.split(".")
    for n in range(2, len(parts) + 1):
        hits.extend(index.get(".".join(parts[:n]), ()))
    return hits


def partition(cases, rows, kind):
    """Assign every case; refuse a case two slices claim, refuse an
    uncovered case for an exhaustive partition, and refuse a group that
    claims nothing (a stale rule is a silent hole in the cover)."""
    assigned = {r["slice"]: [] for r in rows}
    claimed = {(r["slice"], g): 0 for r in rows for g in r["groups"]}
    uncovered = []
    index = group_index(rows)
    for c in cases:
        hits = matching_groups(c, index)
        if len(hits) > 1:
            raise PartitionRefusal(f"{c} is covered by slices "
                                   f"{sorted({h[0] for h in hits})} through "
                                   f"groups {[h[1] for h in hits]}")
        if not hits:
            uncovered.append(c)
            continue
        assigned[hits[0][0]].append(c)
        claimed[hits[0]] += 1
    if uncovered and kind == "exhaustive":
        raise PartitionRefusal(f"{len(uncovered)} corpus cases belong to no "
                               f"slice, first {uncovered[0]}")
    for (name, g), n in claimed.items():
        if n == 0:
            raise PartitionRefusal(f"slice {name}: group {g} claims no "
                                   "corpus case")
    total = sum(len(v) for v in assigned.values())
    if total + len(uncovered) != len(cases):
        raise PartitionRefusal("slice counts do not reconcile with the corpus")
    return assigned, uncovered


def build_manifest(kind, table_path, mustpass_dir, cases, files, rows,
                   assigned, uncovered):
    slices = []
    for r in rows:
        lines = assigned[r["slice"]]
        slices.append({
            "order": r["order"], "slice": r["slice"], "groups": r["groups"],
            "hazard": r["hazard"], "required_evidence": r["required_evidence"],
            "executable": r["hazard"] != "unknown",
            "case_count": len(lines), "caselist_sha256": sha256_lines(lines),
            "caselist": f"{r['slice']}.txt",
        })
    covered = sum(s["case_count"] for s in slices)
    manifest = {
        "manifest_version": MANIFEST_VERSION,
        "kind": kind,
        "partition_table": Path(table_path).name,
        "partition_table_sha256": sha256_file(table_path),
        "corpus_files": files,
        "corpus_case_count": len(cases),
        "corpus_sha256": sha256_lines(cases),
        "covered_case_count": covered,
        "uncovered_case_count": len(uncovered),
        "exact_cover": kind == "exhaustive" and covered == len(cases),
        "slices": slices,
    }
    body = json.dumps({k: v for k, v in manifest.items()}, sort_keys=True,
                      separators=(",", ":")).encode()
    manifest["manifest_sha256"] = hashlib.sha256(body).hexdigest()
    return manifest


def manifest_digest(manifest):
    body = json.dumps({k: v for k, v in manifest.items()
                       if k != "manifest_sha256"}, sort_keys=True,
                      separators=(",", ":")).encode()
    return hashlib.sha256(body).hexdigest()


def generate(table_path, mustpass_dir, out_dir, kind, write=True):
    if kind not in KINDS:
        raise PartitionRefusal(f"kind {kind!r} is not one of {sorted(KINDS)}")
    rows = load_partition(table_path)
    cases, files = read_corpus(mustpass_dir)
    assigned, uncovered = partition(cases, rows, kind)
    manifest = build_manifest(kind, table_path, mustpass_dir, cases, files,
                              rows, assigned, uncovered)
    if write:
        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)
        for s in manifest["slices"]:
            p = out / s["caselist"]
            p.write_text("\n".join(assigned[s["slice"]]) + "\n")
            if sha256_file(p) != s["caselist_sha256"]:
                raise PartitionRefusal(f"{p} digest drifted after write")
        (out / "partition_manifest.json").write_text(
            json.dumps(manifest, indent=1, sort_keys=True) + "\n")
        if uncovered:
            (out / "uncovered.txt").write_text("\n".join(uncovered) + "\n")
    return manifest


def verify_manifest(path):
    """Prove a manifest's self-digest, its caselists' digests and counts,
    and its sum reconciliation; the runner calls this before admitting a
    caselist."""
    p = Path(path)
    manifest = json.loads(p.read_text())
    if manifest.get("manifest_version") != MANIFEST_VERSION:
        raise PartitionRefusal("manifest version unknown")
    if manifest.get("kind") not in KINDS:
        raise PartitionRefusal("manifest kind unknown")
    if manifest_digest(manifest) != manifest.get("manifest_sha256"):
        raise PartitionRefusal("manifest digest does not match its body")
    total = 0
    for s in manifest["slices"]:
        f = p.parent / s["caselist"]
        if not f.is_file():
            raise PartitionRefusal(f"caselist {s['caselist']} is missing")
        lines = [l for l in f.read_text().splitlines() if l]
        if len(lines) != s["case_count"] or \
                sha256_file(f) != s["caselist_sha256"]:
            raise PartitionRefusal(f"caselist {s['caselist']} does not match "
                                   "its recorded count and digest")
        total += s["case_count"]
    if total != manifest["covered_case_count"]:
        raise PartitionRefusal("slice counts do not sum to the covered count")
    if manifest["kind"] == "exhaustive" and (
            not manifest["exact_cover"] or
            manifest["covered_case_count"] != manifest["corpus_case_count"] or
            manifest["uncovered_case_count"] != 0):
        raise PartitionRefusal("an exhaustive manifest without exact cover")
    return manifest


def slice_for_caselist(manifest, caselist_path):
    """The manifest slice whose digest equals the caselist's, or a
    refusal: a caselist outside the manifest carries no slice identity."""
    digest = sha256_file(caselist_path)
    for s in manifest["slices"]:
        if s["caselist_sha256"] == digest:
            return s
    raise PartitionRefusal(f"{caselist_path} matches no manifest slice "
                           "digest")


def report(manifest):
    print(f"{manifest['kind']} partition: {manifest['corpus_case_count']} "
          f"corpus cases, {manifest['covered_case_count']} covered in "
          f"{len(manifest['slices'])} slices, "
          f"{manifest['uncovered_case_count']} uncovered, corpus "
          f"{manifest['corpus_sha256'][:12]}, manifest "
          f"{manifest['manifest_sha256'][:12]}")
    for s in manifest["slices"]:
        flag = "" if s["executable"] else " BLOCKED"
        print(f"  {s['order']:2d} {s['slice']:<22} {s['case_count']:>8} "
              f"{s['hazard']:<10} {s['caselist_sha256'][:12]}{flag}")


def selftest():
    """Calibrate each refusal on a synthetic corpus and prove the
    positive path end to end, including manifest verification and the
    caselist-to-slice binding."""
    def expect(fn, needle):
        try:
            fn()
        except PartitionRefusal as e:
            if needle not in str(e):
                raise SystemExit(f"selftest: wrong refusal {e!r}, wanted "
                                 f"{needle!r}")
            return
        raise SystemExit(f"selftest: {needle!r} was not refused")

    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        corpus = d / "corpus"
        (corpus / "sub").mkdir(parents=True)
        (corpus / "a.txt").write_text(
            "dEQP-VK.info.build\ndEQP-VK.api.smoke.triangle\n"
            "dEQP-VK.api.smoke.asm\n# comment\n")
        (corpus / "sub" / "b.txt").write_text(
            "dEQP-VK.draw.basic\ndEQP-VK.memory.mapping.full\n")
        table = d / "p.tsv"

        def write_table(rows):
            table.write_text("\t".join(HEADER) + "\n" + "".join(
                "\t".join(r) + "\n" for r in rows))

        good = [("1", "info", "dEQP-VK.info", "none", "host-model"),
                ("2", "api", "dEQP-VK.api.smoke", "none", "host-model"),
                ("3", "draw", "dEQP-VK.draw", "submission", "silicon"),
                ("4", "memory", "dEQP-VK.memory", "unknown", "silicon")]
        write_table(good)
        out = d / "out"
        m = generate(table, corpus, out, "exhaustive")
        if m["corpus_case_count"] != 5 or not m["exact_cover"] or \
                [s["case_count"] for s in m["slices"]] != [1, 2, 1, 1]:
            raise SystemExit("selftest: exhaustive cover miscounted")
        if m["slices"][3]["executable"] or not m["slices"][0]["executable"]:
            raise SystemExit("selftest: unknown hazard is not blocked")
        v = verify_manifest(out / "partition_manifest.json")
        if slice_for_caselist(v, out / "api.txt")["slice"] != "api":
            raise SystemExit("selftest: caselist did not bind to its slice")
        expect(lambda: slice_for_caselist(v, corpus / "a.txt"),
               "matches no manifest slice")
        if (out / "api.txt").read_text() != \
                "dEQP-VK.api.smoke.asm\ndEQP-VK.api.smoke.triangle\n":
            raise SystemExit("selftest: caselist is not the sorted slice")

        # Tampered caselist and tampered manifest each refuse.
        (out / "api.txt").write_text("dEQP-VK.api.smoke.asm\n")
        expect(lambda: verify_manifest(out / "partition_manifest.json"),
               "does not match its recorded count")
        mp = out / "partition_manifest.json"
        mp.write_text(mp.read_text().replace('"exhaustive"', '"pilot"', 1))
        expect(lambda: verify_manifest(mp), "digest does not match")

        # Uncovered: exhaustive refuses, pilot admits and records.
        write_table(good[:3])
        expect(lambda: generate(table, corpus, out, "exhaustive", False),
               "belong to no slice")
        m = generate(table, corpus, out / "pilot", "pilot")
        if m["uncovered_case_count"] != 1 or m["exact_cover"] or \
                not (out / "pilot" / "uncovered.txt").is_file():
            raise SystemExit("selftest: pilot did not record the uncovered "
                             "case")
        # Double cover refuses in both kinds.
        write_table(good + [("5", "again", "dEQP-VK.api", "none",
                             "host-model")])
        expect(lambda: generate(table, corpus, out, "pilot", False),
               "is covered by slices")
        # A group claiming nothing refuses.
        write_table(good + [("5", "ghost", "dEQP-VK.ghost", "none",
                             "host-model")])
        expect(lambda: generate(table, corpus, out, "exhaustive", False),
               "claims no corpus case")
        # Table defects.
        write_table(good + [("5", "bad", "dEQP-VK.x", "submission",
                             "host-model")])
        expect(lambda: generate(table, corpus, out, "exhaustive", False),
               "requires silicon")
        write_table(good + [("6", "gap", "dEQP-VK.x", "none", "host-model")])
        expect(lambda: generate(table, corpus, out, "exhaustive", False),
               "order is not 1..N")
        write_table(good + [("5", "info", "dEQP-VK.x", "none", "host-model")])
        expect(lambda: generate(table, corpus, out, "exhaustive", False),
               "repeats")
        write_table(good)
        expect(lambda: generate(table, corpus, out, "weekly", False),
               "is not one of")
        # A case repeated across corpus files refuses.
        (corpus / "sub" / "c.txt").write_text("dEQP-VK.draw.basic\n")
        expect(lambda: generate(table, corpus, out, "exhaustive", False),
               "is listed in")
    print("selftest: exact cover, blocked unknown hazard, caselist binding, "
          "tampered caselist, tampered manifest, uncovered (pilot vs "
          "exhaustive), double cover, empty group, table defects, and "
          "corpus duplicate each hold")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("generate", "check"):
        s = sub.add_parser(name)
        s.add_argument("--partition", required=True)
        s.add_argument("--mustpass-dir")
        s.add_argument("--kind", required=True, choices=sorted(KINDS))
        if name == "generate":
            s.add_argument("--out", required=True)
    v = sub.add_parser("verify-manifest")
    v.add_argument("--manifest", required=True)
    sub.add_parser("selftest")
    args = p.parse_args()
    try:
        if args.cmd == "selftest":
            selftest()
        elif args.cmd == "verify-manifest":
            report(verify_manifest(args.manifest))
        else:
            corpus = args.mustpass_dir or os.environ.get("R3V_DEQP_MUSTPASS_DIR")
            if not corpus:
                load_partition(args.partition)
                print("partition table well-formed; corpus clause not run "
                      "(no mustpass directory named)")
                return
            m = generate(args.partition, corpus,
                         getattr(args, "out", None), args.kind,
                         write=args.cmd == "generate")
            report(m)
    except PartitionRefusal as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
