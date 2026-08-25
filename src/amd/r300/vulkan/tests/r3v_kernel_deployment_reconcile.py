#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reconcile the deployed radeon kernel module with the Mesa kernel
contract before a silicon plan replay.

A plan-bound silicon run binds to the kernel and module identity, so the
run's kernel is a declared fact, and the question before the run is
whether the deployed module carries the contract the driver's memory
model consumes.  Four identities meet here: the Mesa kernel-contract
pins (r3v_kernel_memory_contract_pins.tsv), the kernel repository head,
the source checkpoint the deployed package was built from, and the
srcversion of the module the target is running.  The tool archives the
policy ledgers at both source SHAs and runs the pin check over each,
lists every file the two SHAs differ in, and classifies each difference
from the checked-in classification table as semantic_required,
safety_required, optimization_only, or unrelated.  A file inside the
driver subtree or the policy ledgers with no classification row refuses
(`unclassified_delta`); a file outside both is `unrelated` by
construction.  A pinned row that moved between the two SHAs must be
named by an optimization_only or unrelated row for the deployed
checkpoint to stay in service.

The verdict is finite: `retain_deployed` when every row is
optimization_only or unrelated, every pin holds at the head, and the
running srcversion equals the deployed package's module srcversion;
`deploy_required` when any row is semantic_required or safety_required
or a pin at the head fails; `identity_mismatch` when the running and
installed srcversions differ, whatever the source delta says.  The
selftest calibrates each verdict and the refusal on a synthetic
repository.

Usage:
  r3v_kernel_deployment_reconcile.py --kernel-root DIR --head-sha HEX \\
      --deployed-sha HEX --deployed-package NAME-VER \\
      --installed-srcversion HEX --running-srcversion HEX \\
      [--classification TSV] [--pins TSV] [--out FILE]
  r3v_kernel_deployment_reconcile.py --selftest
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import r3v_kernel_memory_contract_pin as pins_mod  # noqa: E402

HERE = Path(__file__).resolve().parent
CLASSIFICATION = HERE / "r3v_kernel_deployment_delta_classification.tsv"
CLASS_HEADER = ["deployed_sha", "main_sha", "path", "moved_row_id",
                "class", "authority"]
CLASSES = ("semantic_required", "safety_required", "optimization_only",
           "unrelated")
BLOCKING = ("semantic_required", "safety_required")
DRIVER_SUBTREE = "drivers/gpu/drm/radeon/"
POLICY_SUBTREE = "policy/"
VERDICTS = ("retain_deployed", "deploy_required", "identity_mismatch")


class Refusal(Exception):
    pass


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True, text=True).stdout


def load_classification(path):
    rows = []
    with open(path, encoding="utf-8") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        if header != CLASS_HEADER:
            raise Refusal(f"classification header {header} != {CLASS_HEADER}")
        for n, line in enumerate(handle, 2):
            fields = line.rstrip("\n").split("\t")
            if len(fields) != len(CLASS_HEADER):
                raise Refusal(f"classification line {n}: "
                              f"{len(fields)} fields")
            row = dict(zip(CLASS_HEADER, fields))
            if row["class"] not in CLASSES:
                raise Refusal(f"classification line {n}: class "
                              f"{row['class']!r}")
            if not row["authority"]:
                raise Refusal(f"classification line {n}: empty authority")
            rows.append(row)
    return rows


def pins_at(root, sha, pin_rows):
    """The pin failures over the policy ledgers as archived at sha."""
    with tempfile.TemporaryDirectory() as d:
        archive = subprocess.run(
            ["git", "-C", str(root), "archive", "--format=tar", sha,
             POLICY_SUBTREE.rstrip("/")], check=True, capture_output=True)
        with tarfile.open(fileobj=__import__("io").BytesIO(archive.stdout)) \
                as tar:
            tar.extractall(d, filter="data")
        return pins_mod.check(d, pin_rows)


def sha_matches(full, prefix):
    return full.startswith(prefix) and len(prefix) >= 7


