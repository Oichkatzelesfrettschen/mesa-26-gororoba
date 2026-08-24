#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Provision a dEQP-VK binary as a self-describing bundle.

A conformance receipt binds to a dEQP binary by digest and to the release
name the binary writes into its own log; this tool records where those
bytes came from so the digest is traceable.  ``bundle`` copies the binary,
its data directory, and the pinned mustpass corpus into one directory and
writes ``provenance.json`` beside them: source commit, ``git describe``,
tree cleanliness, the CMake cache pins that shape the binary, the
compiler, binutils, the binary's SHA-256, size, dynamic-library
inventory, embedded CTS release name, and GNU x86 ISA-needed property,
the functions whose disassembly the above-K8 screen flags with the allow
pattern that admitted them, the mustpass and data tree digests, and the
toolchain of the host that built it.  Four refusals hold at bundle time
and again at ``verify``: a dirty source tree, a release name built from
another commit than the checkout, an ISA-needed level above the
baseline (the loader's own admission test, kept intact), and a flagged
function outside the allow pattern.  The mnemonic screen is a screen: it
names functions carrying instructions from a finite list, and an
operator's allow pattern asserts that those functions are dispatched
behind a CPU-feature check or reached only by a blocked slice; the
provenance records both so the assertion stays auditable.  ``verify``
runs on the target and recomputes every digest; with
``--strip-isa-property`` recorded, it also refuses when the CPU it runs
on lacks a level the note carried.  The self-digest detects accidental
change after sealing, not substitution by a party who re-seals.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROVENANCE_VERSION = 1
CACHE_PINS = ("CMAKE_BUILD_TYPE", "DEQP_TARGET", "CMAKE_CXX_COMPILER",
              "CMAKE_C_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS",
              "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_C_FLAGS_RELEASE",
              "CMAKE_CXX_COMPILER_VERSION", "CMAKE_CXX_COMPILER_ID")
class ProvisionRefusal(Exception):
    pass


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(argv, cwd=None):
    try:
        p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
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


def source_identity(source_root):
    root = Path(source_root)
    sha = run(["git", "rev-parse", "HEAD"], root).strip()
    describe = run(["git", "describe", "--tags", "--always"], root).strip()
    status = run(["git", "status", "--porcelain=v2"], root)
    return {"root": str(root), "sha": sha, "describe": describe,
            "clean": status.strip() == ""}


def cache_pins(cache_path):
    pins = {}
    for line in Path(cache_path).read_text().splitlines():
        m = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
        if m and m.group(1) in CACHE_PINS:
            pins[m.group(1)] = m.group(2)
    for key in ("CMAKE_BUILD_TYPE", "DEQP_TARGET", "CMAKE_CXX_COMPILER"):
        if key not in pins:
            raise ProvisionRefusal(f"CMakeCache lacks {key}")
    return pins


ABOVE_K8_MNEMONICS = {"pshufb", "pblendvb", "ptest", "pcmpgtq", "roundps",
                      "roundpd", "insertps", "dpps", "popcnt", "movbe",
                      "phaddw", "phaddd", "pmaddubsw", "palignr", "pabsb",
                      "pabsw", "pabsd", "pmulhrsw", "psignb", "psignw",
                      "psignd", "phsubw", "phsubd", "phaddsw", "phsubsw",
                      "pmulld", "pminsd", "pmaxsd", "pminsb", "pmaxsb",
                      "pminuw", "pmaxuw", "pminud", "pmaxud", "packusdw",
                      "pcmpeqq", "blendps", "blendpd", "blendvps",
                      "blendvpd", "pblendw", "mpsadbw", "phminposuw",
                      "roundss", "roundsd", "dppd", "movntdqa", "crc32",
                      "pcmpestri", "pcmpestrm", "pcmpistri", "pcmpistrm",
                      "andn", "bextr", "blsi", "blsmsk", "blsr", "bzhi",
                      "mulx", "pdep", "pext", "rorx", "sarx", "shlx",
                      "shrx", "tzcnt", "lzcnt"} | {
                      f"pmov{s}x{w}" for s in ("s", "z")
                      for w in ("bw", "bd", "bq", "wd", "wq", "dq")}
INSN = re.compile(r"^\s*[0-9a-f]+:\s+(?:(?:data16|data32|lock|rep|repz|"
                  r"repnz|bnd|notrack|cs|ds|es|fs|gs|ss|addr32)\s+)*"
                  r"([a-z][a-z0-9]*)\b")
FUNC = re.compile(r"^[0-9a-f]+ <(.*)>:$")


def binary_identity(binary, allow_symbols=None):
    """Digest, dynamic inventory, the ELF ISA-needed note, and the
    above-K8 screen: each function whose disassembly carries a mnemonic
    from a finite list of SSSE3-or-later and VEX-encoded instructions is
    named with the mnemonics it carries.  The list is a screen and names
    only what it lists; the ELF note the loader enforces is the
    admission test.  The allow pattern is the operator's recorded
    assertion that a named function is dispatched behind a CPU-feature
    check or reached only by a blocked slice; a named function outside
    the pattern refuses the bundle."""
    b = Path(binary)
    if not b.is_file() or not os.access(b, os.X_OK):
        raise ProvisionRefusal(f"{binary} is not an executable file")
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]",
                        run(["readelf", "-d", str(b)]))
    per_function = {}
    fn = "?"
    for line in run(["objdump", "-d", "-C", "--no-show-raw-insn",
                     str(b)]).splitlines():
        m = FUNC.match(line)
        if m:
            fn = m.group(1)
            continue
        m = INSN.match(line)
        if not m:
            continue
        op = m.group(1)
        if op in ABOVE_K8_MNEMONICS or (op.startswith("v") and
                                        op not in ("verr", "verw")):
            per_function.setdefault(fn, set()).add(op)
    above = {f: sorted(ops) for f, ops in sorted(per_function.items())}
    m = re.search(r"x86 ISA needed:\s*([^\n]+)", run(["readelf", "-n", str(b)]))
    isa_needed = [x.strip() for x in m.group(1).split(",")] if m else []
    try:
        allowed = re.compile(allow_symbols) if allow_symbols else None
    except re.error as e:
        raise ProvisionRefusal(f"allow pattern {allow_symbols!r}: {e}")
    unadmitted = [f for f in above if not (allowed and allowed.search(f))]
    # Every CTS-shaped string in the binary, the longest taken: the
    # release name qpGetReleaseName returns carries the commit suffix
    # when the build sits past a tag, so a shorter version-shaped string
    # elsewhere in the binary cannot stand in for it.
    names = re.findall(rb"(?:opengl|vulkan)-cts-[0-9]+(?:\.[0-9]+)+"
                       rb"(?:-[0-9]+-g[0-9a-f]{7,40}(?:-dirty)?)?",
                       b.read_bytes())
    release = max(names, key=len) if names else None
    return {"path": str(b.resolve()), "sha256": sha256_file(b),
            "size": b.stat().st_size, "needed": needed,
            "release_name": release.decode() if release else None,
            "isa_needed": isa_needed,
            "isa_above_k8": above, "isa_allow_symbols": allow_symbols,
            "isa_unadmitted": unadmitted}


