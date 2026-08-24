# SPDX-License-Identifier: MIT
"""Vulkan 1.0 requirement inventory and extension dependency closure.

The inventory has three parts.  The registry-drift tripwire transcribes
the core 1.0 VkPhysicalDeviceFeatures and VkPhysicalDeviceLimits member
lists from vk.xml, so a registry update that renames or moves a member
the driver fills surfaces as a row change.  The source pins read the
driver: the apiVersion 1.0 definition, the granted feature bits, the
queue-family shape (one family, GRAPHICS base, COMPUTE behind the exact
hybrid gate, queueCount 1, timestampValidBits 0), and both extension
tables.  The dependency closure judges the advertised extension surface.
The checked-in TSV must equal the regeneration or the check fails naming
the divergent row.  The core 1.0 command closure is owned by
r3v_native_entrypoint_audit.py.

The dependency closure evaluates each advertised extension against the
registry at core version 1.0: the extension must exist, must be
supported for the vulkan API, must resolve every alias its require
blocks introduce, and must satisfy its depends expression (`,` is OR,
`+` is AND, parenthesized; a VK_VERSION_x_y term is satisfied only by
1.0) from the advertised union alone.  unknown_extension,
disabled_extension, and unsatisfied_depends screen the driver surface;
orphan_alias screens registry integrity (an alias whose base the
registry does not define) and cannot fire on a generated vk.xml.

Usage:
  ... --registry vk.xml --physical-device r3v_physical_device.c \
      --instance r3v_instance.c --private-header r3v_private.h \
      --inventory r3v_vulkan10_requirement_inventory.tsv [--write-inventory]
  ... --selftest --registry vk.xml
Exit 0 when the inventory matches the regeneration and the closure holds.
"""

import argparse
import re
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

CORE_VERSION = (1, 0)

DEVICE_TABLE = "r3v_native_device_extensions_supported"
INSTANCE_TABLE = "r3v_instance_extensions_supported"

HEADER = ["kind", "name", "detail"]


class InventoryFailure(Exception):
    pass


def strip_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def parse_struct_table(text, table_name):
    """Extension names set true in a vk_*_extension_table initializer.

    Comments are stripped first, so a field name inside a comment block
    within the braces never becomes a phantom row, and each row is one
    `.FIELD = VALUE,` line.  The VK_ prefix join is the inverse of
    vk_extensions_gen.py, which emits each field as the extension name
    without its VK_ prefix.
    """
    text = strip_comments(text)
    start = text.find(table_name)
    if start < 0:
        raise InventoryFailure(f"no {table_name} in the source")
    brace = text.find("{", start)
    end = text.find("};", brace)
    if brace < 0 or end < 0:
        raise InventoryFailure(f"{table_name} carries no initializer")
    names = []
    for m in re.finditer(r"^\s*\.(\w+)\s*=\s*([\w>-]+)\s*,\s*$",
                         text[brace:end], re.M):
        field, value = m.group(1), m.group(2)
        if value != "true":
            raise InventoryFailure(
                f"{table_name} sets {field} to {value!r}; the table lists "
                "advertised extensions only, so a non-true row is a parse "
                "or policy defect")
        names.append("VK_" + field)
    if not names:
        raise InventoryFailure(f"{table_name} advertises nothing; an empty "
                               "table is a parse defect, not a surface")
    return names


def parse_api_version(private_header_text):
    m = re.search(r"#define\s+R3V_API_VERSION\s+"
                  r"VK_MAKE_API_VERSION\(\s*0\s*,\s*(\d+)\s*,\s*(\d+)\s*,",
                  private_header_text)
    if m is None:
        raise InventoryFailure(
            "R3V_API_VERSION is not a VK_MAKE_API_VERSION(0, major, minor, "
            "...) definition the pin can read")
    return (int(m.group(1)), int(m.group(2)))


def parse_granted_features(physical_device_text):
    """Feature bits r3v_physical_device_init_features sets true."""
    text = strip_comments(physical_device_text)
    m = re.search(r"r3v_physical_device_init_features\s*\([^)]*\)\s*"
                  r"\{(.*?)\n\}", text, re.S)
    if m is None:
        raise InventoryFailure("no r3v_physical_device_init_features body")
    granted = sorted(set(re.findall(r"features->(\w+)\s*=\s*true",
                                    m.group(1))))
    if not granted:
        raise InventoryFailure("the feature init grants no bit; core 1.0 "
                               "mandates robustBufferAccess")
    return granted


