# SPDX-License-Identifier: MIT
"""Public-surface policy and dispatch-closure audit for the native R3V ICD.

The native device dispatch table is driver entrypoints overlaid by
vk_common_device_entrypoints (r3v_native_device.c).  A common bridge such as
vk_common_BindImageMemory dispatches unconditionally into a downstream slot
(device->dispatch_table.BindImageMemory2); when the native link set carries
no implementation for that slot, the populated bridge is a reachable null
call.  This audit models the final table from the linked symbol set, reads
each common provider's dispatch dependencies from the vulkan runtime source,
and walks the transitive closure: every dependency of a reachable
Vulkan 1.0 device-scope slot must itself resolve to a populated slot.

Closure is one of four verdicts the audit carries.  vkGetDeviceProcAddr
returns a function pointer for every device-level command in the core version
the instance requested, so completeness requires all 121 core 1.0 device-scope
commands populated.  Each command's registry entry fixes the results it may
return, so the conservative whole-surface contract requires the native
refusal result in every native core VkResult command's permitted set.  And
each command belongs to one dispatch scope, fixed by the type of its first
parameter, so the scope census must match the registry.

Modes:
  --selftest   synthetic known-good and known-bad closures
  --pair-fixture NAME
               the lifecycle-pair verdict over one synthetic surface from
               PAIR_FIXTURES, which calibrates that verdict where the
               completeness verdict cannot answer in its place
  --enforce    zero open edges, complete core 1.0 device coverage, a
               registry-permitted refusal result, and every native core
               command classified, over inputs that clear the structural
               floors
  --policy     emit the three-scope public-surface policy as TSV
  --drop NAME  remove NAME from the parsed native symbol set before the
               verdict; the known-bad fixtures use it to prove each verdict
               fails on a surface that lost one entrypoint
  --deny-refusal NAME
               remove REFUSAL_RESULT from NAME's parsed registry result set;
               the known-bad fixture calibrates the full result-bearing set
"""

import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

DEVICE_DISPATCH_TYPES = {"VkDevice", "VkQueue", "VkCommandBuffer"}
INSTANCE_DISPATCH_TYPES = {"VkInstance"}
PHYSICAL_DEVICE_DISPATCH_TYPES = {"VkPhysicalDevice"}

# vkGetInstanceProcAddr takes a VkInstance and accepts VK_NULL_HANDLE for the
# global commands, so the first-parameter rule would file it under instance
# scope where the loader resolves it before any instance exists.
SCOPE_OVERRIDE = {"GetInstanceProcAddr": "global"}

# The result every native refusal returns, held identical to
# R3V_NATIVE_REFUSAL_RESULT in r3v_native.h.  --enforce intersects the
# permitted error sets of every native core 1.0 device command returning
# VkResult and requires this member.  This conservative whole-surface
# intersection binds every result-bearing entrypoint to one registry result
# contract; the behavior class below describes each narrower execution path.
REFUSAL_RESULT = "VK_ERROR_UNKNOWN"

# VK_ERROR_VALIDATION_FAILED also spans every command, and validation layers
# own it, so it stays out of the driver's refusal vocabulary.
LAYER_OWNED_RESULTS = {"VK_ERROR_VALIDATION_FAILED"}

