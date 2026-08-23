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

The unconditional BindImageMemory2 edge is in
src/vulkan/runtime/vk_device.c: vk_common_BindImageMemory is at line 602 and
the device->dispatch_table.BindImageMemory2 call is at line 616.  Discovery:
rg --fixed-strings "vk_common_BindImageMemory" src/vulkan/runtime

Closure is one of four verdicts the audit carries.  vkGetDeviceProcAddr
returns a function pointer for every device-level command in the core version
the instance requested, so completeness requires all 121 core 1.0 device-scope
commands populated.  Each command's registry entry fixes the results it may
return, so the conservative whole-surface contract requires the native
refusal result in every native core VkResult command's permitted set.  And
each command belongs to one dispatch scope, fixed by the type of its first
parameter, so the scope census must match the registry.

Modes:
  --selftest   synthetic known-good and known-bad closures and model shapes
  --pair-fixture NAME
               the lifecycle-pair verdict over one synthetic surface from
               PAIR_FIXTURES, which calibrates that verdict where the
               completeness verdict cannot answer in its place
  --enforce    zero open edges, complete core 1.0 device coverage, a
               registry-permitted refusal result, and every native core
               command classified, over inputs that clear the structural
               floors
  --baseline NAMES
               require the exact comma-separated open-slot set; an empty
               value requires a closed table and unexpected slots fail
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
import tempfile
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
    "CreateComputePipelines": "NATIVE_LIVE",
    "BindImageMemory": "NATIVE_LIVE",
    "CreateImage": "NATIVE_LIVE",
    "CreateImageView": "NATIVE_LIVE",
    "CreateBufferView": "CORE_FAIL_CLOSED",
    "CreateSampler": "CORE_METADATA_ONLY",
    "CreateEvent": "NATIVE_LIVE",
    "CreateQueryPool": "NATIVE_LIVE",
    "CreateDescriptorSetLayout": "NATIVE_LIVE",
    "CreateDescriptorPool": "NATIVE_LIVE",
    "AllocateDescriptorSets": "NATIVE_LIVE",
    "GetEventStatus": "NATIVE_LIVE",
    "SetEvent": "NATIVE_LIVE",
    "ResetEvent": "NATIVE_LIVE",
    "GetQueryPoolResults": "NATIVE_LIVE",
    "GetImageMemoryRequirements": "NATIVE_LIVE",
    "GetImageSparseMemoryRequirements": "CORE_QUERY_ZERO",
    "GetImageSubresourceLayout": "NATIVE_LIVE",
    "GetDeviceMemoryCommitment": "CORE_QUERY_ZERO",
    "DestroyImage": "NATIVE_LIVE",
    "DestroyImageView": "NATIVE_LIVE",
    "DestroyBufferView": "CORE_NULL_DESTROY_SAFE",
    "DestroySampler": "NATIVE_LIVE",
    "DestroyEvent": "NATIVE_LIVE",
    "DestroyQueryPool": "NATIVE_LIVE",
    "DestroyDescriptorPool": "NATIVE_LIVE",
    "FreeDescriptorSets": "NATIVE_LIVE",
    "ResetDescriptorPool": "NATIVE_LIVE",
    "UpdateDescriptorSets": "NATIVE_LIVE",
}

