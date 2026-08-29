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
MANIFEST_VERSION = 3
# A shard is the recovery unit one dEQP process runs; a slice above the
# shard ceiling splits into consecutive shards, each with its own
# caselist and digest, so the recovery unit and the identity unit are
# one object.
SHARD_MAX_CASES = 20000


class PartitionRefusal(Exception):
    pass


def output_safe_slice_basename(name):
    return name not in (".", "..") and \
        "/" not in name and "\\" not in name and \
        all(character.isalnum() or character in "._-" for character in name)


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
    names = [str(f.relative_to(root)) for f in files]
    for f, name in zip(files, names):
        for line in f.read_text().splitlines():
            c = line.strip()
            if not c.startswith("dEQP-VK."):
                continue
            if c in seen:
                raise PartitionRefusal(f"{c} is listed in {seen[c]} and {name}")
            seen[c] = name
    if not seen:
        raise PartitionRefusal(f"{mustpass_dir} lists no dEQP-VK case")
    return sorted(seen), names


def load_corpus_pin(path):
    """The checked-in corpus pin: CTS describe, unique case count, and
    corpus digest.  A corpus that differs from the pin is another
    denominator, so a check against it refuses."""
    pin = {}
    for n, line in enumerate(Path(path).read_text().splitlines(), start=1):
        if not line or line.startswith("#"):
            continue
        k, sep, v = line.partition("\t")
        if not sep or k not in ("cts_describe", "case_count", "corpus_sha256"):
            raise PartitionRefusal(f"{path}:{n} is not a pin row")
        pin[k] = v
    if set(pin) != {"cts_describe", "case_count", "corpus_sha256"}:
        raise PartitionRefusal(f"{path} lacks a pin row")
    return pin


def check_corpus_pin(pin, cases):
    if len(cases) != int(pin["case_count"]) or \
            sha256_lines(cases) != pin["corpus_sha256"]:
        raise PartitionRefusal(
            f"corpus ({len(cases)} cases, {sha256_lines(cases)[:12]}) is not "
            f"the pinned {pin['cts_describe']} corpus ({pin['case_count']} "
            f"cases, {pin['corpus_sha256'][:12]})")


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
        if not output_safe_slice_basename(name):
            raise PartitionRefusal(f"{path}:{n}: slice {name!r} is not an "
                                   "output-safe basename")
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


def shard_lines(lines, shard_max):
    return [lines[i:i + shard_max] for i in range(0, len(lines), shard_max)]


def build_manifest(kind, table_path, cases, files, rows, assigned,
                   uncovered, shard_max=SHARD_MAX_CASES):
    slices = []
    for r in rows:
        lines = assigned[r["slice"]]
        shards = []
        for i, part in enumerate(shard_lines(lines, shard_max)):
            shards.append({"index": i, "caselist": f"{r['slice']}.{i:04d}.txt",
                           "case_count": len(part),
                           "caselist_sha256": sha256_lines(part)})
        slices.append({
            "order": r["order"], "slice": r["slice"], "groups": r["groups"],
            "hazard": r["hazard"], "required_evidence": r["required_evidence"],
            "executable": r["hazard"] != "unknown",
            "case_count": len(lines), "caselist_sha256": sha256_lines(lines),
            "caselist": f"{r['slice']}.txt",
            "shard_max_cases": shard_max, "shard_count": len(shards),
            "shards": shards,
        })
    covered = sum(s["case_count"] for s in slices)
    executable = sum(s["case_count"] for s in slices if s["executable"])
    manifest = {
        "manifest_version": MANIFEST_VERSION,
        "kind": kind,
        "partition_table": Path(table_path).name,
        "partition_table_sha256": sha256_file(table_path),
        "corpus_files": files,
        "corpus_case_count": len(cases),
        "corpus_sha256": sha256_lines(cases),
        "covered_case_count": covered,
        "executable_case_count": executable,
        "uncovered_case_count": len(uncovered),
        "exact_cover": kind == "exhaustive" and covered == len(cases),
        "slices": slices,
    }
    manifest["manifest_sha256"] = manifest_digest(manifest)
    return manifest


def manifest_digest(manifest):
    body = json.dumps({k: v for k, v in manifest.items()
                       if k != "manifest_sha256"}, sort_keys=True,
                      separators=(",", ":")).encode()
    return hashlib.sha256(body).hexdigest()