def reconcile(root, head_sha, deployed_sha, deployed_package,
              installed_srcversion, running_srcversion, class_rows,
              pin_rows):
    head_full = git(root, "rev-parse", "--verify", head_sha + "^{commit}") \
        .strip()
    deployed_full = git(root, "rev-parse", "--verify",
                        deployed_sha + "^{commit}").strip()
    delta = [p for p in git(root, "diff", "--name-only", deployed_full,
                            head_full).splitlines() if p]
    head_failures = pins_at(root, head_full, pin_rows)
    deployed_failures = pins_at(root, deployed_full, pin_rows)
    moved_rows = sorted({f.split(": row ")[1].split(" ")[0]
                         for f in deployed_failures if ": row " in f})
    applicable = [r for r in class_rows
                  if sha_matches(deployed_full, r["deployed_sha"]) and
                  sha_matches(head_full, r["main_sha"])]
    rows = []
    unclassified = []
    for path in delta:
        matches = [r for r in applicable if r["path"] == path]
        in_scope = path.startswith(DRIVER_SUBTREE) or \
            path.startswith(POLICY_SUBTREE)
        if matches:
            for r in matches:
                rows.append({"path": path, "class": r["class"],
                             "moved_row_id": r["moved_row_id"] or None,
                             "authority": r["authority"],
                             "scope": "driver" if path.startswith(
                                 DRIVER_SUBTREE) else "policy"
                             if in_scope else "outside"})
        elif in_scope:
            unclassified.append(path)
        else:
            rows.append({"path": path, "class": "unrelated",
                         "moved_row_id": None,
                         "authority": "outside the driver subtree and the "
                                      "policy ledgers",
                         "scope": "outside"})
    if unclassified:
        raise Refusal("unclassified_delta: " + ", ".join(unclassified))
    named_moves = {r["moved_row_id"] for r in rows if r["moved_row_id"]}
    unnamed_moves = [m for m in moved_rows if m not in named_moves]
    if unnamed_moves:
        raise Refusal("moved pinned rows with no classification: " +
                      ", ".join(unnamed_moves))
    blocking = [r for r in rows if r["class"] in BLOCKING]
    if running_srcversion != installed_srcversion:
        verdict = "identity_mismatch"
    elif blocking or head_failures:
        verdict = "deploy_required"
    else:
        verdict = "retain_deployed"
    counts = {c: sum(1 for r in rows if r["class"] == c) for c in CLASSES}
    result = {
        "schema": "r3v-kernel-deployment-reconciliation-v1",
        "kernel_repository": "linux-radeon-gororoba",
        "head_sha": head_full, "deployed_sha": deployed_full,
        "deployed_package": deployed_package,
        "installed_srcversion": installed_srcversion,
        "running_srcversion": running_srcversion,
        "pins": {"file": pins_mod.PIN_FILE and os.path.basename(
                     pins_mod.PIN_FILE), "count": len(pin_rows),
                 "head_failures": head_failures,
                 "deployed_failures": deployed_failures,
                 "moved_rows": moved_rows},
        "delta_files": delta, "rows": rows, "class_counts": counts,
        "blocking_rows": blocking, "verdict": verdict,
    }
    body = json.dumps(result, sort_keys=True, separators=(",", ":"))
    result["seal_sha256"] = hashlib.sha256(body.encode()).hexdigest()
    return result


