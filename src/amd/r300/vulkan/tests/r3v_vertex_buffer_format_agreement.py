#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Hold the advertised vertex-buffer formats to the ones a draw executes.

Two tables decide whether a vertex format works.  `attribute_format_id`
in r3v_native_pipeline.c maps a VkFormat to the r300_vertex_format_id
the host gather decodes, and a format missing from it refuses at
vkCreateGraphicsPipelines.  The VERTEX_BUFFER_BIT arms of
r3v_get_format_properties in r3v_physical_device.c tell an application
which formats it may bind.  The two are written apart, so they drift
apart: a format advertised and not mapped promises a pipeline the driver
refuses, and a format mapped and not advertised hides a route that
works.

This audit reads both switch statements and refuses unless the two sets
are equal.

Usage:
  r3v_vertex_buffer_format_agreement.py --pipeline PATH \\
      --physical-device PATH
  r3v_vertex_buffer_format_agreement.py --selftest
"""

import argparse
import re
import sys
from pathlib import Path

CASE = re.compile(r"case\s+(VK_FORMAT_[A-Z0-9_]+)\s*:")
RETURN_FORMAT = re.compile(r"return\s+(R300_VERTEX_FORMAT_[A-Z0-9_]+)\s*;")
VERTEX_BUFFER_BIT = "VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT"


class AuditRefusal(Exception):
    pass


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def function_body(text, name):
    """The braced body of the named function, by brace balance."""
    match = re.search(r"\b" + re.escape(name) + r"\s*\(", text)
    if match is None:
        raise AuditRefusal(f"{name} is not defined in the source")
    open_brace = text.find("{", match.end())
    if open_brace < 0:
        raise AuditRefusal(f"{name} carries no body")
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace:i + 1]
    raise AuditRefusal(f"{name}'s body does not close")


def admitted_formats(pipeline_text):
    """Every VkFormat attribute_format_id maps to a real format id.

    Case labels stack ahead of one return, so a label's verdict is the
    first return that follows it.
    """
    body = function_body(strip_comments(pipeline_text), "attribute_format_id")
    admitted = set()
    pending = []
    for token in re.finditer(r"case\s+(VK_FORMAT_[A-Z0-9_]+)\s*:|"
                             r"return\s+(R300_VERTEX_FORMAT_[A-Z0-9_]+)\s*;|"
                             r"(default\s*:)", body):
        name, returned, default = token.groups()
        if name is not None:
            pending.append(name)
        elif default is not None:
            pending = []
        elif returned is not None:
            if returned != "R300_VERTEX_FORMAT_INVALID":
                admitted.update(pending)
            pending = []
    if not admitted:
        raise AuditRefusal("attribute_format_id maps no VkFormat")
    return admitted


def advertised_formats(device_text):
    """Every VkFormat whose arm grants VERTEX_BUFFER_BIT.

    An arm runs from its first case label to its break, so the labels
    that stack ahead of a granting assignment all carry the grant.
    """
    body = function_body(strip_comments(device_text),
                         "r3v_get_format_properties")
    advertised = set()
    for arm in body.split("break;"):
        if VERTEX_BUFFER_BIT in arm:
            advertised.update(CASE.findall(arm))
    if not advertised:
        raise AuditRefusal("r3v_get_format_properties grants "
                           f"{VERTEX_BUFFER_BIT} to no VkFormat")
    return advertised


def audit(pipeline_text, device_text):
    admitted = admitted_formats(pipeline_text)
    advertised = advertised_formats(device_text)
    promised = sorted(advertised - admitted)
    hidden = sorted(admitted - advertised)
    if promised:
        raise AuditRefusal(
            f"{len(promised)} format(s) carry {VERTEX_BUFFER_BIT} and reach no "
            f"r300_vertex_format_id, so a pipeline binding one refuses: "
            f"{', '.join(promised)}")
    if hidden:
        raise AuditRefusal(
            f"{len(hidden)} format(s) map to an r300_vertex_format_id and are "
            f"not advertised, so an application cannot reach a route that "
            f"executes: {', '.join(hidden)}")
    return sorted(admitted)


PIPELINE_FIXTURE = """
static enum r300_vertex_format_id
attribute_format_id(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R32_SFLOAT:
      return R300_VERTEX_FORMAT_F32_1;
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8G8_UNORM:
      return R300_VERTEX_FORMAT_UNORM8_1;
   default:
      return R300_VERTEX_FORMAT_INVALID;
   }
}
"""

DEVICE_FIXTURE = """
static void
r3v_get_format_properties(struct d *device, VkFormat format,
                          VkFormatProperties3 *properties)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UINT:
      properties->bufferFeatures = VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;
      break;
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8G8_UNORM:
      properties->bufferFeatures = VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
      break;
   default:
      break;
   }
}
"""


def refuses(fragment, fn):
    try:
        fn()
    except AuditRefusal as e:
        assert fragment in str(e), \
            f"refusal {str(e)!r} does not name {fragment!r}"
        return
    raise AssertionError(f"expected a refusal naming {fragment!r}")


def selftest():
    formats = audit(PIPELINE_FIXTURE, DEVICE_FIXTURE)
    assert formats == ["VK_FORMAT_R32_SFLOAT", "VK_FORMAT_R8G8_UNORM",
                       "VK_FORMAT_R8_UNORM"], formats

    # A format the query advertises and the pipeline does not map.
    over = DEVICE_FIXTURE.replace(
        "   case VK_FORMAT_R32_SFLOAT:\n",
        "   case VK_FORMAT_R32_SFLOAT:\n   case VK_FORMAT_R16_SFLOAT:\n")
    refuses("VK_FORMAT_R16_SFLOAT",
            lambda: audit(PIPELINE_FIXTURE, over))
    refuses("reach no r300_vertex_format_id",
            lambda: audit(PIPELINE_FIXTURE, over))

    # A format the pipeline maps and the query withholds.
    under = DEVICE_FIXTURE.replace("   case VK_FORMAT_R8G8_UNORM:\n", "")
    refuses("VK_FORMAT_R8G8_UNORM", lambda: audit(PIPELINE_FIXTURE, under))
    refuses("cannot reach a route that executes",
            lambda: audit(PIPELINE_FIXTURE, under))

    # A label whose only path is the INVALID return is not admitted, so
    # advertising it promises a pipeline the driver refuses.
    # The label carries its own INVALID return, ahead of default: a
    # label stacked onto default resets before the return and would
    # never reach the admitting branch at all.
    invalid = PIPELINE_FIXTURE.replace(
        "   default:\n      return R300_VERTEX_FORMAT_INVALID;",
        "   case VK_FORMAT_R64_SFLOAT:\n"
        "      return R300_VERTEX_FORMAT_INVALID;\n   default:\n"
        "      return R300_VERTEX_FORMAT_INVALID;")
    assert "VK_FORMAT_R64_SFLOAT" not in admitted_formats(invalid)
    advertises_invalid = DEVICE_FIXTURE.replace(
        "   case VK_FORMAT_R32_SFLOAT:\n",
        "   case VK_FORMAT_R32_SFLOAT:\n   case VK_FORMAT_R64_SFLOAT:\n")
    refuses("VK_FORMAT_R64_SFLOAT",
            lambda: audit(invalid, advertises_invalid))

    refuses("is not defined in the source",
            lambda: audit("static int nothing(void) { return 0; }",
                          DEVICE_FIXTURE))
    refuses("grants VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT to no VkFormat",
            lambda: audit(PIPELINE_FIXTURE,
                          DEVICE_FIXTURE.replace(VERTEX_BUFFER_BIT,
                                                 "VK_FORMAT_FEATURE_2_BLIT_SRC_BIT")))
    # A comment naming a format is not a case label.
    commented = DEVICE_FIXTURE.replace(
        "   case VK_FORMAT_R32_SFLOAT:\n",
        "   /* case VK_FORMAT_R64_SFLOAT: */\n   case VK_FORMAT_R32_SFLOAT:\n")
    audit(PIPELINE_FIXTURE, commented)
    print("vertex-buffer format agreement selftest: pass")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pipeline", type=Path)
    parser.add_argument("--physical-device", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        selftest()
        return 0
    if args.pipeline is None or args.physical_device is None:
        parser.error("--pipeline and --physical-device are both required")
    try:
        formats = audit(args.pipeline.read_text(encoding="utf-8"),
                        args.physical_device.read_text(encoding="utf-8"))
    except AuditRefusal as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 1
    print(f"vertex-buffer formats: {len(formats)} advertised and executed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