# Behavior class of each core 1.0 device command the native link set provides.
# A native entrypoint that reaches this surface without a class fails
# --enforce, so adding one is a classification decision rather than a silent
# widening.  vkCmd* resolves by prefix except for the public recording
# surface's live subset below.
#
#   NATIVE_LIVE           executes on the native transport
#   CORE_FAIL_CLOSED      refuses with REFUSAL_RESULT
#   CORE_METADATA_ONLY    records state that no execution route consumes
#   CORE_QUERY_ZERO       writes an empty or zero result
#   CORE_NULL_DESTROY_SAFE  no-op over the null handle and the empty count
NATIVE_BEHAVIOR = {
    "AllocateMemory": "NATIVE_LIVE",
    "FreeMemory": "NATIVE_LIVE",
    "MapMemory": "NATIVE_LIVE",
    "UnmapMemory": "NATIVE_LIVE",
    "CreateBuffer": "NATIVE_LIVE",
    "DestroyBuffer": "NATIVE_LIVE",
    "DestroyDevice": "NATIVE_LIVE",
    "GetDeviceProcAddr": "NATIVE_LIVE",
    "BeginCommandBuffer": "NATIVE_LIVE",
    "EndCommandBuffer": "NATIVE_LIVE",
    "FlushMappedMemoryRanges": "NATIVE_LIVE",
    "InvalidateMappedMemoryRanges": "NATIVE_LIVE",
    "CreateGraphicsPipelines": "NATIVE_LIVE",
    "DestroyPipeline": "NATIVE_LIVE",
    "CreateComputePipelines": "CORE_FAIL_CLOSED",
    "BindImageMemory": "NATIVE_LIVE",
    "CreateImage": "NATIVE_LIVE",
    "CreateImageView": "NATIVE_LIVE",
    "CreateBufferView": "CORE_FAIL_CLOSED",
    "CreateSampler": "CORE_FAIL_CLOSED",
    "CreateEvent": "CORE_FAIL_CLOSED",
    "CreateQueryPool": "CORE_FAIL_CLOSED",
    "CreateDescriptorSetLayout": "CORE_FAIL_CLOSED",
    "CreateDescriptorPool": "CORE_FAIL_CLOSED",
    "AllocateDescriptorSets": "CORE_FAIL_CLOSED",
    "GetEventStatus": "CORE_FAIL_CLOSED",
    "SetEvent": "CORE_FAIL_CLOSED",
    "ResetEvent": "CORE_FAIL_CLOSED",
    "GetQueryPoolResults": "CORE_FAIL_CLOSED",
    "GetImageMemoryRequirements": "NATIVE_LIVE",
    "GetImageSparseMemoryRequirements": "CORE_QUERY_ZERO",
    "GetImageSubresourceLayout": "NATIVE_LIVE",
    "GetDeviceMemoryCommitment": "CORE_QUERY_ZERO",
    "DestroyImage": "NATIVE_LIVE",
    "DestroyImageView": "NATIVE_LIVE",
    "DestroyBufferView": "CORE_NULL_DESTROY_SAFE",
    "DestroySampler": "CORE_NULL_DESTROY_SAFE",
    "DestroyEvent": "CORE_NULL_DESTROY_SAFE",
    "DestroyQueryPool": "CORE_NULL_DESTROY_SAFE",
    "DestroyDescriptorPool": "CORE_NULL_DESTROY_SAFE",
    "FreeDescriptorSets": "CORE_NULL_DESTROY_SAFE",
    "ResetDescriptorPool": "CORE_NULL_DESTROY_SAFE",
    "UpdateDescriptorSets": "CORE_NULL_DESTROY_SAFE",
}

# The public recording surface's live command subset: the one begin/bind/
# draw sequence whose lowering is the qualified triangle cell.  Every
# other vkCmd* keeps the fail-closed prefix resolution.
NATIVE_LIVE_CMDS = {
    "CmdBeginRenderPass",
    "CmdEndRenderPass",
    "CmdBindPipeline",
    "CmdBindVertexBuffers",
    "CmdDraw",
}

# Dispatch dependencies the source scanner cannot see through simple
# member-call syntax (function pointers, macro-generated bodies).  Keyed by
# canonical slot name; values are canonical dependency names.
ANNOTATED_DEPS: dict[str, tuple[str, ...]] = {}


def command_scope(name, first_param_type):
    """Dispatch scope of a command, from the type of its first parameter."""
    if name in SCOPE_OVERRIDE:
        return SCOPE_OVERRIDE[name]
    if first_param_type in DEVICE_DISPATCH_TYPES:
        return "device"
    if first_param_type in PHYSICAL_DEVICE_DISPATCH_TYPES:
        return "physical-device"
    if first_param_type in INSTANCE_DISPATCH_TYPES:
        return "instance"
    return "global"


