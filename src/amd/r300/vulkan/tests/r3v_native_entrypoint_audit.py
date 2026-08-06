# SPDX-License-Identifier: MIT
"""Dispatch-closure audit for the native R3V ICD.

The native device dispatch table is driver entrypoints overlaid by
vk_common_device_entrypoints (r3v_native_device.c).  A common bridge such as
vk_common_BindImageMemory dispatches unconditionally into a downstream slot
(device->dispatch_table.BindImageMemory2); when the native link set carries
no implementation for that slot, the populated bridge is a reachable null
call.  This audit models the final table from the linked symbol set, reads
each common provider's dispatch dependencies from the vulkan runtime source,
and walks the transitive closure: every dependency of a reachable
Vulkan 1.0 device-scope slot must itself resolve to a populated slot.

Modes:
  --selftest             synthetic known-good and known-bad closures
  --expect-open NAMES    calibration: the named slots must have open edges
  --enforce              zero open edges required
"""

import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

DEVICE_DISPATCH_TYPES = {"VkDevice", "VkQueue", "VkCommandBuffer"}

# Dispatch dependencies the source scanner cannot see through simple
# member-call syntax (function pointers, macro-generated bodies).  Keyed by
# canonical slot name; values are canonical dependency names.
ANNOTATED_DEPS: dict[str, tuple[str, ...]] = {}


def parse_registry(vk_xml: Path):
    """Return (alias map, device-level set, core-1.0 set), vk-prefix stripped."""
    root = ET.parse(vk_xml).getroot()
    alias = {}
    device_level = set()
    for cmd in root.findall("commands/command"):
        target = cmd.get("alias")
        if target is not None:
            alias[cmd.get("name")[2:]] = target[2:]
            continue
        proto = cmd.find("proto")
        name = proto.find("name").text[2:]
        first_param_type = cmd.find("param/type")
        if first_param_type is not None and \
              first_param_type.text in DEVICE_DISPATCH_TYPES:
            device_level.add(name)
    # The registry splits core 1.0 into base/compute/graphics feature sets
    # (VK_BASE_VERSION_1_0 and peers); core 1.0 is their union.
    core10 = set()
    for feature in root.findall("feature"):
        name = feature.get("name", "")
        if not name.endswith("_VERSION_1_0"):
            continue
        for req in feature.findall("require/command"):
            core10.add(req.get("name")[2:])
    return alias, device_level, core10


def canonical(name, alias):
    seen = set()
    while name in alias and name not in seen:
        seen.add(name)
        name = alias[name]
    return name


def linked_symbols(nm, library):
    """Text-section r3v_* and vk_common_* symbols of the final library."""
    out = subprocess.run([nm, library], check=True, capture_output=True,
                         text=True).stdout
    native, common = set(), set()
    for line in out.splitlines():
        fields = line.split()
        if len(fields) != 3 or fields[1] not in "TtWw":
            continue
        sym = fields[2]
        if sym.startswith("r3v_"):
            native.add(sym[4:])
        elif sym.startswith("vk_common_"):
            common.add(sym[10:])
    return native, common


DEF_RE = re.compile(r"^(\w+)\s*\(", re.MULTILINE)
DEP_RE = re.compile(r"(?:dispatch_table\.|disp->)(\w+)\s*\(")
CALL_RE = re.compile(r"\b(\w+)\s*\(")


def scan_common_deps(runtime_dir: Path):
    """Map vk_common provider name -> dispatch-table calls reachable from it.

    Common providers route dispatch through file-local helpers (the render
    pass emulation's begin_subpass reaches CmdBeginRendering that way), so
    the scan builds a per-file call graph over every function defined at
    column zero and unions dispatch calls transitively through local calls.
    """
    direct = {}
    calls = {}
    for source in sorted(runtime_dir.glob("*.c")):
        text = source.read_text()
        matches = list(DEF_RE.finditer(text))
        for i, m in enumerate(matches):
            end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
            body = text[m.start():end]
            name = m.group(1)
            direct.setdefault(name, set()).update(DEP_RE.findall(body))
            calls.setdefault(name, set()).update(CALL_RE.findall(body))

    resolved = {}

    def resolve(fn, trail):
        if fn in resolved:
            return resolved[fn]
        if fn in trail:
            return set()
        trail.add(fn)
        found = set(direct.get(fn, ()))
        for callee in calls.get(fn, ()):
            if callee != fn and callee in direct:
                found |= resolve(callee, trail)
        resolved[fn] = found
        return found

    deps = {}
    for fn in direct:
        if fn.startswith("vk_common_"):
            found = resolve(fn, set())
            if found:
                deps[fn[10:]] = found
    for slot, extra in ANNOTATED_DEPS.items():
        deps.setdefault(slot, set()).update(extra)
    return deps


