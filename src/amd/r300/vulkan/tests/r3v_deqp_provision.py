#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Provision a dEQP-VK binary as a self-describing bundle.

A conformance receipt binds to a dEQP binary by digest and to the release
name the binary writes into its own log; this tool records where those
bytes came from so the digest is traceable.  ``bundle`` copies the binary,
its data directory, and the pinned mustpass corpus into one directory and
writes ``provenance.json`` beside them: source commit, ``git describe``,
tree cleanliness, the CMake cache pins that shape the binary, the
compiler, binutils, ELF64 machine, dynamic-library and symbol-version
requirements, embedded CTS release name, GNU x86 ISA-needed property,
XED ISA-set census, the functions whose instructions exceed the K8
feature set, the checked-in mustpass pin, and the mustpass and data tree
digests.  Bundle admission binds the CMake source and build directories
to the selected source and binary, validates the corpus pin, rejects an
output below any input root, and publishes through one staging rename.
``verify`` recomputes the identities and confirms that the target's
dynamic providers export every required symbol version.  The loader's
ISA-needed property stays intact; a binary above the baseline requires a
baseline relink because section removal would also erase non-ISA GNU
properties.  The self-digest detects accidental change after sealing,
not substitution by a party who re-seals.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import struct
# External analysis tools execute as argv sequences with the shell disabled.
import subprocess  # nosec B404
import sys
import tempfile
from pathlib import Path

from r3v_conformance_partition import (
    PartitionRefusal,
    check_corpus_pin,
    load_corpus_pin,
    read_corpus,
)

PROVENANCE_VERSION = 2
CACHE_PINS = (
    "CMAKE_BUILD_TYPE",
    "DEQP_TARGET",
    "CMAKE_CXX_COMPILER",
    "CMAKE_C_COMPILER",
    "CMAKE_CXX_FLAGS",
    "CMAKE_C_FLAGS",
    "CMAKE_CXX_FLAGS_RELEASE",
    "CMAKE_C_FLAGS_RELEASE",
    "CMAKE_CXX_COMPILER_VERSION",
    "CMAKE_CXX_COMPILER_ID",
    "CMAKE_HOME_DIRECTORY",
    "CMAKE_CACHEFILE_DIR",
)


class ProvisionRefusal(Exception):
    pass


class ToolUnavailable(Exception):
    """A required tool is absent, so the admission check cannot run."""


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(argv, cwd=None, env=None):
    command_environment = os.environ.copy() if env is None else env.copy()
    command_environment["LC_ALL"] = "C"
    try:
        p = subprocess.run(  # nosec B603
            argv,
            cwd=cwd,
            env=command_environment,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError:
        raise ToolUnavailable(argv[0])
    if p.returncode != 0:
        raise ProvisionRefusal(f"{argv[0]} failed: {p.stderr.strip()[:200]}")
    return p.stdout


def tree_digest(root):
    """SHA-256 over every regular file's relative path and content digest,
    in sorted order, so a directory copy proves itself as a whole."""
    root = Path(root)
    h = hashlib.sha256()
    count = 0
    for p in sorted(x for x in root.rglob("*") if x.is_file()):
        h.update(str(p.relative_to(root)).encode() + b"\0")
        h.update(sha256_file(p).encode() + b"\0")
        count += 1
    return h.hexdigest(), count


def corpus_identity(mustpass_dir, pin_path):
    try:
        cases, files = read_corpus(mustpass_dir)
        pin = load_corpus_pin(pin_path)
        check_corpus_pin(pin, cases)
    except (PartitionRefusal, OSError, ValueError) as error:
        raise ProvisionRefusal(f"mustpass corpus: {error}")
    return {
        "cts_describe": pin["cts_describe"],
        "case_count": int(pin["case_count"]),
        "corpus_sha256": pin["corpus_sha256"],
        "mustpass_files": files,
        "pin_sha256": sha256_file(pin_path),
    }


def validate_copied_corpus(mustpass_dir, pin_path, expected_identity):
    try:
        copied_identity = corpus_identity(mustpass_dir, pin_path)
    except ProvisionRefusal as error:
        raise ProvisionRefusal(f"copied mustpass corpus: {error}")
    if copied_identity != expected_identity:
        raise ProvisionRefusal(
            "copied mustpass corpus or its pin changed during bundle creation"
        )
    return copied_identity


def validate_output_boundary(out, roots):
    destination = Path(out).resolve()
    for label, root in roots:
        resolved_root = Path(root).resolve()
        if destination == resolved_root or destination.is_relative_to(resolved_root):
            raise ProvisionRefusal(
                f"bundle output {destination} is equal to or below "
                f"{label} {resolved_root}"
            )
    return destination


def source_identity(source_root):
    root = Path(source_root).resolve()
    sha = run(["git", "rev-parse", "HEAD"], root).strip()
    describe = run(["git", "describe", "--tags", "--always"], root).strip()
    status = run(["git", "status", "--porcelain=v2"], root)
    return {
        "root": str(root),
        "sha": sha,
        "describe": describe,
        "clean": status.strip() == "",
    }


def cache_pins(cache_path, source_root, binary):
    pins = {}
    for line in Path(cache_path).read_text().splitlines():
        m = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
        if m and m.group(1) in CACHE_PINS:
            pins[m.group(1)] = m.group(2)
    for key in (
        "CMAKE_BUILD_TYPE",
        "DEQP_TARGET",
        "CMAKE_CXX_COMPILER",
        "CMAKE_HOME_DIRECTORY",
        "CMAKE_CACHEFILE_DIR",
    ):
        if key not in pins:
            raise ProvisionRefusal(f"CMakeCache lacks {key}")
    cache_source = Path(pins["CMAKE_HOME_DIRECTORY"]).resolve()
    cache_build = Path(pins["CMAKE_CACHEFILE_DIR"]).resolve()
    expected_source = Path(source_root).resolve()
    selected_binary = Path(binary).resolve()
    if cache_source != expected_source:
        raise ProvisionRefusal(
            f"CMakeCache source {cache_source} does not match source root "
            f"{expected_source}"
        )
    if not selected_binary.is_relative_to(cache_build):
        raise ProvisionRefusal(
            f"binary {selected_binary} is outside CMakeCache build directory "
            f"{cache_build}"
        )
    return pins


# The Turion 64 X2 TL-66 target reports AMD Family 0Fh long mode, CX16,
# LAHF/SAHF, MMX/MMXEXT, 3DNow/3DNowExt, FXSR, SSE, SSE2, SSE3, CLFLUSH,
# PAUSE, and RDTSCP.  XED supplies the finite instruction decoder; this
# set maps those target features to XED ISA-set identities.  AMD_K10 is
# a distinct XED chip model and does not stand in for Family 0Fh.
K8_XED_ISA_SETS = {
    "3DNOW",
    "CLFSH",
    "CMOV",
    "CMPXCHG16B",
    "FAT_NOP",
    "FCMOV",
    "FCOMI",
    "FXSAVE",
    "FXSAVE64",
    "I186",
    "I286PROTECTED",
    "I286REAL",
    "I386",
    "I486",
    "I486REAL",
    "I86",
    "LAHF",
    "LONGMODE",
    "PAUSE",
    "PENTIUMMMX",
    "PENTIUMREAL",
    "PPRO",
    "PREFETCH_NOP",
    "RDPMC",
    "RDTSCP",
    "SEP",
    "SSE",
    "SSE2",
    "SSE2MMX",
    "SSE3",
    "SSE3X87",
    "SSEMXCSR",
    "SSE_PREFETCH",
    "X87",
}
FUNC = re.compile(r"^[0-9a-f]+ <(.*)>:$")
OBJDUMP_INSN = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}(?:\s+|$))+)(.*)$")
XED_INSN = re.compile(r"^XDIS ([0-9a-f]+):\s+\S+\s+\S+\s+(\S+)\s+([0-9A-F]+)\s+(.*)$")