class Registry:
    """The vk.xml facts the audit uses, vk-prefix stripped from every name.

    alias maps a promoted spelling to its canonical target; owner maps a
    canonical name to the feature or extension that introduces it; scope and
    results are per canonical name.
    """

    def __init__(self, alias, device_level, core10, scope, results, owner):
        self.alias = alias
        self.device_level = device_level
        self.core10 = core10
        self.scope = scope
        self.results = results
        self.owner = owner

    def core10_device(self):
        return self.core10 & self.device_level

    def in_scope(self, scope):
        return {n for n, s in self.scope.items() if s == scope}


def parse_registry(vk_xml: Path):
    """Read vk.xml into a Registry."""
    root = ET.parse(vk_xml).getroot()
    alias = {}
    device_level = set()
    scope = {}
    results = {}
    for cmd in root.findall("commands/command"):
        target = cmd.get("alias")
        if target is not None:
            alias[cmd.get("name")[2:]] = target[2:]
            continue
        proto = cmd.find("proto")
        name = proto.find("name").text[2:]
        first_param = cmd.find("param/type")
        first_param_type = first_param.text if first_param is not None else None
        scope[name] = command_scope(name, first_param_type)
        results[name] = (
            proto.find("type").text,
            tuple(c for c in (cmd.get("errorcodes") or "").split(",") if c),
        )
        if first_param_type in DEVICE_DISPATCH_TYPES:
            device_level.add(name)
    # The registry splits core 1.0 into base/compute/graphics feature sets
    # (VK_BASE_VERSION_1_0 and peers); core 1.0 is their union.  The api
    # attribute separates them from VKSC_VERSION_1_0, whose commands belong
    # to Vulkan SC alone.
    core10 = set()
    owner = {}
    for feature in root.findall("feature"):
        name = feature.get("name", "")
        apis = feature.get("api", "vulkan").split(",")
        if "vulkan" not in apis:
            continue
        for req in feature.findall("require/command"):
            cmd = req.get("name")[2:]
            owner.setdefault(cmd, name)
            if name.endswith("_VERSION_1_0"):
                core10.add(cmd)
    # An extension owns the spellings no core feature introduced; the loop
    # above ran first, so a promoted command keeps its core owner.
    for ext in root.findall("extensions/extension"):
        if "vulkan" not in (ext.get("supported") or "").split(","):
            continue
        for req in ext.findall("require/command"):
            owner.setdefault(req.get("name")[2:], ext.get("name"))
    return Registry(alias, device_level, core10, scope, results, owner)


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

    # A native command outside NATIVE_BEHAVIOR reads UNCLASSIFIED, which is
    # what makes a new entrypoint arrive with a classification decision; the
    # vkCmd* prefix resolves without a map entry, and the live draw subset
    # overrides the prefix.
    assert behavior_class("CmdInvented", {"CmdInvented"}, set()) == \
        "CORE_FAIL_CLOSED"
    assert behavior_class("CmdDraw", {"CmdDraw"}, set()) == "NATIVE_LIVE"
    assert behavior_class("CmdDrawIndexed", {"CmdDrawIndexed"}, set()) == \
        "CORE_FAIL_CLOSED"
    assert behavior_class("Invented", {"Invented"}, set()) == "UNCLASSIFIED"
    assert behavior_class("MapMemory", {"MapMemory"}, set()) == "NATIVE_LIVE"
    assert behavior_class("Bridged", set(), {"Bridged"}) == "COMMON_CLOSED"

    # The refusal intersection narrows as commands join it, and a command
    # that permits no shared result empties it.
    reg = Registry({}, set(), set(), {},
                   {"A": ("VkResult", ("VK_ERROR_UNKNOWN", "VK_ERROR_X")),
                    "B": ("VkResult", ("VK_ERROR_UNKNOWN",)),
                    "C": ("VkResult", ("VK_ERROR_X",)),
                    "D": ("void", ())}, {})
    assert permitted_refusal_results(reg, {"A"}) == \
        {"VK_ERROR_UNKNOWN", "VK_ERROR_X"}
    assert permitted_refusal_results(reg, {"A", "B"}) == {"VK_ERROR_UNKNOWN"}
    assert permitted_refusal_results(reg, {"B", "C"}) == set()
    # A void command carries no result set and leaves the intersection alone.
    assert permitted_refusal_results(reg, {"B", "D"}) == {"VK_ERROR_UNKNOWN"}
    # VK_ERROR_VALIDATION_FAILED spans every command and belongs to layers.
    layered = Registry({}, set(), set(), {},
                       {"A": ("VkResult", ("VK_ERROR_VALIDATION_FAILED",))},
                       {})
    assert permitted_refusal_results(layered, {"A"}) == set()

    # The pair verdict runs against synthetic surfaces, where the exact tuple
    # is checkable and no other verdict stands in front of it.
    for fixture, expected in ((name, PAIR_FIXTURES[name][2])
                              for name in sorted(PAIR_FIXTURES)):
        core, populated, _ = PAIR_FIXTURES[fixture]
        found = pair_asymmetries(Registry({}, set(), core, {}, {}, {}),
                                 populated)
        assert found == expected, (fixture, found, expected)

    print("r3v_native_entrypoint_audit selftest: 15 closure and result legs "
          f"OK, {len(PAIR_FIXTURES)} lifecycle-pair legs OK")
    return 0