def closure_audit(native, common, deps, alias, reachable):
    """Return open edges [(slot, missing dep)] over the transitive closure."""
    def populated(slot):
        return slot in native or slot in common

    open_edges = []
    for slot in sorted(reachable):
        if slot in native or slot not in common:
            continue
        stack, visited = [slot], set()
        while stack:
            provider = stack.pop()
            if provider in visited:
                continue
            visited.add(provider)
            for dep in sorted(deps.get(provider, ())):
                cdep = canonical(dep, alias)
                if not populated(cdep):
                    open_edges.append((slot, cdep))
                elif cdep not in native and cdep in common:
                    stack.append(cdep)
    return open_edges


def selftest():
    alias = {"A2KHR": "A2"}
    reachable = {"A"}
    deps = {"A": {"A2KHR"}}
    bad = closure_audit(set(), {"A"}, deps, alias, reachable)
    assert bad == [("A", "A2")], bad
    good = closure_audit({"A2"}, {"A"}, deps, alias, reachable)
    assert good == [], good
    # Deleting the one safe target reopens the edge.
    reopened = closure_audit(set(), {"A"}, deps, alias, reachable)
    assert reopened == [("A", "A2")], reopened
    # A common dependency recurses: A -> B (common) -> B2 (absent).
    chained = closure_audit(
        set(), {"A", "B"}, {"A": {"B"}, "B": {"B2"}}, {}, {"A"})
    assert ("A", "B2") in chained, chained
    print("r3v_native_entrypoint_audit selftest: 4 legs OK")
    return 0


def main():
    if sys.argv[1:] == ["--selftest"]:
        return selftest()

    nm, library, vk_xml, runtime_dir = sys.argv[1:5]
    mode = sys.argv[5]
    alias, device_level, core10 = parse_registry(Path(vk_xml))
    native, common = linked_symbols(nm, library)
    deps = scan_common_deps(Path(runtime_dir))

    populated = {s for s in device_level if s in native or s in common}
    reachable = populated & core10
    open_edges = closure_audit(native, common, deps, alias, reachable)
    open_slots = sorted({slot for slot, _ in open_edges})

    # A closed table and an empty model both report zero open edges, so the
    # inputs carry floors: Vulkan 1.0 fixes the device-scope core set near
    # 123 commands, the linked table holds hundreds of slots, and the
    # runtime scan reaches every common provider.  A parse that collapses
    # (an unreadable library, a registry whose 1.0 feature sets moved)
    # trips these before the verdict claims closure.
    core_device = core10 & device_level
    floors = [
        ("core 1.0 device-scope commands", len(core_device), 100),
        ("populated device slots", len(populated), 100),
        ("common providers with dispatch dependencies", len(deps), 50),
    ]
    for name, value, floor in floors:
        if value < floor:
            print(f"model failure: {name} is {value}, below the floor "
                  f"{floor}; the audit inputs did not parse")
            return 2

    print(f"device-scope commands: {len(device_level)}")
    print(f"populated device slots: {len(populated)} "
          f"(native {len(populated & native)}, "
          f"common {len(populated - native)})")
    print(f"reachable Vulkan 1.0 device slots: {len(reachable)}")
    print(f"open dispatch edges: {len(open_edges)} "
          f"across {len(open_slots)} slots")
    for slot, dep in open_edges:
        print(f"  OPEN {slot} -> {dep}")

    if mode == "--enforce":
        return 1 if open_edges else 0
    if mode == "--expect-open":
        expected = set(sys.argv[6].split(","))
        missing = expected - set(open_slots)
        if missing:
            print("calibration failure, expected-open slots not detected: "
                  + ", ".join(sorted(missing)))
            return 1
        print(f"calibration: all {len(expected)} expected-open slots detected")
        return 0
    print(f"unknown mode {mode}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