def elf_identity(binary):
    header = run(["readelf", "-W", "-h", str(binary)])
    elf_class = re.search(r"^\s*Class:\s+(\S+)$", header, re.MULTILINE)
    machine = re.search(r"^\s*Machine:\s+(.+)$", header, re.MULTILINE)
    if elf_class is None or machine is None:
        raise ProvisionRefusal(f"{binary} has no readable ELF identity")
    identity = {"class": elf_class.group(1), "machine": machine.group(1).strip()}
    if identity != {"class": "ELF64", "machine": "Advanced Micro Devices X86-64"}:
        raise ProvisionRefusal(
            f"{binary} is {identity['class']} {identity['machine']}; "
            "dEQP-VK requires ELF64 Advanced Micro Devices X86-64"
        )
    return identity


def symbol_version_requirements(binary):
    """Map each DT_NEEDED provider to the symbol versions the ELF requests."""
    output = run(["readelf", "-W", "--version-info", str(binary)])
    requirements: dict[str, set[str]] = {}
    provider = None
    in_needs = False
    for line in output.splitlines():
        if line.startswith("Version needs section"):
            in_needs = True
            continue
        if in_needs and line.startswith("Version definition section"):
            break
        if not in_needs:
            continue
        match = re.search(r"\bFile: (\S+)\s+Cnt:", line)
        if match:
            provider = match.group(1)
            requirements.setdefault(provider, set())
            continue
        match = re.search(r"\bName: (\S+)\s+Flags:", line)
        if match and provider is not None:
            requirements[provider].add(match.group(1))
    return {name: sorted(versions) for name, versions in sorted(requirements.items())}


def xed_instruction_identity(binary):
    """Decode every objdump instruction through XED and classify its ISA set."""
    xed = shutil.which("intel-xed")
    if xed is None:
        raise ToolUnavailable("intel-xed")
    disassembly = run(["objdump", "-d", "-C", "--insn-width=16", str(binary)])
    instructions = []
    function = "?"
    for line in disassembly.splitlines():
        match = FUNC.match(line)
        if match:
            function = match.group(1)
            continue
        match = OBJDUMP_INSN.match(line)
        if match is None:
            continue
        raw = bytes.fromhex(match.group(2))
        if not raw:
            continue
        instructions.append((function, match.group(1), raw, match.group(3).strip()))
    if not instructions:
        raise ProvisionRefusal(f"objdump decoded no instructions from {binary}")
    raw_path = None
    try:
        with tempfile.NamedTemporaryFile(prefix="r3v-xed-", delete=False) as f:
            raw_path = Path(f.name)
            for _, _, raw, _ in instructions:
                f.write(raw)
        xed_output = run([xed, "-64", "-isa-set", "-ir", str(raw_path)])
    finally:
        if raw_path is not None:
            raw_path.unlink(missing_ok=True)
    errors = [
        int(value)
        for value in re.findall(r"^# Errors: (\d+)$", xed_output, re.MULTILINE)
    ]
    if not errors or any(errors):
        raise ProvisionRefusal(
            f"intel-xed did not decode the complete instruction stream: "
            f"errors={errors or 'unreported'}"
        )
    decoded = []
    for line in xed_output.splitlines():
        match = XED_INSN.match(line)
        if match:
            decoded.append(
                (match.group(2), bytes.fromhex(match.group(3)), match.group(4))
            )
    if len(decoded) != len(instructions):
        raise ProvisionRefusal(
            f"intel-xed decoded {len(decoded)} instructions after objdump "
            f"decoded {len(instructions)}"
        )
    per_function: dict[str, set[str]] = {}
    isa_set_counts: dict[str, int] = {}
    for source, decoded_instruction in zip(instructions, decoded):
        function, address, raw, _objdump_assembler = source
        isa_set, decoded_raw, xed_assembler = decoded_instruction
        if raw != decoded_raw:
            raise ProvisionRefusal(
                f"intel-xed instruction bytes diverge at {address}: "
                f"{raw.hex()} != {decoded_raw.hex()}"
            )
        isa_set_counts[isa_set] = isa_set_counts.get(isa_set, 0) + 1
        if isa_set not in K8_XED_ISA_SETS:
            mnemonic = xed_assembler.split(None, 1)[0]
            per_function.setdefault(function, set()).add(f"{isa_set}:{mnemonic}")
    version = run([xed, "-version"]).strip().splitlines()[-1]
    return {
        "decoder": {"version": version, "sha256": sha256_file(xed)},
        "instruction_count": len(instructions),
        "isa_set_counts": dict(sorted(isa_set_counts.items())),
        "above_k8": {
            function: sorted(entries)
            for function, entries in sorted(per_function.items())
        },
    }


