# SPDX-License-Identifier: MIT
"""Identity-retaining dEQP-VK conformance runner for the R3V ICD.

A conformance result is decision-grade only when it binds to one exact
configuration, so the runner records the identity before the first case
and seals it with the results: source SHA and cleanliness, Meson build
options, ICD manifest and DSO digest, dEQP binary digest and CTS release
name, kernel release and radeon module srcversion, installed package
versions, GPU PCI identity, boot id, the exact environment the driver
reads, the case list, per-case status, dmesg delta, and timeouts.  The
evidence class is derived rather than declared: a preloaded drm-shim
makes the run host-model, and only a run with no preload on a host whose
render node is the RS4xx PCI device is silicon.

Verdicts are finite.  NotSupported never counts as a pass.  Every
non-pass status is classified against the non-pass ledger; a status the
ledger does not classify makes the run's verdict `unclassified_nonpass`.
A dEQP process ending without `#endSession` truncates the run: the case
in flight becomes `crash` (signal exit) or `timeout` (runner deadline)
and every remaining case is `not_run`.  A dmesg delta matching a hazard
pattern marks the run `dmesg_hazard`, and the ICD DSO digest must equal
the expected digest when one is given, else the run refuses before
launch as `wrong_icd`.

Usage:
  r3v_conformance_runner.py run --deqp-binary BIN --icd MANIFEST \
      --caselist FILE --out DIR [--source-root DIR] [--build-root DIR] \
      [--expect-dso-sha256 HEX] [--timeout SEC] [--case-timeout SEC] \
      [--nonpass-ledger TSV] [--dmesg-command CMD] [--env KEY=VALUE]...
  r3v_conformance_runner.py selftest
  r3v_conformance_runner.py check-ledgers --nonpass-ledger TSV --slices TSV
"""

import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

RECEIPT_VERSION = 1

PASS_STATUS = {"Pass"}
ACCEPTED_NONPASS = {"NotSupported"}
DEQP_STATUSES = {"Pass", "Fail", "QualityWarning", "CompatibilityWarning",
                 "Pending", "NotSupported", "ResourceError", "InternalError",
                 "Crash", "Timeout", "Waiver"}
RUNNER_STATUSES = {"crash", "timeout", "not_run", "truncated"}

DRIVER_ENV_PREFIXES = ("R3V_", "VK_", "RADEON_", "LD_PRELOAD", "MESA_",
                       "DISPLAY", "XDG_RUNTIME_DIR")
DMESG_HAZARD_PATTERNS = [
    r"GPU lockup", r"ring \d+ stalled", r"\[drm:.*\] \*ERROR\*",
    r"radeon.*CS.*(invalid|failed|error)", r"GPU reset",
    r"Unable to handle kernel", r"BUG:", r"Oops:", r"soft lockup",
]
RS4XX_PCI_DEVICES = {"0x5954", "0x5955", "0x5974", "0x5975"}


class RunnerRefusal(Exception):
    pass


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run_capture(argv, cwd=None):
    try:
        p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True,
                           timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, str(e)
    return p.returncode, p.stdout.strip()