def generate(table_path, mustpass_dir, out_dir, kind, write=True,
             pin_path=None, shard_max=SHARD_MAX_CASES):
    if shard_max < 1:
        raise PartitionRefusal("shard ceiling must be at least 1")
    if kind not in KINDS:
        raise PartitionRefusal(f"kind {kind!r} is not one of {sorted(KINDS)}")
    rows = load_partition(table_path)
    pin = load_corpus_pin(pin_path) if pin_path else None
    cases, files = read_corpus(mustpass_dir)
    if pin:
        check_corpus_pin(pin, cases)
    assigned, uncovered = partition(cases, rows, kind)
    manifest = build_manifest(kind, table_path, cases, files, rows,
                              assigned, uncovered, shard_max)
    if pin:
        manifest["cts_describe"] = pin["cts_describe"]
        manifest["manifest_sha256"] = manifest_digest(manifest)
    if write:
        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)
        for s in manifest["slices"]:
            p = out / s["caselist"]
            p.write_text("\n".join(assigned[s["slice"]]) + "\n")
            if sha256_file(p) != s["caselist_sha256"]:
                raise PartitionRefusal(f"{p} digest drifted after write")
            for sh, part in zip(s["shards"],
                                shard_lines(assigned[s["slice"]], shard_max)):
                q = out / sh["caselist"]
                q.write_text("\n".join(part) + "\n")
                if sha256_file(q) != sh["caselist_sha256"]:
                    raise PartitionRefusal(f"{q} digest drifted after write")
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
    if manifest["kind"] == "exhaustive" and not manifest.get("cts_describe"):
        raise PartitionRefusal("an exhaustive manifest names no pinned CTS "
                               "revision")
    if manifest_digest(manifest) != manifest.get("manifest_sha256"):
        raise PartitionRefusal("manifest digest does not match its body")
    total = 0
    executable = 0
    for s in manifest["slices"]:
        if s.get("hazard") not in HAZARDS or \
                s.get("executable") != (s["hazard"] != "unknown"):
            raise PartitionRefusal(f"slice {s.get('slice')}: executable flag "
                                   "does not derive from its hazard")
        executable += s["case_count"] if s["executable"] else 0
        f = p.parent / s["caselist"]
        if not f.is_file():
            raise PartitionRefusal(f"caselist {s['caselist']} is missing")
        lines = [line for line in f.read_text().splitlines() if line]
        if len(lines) != s["case_count"] or \
                sha256_file(f) != s["caselist_sha256"]:
            raise PartitionRefusal(f"caselist {s['caselist']} does not match "
                                   "its recorded count and digest")
        shards = s.get("shards", [])
        if len(shards) != s.get("shard_count") or \
                sum(sh["case_count"] for sh in shards) != s["case_count"] or \
                [sh["index"] for sh in shards] != list(range(len(shards))) or \
                any(sh["case_count"] > s.get("shard_max_cases", 0)
                    for sh in shards):
            raise PartitionRefusal(f"slice {s['slice']}: shards do not "
                                   "reconcile with the slice")
        for sh in shards:
            q = p.parent / sh["caselist"]
            if not q.is_file() or sha256_file(q) != sh["caselist_sha256"] or \
                    len([x for x in q.read_text().splitlines() if x]) != \
                    sh["case_count"]:
                raise PartitionRefusal(f"shard {sh['caselist']} does not "
                                       "match its recorded count and digest")
        total += s["case_count"]
    if total != manifest["covered_case_count"] or \
            executable != manifest.get("executable_case_count"):
        raise PartitionRefusal("slice counts do not sum to the covered and "
                               "executable counts")
    if manifest["kind"] == "exhaustive" and (
            not manifest["exact_cover"] or
            manifest["covered_case_count"] != manifest["corpus_case_count"] or
            manifest["uncovered_case_count"] != 0):
        raise PartitionRefusal("an exhaustive manifest without exact cover")
    return manifest


def slice_for_caselist(manifest, caselist_path):
    """The manifest slice and shard whose digest equals the caselist's,
    or a refusal: a caselist outside the manifest carries no slice
    identity.  A whole-slice caselist binds when the slice is one shard;
    otherwise the shard caselist is the bound unit."""
    digest = sha256_file(caselist_path)
    for s in manifest["slices"]:
        for sh in s.get("shards", []):
            if sh["caselist_sha256"] == digest:
                return s, sh
    raise PartitionRefusal(f"{caselist_path} matches no manifest shard "
                           "digest")