# Lifecycle pairings whose two sides do not share a type suffix.  The
# suffix-derived pairs (Create<X>/Destroy<X>, Allocate<X>/Free<X>) come from
# the command names themselves.
EXPLICIT_PAIRS = (
    ("MapMemory", "UnmapMemory"),
    ("BeginCommandBuffer", "EndCommandBuffer"),
    ("CreateDescriptorPool", "ResetDescriptorPool"),
    ("CreateCommandPool", "ResetCommandPool"),
    ("CreateEvent", "ResetEvent"),
    ("CreateFence", "ResetFences"),
)


def lifecycle_pairs(names):
    """Pair each acquiring command with the releasing command of its type.

    An object a program acquires it must be able to release, so a populated
    releasing command whose acquiring command is absent leaves a handle no
    program can obtain, and an acquiring command with no releasing command
    leaks by construction.
    """
    pairs = set()
    for name in names:
        for acquire, release in (("Create", "Destroy"), ("Allocate", "Free")):
            if not name.startswith(acquire):
                continue
            suffix = name[len(acquire):]
            partner = release + suffix
            # vkFreeDescriptorSets releases what vkAllocateDescriptorSets
            # acquires; the plural forms line up the same way.
            if partner not in names and partner.endswith("s"):
                partner = partner[:-1]
            if partner in names:
                pairs.add((name, partner))
    for acquire, release in EXPLICIT_PAIRS:
        if acquire in names and release in names:
            pairs.add((acquire, release))
    return sorted(pairs)


def pair_asymmetries(reg, populated):
    """Return [(acquire, release, which side is absent)] over core 1.0."""
    core = reg.core10
    found = []
    for acquire, release in lifecycle_pairs(core):
        have_acquire = acquire in populated
        have_release = release in populated
        if have_acquire and not have_release:
            found.append((acquire, release, release))
        elif have_release and not have_acquire:
            found.append((acquire, release, acquire))
    return found


# Synthetic lifecycle surfaces the pair verdict answers directly.  Dropping a
# real entrypoint cannot calibrate that verdict: a core 1.0 command missing
# from the surface fails the completeness verdict in the same run, so a pair
# analysis that had stopped running would still show a failing fixture.  Each
# entry is (core set, populated set, expected asymmetries).
PAIR_FIXTURES = {
    # Create/Destroy, the suffix-derived family.
    "complete": ({"CreateX", "DestroyX"}, {"CreateX", "DestroyX"}, []),
    "missing-destroy": ({"CreateX", "DestroyX"}, {"CreateX"},
                        [("CreateX", "DestroyX", "DestroyX")]),
    "missing-create": ({"CreateX", "DestroyX"}, {"DestroyX"},
                       [("CreateX", "DestroyX", "CreateX")]),
    # MapMemory/UnmapMemory shares no type suffix, so it pairs through
    # EXPLICIT_PAIRS rather than the name rule.
    "explicit-complete": ({"MapMemory", "UnmapMemory"},
                          {"MapMemory", "UnmapMemory"}, []),
    "explicit-missing-release": ({"MapMemory", "UnmapMemory"},
                                 {"MapMemory"},
                                 [("MapMemory", "UnmapMemory",
                                   "UnmapMemory")]),
    # A plural acquiring command releases through the singular spelling, which
    # is how vkAllocateCommandBuffers reaches vkFreeCommandBuffers.
    "plural-missing-release": ({"AllocateThings", "FreeThing"},
                               {"AllocateThings"},
                               [("AllocateThings", "FreeThing",
                                 "FreeThing")]),
}