def binary_transport_identity(binary):
    """Bind executable bytes to their target-visible ELF and loader identity."""
    b = Path(binary)
    try:
        mode = b.stat().st_mode
    except OSError:
        mode = 0
    if not stat.S_ISREG(mode) or not os.access(b, os.X_OK):
        raise ProvisionRefusal(f"{binary} is not an executable file")
    elf = elf_identity(b)
    needed = re.findall(
        r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", run(["readelf", "-d", str(b)])
    )
    versions = symbol_version_requirements(b)
    note_output = run(["readelf", "-n", str(b)])
    match = re.search(
        r"x86 ISA needed:\s*([^\n]+?)(?=,\s*x86 (?:feature|ISA)\s|$)", note_output
    )
    isa_needed = (
        re.findall(r"x86-64-(?:baseline|v[234])", match.group(1)) if match else []
    )
    # Every CTS-shaped string in the binary, the longest taken: the
    # release name qpGetReleaseName returns carries the commit suffix
    # when the build sits past a tag, so a shorter version-shaped string
    # elsewhere in the binary cannot stand in for it.
    names = re.findall(
        rb"(?:opengl|vulkan)-cts-[0-9]+(?:\.[0-9]+)+"
        rb"(?:-[0-9]+-g[0-9a-f]{7,40}(?:-dirty)?)?",
        b.read_bytes(),
    )
    release = max(names, key=len) if names else None
    return {
        "path": str(b.resolve()),
        "sha256": sha256_file(b),
        "size": b.stat().st_size,
        "elf": elf,
        "needed": needed,
        "required_symbol_versions": versions,
        "release_name": release.decode() if release else None,
        "isa_needed": isa_needed,
    }


def binary_identity(binary, allow_symbols=None):
    """Add producer-side XED classification to the transport identity."""
    identity = binary_transport_identity(binary)
    xed = xed_instruction_identity(binary)
    above = xed["above_k8"]
    try:
        allowed = re.compile(allow_symbols) if allow_symbols else None
    except re.error as error:
        raise ProvisionRefusal(f"allow pattern {allow_symbols!r}: {error}")
    unadmitted = [
        function for function in above if not (allowed and allowed.search(function))
    ]
    identity.update(
        {
            "instruction_decoder": xed["decoder"],
            "instruction_count": xed["instruction_count"],
            "isa_set_counts": xed["isa_set_counts"],
            "isa_above_k8": above,
            "isa_allow_symbols": allow_symbols,
            "isa_unadmitted": unadmitted,
        }
    )
    return identity


def release_refusal(binary, src):
    """The release name the binary logs embeds the commit it was built
    from; a bundle whose source checkout sits on another commit would
    pin a corpus the binary does not implement, so the two must agree
    on the commit hash and on the commit count since the tag."""
    name = binary.get("release_name")
    if name is None:
        return "binary carries no embedded CTS release name"
    m = re.match(
        r"((?:opengl|vulkan)-cts-[0-9]+(?:\.[0-9]+)+)"
        r"(?:-([0-9]+)-g([0-9a-f]+))?(-dirty)?$",
        name,
    )
    if m is None:
        return f"binary release name {name!r} has no CTS shape"
    dm = re.match(r"(.*?)(?:-([0-9]+)-g([0-9a-f]+))?$", src["describe"])
    if dm is None:
        return f"source describe {src['describe']!r} has no CTS shape"
    tag, count, sha = m.group(1), m.group(2), m.group(3)
    if m.group(4):
        return f"binary release {name} was built from a dirty tree"
    dtag, dcount, dsha = dm.group(1), dm.group(2), dm.group(3)
    # A build at a tag embeds the bare tag and describe reports the bare
    # tag; a build past a tag embeds the commit count and hash, which
    # must agree with describe's and with HEAD.
    same = (
        tag == dtag
        and count == dcount
        and (
            sha is None
            or (
                dsha is not None
                and src["sha"].startswith(sha)
                and dsha.startswith(sha[:7])
            )
        )
    )
    if not same:
        return (
            f"binary release {name} was built from another commit than "
            f"the source checkout {src['describe']} ({src['sha'][:12]})"
        )
    return None