def release_refusal(binary, src):
    """The release name the binary logs embeds the commit it was built
    from; a bundle whose source checkout sits on another commit would
    pin a corpus the binary does not implement, so the two must agree
    on the commit hash and on the commit count since the tag."""
    name = binary.get("release_name")
    if name is None:
        return "binary carries no embedded CTS release name"
    m = re.match(r"((?:opengl|vulkan)-cts-[0-9]+(?:\.[0-9]+)+)"
                 r"(?:-([0-9]+)-g([0-9a-f]+))?(-dirty)?$", name)
    if m is None:
        return f"binary release name {name!r} has no CTS shape"
    dm = re.match(r"(.*?)(?:-([0-9]+)-g([0-9a-f]+))?$", src["describe"])
    tag, count, sha = m.group(1), m.group(2), m.group(3)
    if m.group(4):
        return f"binary release {name} was built from a dirty tree"
    dtag, dcount, dsha = dm.group(1), dm.group(2), dm.group(3)
    # A build at a tag embeds the bare tag and describe reports the bare
    # tag; a build past a tag embeds the commit count and hash, which
    # must agree with describe's and with HEAD.
    same = tag == dtag and count == dcount and (
        sha is None or (dsha is not None and src["sha"].startswith(sha) and
                        dsha.startswith(sha[:7])))
    if not same:
        return (f"binary release {name} was built from another commit than "
                f"the source checkout {src['describe']} ({src['sha'][:12]})")
    return None