def bind_caselist(manifest_path, manifest, caselist_path):
    """The slice, shard, and subset identity a caselist binds to.  A
    caselist whose digest equals a shard's binds as that whole shard
    (subset None).  Otherwise every case of the caselist must belong to
    one shard of the verified manifest, and the caselist binds as a
    proper subset of that shard: a one-case planning or replay run keeps
    the slice's hazard, evidence requirement, and corpus identity while
    the receipt records the subset's own count and digest.  An empty
    caselist, a duplicated case, a case outside every shard, and a
    caselist spanning two shards each refuse by name; the shard files
    are read beside the manifest, whose verification already proved
    their digests."""
    try:
        return (*slice_for_caselist(manifest, caselist_path), None)
    except PartitionRefusal:
        pass
    lines = [x for x in Path(caselist_path).read_text().splitlines() if x]
    if not lines:
        raise PartitionRefusal(f"{caselist_path} is empty")
    if len(set(lines)) != len(lines):
        raise PartitionRefusal(f"{caselist_path} repeats a case")
    wanted = set(lines)
    root = Path(manifest_path).parent
    owner = None
    for s in manifest["slices"]:
        for sh in s.get("shards", []):
            members = set(x for x in (root / sh["caselist"]).read_text()
                          .splitlines() if x)
            hit = wanted & members
            if not hit:
                continue
            if owner is not None:
                raise PartitionRefusal(
                    f"{caselist_path} spans shards "
                    f"{owner[1]['caselist']} and {sh['caselist']}")
            owner = (s, sh)
            wanted -= hit
    if owner is None or wanted:
        stray = sorted(wanted)[0] if wanted else lines[0]
        raise PartitionRefusal(f"{caselist_path}: case {stray} is outside "
                               "every manifest shard")
    subset = {"case_count": len(lines),
              "caselist_sha256": sha256_file(caselist_path)}
    return owner[0], owner[1], subset