def isa_level_refusal(binary):
    """The GNU x86 ISA-needed property is the loader's own admission test:
    ld.so refuses a binary marked above the host's level before main
    runs (``CPU ISA level is lower than required``), so a marker other
    than the baseline refuses the bundle for the K8 host outright."""
    above = [x for x in binary["isa_needed"] if x != "x86-64-baseline"]
    if above:
        return (
            "binary's ELF property requires ISA levels the K8 host "
            f"lacks: {above}; build with -march=x86-64 and link only "
            "baseline objects"
        )
    return None


def compiler_identity(compiler):
    """The compiler's own version line; the path comes from the CMake
    cache, so only a regular executable file is run."""
    if not (os.path.isfile(compiler) and os.access(compiler, os.X_OK)):
        return None
    try:
        return run([compiler, "--version"]).splitlines()[0]
    except (ProvisionRefusal, IndexError):
        return None


def binutils_identity():
    """readelf and objdump decide the ISA note and the screen, so their
    version is part of the verdict's provenance."""
    return {
        tool: run([tool, "--version"]).splitlines()[0]
        for tool in ("readelf", "objdump", "objcopy")
    }


def dynamic_provider_paths(binary):
    def interpreter_path(executable):
        output = run(["readelf", "-W", "--program-headers", str(executable)])
        match = re.search(r"Requesting program interpreter: ([^\]]+)", output)
        if match is None:
            raise ProvisionRefusal(f"{executable} has no dynamic interpreter")
        return Path(match.group(1))

    binary_interpreter = interpreter_path(binary)
    host_interpreter = interpreter_path("/proc/self/exe")
    if not binary_interpreter.is_file():
        raise ProvisionRefusal(
            f"target dynamic interpreter {binary_interpreter} is unavailable"
        )
    if binary_interpreter.resolve() != host_interpreter.resolve():
        raise ProvisionRefusal(
            f"binary interpreter {binary_interpreter} differs from the host "
            f"interpreter {host_interpreter}"
        )
    loader_environment = {
        key: value for key, value in os.environ.items() if not key.startswith("LD_")
    }
    output = run([str(host_interpreter), "--list", str(binary)], env=loader_environment)
    providers = {}
    for line in output.splitlines():
        match = re.match(r"^\s*(\S+)\s+=>\s+(\S+)", line)
        if match and Path(match.group(2)).is_file():
            providers[match.group(1)] = match.group(2)
            continue
        match = re.match(r"^\s*(/\S+)\s+\(", line)
        if match and Path(match.group(1)).is_file():
            providers[Path(match.group(1)).name] = match.group(1)
    return providers


def provided_symbol_versions(provider):
    output = run(["readelf", "-W", "--version-info", str(provider)])
    versions = set()
    in_definitions = False
    for line in output.splitlines():
        if line.startswith("Version definition section"):
            in_definitions = True
            continue
        if in_definitions and line.startswith("Version needs section"):
            break
        if not in_definitions:
            continue
        match = re.search(r"\bName: (\S+)(?:\s|$)", line)
        if match:
            versions.add(match.group(1))
    return versions


def verify_symbol_versions(binary, requirements):
    if not requirements:
        return {}
    providers = dynamic_provider_paths(binary)
    checked = {}
    for provider, required in requirements.items():
        path = providers.get(provider)
        if path is None:
            raise ProvisionRefusal(
                f"target loader did not resolve required provider {provider}"
            )
        available = provided_symbol_versions(path)
        missing = sorted(set(required) - available)
        if missing:
            raise ProvisionRefusal(
                f"target provider {path} lacks symbol versions {missing} "
                f"required from {provider}"
            )
        checked[provider] = {
            "path": str(Path(path).resolve()),
            "required": list(required),
        }
    return checked