def pair_fixture(name):
    """Run the lifecycle-pair verdict over one synthetic surface."""
    core, populated, _ = PAIR_FIXTURES[name]
    found = pair_asymmetries(Registry({}, set(), core, {}, {}, {}), populated)
    print(f"lifecycle-pair fixture {name}: {len(found)} one-sided")
    for acquire, release, missing in found:
        print(f"  ONE-SIDED {acquire}/{release}: {missing} is absent")
    return 1 if found else 0


def permitted_refusal_results(reg, refusing):
    """Error results every refusing command permits, layer-owned ones removed."""
    sets = [set(reg.results[name][1]) for name in sorted(refusing)
            if reg.results.get(name, ("void", ()))[0] == "VkResult"]
    if not sets:
        return set()
    return set.intersection(*sets) - LAYER_OWNED_RESULTS


def behavior_class(name, native, common):
    """Behavior class of a populated core 1.0 device command."""
    if name not in native:
        return "COMMON_CLOSED" if name in common else "ABSENT"
    if name in NATIVE_LIVE_CMDS:
        return "NATIVE_LIVE"
    if name.startswith("Cmd"):
        return "CORE_FAIL_CLOSED"
    return NATIVE_BEHAVIOR.get(name, "UNCLASSIFIED")


def emit_policy(reg, native, common, deps):
    """Emit one TSV row per Vulkan 1.0 command across all four scopes."""
    columns = ("command", "canonical", "scope", "owner", "returns",
               "provider", "behavior", "dispatch-dependencies")
    print("\t".join(columns))
    for name in sorted(reg.core10):
        canon = canonical(name, reg.alias)
        scope = reg.scope.get(canon, "unknown")
        provider = ("native" if canon in native else
                    "common" if canon in common else "absent")
        behavior = (behavior_class(canon, native, common)
                    if scope == "device" else f"SCOPE_{scope.upper()}")
        print("\t".join((
            name, canon, scope, reg.owner.get(name, ""),
            reg.results.get(canon, ("void", ()))[0], provider, behavior,
            ",".join(sorted(deps.get(canon, ()))) or "-")))


SCOPE_ENUM = {"global": "R3V_SCOPE_GLOBAL",
              "instance": "R3V_SCOPE_INSTANCE",
              "physical-device": "R3V_SCOPE_PHYSICAL_DEVICE",
              "device": "R3V_SCOPE_DEVICE"}

# Extensions the sweeps expect absent from the native surface.  The device
# extension table is empty, so every VK_KHR_swapchain command must resolve to
# NULL; VK_KHR_surface is an instance extension the sweep leaves unenabled,
# so its commands must resolve to NULL as well.
CLOSED_EXTENSIONS = ("VK_KHR_swapchain", "VK_KHR_surface")