def report(manifest):
    print(f"{manifest['kind']} partition: {manifest['corpus_case_count']} "
          f"corpus cases, {manifest['covered_case_count']} covered in "
          f"{len(manifest['slices'])} slices, "
          f"{manifest['executable_case_count']} executable, "
          f"{manifest['uncovered_case_count']} uncovered, corpus "
          f"{manifest['corpus_sha256'][:12]}, manifest "
          f"{manifest['manifest_sha256'][:12]}")
    for s in manifest["slices"]:
        flag = "" if s["executable"] else " BLOCKED"
        print(f"  {s['order']:2d} {s['slice']:<22} {s['case_count']:>8} "
              f"{s['hazard']:<10} {s['caselist_sha256'][:12]} "
              f"{s['shard_count']:>3} shards{flag}")


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
        if m["slices"][0]["shard_count"] != 1 or \
                m["slices"][0]["shards"][0]["caselist_sha256"] != \
                m["slices"][0]["caselist_sha256"]:
            raise SystemExit("selftest: a one-shard slice's shard digest is "
                             "not the slice digest")
        expect(lambda: verify_manifest(out / "partition_manifest.json"),
               "names no pinned CTS revision")
        pin0 = d / "pin0.tsv"
        pin0.write_text(f"cts_describe\tfixture\ncase_count\t5\n"
                        f"corpus_sha256\t{m['corpus_sha256']}\n")
        m = generate(table, corpus, out, "exhaustive", pin_path=pin0)
        if m["corpus_case_count"] != 5 or not m["exact_cover"] or \
                [s["case_count"] for s in m["slices"]] != [1, 2, 1, 1]:
            raise SystemExit("selftest: exhaustive cover miscounted")
        if m["slices"][3]["executable"] or not m["slices"][0]["executable"]:
            raise SystemExit("selftest: unknown hazard is not blocked")
        v = verify_manifest(out / "partition_manifest.json")
        bound, shard = slice_for_caselist(v, out / "api.txt")
        if bound["slice"] != "api" or shard["index"] != 0 or \
                bound["shard_count"] != 1:
            raise SystemExit("selftest: caselist did not bind to its slice")
        # A slice above the shard ceiling splits into consecutive shards
        # that reconcile with the slice and each bind on their own.
        ms = generate(table, corpus, out / "sharded", "exhaustive",
                      pin_path=pin0, shard_max=1)
        api = ms["slices"][1]
        if api["shard_count"] != 2 or \
                [sh["case_count"] for sh in api["shards"]] != [1, 1]:
            raise SystemExit("selftest: shards did not split the slice")
        vs = verify_manifest(out / "sharded" / "partition_manifest.json")
        b1, sh1 = slice_for_caselist(vs, out / "sharded" / "api.0001.txt")
        if b1["slice"] != "api" or sh1["index"] != 1:
            raise SystemExit("selftest: shard did not bind")
        expect(lambda: slice_for_caselist(vs, out / "sharded" / "api.txt"),
               "matches no manifest shard")
        (out / "sharded" / "api.0001.txt").write_text("dEQP-VK.api.smoke.x\n")
        expect(lambda: verify_manifest(out / "sharded" /
                                       "partition_manifest.json"),
               "shard api.0001.txt does not match")
        expect(lambda: generate(table, corpus, out / "z", "exhaustive",
                                False, pin_path=pin0, shard_max=0),
               "at least 1")
        expect(lambda: slice_for_caselist(v, corpus / "a.txt"),
               "matches no manifest shard")
        # A subset of one shard binds to that shard with its own count
        # and digest; a whole shard binds with no subset; an empty, a
        # repeated, a stray, and a shard-spanning caselist each refuse.
        sub = d / "sub.txt"
        sub.write_text("dEQP-VK.api.smoke.triangle\n")
        bs, shs, subset = bind_caselist(
            out / "partition_manifest.json", v, sub)
        if bs["slice"] != "api" or shs["index"] != 0 or \
                subset != {"case_count": 1,
                           "caselist_sha256": sha256_file(sub)}:
            raise SystemExit("selftest: subset did not bind to its shard")
        if bind_caselist(out / "partition_manifest.json", v,
                         out / "api.txt")[2] is not None:
            raise SystemExit("selftest: a whole shard bound as a subset")
        sub.write_text("")
        expect(lambda: bind_caselist(out / "partition_manifest.json", v,
                                     sub), "is empty")
        sub.write_text("dEQP-VK.api.smoke.triangle\n"
                       "dEQP-VK.api.smoke.triangle\n")
        expect(lambda: bind_caselist(out / "partition_manifest.json", v,
                                     sub), "repeats a case")
        sub.write_text("dEQP-VK.api.smoke.triangle\ndEQP-VK.nowhere\n")
        expect(lambda: bind_caselist(out / "partition_manifest.json", v,
                                     sub), "outside every manifest shard")
        sub.write_text("dEQP-VK.api.smoke.triangle\ndEQP-VK.info.build\n")
        expect(lambda: bind_caselist(out / "partition_manifest.json", v,
                                     sub), "spans shards")
        # Under the sharded manifest the two smoke cases sit in different
        # shards (api.0001.txt is tampered above, so the triangle sits in
        # no shard on disk and the pair refuses as stray).
        sub.write_text("dEQP-VK.api.smoke.asm\ndEQP-VK.api.smoke.triangle\n")
        expect(lambda: bind_caselist(
            out / "sharded" / "partition_manifest.json", vs, sub),
            "outside every manifest shard")
        if (out / "api.txt").read_text() != \
                "dEQP-VK.api.smoke.asm\ndEQP-VK.api.smoke.triangle\n":
            raise SystemExit("selftest: caselist is not the sorted slice")
        if m["executable_case_count"] != 4:
            raise SystemExit("selftest: executable count is not the sum of "
                             "executable slices")
        # Group matching is a dotted-namespace prefix: equality and
        # continuation hit; substrings, siblings, and mid-path miss.
        index = group_index([{"slice": "s", "groups": ["dEQP-VK.api"]}])
        hits = [c for c in ("dEQP-VK.api", "dEQP-VK.api.x", "dEQP-VK.apix",
                            "dEQP-VK.apixyz.x", "dEQP-VK.b.api.x")
                if matching_groups(c, index)]
        if hits != ["dEQP-VK.api", "dEQP-VK.api.x"]:
            raise SystemExit(f"selftest: prefix matching hit {hits}")
        # Corpus pin: the pinned corpus admits, another refuses.
        pin = d / "pin.tsv"
        pin.write_text(f"cts_describe\tfixture\ncase_count\t5\n"
                       f"corpus_sha256\t{m['corpus_sha256']}\n")
        pm = generate(table, corpus, out, "exhaustive", False, pin_path=pin)
        if pm.get("cts_describe") != "fixture" or \
                manifest_digest(pm) != pm["manifest_sha256"]:
            raise SystemExit("selftest: pin did not enter the manifest")
        pin.write_text("cts_describe\tfixture\ncase_count\t6\n"
                       f"corpus_sha256\t{m['corpus_sha256']}\n")
        expect(lambda: generate(table, corpus, out, "exhaustive", False,
                                pin_path=pin), "is not the pinned")
        pin.write_text("cts_describe\tfixture\n")
        expect(lambda: generate(table, corpus, out, "exhaustive", False,
                                pin_path=pin), "lacks a pin row")
        # Manifest tampers the self-digest alone does not catch are
        # refused by their own clauses once the digest is re-sealed.
        mp = out / "partition_manifest.json"

        def tampered(edit):
            mm = json.loads(mp.read_text())
            edit(mm)
            mm["manifest_sha256"] = manifest_digest(mm)
            mp.write_text(json.dumps(mm))
            return mm

        good_text = mp.read_text()

        def flip(k, v):
            def e(mm):
                mm[k] = v
            return e

        for edit, needle in (
                (lambda mm: mm["slices"][3].__setitem__("executable", True),
                 "does not derive"),
                (flip("executable_case_count", 5), "executable counts"),
                (flip("exact_cover", False), "without exact cover"),
                (flip("manifest_version", 2), "version unknown"),
                (lambda mm: mm["slices"][0].__setitem__("shard_count", 2),
                 "shards do not reconcile"),
                (flip("cts_describe", ""), "names no pinned"),
                (flip("kind", "weekly"), "kind unknown"),
                (lambda mm: mm["slices"][0].__setitem__("caselist", "x.txt"),
                 "is missing")):
            tampered(edit)
            expect(lambda: verify_manifest(mp), needle)
            mp.write_text(good_text)

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
        write_table(good + [("5", "../escaped", "dEQP-VK.x", "none",
                             "host-model")])
        expect(lambda: generate(table, corpus, out, "pilot", False),
               "output-safe basename")
        write_table(good)
        expect(lambda: generate(table, corpus, out, "weekly", False),
               "is not one of")
        for bad, needle in (
                ("dEQP-VK.api.", "is not a group"),
                ("dEQP-VK..api", "is not a group"),
                ("dEQP-GL.api", "is not a group")):
            write_table(good + [("5", "syn", bad, "none", "host-model")])
            expect(lambda: generate(table, corpus, out, "pilot", False),
                   needle)
        write_table(good + [("5", "hz", "dEQP-VK.x", "fire", "silicon")])
        expect(lambda: generate(table, corpus, out, "pilot", False),
               "hazard 'fire' unknown")
        write_table(good + [("5", "ev", "dEQP-VK.x", "none", "moon")])
        expect(lambda: generate(table, corpus, out, "pilot", False),
               "evidence 'moon' unknown")
        write_table(good + [("5", "short", "dEQP-VK.x", "none")])
        expect(lambda: generate(table, corpus, out, "pilot", False),
               "five-field")
        table.write_text("order\tslice\n1\ta\n")
        expect(lambda: generate(table, corpus, out, "pilot", False),
               "header is not")
        write_table(good)
        empty = d / "empty"
        empty.mkdir()
        expect(lambda: generate(table, empty, out, "pilot", False),
               "holds no mustpass files")
        (empty / "z.txt").write_text("# nothing\n")
        expect(lambda: generate(table, empty, out, "pilot", False),
               "lists no dEQP-VK case")
        # A case repeated across corpus files refuses.
        (corpus / "sub" / "c.txt").write_text("dEQP-VK.draw.basic\n")
        expect(lambda: generate(table, corpus, out, "exhaustive", False),
               "is listed in")
    print("selftest: exact cover, shards (split, bind, tamper), executable "
          "count, prefix matching, "
          "corpus pin, blocked unknown hazard, caselist binding, tampered "
          "caselist, tampered and re-sealed manifest clauses, uncovered "
          "(pilot vs exhaustive), double cover, empty group, group syntax, "
          "hazard/evidence/field/header defects, empty corpus, and corpus "
          "duplicate each hold")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("generate", "check"):
        s = sub.add_parser(name)
        s.add_argument("--partition", required=True)
        s.add_argument("--mustpass-dir")
        s.add_argument("--kind", required=True, choices=sorted(KINDS))
        s.add_argument("--corpus-pin")
        if name == "generate":
            s.add_argument("--out", required=True)
        s.add_argument("--shard-max-cases", type=int,
                       default=SHARD_MAX_CASES)
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
                if args.corpus_pin:
                    load_corpus_pin(args.corpus_pin)
                print("partition table well-formed; corpus clause not run "
                      "(no mustpass directory named)")
                sys.exit(77)
            m = generate(args.partition, corpus,
                         getattr(args, "out", None), args.kind,
                         write=args.cmd == "generate",
                         pin_path=args.corpus_pin,
                         shard_max=args.shard_max_cases)
            report(m)
    except PartitionRefusal as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