def isa_level_refusal(binary):
    """The GNU x86 ISA-needed property is the loader's own admission test:
    ld.so refuses a binary marked above the host's level before main
    runs (``CPU ISA level is lower than required``), so a marker other
    than the baseline refuses the bundle for the K8 host outright."""
    above = [x for x in binary["isa_needed"] if x != "x86-64-baseline"]
    if above:
        return ("binary's ELF property requires ISA levels the K8 host "
                f"lacks: {above}; build with -march=x86-64 and link only "
                "baseline objects")
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
    return {tool: run([tool, "--version"]).splitlines()[0]
            for tool in ("readelf", "objdump", "objcopy")}


class ToolUnavailable(Exception):
    """A required tool is absent: the check is not run, which is a
    distinct outcome from a refusal."""


def bundle(args):
    out = Path(args.out)
    if out.exists() and any(out.iterdir()):
        raise ProvisionRefusal(f"{out} is not empty; a bundle takes a fresh "
                               "directory")
    src = source_identity(args.source_root)
    if not src["clean"]:
        raise ProvisionRefusal("source tree is dirty; no bundle is written")
    pins = cache_pins(args.cmake_cache)
    out.mkdir(parents=True, exist_ok=True)
    binary = binary_identity(args.binary, args.isa_allow_symbols)
    level = isa_level_refusal(binary)
    if level and not args.strip_isa_property:
        raise ProvisionRefusal(level)
    rel = release_refusal(binary, src)
    if rel:
        raise ProvisionRefusal(rel)
    if binary["isa_unadmitted"]:
        raise ProvisionRefusal("binary uses instructions above the K8 "
                               "ceiling in functions no allow pattern "
                               f"admits: {binary['isa_unadmitted'][:4]}")
    data = Path(args.data_dir)
    mustpass = Path(args.mustpass_dir)
    shutil.copy2(args.binary, out / "deqp-vk")
    stripped_from = None
    binary_source_sha256 = binary["sha256"]
    if sha256_file(out / "deqp-vk") != binary_source_sha256:
        raise ProvisionRefusal("binary changed between the scan and the copy")
    if args.strip_isa_property:
        # The GNU x86 ISA-needed note is the loader's admission test; a
        # strip removes it from the bundle copy and records the levels
        # it carried, and verify admits the stripped bundle only on a CPU
        # that satisfies every recorded level.  The K8 route keeps the
        # note and links over the target's baseline start files instead.
        stripped_from = binary["isa_needed"]
        run(["objcopy", "--remove-section", ".note.gnu.property",
             str(out / "deqp-vk")])
        binary = binary_identity(str(out / "deqp-vk"),
                                 args.isa_allow_symbols)
        binary["isa_property_stripped_from"] = stripped_from
        level = isa_level_refusal(binary)
        if level:
            raise ProvisionRefusal(level)
    shutil.copytree(data, out / "vulkan")
    shutil.copytree(mustpass, out / "mustpass")
    data_digest, data_count = tree_digest(out / "vulkan")
    mp_digest, mp_count = tree_digest(out / "mustpass")
    prov = {
        "provenance_version": PROVENANCE_VERSION,
        "source": src,
        "cmake": pins,
        "compiler": compiler_identity(pins["CMAKE_CXX_COMPILER"]),
        "binutils": binutils_identity(),
        "startfiles": args.startfiles,
        "build_host": {"kernel": os.uname().release,
                       "machine": os.uname().machine,
                       "glibc": run(["ldd", "--version"]).splitlines()[0]},
        "binary": {k: v for k, v in binary.items() if k != "path"},
        "binary_source_path": str(Path(args.binary).resolve()),
        "binary_source_sha256": binary_source_sha256,
        "data": {"sha256": data_digest, "files": data_count,
                 "source_path": str(data.resolve())},
        "mustpass": {"sha256": mp_digest, "files": mp_count,
                     "source_path": str(mustpass.resolve())},
    }
    body = json.dumps(prov, sort_keys=True, separators=(",", ":")).encode()
    prov["provenance_sha256"] = hashlib.sha256(body).hexdigest()
    (out / "provenance.json").write_text(json.dumps(prov, indent=1,
                                                    sort_keys=True) + "\n")
    print(f"bundle {out}: deqp-vk {binary['sha256'][:12]} from "
          f"{src['describe']} ({'clean' if src['clean'] else 'DIRTY'}), "
          f"{pins['CMAKE_BUILD_TYPE']}/{pins['DEQP_TARGET']}, data "
          f"{data_count} files, mustpass {mp_count} files, provenance "
          f"{prov['provenance_sha256'][:12]}")