# The public recording surface's live command subset: the one begin/bind/
# draw sequence whose lowering is the qualified triangle cell.  Every
# other vkCmd* keeps the fail-closed prefix resolution.
NATIVE_LIVE_CMDS = {
    "CmdBeginRenderPass",
    "CmdBindDescriptorSets",
    "CmdDispatch",
    "CmdEndRenderPass",
    "CmdBindPipeline",
    "CmdBindVertexBuffers",
    "CmdBindIndexBuffer",
    "CmdDraw",
    "CmdDrawIndexed",
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


# Shape expectations make a registry, linked table, and runtime scan carry
# enough structure for the closure verdict to have evidence behind it.
MODEL_SCOPE_GLOBAL = "core 1.0 global-scope commands"
MODEL_SCOPE_INSTANCE = "core 1.0 instance-scope commands"
MODEL_SCOPE_PHYSICAL_DEVICE = "core 1.0 physical-device-scope commands"
MODEL_SCOPE_DEVICE = "core 1.0 device-scope commands"
MODEL_POPULATED_SLOTS = "populated device slots"
MODEL_COMMON_DEPENDENCIES = "common providers with dispatch dependencies"
MODEL_SCOPE_REGISTRY = (
    (MODEL_SCOPE_GLOBAL, "global"),
    (MODEL_SCOPE_INSTANCE, "instance"),
    (MODEL_SCOPE_PHYSICAL_DEVICE, "physical-device"),
)

MODEL_CENSUS = (
    (MODEL_SCOPE_GLOBAL, 4),
    (MODEL_SCOPE_INSTANCE, 2),
    (MODEL_SCOPE_PHYSICAL_DEVICE, 10),
    (MODEL_SCOPE_DEVICE, 121),
)

# The selftest oracle derives scope counts from the core-command membership in
# the checked-in Vulkan registry.  MODEL_CENSUS remains the independent policy
# expectation, so changing either the registry membership or the expected
# count exercises a distinct calibration path.

MODEL_FLOORS = (
    (MODEL_POPULATED_SLOTS, 100),
    (MODEL_COMMON_DEPENDENCIES, 50),
)
MODEL_FLOOR_COUNTS = dict(MODEL_FLOORS)

# Explicit malformed counts keep each scope expectation independent from the
# model census used by the known-good fixture.
MODEL_SCOPE_CENSUS_REJECTION_FIXTURES = (
    (MODEL_SCOPE_GLOBAL, 3),
    (MODEL_SCOPE_INSTANCE, 1),
    (MODEL_SCOPE_PHYSICAL_DEVICE, 9),
    (MODEL_SCOPE_DEVICE, 120),
)


def model_failures(scope_counts, populated_count, dependency_count):
    """Return model-shape defects before the closure verdict runs."""
    failures = []
    for name, expected in MODEL_CENSUS:
        if name not in scope_counts:
            failures.append(f"{name} is missing; the audit inputs did not "
                            "parse")
            continue
        value = scope_counts[name]
        if value != expected:
            failures.append(f"{name} is {value}, not the {expected} "
                            "Vulkan 1.0 fixes; the audit inputs did not "
                            "parse")

    floor_values = {
        MODEL_POPULATED_SLOTS: populated_count,
        MODEL_COMMON_DEPENDENCIES: dependency_count,
    }
    for name, floor in MODEL_FLOORS:
        value = floor_values[name]
        if value < floor:
            failures.append(f"{name} is {value}, below the floor {floor}; "
                            "the audit inputs did not parse")
    return failures


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


def model_scope_census_known_good():
    """Count core Vulkan 1.0 commands by their registry dispatch scope."""
    registry_path = Path(__file__).resolve().parents[4] / "vulkan/registry/vk.xml"
    registry = parse_registry(registry_path)
    core = registry.core10 & set(registry.scope)
    labels = {
        "global": MODEL_SCOPE_GLOBAL,
        "instance": MODEL_SCOPE_INSTANCE,
        "physical-device": MODEL_SCOPE_PHYSICAL_DEVICE,
        "device": MODEL_SCOPE_DEVICE,
    }
    return {
        labels[scope]: sum(registry.scope[name] == scope for name in core)
        for scope in labels
    }


def canonical(name, alias):
    seen = set()
    while name in alias and name not in seen:
        seen.add(name)
        name = alias[name]
    return name


def canonical_set(names, alias):
    """Canonicalize linked dispatch slots through the registry aliases."""
    return {canonical(name, alias) for name in names}


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
IF_BLOCK_RE = re.compile(r"\bif\s*\((?P<condition>[^{}]*)\)\s*\{")
GUARD_RE = re.compile(r"(?:dispatch_table\.|disp->)(\w+)")


def strip_c_comments(text):
    """Replace C comments with spaces while preserving source positions."""
    chars = list(text)
    state = "code"
    index = 0
    while index < len(chars):
        if state == "code":
            if chars[index] == "/" and index + 1 < len(chars):
                if chars[index + 1] == "/":
                    chars[index] = chars[index + 1] = " "
                    state = "line-comment"
                    index += 2
                    continue
                if chars[index + 1] == "*":
                    chars[index] = chars[index + 1] = " "
                    state = "block-comment"
                    index += 2
                    continue
            if chars[index] == '"':
                state = "string"
            elif chars[index] == "'":
                state = "character"
            index += 1
            continue

        if state == "line-comment":
            if chars[index] == "\n":
                state = "code"
            else:
                chars[index] = " "
            index += 1
            continue

        if state == "block-comment":
            if chars[index] == "*" and index + 1 < len(chars) and \
                    chars[index + 1] == "/":
                chars[index] = chars[index + 1] = " "
                state = "code"
                index += 2
            else:
                if chars[index] != "\n":
                    chars[index] = " "
                index += 1
            continue

        if chars[index] == "\\":
            index += 2
        else:
            if ((state == "string" and chars[index] == '"') or
                    (state == "character" and chars[index] == "'")):
                state = "code"
            index += 1
    return "".join(chars)


def matching_brace(text, opening):
    """Return the closing brace for an opening brace in comment-free C."""
    depth = 0
    state = "code"
    index = opening
    while index < len(text):
        char = text[index]
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return index
        else:
            if char == "\\":
                index += 1
            elif ((state == "string" and char == '"') or
                  (state == "character" and char == "'")):
                state = "code"
        index += 1
    return len(text)


def optional_ranges(text):
    """Return ranges guarded by a dispatch-pointer condition."""
    ranges = []
    for match in IF_BLOCK_RE.finditer(text):
        condition = match.group("condition")
        if not GUARD_RE.search(condition):
            continue
        opening = match.end() - 1
        closing = matching_brace(text, opening)
        ranges.append((match.end(), closing))
    return ranges


def matches_in_ranges(regex, text, ranges):
    """Return call names whose matches fall inside guarded ranges."""
    found = set()
    for match in regex.finditer(text):
        if any(start <= match.start() < end for start, end in ranges):
            found.add(match.group(1))
    return found


def scan_common_deps(runtime_dir: Path):
    """Map vk_common provider name -> dispatch-table calls reachable from it.

    Common providers route dispatch through file-local helpers (the render
    pass emulation's begin_subpass reaches CmdBeginRendering that way), so
    the scan builds a per-file call graph over every function defined at
    column zero and unions dispatch calls transitively through local calls.
    """
    direct = {}
    optional_direct = {}
    calls = {}
    optional_calls = {}
    for source in sorted(runtime_dir.glob("*.c")):
        text = strip_c_comments(source.read_text())
        matches = list(DEF_RE.finditer(text))
        for i, m in enumerate(matches):
            end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
            body = text[m.start():end]
            name = m.group(1)
            guarded = optional_ranges(body)
            dispatches = set(DEP_RE.findall(body))
            optional_dispatches = matches_in_ranges(DEP_RE, body, guarded)
            direct.setdefault(name, set()).update(dispatches -
                                                   optional_dispatches)
            optional_direct.setdefault(name, set()).update(
                optional_dispatches)
            function_calls = set(CALL_RE.findall(body))
            guarded_calls = matches_in_ranges(CALL_RE, body, guarded)
            calls.setdefault(name, set()).update(function_calls -
                                                  guarded_calls)
            optional_calls.setdefault(name, set()).update(guarded_calls)

    resolved = {}

    def resolve(fn, trail):
        if fn in resolved:
            return resolved[fn]
        if fn in trail:
            return set(), set()
        next_trail = trail | {fn}
        found = set(direct.get(fn, ()))
        optional_found = set(optional_direct.get(fn, ()))
        for callee in calls.get(fn, ()):
            if callee != fn and callee in direct:
                required, optional = resolve(callee, next_trail)
                found |= required
                optional_found |= optional
        for callee in optional_calls.get(fn, ()):
            if callee != fn and callee in direct:
                required, optional = resolve(callee, next_trail)
                optional_found |= required | optional
        resolved[fn] = found, optional_found - found
        return resolved[fn]

    deps = {}
    optional_deps = {}
    for fn in direct:
        if fn.startswith("vk_common_"):
            found, optional = resolve(fn, set())
            if found or optional:
                deps[fn[10:]] = found
                optional_deps[fn[10:]] = optional
    for slot, extra in ANNOTATED_DEPS.items():
        deps.setdefault(slot, set()).update(extra)
    return deps, optional_deps


def canonical_dependencies(deps, optional_deps, alias):
    """Canonicalize provider keys and dependency names together."""
    canonical_required = {}
    canonical_optional = {}
    for provider, dependencies in deps.items():
        canonical_provider = canonical(provider, alias)
        canonical_required.setdefault(canonical_provider, set()).update(
            canonical_set(dependencies, alias))
    for provider, dependencies in optional_deps.items():
        canonical_provider = canonical(provider, alias)
        canonical_optional.setdefault(canonical_provider, set()).update(
            canonical_set(dependencies, alias))
    for provider in set(canonical_required) | set(canonical_optional):
        canonical_optional.setdefault(provider, set()).difference_update(
            canonical_required.get(provider, set()))
    canonical_optional = {
        provider: dependencies
        for provider, dependencies in canonical_optional.items()
        if dependencies
    }
    return canonical_required, canonical_optional


def closure_audit(native, common, deps, optional_deps, reachable):
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
                cdep = dep
                if not populated(cdep):
                    open_edges.append((slot, cdep))
                elif cdep not in native and cdep in common:
                    stack.append(cdep)
            for dep in sorted(optional_deps.get(provider, ())):
                if populated(dep) and dep not in native and dep in common:
                    stack.append(dep)
    return open_edges


def baseline_failures(expected, observed):
    """Return exact-set differences for the calibrated open-slot baseline."""
    failures = []
    missing = sorted(expected - observed)
    unexpected = sorted(observed - expected)
    if missing:
        failures.append("missing baseline slots: " + ", ".join(missing))
    if unexpected:
        failures.append("unexpected baseline slots: " +
                        ", ".join(unexpected))
    return failures


def selftest_check(label, actual, expected):
    """Report one explicit selftest mismatch in every Python optimization mode."""
    if actual == expected:
        return True
    print(f"selftest failure: {label}: expected {expected!r}, "
          f"got {actual!r}", file=sys.stderr)
    return False


def selftest():
    alias = {"AEXT": "A", "A2KHR": "A2"}
    reachable = {"A"}
    native = canonical_set({"A2KHR"}, alias)
    common = canonical_set({"AEXT"}, alias)
    required, optional = canonical_dependencies(
        {"AEXT": {"A2KHR"}}, {}, alias)
    if not selftest_check("canonical provider set", common, {"A"}):
        return 1
    if not selftest_check("canonical dependency map", required,
                          {"A": {"A2"}}):
        return 1
    if not selftest_check("canonical optional dependency map", optional, {}):
        return 1
    bad = closure_audit(set(), common, required, optional, reachable)
    if not selftest_check("canonical alias-provider bad closure", bad,
                          [("A", "A2")]):
        return 1
    good = closure_audit(native, common, required, optional, reachable)
    if not selftest_check("canonical alias-provider good closure", good, []):
        return 1
    # Deleting the one safe target reopens the edge.
    reopened = closure_audit(set(), common, required, optional, reachable)
    if not selftest_check("canonical alias-provider reopened closure", reopened,
                          [("A", "A2")]):
        return 1
    # A common dependency recurses: A -> B (common) -> B2 (absent).
    chained = closure_audit(
        set(), {"A", "B"}, {"A": {"B"}, "B": {"B2"}}, {}, {"A"})
    if not selftest_check("transitive missing dependency", ("A", "B2") in
                          chained, True):
        return 1

    # A comment that names a dispatch symbol carries no runtime dependency,
    # while a null-checked dispatch call is optional until its target exists.
    with tempfile.TemporaryDirectory() as temp_dir:
        Path(temp_dir, "runtime.c").write_text(
            "/* vk_common_CmdSetEvent() and disp->CommentOnly(); */\n"
            "vk_common_Test(\n"
            "{\n"
            "   if (disp->Optional) {\n"
            "      disp->Optional(...);\n"
            "   }\n"
            "   disp->Required(...);\n"
            "}\n")
        scanned, scanned_optional = scan_common_deps(Path(temp_dir))
    if not selftest_check("comment stripping", scanned.get("Test", set()),
                          {"Required"}):
        return 1
    if not selftest_check("guarded dispatch optional", scanned_optional.get(
            "Test", set()), {"Optional"}):
        return 1
    optional_good = closure_audit(set(), {"A"}, {}, {"A": {"A2"}}, {"A"})
    if not selftest_check("absent optional dependency", optional_good, []):
        return 1
    optional_nested = closure_audit(
        set(), {"A", "A2"}, {"A2": {"B"}}, {"A": {"A2"}}, {"A"})
    if not selftest_check("populated optional transitive dependency",
                          optional_nested, [("A", "B")]):
        return 1

    if not selftest_check("empty baseline", baseline_failures(set(), set()),
                          []):
        return 1
    if not selftest_check(
            "unexpected baseline slot",
            baseline_failures({"A"}, {"A", "B"}),
            ["unexpected baseline slots: B"]):
        return 1
    if not selftest_check(
            "missing baseline slot",
            baseline_failures({"A", "B"}, {"A"}),
            ["missing baseline slots: B"]):
        return 1

    # A native command outside NATIVE_BEHAVIOR reads UNCLASSIFIED, which is
    # what makes a new entrypoint arrive with a classification decision; the
    # vkCmd* prefix resolves without a map entry, and the live draw subset
    # overrides the prefix.
    behavior_legs = (
        ("CmdInvented", {"CmdInvented"}, set(), "CORE_FAIL_CLOSED"),
        ("CmdDraw", {"CmdDraw"}, set(), "NATIVE_LIVE"),
        ("CmdDrawIndirect", {"CmdDrawIndirect"}, set(), "CORE_FAIL_CLOSED"),
        ("Invented", {"Invented"}, set(), "UNCLASSIFIED"),
        ("MapMemory", {"MapMemory"}, set(), "NATIVE_LIVE"),
        ("Bridged", set(), {"Bridged"}, "COMMON_CLOSED"),
    )
    for name, native_surface, common_surface, expected in behavior_legs:
        if not selftest_check(f"behavior class {name}", behavior_class(
                name, native_surface, common_surface), expected):
            return 1

    # The refusal intersection narrows as commands join it, and a command
    # that permits no shared result empties it.
    reg = Registry({}, set(), set(), {},
                   {"A": ("VkResult", ("VK_ERROR_UNKNOWN", "VK_ERROR_X")),
                    "B": ("VkResult", ("VK_ERROR_UNKNOWN",)),
                    "C": ("VkResult", ("VK_ERROR_X",)),
                    "D": ("void", ())}, {})
    refusal_legs = (
        ({"A"}, {"VK_ERROR_UNKNOWN", "VK_ERROR_X"}),
        ({"A", "B"}, {"VK_ERROR_UNKNOWN"}),
        ({"B", "C"}, set()),
    )
    for refusing, expected in refusal_legs:
        if not selftest_check(
                f"refusal intersection {sorted(refusing)}",
                permitted_refusal_results(reg, refusing), expected):
            return 1
    # A void command carries no result set and leaves the intersection alone.
    if not selftest_check("void refusal intersection",
                          permitted_refusal_results(reg, {"B", "D"}),
                          {"VK_ERROR_UNKNOWN"}):
        return 1
    # VK_ERROR_VALIDATION_FAILED spans every command and belongs to layers.
    layered = Registry({}, set(), set(), {},
                       {"A": ("VkResult", ("VK_ERROR_VALIDATION_FAILED",))},
                       {})
    if not selftest_check("layer-owned refusal intersection",
                          permitted_refusal_results(layered, {"A"}), set()):
        return 1

    # The result-bearing selection covers native live commands as well as
    # fail-closed commands.  Removing the shared refusal result from the live
    # member must make the full surface illegal; filtering live commands out
    # would leave the synthetic surface apparently calibrated.
    result_surface = Registry(
        {}, set(),
        {"FlushMappedMemoryRanges", "CreateComputePipelines", "DestroyDevice"},
        {},
        {"FlushMappedMemoryRanges": (
            "VkResult", ("VK_ERROR_UNKNOWN", "VK_ERROR_OUT_OF_HOST_MEMORY")),
         "CreateComputePipelines": ("VkResult", ("VK_ERROR_UNKNOWN",)),
         "DestroyDevice": ("void", ())},
        {})
    result_core = result_surface.core10
    result_native = result_core.copy()
    result_bearing = native_result_commands(
        result_surface, result_core, result_native)
    if not selftest_check(
            "live VkResult behavior class",
            behavior_class("FlushMappedMemoryRanges", result_native, set()),
            "NATIVE_LIVE"):
        return 1
    if not selftest_check(
            "native VkResult surface includes live command", result_bearing,
            {"FlushMappedMemoryRanges", "CreateComputePipelines"}):
        return 1
    result_surface.results["FlushMappedMemoryRanges"] = (
        "VkResult", ("VK_ERROR_OUT_OF_HOST_MEMORY",))
    if not selftest_check(
            "live VkResult refusal denial",
            permitted_refusal_results(result_surface, result_bearing), set()):
        return 1

    # The pair verdict runs against synthetic surfaces, where the exact tuple
    # is checkable and no other verdict stands in front of it.
    for fixture, expected in ((name, PAIR_FIXTURES[name][2])
                              for name in sorted(PAIR_FIXTURES)):
        core, populated, _ = PAIR_FIXTURES[fixture]
        found = pair_asymmetries(Registry({}, set(), core, {}, {}, {}),
                                 populated)
        if not selftest_check(f"lifecycle pair {fixture}", found, expected):
            return 1

    model_counts = model_scope_census_known_good()
    if not selftest_check(
            "model floors",
            model_failures(
                model_counts,
                MODEL_FLOOR_COUNTS[MODEL_POPULATED_SLOTS],
                MODEL_FLOOR_COUNTS[MODEL_COMMON_DEPENDENCIES],
            ),
            []):
        return 1
    model_scope_rejection_fixtures = (
        MODEL_SCOPE_CENSUS_REJECTION_FIXTURES +
        tuple((name, model_counts[name] + 1)
              for name in model_counts)
    )
    for name, invalid_count in model_scope_rejection_fixtures:
        bad_counts = model_counts.copy()
        bad_counts[name] = invalid_count
        found = model_failures(
            bad_counts,
            MODEL_FLOOR_COUNTS[MODEL_POPULATED_SLOTS],
            MODEL_FLOOR_COUNTS[MODEL_COMMON_DEPENDENCIES],
        )
        if not selftest_check(f"model rejection count {name}", len(found), 1):
            return 1
        if not selftest_check(
                f"model rejection marker {name}",
                all(marker in found[0]
                    for marker in (name, str(invalid_count))), True):
            return 1

    model_floor_rejection_fixtures = (
        (MODEL_POPULATED_SLOTS,
         MODEL_FLOOR_COUNTS[MODEL_POPULATED_SLOTS] - 1,
         MODEL_FLOOR_COUNTS[MODEL_COMMON_DEPENDENCIES],
         (MODEL_POPULATED_SLOTS,
          str(MODEL_FLOOR_COUNTS[MODEL_POPULATED_SLOTS] - 1))),
        (MODEL_COMMON_DEPENDENCIES,
         MODEL_FLOOR_COUNTS[MODEL_POPULATED_SLOTS],
         MODEL_FLOOR_COUNTS[MODEL_COMMON_DEPENDENCIES] - 1,
         (MODEL_COMMON_DEPENDENCIES,
          str(MODEL_FLOOR_COUNTS[MODEL_COMMON_DEPENDENCIES] - 1))),
    )
    for (name, populated_count, dependency_count,
         expected_markers) in model_floor_rejection_fixtures:
        bad_counts = model_counts.copy()
        found = model_failures(bad_counts, populated_count, dependency_count)
        if not selftest_check(f"model rejection count {name}", len(found), 1):
            return 1
        if not selftest_check(
                f"model rejection marker {name}",
                all(marker in found[0] for marker in expected_markers), True):
            return 1

    model_floor_positive_fixtures = (
        (MODEL_POPULATED_SLOTS,
         MODEL_FLOOR_COUNTS[MODEL_POPULATED_SLOTS] + 1,
         MODEL_FLOOR_COUNTS[MODEL_COMMON_DEPENDENCIES]),
        (MODEL_COMMON_DEPENDENCIES,
         MODEL_FLOOR_COUNTS[MODEL_POPULATED_SLOTS],
         MODEL_FLOOR_COUNTS[MODEL_COMMON_DEPENDENCIES] + 1),
    )
    for name, populated_count, dependency_count in model_floor_positive_fixtures:
        if not selftest_check(
                f"model above-floor acceptance {name}",
                model_failures(
                    model_counts, populated_count, dependency_count), []):
            return 1

    missing_counts = model_counts.copy()
    del missing_counts[MODEL_SCOPE_INSTANCE]
    found = model_failures(
        missing_counts,
        MODEL_FLOOR_COUNTS[MODEL_POPULATED_SLOTS],
        MODEL_FLOOR_COUNTS[MODEL_COMMON_DEPENDENCIES],
    )
    if not selftest_check("missing model census count", len(found), 1):
        return 1
    if not selftest_check("missing model census marker",
                          MODEL_SCOPE_INSTANCE in found[0], True):
        return 1
    if not selftest_check("missing model census wording", "missing" in
                          found[0], True):
        return 1

    model_calibration_leg_count = sum((
        len(model_scope_rejection_fixtures),
        len(model_floor_rejection_fixtures),
        len(model_floor_positive_fixtures),
        1,
    ))
    print("r3v_native_entrypoint_audit selftest: canonical-provider, "
          "comment-strip, optional-guard, and exact-baseline legs OK; "
          "18 closure and result legs OK, "
          f"{model_calibration_leg_count} model-shape "
          "calibration legs OK, "
          f"{len(PAIR_FIXTURES)} lifecycle-pair legs OK")
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


def native_result_commands(reg, core_device, native):
    """Return native core device commands that return VkResult."""
    return {name for name in core_device & native
            if reg.results.get(name, ("void", ()))[0] == "VkResult"}


def behavior_class(name, native, common):
    """Behavior class of a populated core 1.0 device command."""
    if name not in native:
        return "COMMON_CLOSED" if name in common else "ABSENT"
    if name in NATIVE_LIVE_CMDS:
        return "NATIVE_LIVE"
    if name.startswith("Cmd"):
        return "CORE_FAIL_CLOSED"
    return NATIVE_BEHAVIOR.get(name, "UNCLASSIFIED")


def emit_policy(reg, native, common, deps, optional_deps):
    """Emit one TSV row per Vulkan 1.0 command across all four scopes."""
    columns = ("command", "canonical", "scope", "owner", "returns",
               "provider", "behavior", "dispatch-dependencies",
               "optional-dispatch-dependencies")
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
            ",".join(sorted(deps.get(canon, ()))) or "-",
            ",".join(sorted(optional_deps.get(canon, ()))) or "-")))


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
    raw_native, raw_common = linked_symbols(nm, library)
    raw_deps, raw_optional_deps = scan_common_deps(Path(runtime_dir))
    native = canonical_set(raw_native, reg.alias)
    common = canonical_set(raw_common, reg.alias)
    deps, optional_deps = canonical_dependencies(
        raw_deps, raw_optional_deps, reg.alias)
    # The known-bad fixtures remove one entrypoint from the parsed surface;
    # every verdict below must then fail, which is what makes a pass on the
    # real surface load-bearing.
    dropped = canonical_set(dropped, reg.alias)
    native -= dropped
    common -= dropped

    populated = {s for s in reg.device_level if s in native or s in common}
    reachable = populated & reg.core10
    open_edges = closure_audit(native, common, deps, optional_deps, reachable)
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
    scope_counts = {
        name: len(reg.core10 & reg.in_scope(registry_scope))
        for name, registry_scope in MODEL_SCOPE_REGISTRY
    }
    scope_counts[MODEL_SCOPE_DEVICE] = len(core_device)
    model_defects = model_failures(scope_counts, len(populated), len(deps))
    for failure in model_defects:
        print(f"model failure: {failure}")
    if model_defects:
        return 2

    if mode == "--baseline":
        if len(argv) < 2:
            print("model failure: --baseline requires a comma-separated "
                  "expected open-slot set")
            return 2
        expected = canonical_set(
            {name for name in argv[1].split(",") if name}, reg.alias)
        failures = baseline_failures(expected, set(open_slots))
        print(f"baseline open slots: {','.join(open_slots) or '-'}")
        for failure in failures:
            print(f"calibration failure: {failure}")
        if failures:
            return 1
        print(f"calibration: exact {len(expected)} baseline open slots")
        return 0

    if mode == "--policy":
        emit_policy(reg, native, common, deps, optional_deps)
        return 0

    # Every refusing command's registry result set must permit the refusal
    # result, and every native core command carries one behavior class.  The
    # refusal-result legality check conservatively spans every native core
    # command returning VkResult.  The whole result-bearing surface therefore
    # shares one registry result contract, while individual implementations
    # may expose narrower conditional paths:
    # vkFlushMappedMemoryRanges executes on the native transport and still
    # returns the refusal result for a range it rejects.
    result_bearing = native_result_commands(reg, core_device, native)
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