def emit_c_surface(reg, out_path: Path):
    """Write the sweep's command table: the core 1.0 set and the absent sets.

    The sweeps read one generated table so the surface they check and the
    surface the policy audits come from the same registry parse.
    """
    def owned_by_higher_core(owner):
        return (owner.endswith(("_VERSION_1_1", "_VERSION_1_2",
                                "_VERSION_1_3", "_VERSION_1_4")) and
                "VERSION_1_0" not in owner)

    # An alias command carries no scope entry of its own -- the registry gives
    # it a target instead of a prototype -- so its scope comes from the
    # canonical spelling it names.
    def scope_of(name):
        return reg.scope.get(name) or reg.scope.get(canonical(name,
                                                              reg.alias))

    core = sorted(n for n in reg.core10 if n in reg.scope)
    higher = sorted(n for n, own in reg.owner.items()
                    if n in reg.scope and n not in reg.core10 and
                    owned_by_higher_core(own))
    # Every promoted KHR/EXT spelling belongs to the extension that
    # introduced it, and the device extension table is empty, so each must
    # resolve to NULL alongside the core spelling it aliases.
    aliases = sorted(n for n in reg.alias
                     if scope_of(n) is not None and
                     canonical(n, reg.alias) not in reg.core10)
    closed_ext = sorted(n for n, own in reg.owner.items()
                        if scope_of(n) is not None and
                        own in CLOSED_EXTENSIONS)

    lines = ["/* SPDX-License-Identifier: MIT */",
             "/* Generated from vk.xml by r3v_native_entrypoint_audit.py. */",
             "",
             '#include "r3v_native_surface.h"',
             ""]

    def table(symbol, names):
        lines.append(f"const struct r3v_surface_command {symbol}[] = {{")
        for name in names:
            lines.append(f'   {{ "vk{name}", {SCOPE_ENUM[scope_of(name)]} }},')
        lines.append("};")
        lines.append(f"const uint32_t {symbol}_count = "
                     f"ARRAY_SIZE({symbol});")
        lines.append("")

    table("r3v_surface_core10", core)
    table("r3v_surface_higher_core", higher)
    table("r3v_surface_alias", aliases)
    table("r3v_surface_closed_extension", closed_ext)
    out_path.write_text("\n".join(lines))
    return len(core), len(higher), len(aliases), len(closed_ext)