def host_isa_levels():
    """The x86-64 levels this CPU satisfies, from /proc/cpuinfo flags."""
    flags = set()
    for line in Path("/proc/cpuinfo").read_text().splitlines():
        if line.startswith("flags"):
            flags = set(line.split(":", 1)[1].split())
            break
    levels = ["x86-64-baseline"]
    v2 = {"cx16", "lahf_lm", "popcnt", "pni", "sse4_1", "sse4_2", "ssse3"}
    v3 = v2 | {"avx", "avx2", "bmi1", "bmi2", "f16c", "fma", "movbe"}
    v4 = v3 | {"avx512f", "avx512bw", "avx512cd", "avx512dq", "avx512vl"}
    for name, need in (("x86-64-v2", v2), ("x86-64-v3", v3),
                       ("x86-64-v4", v4)):
        if need <= flags:
            levels.append(name)
        else:
            break
    return levels


def verify(args):
    out = Path(args.bundle)
    prov = json.loads((out / "provenance.json").read_text())
    if prov.get("provenance_version") != PROVENANCE_VERSION:
        raise ProvisionRefusal("provenance version unknown")
    body = json.dumps({k: v for k, v in prov.items()
                       if k != "provenance_sha256"}, sort_keys=True,
                      separators=(",", ":")).encode()
    if hashlib.sha256(body).hexdigest() != prov.get("provenance_sha256"):
        raise ProvisionRefusal("provenance digest does not match its body")
    if sha256_file(out / "deqp-vk") != prov["binary"]["sha256"]:
        raise ProvisionRefusal("deqp-vk does not match its provenance digest")
    for name, key in (("vulkan", "data"), ("mustpass", "mustpass")):
        digest, count = tree_digest(out / name)
        if digest != prov[key]["sha256"] or count != prov[key]["files"]:
            raise ProvisionRefusal(f"{name}/ does not match its provenance "
                                   "digest")
    if not prov["source"]["clean"]:
        raise ProvisionRefusal("bundle was made from a dirty source tree")
    level = isa_level_refusal(prov["binary"])
    if level:
        raise ProvisionRefusal(level)
    stripped = prov["binary"].get("isa_property_stripped_from")
    if stripped:
        host = host_isa_levels()
        lacking = [x for x in stripped if x not in host]
        if lacking:
            raise ProvisionRefusal("the ISA note was stripped from a binary "
                                   f"marked {stripped}, and this CPU lacks "
                                   f"{lacking}")
    rel = release_refusal(prov["binary"], prov["source"])
    if rel:
        raise ProvisionRefusal(rel)
    if prov["binary"]["isa_unadmitted"]:
        raise ProvisionRefusal("bundle binary uses instructions above the "
                               "K8 ceiling in unadmitted functions")
    print(f"bundle verified: deqp-vk {prov['binary']['sha256'][:12]} from "
          f"{prov['source']['describe']}, ISA note "
          f"{prov['binary']['isa_needed'] or 'absent'}"
          f"{' (stripped from ' + str(stripped) + ')' if stripped else ''}, "
          f"provenance {prov['provenance_sha256'][:12]}")