def parse_queue_family(physical_device_text):
    """The queue family as source facts: bare GRAPHICS base, COMPUTE
    behind the hybrid gate, queueCount 1, timestampValidBits 0."""
    text = strip_comments(physical_device_text)
    if not re.search(r"VkQueueFlags\s+queue_flags\s*=\s*"
                     r"VK_QUEUE_GRAPHICS_BIT\s*;", text):
        raise InventoryFailure(
            "the queue family base flags are not the bare "
            "VK_QUEUE_GRAPHICS_BIT assignment the inventory pins")
    if not re.search(r"if\s*\(\s*pdev->hybrid_compute_enabled\s*\)\s*"
                     r"queue_flags\s*\|=\s*VK_QUEUE_COMPUTE_BIT", text):
        raise InventoryFailure(
            "the COMPUTE queue bit is not gated on "
            "pdev->hybrid_compute_enabled; a gate change re-derives the "
            "inventory row deliberately")
    for field, value in (("queueCount", "1"), ("timestampValidBits", "0")):
        if not re.search(r"\." + field + r"\s*=\s*" + value + r"\s*,", text):
            raise InventoryFailure(
                f"the queue family does not set .{field} = {value}")
    return "graphics_base_compute_gated"


class Registry:
    def __init__(self, root):
        self.root = root
        self.extensions = {}
        for ext in root.find("extensions").findall("extension"):
            self.extensions[ext.get("name")] = ext
        self.type_names = set()
        self.aliases = {}
        for t in root.find("types").findall("type"):
            name = t.get("name")
            if name is None:
                name_el = t.find("name")
                name = name_el.text if name_el is not None else None
            if name:
                self.type_names.add(name)
                if t.get("alias"):
                    self.aliases[name] = t.get("alias")
        self.command_names = set()
        for c in root.find("commands").findall("command"):
            name = c.get("name")
            if name is None:
                proto = c.find("proto/name")
                name = proto.text if proto is not None else None
            if name:
                self.command_names.add(name)
                if c.get("alias"):
                    self.aliases[name] = c.get("alias")

    @classmethod
    def load(cls, path):
        return cls(ET.parse(path).getroot())

    def struct_members(self, struct_name):
        for t in self.root.find("types").findall("type"):
            if t.get("name") == struct_name:
                return [m.find("name").text for m in t.findall("member")]
        raise InventoryFailure(f"registry defines no {struct_name}")


def version_term(term):
    m = re.fullmatch(r"VK_VERSION_(\d+)_(\d+)", term)
    return (int(m.group(1)), int(m.group(2))) if m else None


def depends_satisfied(expr, advertised):
    """Evaluate a registry depends expression at CORE_VERSION.

    Grammar per the registry schema: `,` is OR with the lowest
    precedence, `+` is AND, parentheses group; whitespace is not part
    of the grammar and is removed first.
    """
    if not expr or not expr.strip():
        raise InventoryFailure("empty depends expression")
    s = expr.replace(" ", "")

    def parse_or(i):
        val, i = parse_and(i)
        while i < len(s) and s[i] == ",":
            rhs, i = parse_and(i + 1)
            val = val or rhs
        return val, i

    def parse_and(i):
        val, i = parse_term(i)
        while i < len(s) and s[i] == "+":
            rhs, i = parse_term(i + 1)
            val = val and rhs
        return val, i

    def parse_term(i):
        if i >= len(s):
            raise InventoryFailure(f"depends {expr!r} ends inside a term")
        if s[i] == "(":
            val, i = parse_or(i + 1)
            if i >= len(s) or s[i] != ")":
                raise InventoryFailure(f"unbalanced depends {expr!r}")
            return val, i + 1
        j = i
        while j < len(s) and s[j] not in ",+()":
            j += 1
        term = s[i:j]
        if not term:
            raise InventoryFailure(f"empty term in depends {expr!r}")
        v = version_term(term)
        if v is not None:
            return v <= CORE_VERSION, j
        return term in advertised, j

    val, i = parse_or(0)
    if i != len(s):
        raise InventoryFailure(f"trailing depends text in {expr!r}")
    return val


def closure_check(registry, advertised):
    """Reject each advertised extension that fails a closure clause."""
    for name in sorted(advertised):
        ext = registry.extensions.get(name)
        if ext is None:
            raise InventoryFailure(
                f"unknown_extension: {name} has no registry entry")
        supported = (ext.get("supported") or "").split(",")
        if "vulkan" not in supported:
            raise InventoryFailure(
                f"disabled_extension: {name} is supported for "
                f"{supported!r}, which excludes the vulkan API")
        for req in ext.findall("require"):
            for el in req.findall("type") + req.findall("command"):
                token = el.get("name")
                target = registry.aliases.get(token)
                if target is None:
                    continue
                known = (target in registry.type_names or
                         target in registry.command_names)
                if not known:
                    raise InventoryFailure(
                        f"orphan_alias: {name} introduces {token}, an "
                        f"alias of {target}, which the registry does not "
                        "define")
        depends = ext.get("depends")
        if depends is not None and not depends_satisfied(depends, advertised):
            raise InventoryFailure(
                f"unsatisfied_depends: {name} requires {depends!r}, which "
                f"the advertised union at core "
                f"{CORE_VERSION[0]}.{CORE_VERSION[1]} does not satisfy")