def bundle(args):
    source_root = Path(args.source_root).resolve()
    binary_path = Path(args.binary).resolve()
    data = Path(args.data_dir).resolve()
    mustpass = Path(args.mustpass_dir).resolve()
    for label, path in (
        ("source root", source_root),
        ("data directory", data),
        ("mustpass directory", mustpass),
    ):
        if not path.is_dir():
            raise ProvisionRefusal(f"{label} {path} is not a directory")
    out = validate_output_boundary(
        args.out,
        (
            ("source root", source_root),
            ("data directory", data),
            ("mustpass directory", mustpass),
        ),
    )
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise ProvisionRefusal(
            f"{out} is not empty; a bundle takes a fresh " "directory"
        )
    src = source_identity(args.source_root)
    if not src["clean"]:
        raise ProvisionRefusal("source tree is dirty; no bundle is written")
    pins = cache_pins(args.cmake_cache, source_root, binary_path)
    corpus_pin = corpus_identity(mustpass, args.corpus_pin)
    if corpus_pin["cts_describe"] != src["describe"]:
        raise ProvisionRefusal(
            f"corpus pin {corpus_pin['cts_describe']} does not match source "
            f"checkout {src['describe']}"
        )
    binary = binary_identity(args.binary, args.isa_allow_symbols)
    level = isa_level_refusal(binary)
    if args.strip_isa_property:
        raise ProvisionRefusal(
            "ISA-property stripping is unsupported because it removes "
            "non-ISA GNU properties; relink with baseline start files"
        )
    if level:
        raise ProvisionRefusal(level)
    rel = release_refusal(binary, src)
    if rel:
        raise ProvisionRefusal(rel)
    if binary["isa_unadmitted"]:
        raise ProvisionRefusal(
            "binary uses instructions above the K8 "
            "ceiling in functions no allow pattern "
            f"admits: {binary['isa_unadmitted'][:4]}"
        )
    binary_source_sha256 = binary["sha256"]
    out.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{out.name}.staging-", dir=out.parent))
    try:
        shutil.copy2(binary_path, stage / "deqp-vk")
        if sha256_file(stage / "deqp-vk") != binary_source_sha256:
            raise ProvisionRefusal("binary changed between the scan and the copy")
        shutil.copytree(data, stage / "vulkan")
        shutil.copytree(mustpass, stage / "mustpass")
        validate_copied_corpus(stage / "mustpass", args.corpus_pin, corpus_pin)
        data_digest, data_count = tree_digest(stage / "vulkan")
        mp_digest, mp_count = tree_digest(stage / "mustpass")
        prov = {
            "provenance_version": PROVENANCE_VERSION,
            "source": src,
            "cmake": pins,
            "compiler": compiler_identity(pins["CMAKE_CXX_COMPILER"]),
            "binutils": binutils_identity(),
            "startfiles": args.startfiles,
            "build_host": {
                "kernel": os.uname().release,
                "machine": os.uname().machine,
                "glibc": run(["ldd", "--version"]).splitlines()[0],
            },
            "binary": {key: value for key, value in binary.items() if key != "path"},
            "binary_source_path": str(binary_path),
            "binary_source_sha256": binary_source_sha256,
            "data": {
                "sha256": data_digest,
                "files": data_count,
                "source_path": str(data),
            },
            "mustpass": {
                "sha256": mp_digest,
                "files": mp_count,
                "source_path": str(mustpass),
                "corpus_pin": corpus_pin,
            },
        }
        body = json.dumps(prov, sort_keys=True, separators=(",", ":")).encode()
        prov["provenance_sha256"] = hashlib.sha256(body).hexdigest()
        (stage / "provenance.json").write_text(
            json.dumps(prov, indent=1, sort_keys=True) + "\n"
        )
        stage.rename(out)
    except Exception:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    print(
        f"bundle {out}: deqp-vk {binary['sha256'][:12]} from "
        f"{src['describe']} ({'clean' if src['clean'] else 'DIRTY'}), "
        f"{pins['CMAKE_BUILD_TYPE']}/{pins['DEQP_TARGET']}, data "
        f"{data_count} files, mustpass {mp_count} files, provenance "
        f"{prov['provenance_sha256'][:12]}"
    )


def verify(args):
    out = Path(args.bundle)
    prov = json.loads((out / "provenance.json").read_text())
    if prov.get("provenance_version") != PROVENANCE_VERSION:
        raise ProvisionRefusal("provenance version unknown")
    body = json.dumps(
        {k: v for k, v in prov.items() if k != "provenance_sha256"},
        sort_keys=True,
        separators=(",", ":"),
    ).encode()
    if hashlib.sha256(body).hexdigest() != prov.get("provenance_sha256"):
        raise ProvisionRefusal("provenance digest does not match its body")
    transported_binary = out / "deqp-vk"
    try:
        mode = transported_binary.stat().st_mode
    except OSError:
        mode = 0
    if not stat.S_ISREG(mode) or not os.access(transported_binary, os.X_OK):
        raise ProvisionRefusal("deqp-vk is not a regular executable file")
    if not prov["source"]["clean"]:
        raise ProvisionRefusal("bundle was made from a dirty source tree")
    if prov["binary"].get("isa_property_stripped_from"):
        raise ProvisionRefusal(
            "bundle removed GNU properties; relink with baseline start files"
        )
    if prov["binary"]["isa_unadmitted"]:
        raise ProvisionRefusal(
            "bundle binary uses instructions above the "
            "K8 ceiling in unadmitted functions"
        )
    current_binary = binary_transport_identity(transported_binary)
    current_binary.pop("path", None)
    for key, value in current_binary.items():
        if value != prov["binary"].get(key):
            raise ProvisionRefusal(f"deqp-vk {key} does not match its provenance")
    for name, key in (("vulkan", "data"), ("mustpass", "mustpass")):
        digest, count = tree_digest(out / name)
        if digest != prov[key]["sha256"] or count != prov[key]["files"]:
            raise ProvisionRefusal(f"{name}/ does not match its provenance " "digest")
    if prov["mustpass"]["corpus_pin"]["cts_describe"] != prov["source"]["describe"]:
        raise ProvisionRefusal("mustpass corpus pin does not match source describe")
    try:
        cases, files = read_corpus(out / "mustpass")
        check_corpus_pin(prov["mustpass"]["corpus_pin"], cases)
    except (PartitionRefusal, OSError, ValueError) as error:
        raise ProvisionRefusal(f"mustpass corpus: {error}")
    if files != prov["mustpass"]["corpus_pin"]["mustpass_files"]:
        raise ProvisionRefusal("mustpass file membership does not match its corpus pin")
    level = isa_level_refusal(prov["binary"])
    if level:
        raise ProvisionRefusal(level)
    rel = release_refusal(prov["binary"], prov["source"])
    if rel:
        raise ProvisionRefusal(rel)
    providers = verify_symbol_versions(
        transported_binary, prov["binary"]["required_symbol_versions"]
    )
    print(
        f"bundle verified: deqp-vk {prov['binary']['sha256'][:12]} from "
        f"{prov['source']['describe']}, ISA note "
        f"{prov['binary']['isa_needed'] or 'absent'}, "
        f"{len(providers)} symbol-version providers, "
        f"provenance {prov['provenance_sha256'][:12]}"
    )