def selftest():
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        src = d / "src"
        src.mkdir()
        run(["git", "init", "-q", str(src)])
        run(["git", "-C", str(src), "-c", "user.name=t", "-c",
             "user.email=t@t", "commit", "-q", "--allow-empty", "-m", "x"])
        cache = d / "CMakeCache.txt"
        cache.write_text("CMAKE_BUILD_TYPE:STRING=Release\n"
                         "DEQP_TARGET:STRING=surfaceless\n"
                         "CMAKE_CXX_COMPILER:FILEPATH=/bin/false\n")
        binary = d / "bin"
        cc = shutil.which("cc") or shutil.which("gcc")
        if cc is None:
            raise ToolUnavailable("cc")
        run(["git", "-C", str(src), "tag", "vulkan-cts-1.0.0.0"])
        run(["git", "-C", str(src), "-c", "user.name=t", "-c",
             "user.email=t@t", "commit", "-q", "--allow-empty", "-m", "y"])
        head = run(["git", "-C", str(src), "rev-parse", "HEAD"]).strip()
        (d / "t.c").write_text(
            f'const char *r = "vulkan-cts-1.0.0.0-1-g{head}";\n'
            "int main(void){return 0;}\n")
        run([cc, "-march=x86-64", "-O1", "-o", str(binary), str(d / "t.c")])
        high_bin = None
        if isa_level_refusal(binary_identity(str(binary))):
            # This host's crt objects stamp a higher level: keep the
            # unstripped link as the known-bad and strip the fixture.
            high_bin = d / "high"
            high_bin.write_bytes(binary.read_bytes())
            high_bin.chmod(0o755)
            run(["objcopy", "--remove-section", ".note.gnu.property",
                 str(binary)])
        (d / "s.c").write_text(
            f'const char *r = "vulkan-cts-1.0.0.0-7-g{head}";\n'
            "int main(void){return 0;}\n")
        stale = d / "stale"
        run([cc, "-march=x86-64", "-O1", "-o", str(stale), str(d / "s.c")])
        run(["objcopy", "--remove-section", ".note.gnu.property", str(stale)])
        data = d / "data"
        data.mkdir()
        (data / "f.txt").write_text("data\n")
        mp = d / "mp"
        mp.mkdir()
        (mp / "api.txt").write_text("dEQP-VK.api.x\n")

        def do_bundle(out, allow=".*", bin_=None, strip=False):
            bundle(argparse.Namespace(out=str(out), source_root=str(src),
                                      cmake_cache=str(cache),
                                      binary=str(bin_ or binary),
                                      data_dir=str(data),
                                      mustpass_dir=str(mp),
                                      isa_allow_symbols=allow,
                                      strip_isa_property=strip,
                                      startfiles=None))

        def expect(fn, needle):
            try:
                fn()
            except ProvisionRefusal as e:
                if needle not in str(e):
                    raise SystemExit(f"selftest: wrong refusal {e!r}")
                return
            raise SystemExit(f"selftest: {needle!r} not refused")

        out = d / "out"
        do_bundle(out)
        verify(argparse.Namespace(bundle=str(out)))
        expect(lambda: do_bundle(out), "is not empty")
        (out / "mustpass" / "api.txt").write_text("dEQP-VK.api.y\n")
        expect(lambda: verify(argparse.Namespace(bundle=str(out))),
               "mustpass/ does not match")
        (out / "mustpass" / "api.txt").write_text("dEQP-VK.api.x\n")
        p = out / "provenance.json"
        p.write_text(p.read_text().replace('"Release"', '"Debug"'))
        expect(lambda: verify(argparse.Namespace(bundle=str(out))),
               "provenance digest")
        if high_bin is not None:
            expect(lambda: do_bundle(d / "out-high", bin_=high_bin),
                   "ISA levels the K8 host lacks")
            do_bundle(d / "out-stripped", bin_=high_bin, strip=True)
            sp = json.loads((d / "out-stripped" / "provenance.json")
                            .read_text())
            if not sp["binary"]["isa_property_stripped_from"] or \
                    sp["binary"]["isa_needed"]:
                raise SystemExit("selftest: the strip was not recorded")
            verify(argparse.Namespace(bundle=str(d / "out-stripped")))
        expect(lambda: do_bundle(d / "out-stale", bin_=stale),
               "built from another commit")
        prov = json.loads((out / "provenance.json").read_text())
        prov["binary"]["isa_unadmitted"] = ["f"]
        body = json.dumps({k: v for k, v in prov.items()
                           if k != "provenance_sha256"}, sort_keys=True,
                          separators=(",", ":")).encode()
        prov["provenance_sha256"] = hashlib.sha256(body).hexdigest()
        (out / "provenance.json").write_text(json.dumps(prov))
        expect(lambda: verify(argparse.Namespace(bundle=str(out))),
               "unadmitted functions")
        (src / "dirty").write_text("x")
        expect(lambda: do_bundle(d / "out2"), "dirty")
        if (d / "out2").exists():
            raise SystemExit("selftest: a dirty source left a bundle")
        (src / "dirty").unlink()
        # A stripped note whose level this CPU lacks refuses at verify.
        prov = json.loads((out / "provenance.json").read_text())
        prov["binary"]["isa_property_stripped_from"] = ["x86-64-v9"]
        body = json.dumps({k: v for k, v in prov.items()
                           if k != "provenance_sha256"}, sort_keys=True,
                          separators=(",", ":")).encode()
        prov["provenance_sha256"] = hashlib.sha256(body).hexdigest()
        (out / "provenance.json").write_text(json.dumps(prov))
        expect(lambda: verify(argparse.Namespace(bundle=str(out))),
               "this CPU lacks")
        prov["binary"]["isa_property_stripped_from"] = None
        prov["source"]["clean"] = False
        body = json.dumps({k: v for k, v in prov.items()
                           if k != "provenance_sha256"}, sort_keys=True,
                          separators=(",", ":")).encode()
        prov["provenance_sha256"] = hashlib.sha256(body).hexdigest()
        (out / "provenance.json").write_text(json.dumps(prov))
        expect(lambda: verify(argparse.Namespace(bundle=str(out))),
               "dirty source tree")
        # A build at the tag itself embeds the bare tag.
        run(["git", "-C", str(src), "tag", "vulkan-cts-2.0.0.0"])
        (d / "tagged.c").write_text('const char *r = "vulkan-cts-2.0.0.0";\n'
                                    "int main(void){return 0;}\n")
        tagged = d / "tagged"
        run([cc, "-march=x86-64", "-O1", "-o", str(tagged), str(d / "tagged.c")])
        run(["objcopy", "--remove-section", ".note.gnu.property", str(tagged)])
        do_bundle(d / "out-tagged", bin_=tagged)
        # A shorter version-shaped string ahead of the release name is
        # a decoy the longest match passes over.
        (d / "decoy.c").write_text(
            'const char *a = "see vulkan-cts-1.0.0";\n'
            f'const char *r = "vulkan-cts-1.0.0.0-1-g{head}";\n'
            "int main(void){return 0;}\n")
        decoy = d / "decoy"
        run([cc, "-march=x86-64", "-O1", "-o", str(decoy), str(d / "decoy.c")])
        run(["objcopy", "--remove-section", ".note.gnu.property", str(decoy)])
        if binary_identity(str(decoy))["release_name"] != \
                f"vulkan-cts-1.0.0.0-1-g{head}":
            raise SystemExit("selftest: a decoy release name won")
        expect(lambda: bundle(argparse.Namespace(
            out=str(d / "out-pattern"), source_root=str(src),
            cmake_cache=str(cache), binary=str(binary), data_dir=str(data),
            mustpass_dir=str(mp), isa_allow_symbols="(",
            strip_isa_property=False, startfiles=None)),
            "missing )")
        cache.write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
        expect(lambda: do_bundle(d / "out3"), "lacks DEQP_TARGET")
    print("selftest: bundle, verify, non-empty target, tampered corpus, "
          "tampered provenance, ISA level above baseline with its recorded "
          "strip (when the host toolchain stamps one), stale release name, "
          "tagged release name, unadmitted above-K8 function, stripped "
          "level this CPU lacks, dirty source at bundle and at verify, and "
          "missing cache pin each hold")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bundle")
    b.add_argument("--source-root", required=True)
    b.add_argument("--cmake-cache", required=True)
    b.add_argument("--binary", required=True)
    b.add_argument("--data-dir", required=True)
    b.add_argument("--mustpass-dir", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--startfiles",
                   help="recorded origin of the crt and libgcc objects the "
                        "link used (the target's own baseline set, through "
                        "gcc -B); a record for the reader, the ISA note "
                        "carries the guarantee")
    b.add_argument("--strip-isa-property", action="store_true",
                   help="remove .note.gnu.property from the bundle copy "
                        "and record the levels it carried")
    b.add_argument("--isa-allow-symbols",
                   help="regex over demangled function names whose "
                        "above-K8 instructions are admitted")
    v = sub.add_parser("verify")
    v.add_argument("--bundle", required=True)
    sub.add_parser("selftest")
    args = p.parse_args()
    try:
        {"bundle": bundle, "verify": verify,
         "selftest": lambda a: selftest()}[args.cmd](args)
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