def read_optional(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def source_identity(root):
    if root is None:
        return {"available": False}
    rc, sha = run_capture(["git", "rev-parse", "HEAD"], cwd=root)
    if rc != 0:
        return {"available": False, "error": sha}
    _, status = run_capture(["git", "status", "--porcelain=v2"], cwd=root)
    return {"available": True, "sha": sha, "clean": status == "",
            "dirty_entries": len(status.splitlines())}


def build_identity(root):
    if root is None:
        return {"available": False}
    opts = Path(root) / "meson-info" / "intro-buildoptions.json"
    if not opts.is_file():
        return {"available": False, "error": f"{opts} absent"}
    data = json.loads(opts.read_text())
    selected = {o["name"]: o["value"] for o in data
                if o.get("section") in ("user", "core") and
                o["name"] in ("buildtype", "vulkan-drivers", "gallium-drivers",
                              "werror", "b_sanitize", "tools", "optimization")}
    return {"available": True, "options": selected,
            "buildoptions_sha256": sha256_file(opts)}


def icd_identity(manifest):
    data = json.loads(Path(manifest).read_text())
    lib = data.get("ICD", {}).get("library_path")
    if not lib:
        raise RunnerRefusal(f"{manifest} names no ICD.library_path")
    lib_path = Path(lib)
    if not lib_path.is_absolute():
        lib_path = Path(manifest).resolve().parent / lib_path
    if not lib_path.is_file():
        raise RunnerRefusal(f"ICD library {lib_path} does not exist")
    return {"manifest": str(Path(manifest).resolve()),
            "library_path": str(lib_path),
            "dso_sha256": sha256_file(lib_path),
            "api_version": data.get("ICD", {}).get("api_version")}


def deqp_identity(binary):
    b = Path(binary)
    if not b.is_file():
        raise RunnerRefusal(f"dEQP binary {binary} does not exist")
    ident = {"binary": str(b.resolve()), "sha256": sha256_file(b)}
    repo = b.resolve()
    for _ in range(8):
        repo = repo.parent
        if (repo / ".git").exists():
            rc, desc = run_capture(["git", "describe", "--tags", "--always",
                                    "--dirty"], cwd=repo)
            ident["cts_git"] = desc if rc == 0 else None
            break
    return ident


def host_identity():
    ident = {"kernel_release": platform.release(),
             "machine": platform.machine(),
             "boot_id": read_optional("/proc/sys/kernel/random/boot_id"),
             "radeon_srcversion":
                 read_optional("/sys/module/radeon/srcversion"),
             "hostname_sha256":
                 hashlib.sha256(platform.node().encode()).hexdigest()[:16]}
    rc, pkgs = run_capture(["pacman", "-Q"])
    if rc == 0:
        ident["packages"] = sorted(
            l for l in pkgs.splitlines()
            if any(k in l for k in ("mesa", "vulkan", "radeon", "linux")))
    gpus = []
    for dev in sorted(Path("/sys/bus/pci/devices").glob("*")):
        cls = read_optional(dev / "class") or ""
        if cls.startswith("0x03"):
            gpus.append({"slot": dev.name,
                         "vendor": read_optional(dev / "vendor"),
                         "device": read_optional(dev / "device"),
                         "revision": read_optional(dev / "revision"),
                         "subsystem_vendor":
                             read_optional(dev / "subsystem_vendor"),
                         "subsystem_device":
                             read_optional(dev / "subsystem_device")})
    ident["gpus"] = gpus
    return ident


def environment_identity(env):
    return {k: v for k, v in sorted(env.items())
            if k.startswith(DRIVER_ENV_PREFIXES)}


def evidence_class(env, host):
    preload = env.get("LD_PRELOAD", "")
    if "drm_shim" in preload or "drm-shim" in preload:
        return "host-model"
    rs4xx = [g for g in host["gpus"] if g["vendor"] == "0x1002" and
             g["device"] in RS4XX_PCI_DEVICES]
    return "silicon" if rs4xx else "host-unknown"


def read_dmesg(command):
    if not command:
        return None
    rc, out = run_capture(shlex.split(command))
    if rc != 0:
        return None
    return out.splitlines()


def dmesg_delta(before, after):
    if before is None or after is None:
        return None
    seen = set(before)
    return [l for l in after if l not in seen]


def hazard_lines(delta):
    if not delta:
        return []
    return [l for l in delta
            if any(re.search(p, l) for p in DMESG_HAZARD_PATTERNS)]


def parse_qpa(text):
    """Per-case status from a qpa log; the in-flight case is reported
    separately when the log ends without its end marker."""
    results = {}
    in_flight = None
    for line in text.splitlines():
        if line.startswith("#beginTestCaseResult "):
            in_flight = line.split(" ", 1)[1].strip()
            results[in_flight] = {"status": "truncated", "detail": ""}
        elif line.startswith("#endTestCaseResult"):
            in_flight = None
        elif line.startswith("#terminateTestCaseResult"):
            parts = line.split(" ", 1)
            status = parts[1].strip() if len(parts) > 1 else "Crash"
            if in_flight:
                results[in_flight] = {"status": status, "detail":
                                      "terminated"}
            in_flight = None
        elif in_flight and "<Result StatusCode=" in line:
            m = re.search(r'StatusCode="(\w+)">(.*?)</Result>', line)
            if m:
                results[in_flight] = {"status": m.group(1),
                                      "detail": m.group(2)[:400]}
    session_closed = "#endSession" in text
    return results, in_flight, session_closed


def load_nonpass_ledger(path):
    rows = []
    if path is None:
        return rows
    lines = Path(path).read_text().splitlines()
    header = lines[0].split("\t")
    expected = ["class", "status", "case_pattern", "detail_pattern",
                "disposition", "authority"]
    if header != expected:
        raise RunnerRefusal(f"{path} header is {header}, not {expected}")
    for n, line in enumerate(lines[1:], start=2):
        f = line.split("\t")
        if len(f) != 6 or not all(f):
            raise RunnerRefusal(f"{path}:{n} is not a six-field row")
        rows.append({"class": f[0], "status": f[1], "pattern": f[2],
                     "detail": f[3], "disposition": f[4], "authority": f[5]})
    return rows


def classify(case, status, detail, ledger):
    """The ledger is ordered most-specific-first, so the first row whose
    status, case pattern, and result-text pattern match carries the
    class; a `-` detail pattern matches any result text, and a status
    no row names is unclassified and blocks."""
    if status in RUNNER_STATUSES:
        return f"runner_{status}", "blocks"
    for r in ledger:
        if r["status"] != status or not re.fullmatch(r["pattern"], case):
            continue
        if r["detail"] != "-" and not re.search(r["detail"], detail or ""):
            continue
        return r["class"], r["disposition"]
    return "unclassified", "blocks"


def summarize(results, ledger):
    counts = {}
    classes = {}
    blocking = 0
    for case, r in sorted(results.items()):
        st = r["status"]
        counts[st] = counts.get(st, 0) + 1
        if st in PASS_STATUS:
            continue
        cls, disp = classify(case, st, r.get("detail", ""), ledger)
        r["class"] = cls
        r["disposition"] = disp
        classes[cls] = classes.get(cls, 0) + 1
        if disp == "blocks":
            blocking += 1
    return counts, classes, blocking


def verdict_for(run):
    if run["refusal"]:
        return run["refusal"]
    if run["hazard_lines"]:
        return "dmesg_hazard"
    if run.get("framework_abort"):
        return "framework_precondition"
    if not run["session_closed"]:
        return "truncated_run"
    if run["classes"].get("unclassified", 0) or run["classes"].get(
            "ambiguous", 0):
        return "unclassified_nonpass"
    if run["blocking"]:
        return "classified_nonpass"
    if run["counts"].get("Pass", 0) == 0:
        return "no_pass_observed"
    return "pass_with_accepted_nonpass" if any(
        k not in PASS_STATUS for k in run["counts"]) else "pass"


def seal(receipt):
    body = json.dumps(receipt, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(body.encode()).hexdigest()


def execute(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    for kv in args.env or []:
        k, _, v = kv.partition("=")
        env[k] = v
    cases = [l.strip() for l in Path(args.caselist).read_text().splitlines()
             if l.strip().startswith("dEQP-VK.")]
    if not cases:
        raise RunnerRefusal(f"{args.caselist} lists no dEQP-VK cases")
    host = host_identity()
    receipt = {
        "receipt_version": RECEIPT_VERSION,
        "source": source_identity(args.source_root),
        "build": build_identity(args.build_root),
        "icd": icd_identity(args.icd),
        "deqp": deqp_identity(args.deqp_binary),
        "host": host,
        "environment": environment_identity(env),
        "evidence_class": evidence_class(env, host),
        "timeouts": {"total_seconds": args.timeout,
                     "case_seconds": args.case_timeout},
        "case_count": len(cases),
        "caselist_sha256": hashlib.sha256(
            "\n".join(cases).encode()).hexdigest(),
    }
    refusal = None
    if args.expect_dso_sha256 and \
            receipt["icd"]["dso_sha256"] != args.expect_dso_sha256:
        refusal = "wrong_icd"
    if receipt["source"].get("available") and not receipt["source"]["clean"]:
        receipt["decision_grade"] = False
        receipt["decision_grade_reason"] = "source tree dirty"
    else:
        receipt["decision_grade"] = receipt["evidence_class"] != "host-unknown"
    ledger = load_nonpass_ledger(args.nonpass_ledger)
    results = {c: {"status": "not_run", "detail": ""} for c in cases}
    exit_code = None
    session_closed = False
    in_flight = None
    dmesg_before = read_dmesg(args.dmesg_command)
    log = out / "run.qpa"
    if refusal is None:
        caselist_file = out / "caselist.txt"
        caselist_file.write_text("\n".join(cases) + "\n")
        argv = [str(Path(args.deqp_binary).resolve()),
                f"--deqp-caselist-file={caselist_file}",
                f"--deqp-log-filename={log}",
                f"--deqp-watchdog=disable"] + (args.deqp_arg or [])
        env["VK_DRIVER_FILES"] = receipt["icd"]["manifest"]
        env.pop("VK_ICD_FILENAMES", None)
        receipt["environment"] = environment_identity(env)
        receipt["argv"] = argv
        started = time.monotonic()
        try:
            p = subprocess.run(argv, cwd=Path(args.deqp_binary).resolve().parent,
                               env=env, capture_output=True, text=True,
                               timeout=args.timeout)
            exit_code = p.returncode
            (out / "stdout.txt").write_text(p.stdout)
            (out / "stderr.txt").write_text(p.stderr)
        except subprocess.TimeoutExpired as e:
            exit_code = "timeout"
            (out / "stdout.txt").write_text(e.stdout.decode() if e.stdout else "")
            (out / "stderr.txt").write_text(e.stderr.decode() if e.stderr else "")
        receipt["wall_seconds"] = round(time.monotonic() - started, 3)
        if log.is_file():
            parsed, in_flight, session_closed = parse_qpa(log.read_text())
            for c, r in parsed.items():
                if c in results:
                    results[c] = r
        if in_flight:
            if exit_code == "timeout":
                results[in_flight]["status"] = "timeout"
            elif isinstance(exit_code, int) and exit_code < 0:
                results[in_flight]["status"] = "crash"
                results[in_flight]["detail"] = \
                    f"signal {signal.Signals(-exit_code).name}"
            elif isinstance(exit_code, int) and exit_code != 0:
                results[in_flight]["status"] = "crash"
                results[in_flight]["detail"] = f"exit {exit_code}"
    stderr_text = (out / "stderr.txt").read_text() if (out / "stderr.txt").is_file() else ""
    receipt["framework_abort"] = next(
        (m.group(0) for m in [re.search(r"FATAL ERROR: [^\n]*", stderr_text)] if m),
        None)
    dmesg_after = read_dmesg(args.dmesg_command)
    delta = dmesg_delta(dmesg_before, dmesg_after)
    receipt["exit_code"] = exit_code
    receipt["session_closed"] = session_closed
    receipt["refusal"] = refusal
    receipt["dmesg"] = {"available": delta is not None,
                        "delta_lines": len(delta) if delta else 0}
    if receipt["evidence_class"] == "silicon" and delta is None:
        receipt["decision_grade"] = False
        receipt["decision_grade_reason"] = "dmesg unavailable on a silicon run"
    receipt["hazard_lines"] = hazard_lines(delta)
    if delta:
        (out / "dmesg_delta.txt").write_text("\n".join(delta) + "\n")
    counts, classes, blocking = summarize(results, ledger)
    receipt["counts"] = counts
    receipt["classes"] = classes
    receipt["blocking"] = blocking
    receipt["results"] = results
    receipt["verdict"] = verdict_for(receipt)
    receipt["seal_sha256"] = seal(receipt)
    (out / "receipt.json").write_text(json.dumps(receipt, indent=1,
                                                 sort_keys=True) + "\n")
    print(f"verdict={receipt['verdict']} evidence={receipt['evidence_class']}"
          f" counts={counts} classes={classes} seal={receipt['seal_sha256'][:12]}")
    return receipt


FAKE_DEQP = r'''#!/usr/bin/env python3
import os, sys, time, signal
mode = os.environ["FAKE_DEQP_MODE"]
log = [a.split("=",1)[1] for a in sys.argv if a.startswith("--deqp-log-filename=")][0]
cl = [a.split("=",1)[1] for a in sys.argv if a.startswith("--deqp-caselist-file=")][0]
cases = [l.strip() for l in open(cl) if l.strip()]
f = open(log, "w")
f.write("#sessionInfo releaseName fake\n#beginSession\n"); f.flush()
def case(name, status, detail="x"):
    f.write(f"#beginTestCaseResult {name}\n <Result StatusCode=\"{status}\">{detail}</Result>\n#endTestCaseResult\n"); f.flush()
if mode == "mixed":
    sts = ["Pass", "NotSupported", "Fail", "Pass", "QualityWarning"]
    for i, c in enumerate(cases): case(c, sts[i % len(sts)])
elif mode == "truncated":
    case(cases[0], "Pass")
    f.write(f"#beginTestCaseResult {cases[1]}\n"); f.flush()
    sys.exit(0)
elif mode == "timeout":
    case(cases[0], "Pass")
    f.write(f"#beginTestCaseResult {cases[1]}\n"); f.flush()
    time.sleep(30)
elif mode == "crash":
    case(cases[0], "Pass")
    f.write(f"#beginTestCaseResult {cases[1]}\n"); f.flush()
    os.kill(os.getpid(), signal.SIGSEGV)
elif mode == "device_loss":
    case(cases[0], "Pass")
    case(cases[1], "Fail", "vkQueueSubmit returned VK_ERROR_DEVICE_LOST")
    f.write("#terminateTestCaseResult Crash\n")
    sys.exit(1)
elif mode == "all_pass":
    for c in cases: case(c, "Pass")
elif mode == "framework":
    sys.stderr.write("FATAL ERROR: No matching queue found: findQueueFamilyIndexWithCaps(requiredCaps=0x3, excludedCaps=0x0) at vktTestCase.cpp:508\n")
    sys.exit(1)
f.write("#endSession\n"); f.close()
'''

FAKE_DMESG = '''#!/bin/sh
cat "$FAKE_DMESG_FILE"
'''

SELFTEST_LEDGER = """class\tstatus\tcase_pattern\tdetail_pattern\tdisposition\tauthority
withheld_feature\tNotSupported\tdEQP-VK\\..*\t-\taccepted\tunimplemented optional path
quality_warning\tQualityWarning\tdEQP-VK\\..*\t-\taccepted\tdEQP quality warning
device_loss\tFail\tdEQP-VK\\.fake\\.b\tDEVICE_LOST\tblocks\tqueue loss classification
open_defect\tFail\tdEQP-VK\\.fake\\.[abd]\t-\tblocks\tcatch-all after the specific row
"""


def selftest():
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        fake = d / "deqp-vk"
        fake.write_text(FAKE_DEQP)
        fake.chmod(0o755)
        lib = d / "libvulkan_fake.so"
        lib.write_bytes(b"fake dso")
        manifest = d / "fake_icd.json"
        manifest.write_text(json.dumps({"file_format_version": "1.0.0",
                                        "ICD": {"library_path": str(lib),
                                                "api_version": "1.0.0"}}))
        caselist = d / "cases.txt"
        caselist.write_text("dEQP-VK.fake.a\ndEQP-VK.fake.b\ndEQP-VK.fake.c\n"
                            "dEQP-VK.fake.d\ndEQP-VK.fake.e\n")
        ledger = d / "ledger.tsv"
        ledger.write_text(SELFTEST_LEDGER)
        dmesg_file = d / "dmesg.txt"
        dmesg_file.write_text("[1.0] boot\n")
        dmesg_cmd = d / "dmesg.sh"
        dmesg_cmd.write_text(FAKE_DMESG)
        dmesg_cmd.chmod(0o755)

        def run(mode, expect, extra=None, dmesg_after=None, dso=None):
            os.environ["FAKE_DEQP_MODE"] = mode
            os.environ["FAKE_DMESG_FILE"] = str(dmesg_file)
            outdir = d / f"out-{mode}-{expect}"
            args = argparse.Namespace(
                deqp_binary=str(fake), icd=str(manifest),
                caselist=str(caselist), out=str(outdir), source_root=None,
                build_root=None, expect_dso_sha256=dso, timeout=5,
                case_timeout=5, nonpass_ledger=str(ledger),
                dmesg_command=str(dmesg_cmd), env=["LD_PRELOAD=drm_shim"],
                deqp_arg=None)
            if dmesg_after is not None:
                orig = dmesg_file.read_text()
                r = _run_with_dmesg_change(args, dmesg_file, orig,
                                           orig + dmesg_after)
            else:
                r = execute(args)
            if r["verdict"] != expect:
                raise SystemExit(
                    f"selftest {mode}: verdict {r['verdict']}, expected "
                    f"{expect}: {r['counts']} {r['classes']}")
            return r

        r = run("all_pass", "pass")
        assert r["counts"] == {"Pass": 5}, r["counts"]
        assert r["evidence_class"] == "host-model"
        r = run("mixed", "unclassified_nonpass")
        assert r["counts"] == {"Pass": 2, "NotSupported": 1, "Fail": 1,
                               "QualityWarning": 1}, r["counts"]
        assert r["classes"] == {"withheld_feature": 1, "quality_warning": 1,
                                "unclassified": 1}, r["classes"]
        r = run("truncated", "truncated_run")
        assert r["results"]["dEQP-VK.fake.b"]["status"] == "truncated"
        assert r["results"]["dEQP-VK.fake.c"]["status"] == "not_run"
        r = run("timeout", "truncated_run")
        assert r["results"]["dEQP-VK.fake.b"]["status"] == "timeout"
        r = run("crash", "truncated_run")
        assert r["results"]["dEQP-VK.fake.b"]["status"] == "crash"
        assert "SIGSEGV" in r["results"]["dEQP-VK.fake.b"]["detail"]
        r = run("device_loss", "truncated_run")
        assert r["results"]["dEQP-VK.fake.b"]["class"] == "device_loss"
        r = run("framework", "framework_precondition")
        assert r["counts"] == {"not_run": 5}, r["counts"]
        assert "No matching queue" in r["framework_abort"]
        r = run("all_pass", "wrong_icd", dso="0" * 64)
        assert r["counts"] == {"not_run": 5}
        assert r["classes"] == {"runner_not_run": 5}, r["classes"]
        r = run("all_pass", "dmesg_hazard",
                dmesg_after="[2.0] radeon 0000:01:05.0: GPU lockup\n")
        assert r["hazard_lines"]
    print("selftest: pass, mixed (NotSupported never a pass; Fail "
          "unclassified blocks), truncated, timeout, crash, device-loss, "
          "framework-abort, wrong-ICD, and dmesg-hazard fixtures each yield "
          "their verdict")


def _run_with_dmesg_change(args, dmesg_file, before, after):
    """The fake dmesg reads a file; swap its content between the two
    reads by wrapping read_dmesg."""
    global read_dmesg
    orig = read_dmesg
    state = {"n": 0}

    def swapped(command):
        state["n"] += 1
        dmesg_file.write_text(before if state["n"] == 1 else after)
        return orig(command)
    read_dmesg = swapped
    try:
        return execute(args)
    finally:
        read_dmesg = orig


SLICE_HEADER = ["order", "slice", "groups", "hazard", "evidence_floor"]
SLICE_HAZARDS = {"none", "submission", "display"}
SLICE_FLOORS = {"host-model", "silicon"}


def check_ledgers(nonpass_path, slices_path, mustpass_dir=None):
    """Hold both TSVs to their schemas: every ledger row compiles, the
    status-wide catch-all rows sit after the specific rows of their
    status, every slice row is well-formed with a hazard-consistent
    evidence floor, and with a mustpass corpus every slice group and
    every case pattern matches at least one case."""
    ledger = load_nonpass_ledger(nonpass_path)
    seen_catchall = set()
    for r in ledger:
        for key in ("pattern", "detail"):
            if r[key] != "-":
                try:
                    re.compile(r[key])
                except re.error as e:
                    raise RunnerRefusal(f"{r['class']}: {key} {r[key]!r}: {e}")
        if r["status"] not in DEQP_STATUSES:
            raise RunnerRefusal(f"{r['class']}: status {r['status']} is not "
                                "a dEQP status")
        if r["disposition"] not in ("accepted", "blocks"):
            raise RunnerRefusal(f"{r['class']}: disposition must be accepted "
                                "or blocks")
        catchall = r["pattern"] == r"dEQP-VK\..*" and r["detail"] == "-"
        if r["status"] in seen_catchall:
            raise RunnerRefusal(f"{r['class']}: a {r['status']} row after "
                                "that status's catch-all is unreachable")
        if catchall:
            seen_catchall.add(r["status"])
    lines = Path(slices_path).read_text().splitlines()
    if lines[0].split("\t") != SLICE_HEADER:
        raise RunnerRefusal(f"{slices_path} header is not {SLICE_HEADER}")
    orders = []
    groups = []
    for n, line in enumerate(lines[1:], start=2):
        f = line.split("\t")
        if len(f) != 5 or not all(f):
            raise RunnerRefusal(f"{slices_path}:{n} is not a five-field row")
        orders.append(int(f[0]))
        if f[3] not in SLICE_HAZARDS or f[4] not in SLICE_FLOORS:
            raise RunnerRefusal(f"{slices_path}:{n} hazard/floor unknown")
        if (f[3] == "none") != (f[4] == "host-model"):
            raise RunnerRefusal(f"{slices_path}:{n}: a hazard-free slice "
                                "runs on the host model and a hazardous "
                                "slice waits for silicon")
        for g in f[2].split(" "):
            if not re.fullmatch(r"dEQP-VK(\.[A-Za-z0-9_]+)+", g):
                raise RunnerRefusal(f"{slices_path}:{n}: {g!r} is not a group")
            groups.append(g)
    if orders != list(range(1, len(orders) + 1)):
        raise RunnerRefusal("slice order is not 1..N")
    corpus = None
    if mustpass_dir:
        cases = set()
        for f in Path(mustpass_dir).rglob("*.txt"):
            cases.update(l.strip() for l in f.read_text().splitlines()
                         if l.startswith("dEQP-VK."))
        for g in groups:
            if not any(c == g or c.startswith(g + ".") for c in cases):
                raise RunnerRefusal(f"slice group {g} matches no mustpass case")
        for r in ledger:
            if not any(re.fullmatch(r["pattern"], c) for c in cases):
                raise RunnerRefusal(f"{r['class']}: case pattern matches no "
                                    "mustpass case")
        corpus = f"{len(cases)} mustpass cases"
    print(f"ledgers hold: {len(ledger)} non-pass rows, {len(orders)} slices, "
          f"{len(groups)} groups, "
          f"{corpus or 'mustpass clause not run (no corpus named)'}")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--deqp-binary", required=True)
    r.add_argument("--icd", required=True)
    r.add_argument("--caselist", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--source-root")
    r.add_argument("--build-root")
    r.add_argument("--expect-dso-sha256")
    r.add_argument("--timeout", type=float, default=3600.0)
    r.add_argument("--case-timeout", type=float, default=600.0)
    r.add_argument("--nonpass-ledger")
    r.add_argument("--dmesg-command", default="dmesg")
    r.add_argument("--env", action="append")
    r.add_argument("--deqp-arg", action="append")
    sub.add_parser("selftest")
    c = sub.add_parser("check-ledgers")
    c.add_argument("--nonpass-ledger", required=True)
    c.add_argument("--slices", required=True)
    args = p.parse_args()
    if args.cmd == "selftest":
        selftest()
        return
    if args.cmd == "check-ledgers":
        try:
            check_ledgers(args.nonpass_ledger, args.slices,
                          os.environ.get("R3V_DEQP_MUSTPASS_DIR"))
        except RunnerRefusal as e:
            print(f"FAIL: {e}", file=sys.stderr)
            sys.exit(1)
        return
    try:
        receipt = execute(args)
    except RunnerRefusal as e:
        print(f"REFUSED: {e}", file=sys.stderr)
        sys.exit(2)
    sys.exit(0 if receipt["verdict"] in ("pass", "pass_with_accepted_nonpass")
             else 1)


if __name__ == "__main__":
    main()