def ext_detail(registry, name):
    ext = registry.extensions[name]
    depends = ext.get("depends") or "-"
    promoted = ext.get("promotedto") or "-"
    return f"depends={depends} promotedto={promoted}"


def generate_rows(registry, device_exts, instance_exts, api_version,
                  queue_shape, granted):
    advertised = set(device_exts) | set(instance_exts)
    closure_check(registry, advertised)
    rows = [("api_version", f"{api_version[0]}.{api_version[1]}",
             "R3V_API_VERSION pins core 1.0; a raise is a new campaign")]
    rows.append(("queue_family", queue_shape,
                 "one family, GRAPHICS base, COMPUTE behind the exact "
                 "hybrid gate, queueCount 1, timestampValidBits 0"))
    for name in granted:
        rows.append(("feature_granted", name,
                     "r3v_physical_device_init_features sets it true"))
    for name in registry.struct_members("VkPhysicalDeviceFeatures"):
        rows.append(("feature_member", name, "core 1.0 feature struct"))
    for name in registry.struct_members("VkPhysicalDeviceLimits"):
        rows.append(("limit_member", name, "core 1.0 limit struct"))
    for name in sorted(instance_exts):
        rows.append(("instance_extension", name, ext_detail(registry, name)))
    for name in sorted(device_exts):
        rows.append(("device_extension", name, ext_detail(registry, name)))
    return rows


def rows_to_text(rows):
    lines = ["\t".join(HEADER)]
    lines += ["\t".join(r) for r in rows]
    return "\n".join(lines) + "\n"


def check_inventory(rows, inventory_path):
    expected = rows_to_text(rows)
    actual = Path(inventory_path).read_text()
    if actual == expected:
        return
    exp_lines = expected.splitlines()
    act_lines = actual.splitlines()
    for i, (e, a) in enumerate(zip(exp_lines, act_lines)):
        if e != a:
            raise InventoryFailure(
                f"inventory line {i + 1} diverges from the regeneration:\n"
                f"  checked in: {a}\n  regenerated: {e}")
    longer = exp_lines if len(exp_lines) > len(act_lines) else act_lines
    extra = longer[min(len(exp_lines), len(act_lines))]
    side = "regenerated" if longer is exp_lines else "checked-in"
    raise InventoryFailure(
        f"inventory length diverges: {len(act_lines)} checked-in lines, "
        f"{len(exp_lines)} regenerated; first {side}-only row: {extra}")


def run_check(args, write=False):
    registry = Registry.load(args.registry)
    pdev_text = Path(args.physical_device).read_text()
    device_exts = parse_struct_table(pdev_text, DEVICE_TABLE)
    instance_exts = parse_struct_table(
        Path(args.instance).read_text(), INSTANCE_TABLE)
    api_version = parse_api_version(Path(args.private_header).read_text())
    if api_version != CORE_VERSION:
        raise InventoryFailure(
            f"R3V_API_VERSION is {api_version[0]}.{api_version[1]}; the "
            "1.0 pin holds until a new conformance campaign raises it "
            "with its own evidence")
    queue_shape = parse_queue_family(pdev_text)
    granted = parse_granted_features(pdev_text)
    rows = generate_rows(registry, device_exts, instance_exts, api_version,
                         queue_shape, granted)
    if write:
        Path(args.inventory).write_text(rows_to_text(rows))
        print(f"wrote {len(rows)} rows to {args.inventory}")
        return
    check_inventory(rows, args.inventory)
    print(f"inventory holds: {len(rows)} rows, closure satisfied over "
          f"{len(device_exts)} device + {len(instance_exts)} instance "
          "extensions")


ORPHAN_REGISTRY = """<registry>
  <types>
    <type category="struct" name="VkPhysicalDeviceFeatures">
      <member><type>VkBool32</type> <name>robustBufferAccess</name></member>
    </type>
    <type category="struct" name="VkPhysicalDeviceLimits">
      <member><type>uint32_t</type> <name>maxImageDimension2D</name></member>
    </type>
    <type category="struct" name="VkFakeInfoKHR" alias="VkMissingBase"/>
  </types>
  <commands/>
  <extensions>
    <extension name="VK_KHR_fake" supported="vulkan">
      <require><type name="VkFakeInfoKHR"/></require>
    </extension>
  </extensions>
</registry>"""