def write_selftest_provenance(bundle_root, provenance):
    body = json.dumps(
        {key: value for key, value in provenance.items() if key != "provenance_sha256"},
        sort_keys=True,
        separators=(",", ":"),
    ).encode()
    provenance["provenance_sha256"] = hashlib.sha256(body).hexdigest()
    (Path(bundle_root) / "provenance.json").write_text(
        json.dumps(provenance, indent=1, sort_keys=True) + "\n"
    )


def selftest():
    with tempfile.TemporaryDirectory() as temporary_directory:
        d = Path(temporary_directory)
        src = d / "src"
        src.mkdir()
        global_config = d / "gitconfig"
        global_config.write_text(
            "[commit]\n\tgpgsign = true\n" "[tag]\n\tgpgSign = true\n"
        )
        git_environment = os.environ.copy()
        git_environment["GIT_CONFIG_GLOBAL"] = str(global_config)
        run(["git", "init", "-q", str(src)], env=git_environment)
        run(
            [
                "git",
                "-C",
                str(src),
                "-c",
                "commit.gpgsign=false",
                "-c",
                "user.name=t",
                "-c",
                "user.email=t@t",
                "commit",
                "-q",
                "--allow-empty",
                "-m",
                "x",
            ],
            env=git_environment,
        )
        cache = d / "CMakeCache.txt"
        cache.write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\n"
            "DEQP_TARGET:STRING=surfaceless\n"
            "CMAKE_CXX_COMPILER:FILEPATH=/bin/false\n"
            f"CMAKE_HOME_DIRECTORY:INTERNAL={src}\n"
            f"CMAKE_CACHEFILE_DIR:INTERNAL={d}\n"
        )
        binary = d / "bin"
        cc = shutil.which("cc") or shutil.which("gcc")
        if cc is None:
            raise ToolUnavailable("cc")
        run(
            [
                "git",
                "-C",
                str(src),
                "-c",
                "tag.gpgsign=false",
                "tag",
                "vulkan-cts-1.0.0.0",
            ],
            env=git_environment,
        )
        run(
            [
                "git",
                "-C",
                str(src),
                "-c",
                "commit.gpgsign=false",
                "-c",
                "user.name=t",
                "-c",
                "user.email=t@t",
                "commit",
                "-q",
                "--allow-empty",
                "-m",
                "y",
            ],
            env=git_environment,
        )
        head = run(["git", "-C", str(src), "rev-parse", "HEAD"]).strip()
        (d / "t.S").write_text(
            ".text\n.global _start\n_start:\n"
            " xor %edi,%edi\n mov $60,%eax\n syscall\n"
            ".section .rodata\n"
            f'.string "vulkan-cts-1.0.0.0-1-g{head}"\n'
        )
        run(
            [
                cc,
                "-nostdlib",
                "-no-pie",
                "-march=x86-64",
                "-o",
                str(binary),
                str(d / "t.S"),
            ]
        )
        (d / "s.S").write_text(
            ".text\n.global _start\n_start:\n"
            " xor %edi,%edi\n mov $60,%eax\n syscall\n"
            ".section .rodata\n"
            f'.string "vulkan-cts-1.0.0.0-7-g{head}"\n'
        )
        stale = d / "stale"
        run(
            [
                cc,
                "-nostdlib",
                "-no-pie",
                "-march=x86-64",
                "-o",
                str(stale),
                str(d / "s.S"),
            ]
        )
        data = d / "data"
        data.mkdir()
        (data / "f.txt").write_text("data\n")
        mp = d / "mp"
        mp.mkdir()
        (mp / "api.txt").write_text("dEQP-VK.api.x\n")
        pin = d / "corpus.pin"
        pin.write_text(
            f"cts_describe\tvulkan-cts-1.0.0.0-1-g{head[:7]}\n"
            "case_count\t1\n"
            f"corpus_sha256\t{hashlib.sha256(b'dEQP-VK.api.x\n').hexdigest()}\n"
        )
        copied_mp = d / "copied-mp"
        shutil.copytree(mp, copied_mp)
        (copied_mp / "api.txt").write_text("dEQP-VK.api.changed\n")
        expected_corpus_identity = corpus_identity(mp, pin)

        def do_bundle(
            out,
            allow: str | None = ".*",
            bin_=None,
            strip=False,
            cache_=None,
            pin_=None,
            data_=None,
            mustpass_=None,
        ):
            bundle(
                argparse.Namespace(
                    out=str(out),
                    source_root=str(src),
                    cmake_cache=str(cache_ or cache),
                    binary=str(bin_ or binary),
                    data_dir=str(data_ or data),
                    mustpass_dir=str(mustpass_ or mp),
                    corpus_pin=str(pin_ or pin),
                    isa_allow_symbols=allow,
                    strip_isa_property=strip,
                    startfiles=None,
                )
            )

        def expect(fn, needle):
            try:
                fn()
            except ProvisionRefusal as e:
                if needle not in str(e):
                    raise SystemExit(f"selftest: wrong refusal {e!r}")
                return
            raise SystemExit(f"selftest: {needle!r} not refused")

        expect(
            lambda: validate_copied_corpus(copied_mp, pin, expected_corpus_identity),
            "copied mustpass corpus",
        )
        out = d / "out"
        do_bundle(out)
        verify(argparse.Namespace(bundle=str(out)))
        expect(lambda: do_bundle(out), "is not empty")
        empty_out = d / "empty-out"
        empty_out.mkdir()
        do_bundle(empty_out)
        verify(argparse.Namespace(bundle=str(empty_out)))
        executable_mode = (out / "deqp-vk").stat().st_mode
        (out / "deqp-vk").chmod(executable_mode & ~0o111)
        expect(
            lambda: verify(argparse.Namespace(bundle=str(out))), "regular executable"
        )
        (out / "deqp-vk").chmod(executable_mode)
        (out / "mustpass" / "api.txt").write_text("dEQP-VK.api.y\n")
        expect(
            lambda: verify(argparse.Namespace(bundle=str(out))),
            "mustpass/ does not match",
        )
        (out / "mustpass" / "api.txt").write_text("dEQP-VK.api.x\n")
        p = out / "provenance.json"
        p.write_text(p.read_text().replace('"Release"', '"Debug"'))
        expect(lambda: verify(argparse.Namespace(bundle=str(out))), "provenance digest")
        do_bundle(d / "out-recreated")
        high_bin = d / "high"
        shutil.copy2(binary, high_bin)
        note = d / "gnu-property-note"
        note.write_bytes(
            struct.pack("<III4sIII4x", 4, 16, 5, b"GNU\0", 0xC0008002, 4, 4)
        )
        run(["objcopy", "--remove-section", ".note.gnu.property", str(high_bin)])
        run(
            [
                "objcopy",
                "--add-section",
                f".note.gnu.property={note}",
                "--set-section-flags",
                ".note.gnu.property=alloc,readonly,contents",
                str(high_bin),
            ]
        )
        if binary_identity(high_bin, ".*")["isa_needed"] != ["x86-64-v3"]:
            raise SystemExit("selftest: deterministic elevated ISA note absent")
        expect(
            lambda: do_bundle(d / "out-high", bin_=high_bin),
            "ISA levels the K8 host lacks",
        )
        expect(
            lambda: do_bundle(d / "out-strip", strip=True), "stripping is unsupported"
        )
        expect(
            lambda: do_bundle(d / "out-stale", bin_=stale), "built from another commit"
        )
        (d / "pblend.S").write_text(
            ".text\n.global _start\n_start:\n"
            " pblendw $0,%xmm1,%xmm0\n xor %edi,%edi\n"
            " mov $60,%eax\n syscall\n.section .rodata\n"
            f'.string "vulkan-cts-1.0.0.0-1-g{head}"\n'
        )
        pblend = d / "pblend"
        run(
            [
                cc,
                "-nostdlib",
                "-no-pie",
                "-march=x86-64",
                "-o",
                str(pblend),
                str(d / "pblend.S"),
            ]
        )
        expect(
            lambda: do_bundle(d / "out-pblend", bin_=pblend, allow=None),
            "no allow pattern admits",
        )
        prov = json.loads((d / "out-recreated" / "provenance.json").read_text())
        prov["binary"]["isa_unadmitted"] = ["f"]
        write_selftest_provenance(d / "out-recreated", prov)
        expect(
            lambda: verify(argparse.Namespace(bundle=str(d / "out-recreated"))),
            "unadmitted functions",
        )
        (src / "dirty").write_text("x")
        expect(lambda: do_bundle(d / "out2"), "dirty")
        if (d / "out2").exists():
            raise SystemExit("selftest: a dirty source left a bundle")
        (src / "dirty").unlink()
        do_bundle(d / "out-dirty-provenance")
        prov = json.loads((d / "out-dirty-provenance" / "provenance.json").read_text())
        prov["source"]["clean"] = False
        write_selftest_provenance(d / "out-dirty-provenance", prov)
        expect(
            lambda: verify(argparse.Namespace(bundle=str(d / "out-dirty-provenance"))),
            "dirty source tree",
        )
        # A build at the tag itself embeds the bare tag.
        run(
            [
                "git",
                "-C",
                str(src),
                "-c",
                "tag.gpgsign=false",
                "tag",
                "vulkan-cts-2.0.0.0",
            ],
            env=git_environment,
        )
        (d / "tagged.S").write_text(
            ".text\n.global _start\n_start:\n"
            " xor %edi,%edi\n mov $60,%eax\n syscall\n"
            '.section .rodata\n.string "vulkan-cts-2.0.0.0"\n'
        )
        tagged = d / "tagged"
        run(
            [
                cc,
                "-nostdlib",
                "-no-pie",
                "-march=x86-64",
                "-o",
                str(tagged),
                str(d / "tagged.S"),
            ]
        )
        tagged_pin = d / "tagged.pin"
        tagged_pin.write_text(
            "cts_describe\tvulkan-cts-2.0.0.0\ncase_count\t1\n"
            f"corpus_sha256\t{hashlib.sha256(b'dEQP-VK.api.x\n').hexdigest()}\n"
        )
        do_bundle(d / "out-tagged", bin_=tagged, pin_=tagged_pin)
        # A shorter version-shaped string ahead of the release name is
        # a decoy the longest match passes over.
        (d / "decoy.S").write_text(
            ".text\n.global _start\n_start:\n"
            " xor %edi,%edi\n mov $60,%eax\n syscall\n"
            ".section .rodata\n"
            '.string "see vulkan-cts-1.0.0"\n'
            f'.string "vulkan-cts-1.0.0.0-1-g{head}"\n'
        )
        decoy = d / "decoy"
        run(
            [
                cc,
                "-nostdlib",
                "-no-pie",
                "-march=x86-64",
                "-o",
                str(decoy),
                str(d / "decoy.S"),
            ]
        )
        if (
            binary_identity(str(decoy))["release_name"]
            != f"vulkan-cts-1.0.0.0-1-g{head}"
        ):
            raise SystemExit("selftest: a decoy release name won")
        expect(
            lambda: bundle(
                argparse.Namespace(
                    out=str(d / "out-pattern"),
                    source_root=str(src),
                    cmake_cache=str(cache),
                    binary=str(binary),
                    data_dir=str(data),
                    mustpass_dir=str(mp),
                    corpus_pin=str(tagged_pin),
                    isa_allow_symbols="(",
                    strip_isa_property=False,
                    startfiles=None,
                )
            ),
            "missing )",
        )
        bad_cache = d / "bad-cache.txt"
        bad_cache.write_text(
            cache.read_text().replace(
                f"CMAKE_HOME_DIRECTORY:INTERNAL={src}",
                f"CMAKE_HOME_DIRECTORY:INTERNAL={d}",
            )
        )
        expect(
            lambda: do_bundle(
                d / "out-cache-source", cache_=bad_cache, pin_=tagged_pin
            ),
            "does not match source root",
        )
        bad_cache.write_text(
            cache.read_text().replace(
                f"CMAKE_CACHEFILE_DIR:INTERNAL={d}",
                f"CMAKE_CACHEFILE_DIR:INTERNAL={src}",
            )
        )
        expect(
            lambda: do_bundle(d / "out-cache-build", cache_=bad_cache, pin_=tagged_pin),
            "outside CMakeCache build directory",
        )
        expect(
            lambda: do_bundle(src / "provisioned", pin_=tagged_pin),
            "equal to or below source root",
        )
        expect(
            lambda: do_bundle(data / "provisioned", pin_=tagged_pin),
            "equal to or below data directory",
        )
        expect(
            lambda: do_bundle(mp / "provisioned", pin_=tagged_pin),
            "equal to or below mustpass directory",
        )
        bad_pin = d / "bad.pin"
        bad_pin.write_text(
            tagged_pin.read_text().replace("case_count\t1", "case_count\t2")
        )
        expect(lambda: do_bundle(d / "out-bad-pin", pin_=bad_pin), "is not the pinned")
        binary32 = d / "binary32"
        run(["objcopy", "-O", "elf32-i386", str(binary), str(binary32)])
        binary32.chmod(0o755)
        expect(
            lambda: do_bundle(d / "out-elf32", bin_=binary32, pin_=tagged_pin),
            "requires ELF64",
        )
        dynamic_source = d / "dynamic.c"
        dynamic_source.write_text("int main(void) { return 0; }\n")
        dynamic_binary = d / "dynamic"
        run(
            [cc, "-march=x86-64", "-O1", "-o", str(dynamic_binary), str(dynamic_source)]
        )
        requirements = symbol_version_requirements(dynamic_binary)
        if not requirements or not verify_symbol_versions(dynamic_binary, requirements):
            raise SystemExit("selftest: symbol-version provider check absent")
        provider = next(iter(requirements))
        bad_requirements = {
            name: list(versions) for name, versions in requirements.items()
        }
        bad_requirements[provider].append("R3V_VERSION_DOES_NOT_EXIST")
        expect(
            lambda: verify_symbol_versions(dynamic_binary, bad_requirements),
            "lacks symbol versions",
        )
        unknown = json.loads((d / "out-tagged" / "provenance.json").read_text())
        unknown["provenance_version"] = PROVENANCE_VERSION + 1
        write_selftest_provenance(d / "out-tagged", unknown)
        expect(
            lambda: verify(argparse.Namespace(bundle=str(d / "out-tagged"))),
            "version unknown",
        )
        cache.write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
        expect(lambda: do_bundle(d / "out3", pin_=tagged_pin), "lacks DEQP_TARGET")
    print(
        "selftest: bundle, verify, non-empty target, tampered corpus, "
        "copied-corpus admission, tampered provenance, executable mode, "
        "deterministic elevated ISA "
        "note, rejected GNU-property stripping, stale and tagged release "
        "names, XED-classified SSE4, dirty source, cache source/build "
        "binding, output containment, corpus pin, ELF64 machine, target "
        "symbol versions, unknown schema, signing isolation, and missing "
        "cache pin each hold"
    )


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bundle")
    b.add_argument("--source-root", required=True)
    b.add_argument("--cmake-cache", required=True)
    b.add_argument("--binary", required=True)
    b.add_argument("--data-dir", required=True)
    b.add_argument("--mustpass-dir", required=True)
    b.add_argument("--corpus-pin", required=True)
    b.add_argument("--out", required=True)
    b.add_argument(
        "--startfiles",
        help="recorded origin of the crt and libgcc objects the "
        "link used (the target's own baseline set, through "
        "gcc -B); a record for the reader, the ISA note "
        "carries the guarantee",
    )
    b.add_argument(
        "--strip-isa-property",
        action="store_true",
        help="retained for explicit refusal; relink against "
        "baseline start files instead",
    )
    b.add_argument(
        "--isa-allow-symbols",
        help="regex over demangled function names whose "
        "above-K8 instructions are admitted",
    )
    v = sub.add_parser("verify")
    v.add_argument("--bundle", required=True)
    sub.add_parser("selftest")
    args = p.parse_args()
    try:
        {"bundle": bundle, "verify": verify, "selftest": lambda _args: selftest()}[
            args.cmd
        ](args)
    except ProvisionRefusal as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(2)
    except ToolUnavailable as e:
        print(f"not run: {e} is unavailable", file=sys.stderr)
        sys.exit(77 if args.cmd == "selftest" else 3)
    except (OSError, KeyError, ValueError, re.error, shutil.Error) as e:
        print(f"FAIL: {type(e).__name__}: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