def selftest():
    def run(*a, **k):
        return subprocess.run(a, check=True, capture_output=True, text=True,
                              **k)
    with tempfile.TemporaryDirectory() as d:
        root = Path(d) / "kernel"
        (root / "policy").mkdir(parents=True)
        (root / DRIVER_SUBTREE).mkdir(parents=True)
        run("git", "init", "-q", str(root))
        run("git", "-C", str(root), "config", "user.email", "t@t")
        run("git", "-C", str(root), "config", "user.name", "t")
        ledger = root / "policy" / "l.tsv"
        ledger.write_text("ROW_A\tone\nROW_B\ttwo\n")
        drv = root / DRIVER_SUBTREE / "o.c"
        drv.write_text("int a;\n")
        run("git", "-C", str(root), "add", "-A")
        run("git", "-C", str(root), "commit", "-q", "-m", "deployed")
        deployed = run("git", "-C", str(root), "rev-parse", "HEAD") \
            .stdout.strip()
        ledger.write_text("ROW_A\tone\nROW_B\ttwo moved\n")
        drv.write_text("int a; int b;\n")
        (root / "README").write_text("x\n")
        run("git", "-C", str(root), "add", "-A")
        run("git", "-C", str(root), "commit", "-q", "-m", "head")
        head = run("git", "-C", str(root), "rev-parse", "HEAD").stdout.strip()
        pin_rows = [("policy/l.tsv", "ROW_A", hashlib.sha256(
                        b"ROW_A\tone").hexdigest()),
                    ("policy/l.tsv", "ROW_B", hashlib.sha256(
                        b"ROW_B\ttwo moved").hexdigest())]

        def rows(cls, name_move=True):
            return [{"deployed_sha": deployed[:12], "main_sha": head[:12],
                     "path": DRIVER_SUBTREE + "o.c", "moved_row_id": "",
                     "class": cls, "authority": "a"},
                    {"deployed_sha": deployed[:12], "main_sha": head[:12],
                     "path": "policy/l.tsv",
                     "moved_row_id": "ROW_B" if name_move else "",
                     "class": cls, "authority": "b"}]

        r = reconcile(root, head, deployed, "pkg-1", "S1", "S1",
                      rows("optimization_only"), pin_rows)
        assert r["verdict"] == "retain_deployed", r["verdict"]
        assert r["pins"]["moved_rows"] == ["ROW_B"] and \
            not r["pins"]["head_failures"], r["pins"]
        assert r["class_counts"] == {"semantic_required": 0,
                                     "safety_required": 0,
                                     "optimization_only": 2,
                                     "unrelated": 1}, r["class_counts"]
        r = reconcile(root, head, deployed, "pkg-1", "S1", "S1",
                      rows("semantic_required"), pin_rows)
        assert r["verdict"] == "deploy_required" and len(
            r["blocking_rows"]) == 2
        r = reconcile(root, head, deployed, "pkg-1", "S1", "S2",
                      rows("optimization_only"), pin_rows)
        assert r["verdict"] == "identity_mismatch"
        for bad_rows, needle in ((rows("optimization_only")[1:],
                                  "unclassified_delta"),
                                 (rows("optimization_only", False),
                                  "no classification")):
            try:
                reconcile(root, head, deployed, "pkg-1", "S1", "S1",
                          bad_rows, pin_rows)
            except Refusal as e:
                assert needle in str(e), str(e)
            else:
                raise SystemExit(f"selftest: {needle} was not refused")
        # A pin that fails at the head forces deployment whatever the
        # classification says.
        stale = [("policy/l.tsv", "ROW_B",
                  hashlib.sha256(b"ROW_B\ttwo").hexdigest())]
        r = reconcile(root, head, deployed, "pkg-1", "S1", "S1",
                      rows("optimization_only"), stale)
        assert r["verdict"] == "deploy_required" and r["pins"][
            "head_failures"]
        # A classification row for other SHAs does not apply.
        foreign = [dict(x, deployed_sha="0" * 12) for x in
                   rows("optimization_only")]
        try:
            reconcile(root, head, deployed, "pkg-1", "S1", "S1", foreign,
                      pin_rows)
        except Refusal as e:
            assert "unclassified_delta" in str(e)
        else:
            raise SystemExit("selftest: foreign rows applied")
    print("selftest: retain_deployed, deploy_required (blocking row, "
          "failing head pin), identity_mismatch, unclassified delta, "
          "unnamed moved row, and foreign-SHA rows each hold")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--selftest", action="store_true")
    p.add_argument("--kernel-root")
    p.add_argument("--head-sha")
    p.add_argument("--deployed-sha")
    p.add_argument("--deployed-package")
    p.add_argument("--installed-srcversion")
    p.add_argument("--running-srcversion")
    p.add_argument("--classification", default=str(CLASSIFICATION))
    p.add_argument("--pins", default=pins_mod.PIN_FILE)
    p.add_argument("--out")
    a = p.parse_args()
    if a.selftest:
        selftest()
        return 0
    needed = ("kernel_root", "head_sha", "deployed_sha", "deployed_package",
              "installed_srcversion", "running_srcversion")
    missing = [n for n in needed if not getattr(a, n)]
    if missing:
        p.error("missing " + ", ".join("--" + n.replace("_", "-")
                                       for n in missing))
    try:
        result = reconcile(a.kernel_root, a.head_sha, a.deployed_sha,
                           a.deployed_package, a.installed_srcversion,
                           a.running_srcversion,
                           load_classification(a.classification),
                           pins_mod.load_pins(a.pins))
    except Refusal as e:
        print(f"REFUSED: {e}", file=sys.stderr)
        return 2
    text = json.dumps(result, indent=1, sort_keys=True) + "\n"
    if a.out:
        Path(a.out).write_text(text)
    print(f"verdict={result['verdict']} delta_files={len(result['delta_files'])}"
          f" classes={result['class_counts']} moved_rows="
          f"{result['pins']['moved_rows']} head_pin_failures="
          f"{len(result['pins']['head_failures'])} seal="
          f"{result['seal_sha256'][:12]}")
    return 0 if result["verdict"] == "retain_deployed" else 1


if __name__ == "__main__":
    sys.exit(main())