def selftest(registry_path):
    registry = Registry.load(registry_path)
    good = {"VK_KHR_get_memory_requirements2", "VK_KHR_bind_memory2",
            "VK_KHR_dedicated_allocation", "VK_KHR_surface",
            "VK_KHR_get_physical_device_properties2"}
    closure_check(registry, good)

    def expect(fragment, fn, *a, **kw):
        try:
            fn(*a, **kw)
        except InventoryFailure as e:
            if fragment in str(e):
                return
            raise SystemExit(f"selftest: expected {fragment!r}, observed: {e}")
        raise SystemExit(f"selftest: fixture for {fragment!r} passed")

    expect("unknown_extension:", closure_check, registry,
           {"VK_KHR_nonexistent_extension"})
    expect("unsatisfied_depends:", closure_check, registry,
           {"VK_KHR_swapchain"})
    disabled = [n for n, e in registry.extensions.items()
                if "vulkan" not in (e.get("supported") or "").split(",")]
    if not disabled:
        raise SystemExit("selftest: the registry carries no disabled "
                         "extension to calibrate with")
    expect("disabled_extension:", closure_check, registry, {disabled[0]})
    expect("orphan_alias:", closure_check,
           Registry(ET.fromstring(ORPHAN_REGISTRY)), {"VK_KHR_fake"})

    table = ("static const struct t T = {\n"
             "   /* .KHR_display = true when a KMS lane lands */\n"
             "   .KHR_surface = true,\n};\n")
    got = parse_struct_table(table, "T")
    if got != ["VK_KHR_surface"]:
        raise SystemExit(f"selftest: comment-embedded field leaked: {got}")
    expect("non-true row", parse_struct_table,
           "T = {\n   .KHR_x = pdev->has_x,\n};", "T")
    expect("advertises nothing", parse_struct_table, "T = {\n};", "T")
    expect("not a VK_MAKE_API_VERSION", parse_api_version,
           "#define R3V_API_VERSION VK_API_VERSION_1_1\n")
    good_queue = ("VkQueueFlags queue_flags = VK_QUEUE_GRAPHICS_BIT;\n"
                  "if (pdev->hybrid_compute_enabled)\n"
                  "   queue_flags |= VK_QUEUE_COMPUTE_BIT;\n"
                  ".queueCount = 1,\n.timestampValidBits = 0,\n")
    parse_queue_family(good_queue)
    expect("bare", parse_queue_family,
           good_queue.replace("VK_QUEUE_GRAPHICS_BIT;",
                              "VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;"))
    expect("timestampValidBits", parse_queue_family,
           good_queue.replace("timestampValidBits = 0",
                              "timestampValidBits = 32"))
    expect("grants no bit", parse_granted_features,
           "r3v_physical_device_init_features(struct vk_features *f)\n"
           "{\n   memset(f, 0, sizeof(*f));\n}\n")
    for bad in ("", "VK_KHR_surface+", "(VK_KHR_surface"):
        expect("depends", depends_satisfied, bad, set())
    if not depends_satisfied("VK_KHR_a, VK_KHR_b", {"VK_KHR_b"}):
        raise SystemExit("selftest: spaced OR expression misparsed")
    if depends_satisfied("VK_KHR_a+VK_KHR_b", {"VK_KHR_b"}):
        raise SystemExit("selftest: AND expression misparsed")

    rows = [("api_version", "1.0", "pin")]
    with tempfile.NamedTemporaryFile("w", suffix=".tsv",
                                     delete=False) as f:
        f.write(rows_to_text(rows).replace("1.0", "1.1"))
        stale = f.name
    try:
        expect("diverges", check_inventory, rows, stale)
    finally:
        Path(stale).unlink()
    print("selftest: known-good closure passes; unknown_extension, "
          "unsatisfied_depends, disabled_extension, orphan_alias, "
          "stale-inventory, comment-embedded field, non-true row, empty "
          "table, wrong apiVersion, non-bare queue base, "
          "timestampValidBits, no granted bit, and degenerate depends "
          "fixtures each fail by their clause")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--registry", required=True)
    p.add_argument("--physical-device")
    p.add_argument("--instance")
    p.add_argument("--private-header")
    p.add_argument("--inventory")
    p.add_argument("--write-inventory", action="store_true")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args()
    if args.selftest:
        selftest(args.registry)
        return
    for field in ("physical_device", "instance", "private_header",
                  "inventory"):
        if getattr(args, field) is None:
            p.error(f"--{field.replace('_', '-')} is required for the check")
    try:
        run_check(args, write=args.write_inventory)
    except InventoryFailure as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