def main():
    if sys.argv[1:] == ["--selftest"]:
        return selftest()

    # The lifecycle-pair verdict over one synthetic surface, registered as its
    # own passing and failing tests so the rule is calibrated where no other
    # verdict can answer for it.
    if sys.argv[1:2] == ["--pair-fixture"]:
        return pair_fixture(sys.argv[2])

    # Table generation reads the registry alone; the surface it describes is
    # what the sweeps then measure against the built library.
    if sys.argv[1] == "--emit-c":
        reg = parse_registry(Path(sys.argv[2]))
        core, higher, aliases, closed = emit_c_surface(reg, Path(sys.argv[3]))
        print(f"surface table: {core} core 1.0, {higher} higher core, "
              f"{aliases} promoted aliases, {closed} closed-extension")
        # The alias spellings live behind a registry attribute the scope walk
        # skips, so a parse that loses them empties that table in silence.
        if core != 137:
            print(f"model failure: core 1.0 command set is {core}, not the "
                  f"137 Vulkan 1.0 fixes")
            return 2
        if aliases < 100 or higher < 50 or closed < 5:
            print(f"model failure: the absent tables are {higher}/{aliases}/"
                  f"{closed}, too small to have parsed")
            return 2
        return 0

    nm, library, vk_xml, runtime_dir = sys.argv[1:5]
    argv = sys.argv[5:]
    mode = argv[0]
    dropped = {argv[i + 1] for i, a in enumerate(argv) if a == "--drop"}
    denied_refusal = {
        argv[i + 1] for i, a in enumerate(argv) if a == "--deny-refusal"
    }

    reg = parse_registry(Path(vk_xml))
    for command in sorted(denied_refusal):
        return_type, results = reg.results.get(command, ("", ()))
        if return_type != "VkResult" or REFUSAL_RESULT not in results:
            print(f"model failure: {command} cannot calibrate the refusal "
                  "result set")
            return 2
        reg.results[command] = (
            return_type,
            tuple(result for result in results if result != REFUSAL_RESULT),
        )
        print(f"refusal-result fixture denies {REFUSAL_RESULT} for {command}")
    native, common = linked_symbols(nm, library)
    deps = scan_common_deps(Path(runtime_dir))
    # The known-bad fixtures remove one entrypoint from the parsed surface;
    # every verdict below must then fail, which is what makes a pass on the
    # real surface load-bearing.
    native -= dropped
    common -= dropped

    populated = {s for s in reg.device_level if s in native or s in common}
    reachable = populated & reg.core10
    open_edges = closure_audit(native, common, deps, reg.alias, reachable)
    open_slots = sorted({slot for slot, _ in open_edges})
    core_device = reg.core10_device()
    absent = sorted(core_device - populated)

    # A closed table and an empty model both report zero open edges, so the
    # inputs carry their own shape checks.  The released Vulkan 1.0 command
    # set is frozen, so each scope census is an exact count and a registry
    # that refiles a command trips it.  The linked table and the runtime scan
    # vary with the build, so those carry floors.  A collapsed parse -- an
    # unreadable library, a registry whose 1.0 feature sets moved -- fails
    # here before any verdict claims closure.
    census = [
        ("core 1.0 global-scope commands",
         len(reg.core10 & reg.in_scope("global")), 4),
        ("core 1.0 instance-scope commands",
         len(reg.core10 & reg.in_scope("instance")), 2),
        ("core 1.0 physical-device-scope commands",
         len(reg.core10 & reg.in_scope("physical-device")), 10),
        ("core 1.0 device-scope commands", len(core_device), 121),
    ]
    for name, value, expected in census:
        if value != expected:
            print(f"model failure: {name} is {value}, not the {expected} "
                  f"Vulkan 1.0 fixes; the audit inputs did not parse")
            return 2

    floors = [
        ("populated device slots", len(populated), 100),
        ("common providers with dispatch dependencies", len(deps), 50),
    ]
    for name, value, floor in floors:
        if value < floor:
            print(f"model failure: {name} is {value}, below the floor "
                  f"{floor}; the audit inputs did not parse")
            return 2

    if mode == "--policy":
        emit_policy(reg, native, common, deps)
        return 0

    # Every refusing command's registry result set must permit the refusal
    # result, and every native core command carries one behavior class.  The
    # refusal-result legality check conservatively spans every native core
    # command returning VkResult.  The whole result-bearing surface therefore
    # shares one registry result contract, while individual implementations
    # may expose narrower conditional paths:
    # vkFlushMappedMemoryRanges executes on the native transport and still
    # returns the refusal result for a range it rejects.
    result_bearing = {n for n in core_device & native
                      if reg.results.get(n, ("void", ()))[0] == "VkResult"}
    permitted = permitted_refusal_results(reg, result_bearing)
    unclassified = sorted(n for n in core_device & native
                          if behavior_class(n, native, common) ==
                          "UNCLASSIFIED")
    all_populated = populated | {n for n in reg.core10
                                 if n in native or n in common}
    asymmetries = pair_asymmetries(reg, all_populated)

    print(f"device-scope commands: {len(reg.device_level)}")
    print(f"populated device slots: {len(populated)} "
          f"(native {len(populated & native)}, "
          f"common {len(populated - native)})")
    print(f"core Vulkan 1.0 device commands: {len(populated & reg.core10)} "
          f"of {len(core_device)} populated")
    print(f"open dispatch edges: {len(open_edges)} "
          f"across {len(open_slots)} slots")
    print(f"refusal result {REFUSAL_RESULT} permitted by all "
          f"{len(result_bearing)} native VkResult commands: "
          f"{REFUSAL_RESULT in permitted}")
    print(f"core 1.0 lifecycle pairs: {len(lifecycle_pairs(reg.core10))}, "
          f"{len(asymmetries)} one-sided")
    for slot, dep in open_edges:
        print(f"  OPEN {slot} -> {dep}")
    for name in absent:
        print(f"  ABSENT {name}")
    for name in unclassified:
        print(f"  UNCLASSIFIED {name}")
    for acquire, release, missing in asymmetries:
        print(f"  ONE-SIDED {acquire}/{release}: {missing} is absent")

    if mode == "--enforce":
        return 1 if (open_edges or absent or unclassified or asymmetries or
                     REFUSAL_RESULT not in permitted) else 0
    print(f"unknown mode {mode}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
