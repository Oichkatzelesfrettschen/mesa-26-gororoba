# SPDX-License-Identifier: MIT
#
# The plan-capture contract of the public RB2D fill cell, four facts a
# single run of the loader application proves together:
#
#   generic plan capture with the hazard gate open   MUST REFUSE
#   exact submit capture                             MUST occur at the
#                                                    drm-shim interception
#   independent plan                                 MUST be assembled here
#                                                    from raw packet words,
#                                                    apart from the common
#                                                    RB2D plan
#   comparison                                       MUST prove exact IB
#                                                    equality plus semantic
#                                                    equality of relocations,
#                                                    buffer roles, domains,
#                                                    and rectangle geometry
#
# The device refuses a capture session while the hazard gate is open, and
# the route runs only with it open, so the capture file can never observe
# this cell; that refusal is the first invariant.  The shim's retained
# submit object is the second, and it is what the attempt must reproduce.
# The checker fails when capture unexpectedly succeeds or when the shim
# artifact is absent.
#
# Usage: r3v_native_rb2d_fill_capture_contract_check.py <loader-app> <runner>

import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r3v_native_loader_fill_application_check as loader  # noqa: E402

FILL_OFFSET = 12
FILL_BYTES = 4992
FILL_VALUE = 0x11223344
PITCH = 256
CPP = 4
ALLOCATION = loader.CELL_ALLOCATION_BYTES
COMPLETION = loader.COMPLETION_BYTES
GTT = loader.GTT

REG = {"DST_PITCH_OFFSET": 0x142C, "SC_TOP_LEFT": 0x16EC,
       "SC_BOTTOM_RIGHT": 0x16F0, "DEFAULT_SC_BOTTOM_RIGHT": 0x16E8,
       "DP_GUI_MASTER_CNTL": 0x146C, "DP_CNTL": 0x16C0,
       "DP_WRITE_MSK": 0x16CC, "DP_BRUSH_FRGD_CLR": 0x147C,
       "DST_Y_X": 0x1438, "DST_WIDTH_HEIGHT": 0x1598,
       "DSTCACHE_CTLSTAT": 0x1714, "WAIT_UNTIL": 0x1720}
REG_NAME = {v: k for k, v in REG.items()}


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def linear_span_rects(offset, size, pitch, cpp):
    """The rectangle tiling of one byte interval on a virtual surface:
    a first partial row, whole rows, and a tail, each nonempty."""
    row_pixels = pitch // cpp
    first_pixel = offset // cpp
    pixels = size // cpp
    if offset % cpp or size % cpp:
        raise ValueError("interval is not pixel aligned")
    rects = []
    x, y = first_pixel % row_pixels, first_pixel // row_pixels
    if x:
        first = min(pixels, row_pixels - x)
        rects.append((x, y, first, 1))
        pixels -= first
        y += 1
    whole = pixels // row_pixels
    if whole:
        rects.append((0, y, row_pixels, whole))
        pixels -= whole * row_pixels
        y += whole
    if pixels:
        rects.append((0, y, pixels, 1))
    for _x, _y, w, h in rects:
        if w == 0 or h == 0:
            raise ValueError("tiling produced an empty rectangle")
    return rects


def assemble_independent_plan(rects, value, pitch, reloc_index):
    words = []

    def pkt0(reg, v):
        words.append(REG[reg] >> 2)
        words.append(v & 0xffffffff)

    pkt0("DST_PITCH_OFFSET", (pitch >> 6) << 22)
    words.append((3 << 30) | (0x10 << 8))
    words.append(reloc_index * 4)
    pkt0("SC_TOP_LEFT", 0)
    pkt0("SC_BOTTOM_RIGHT", 0x1fff1fff)
    pkt0("DEFAULT_SC_BOTTOM_RIGHT", 0x1fff1fff)
    pkt0("DP_GUI_MASTER_CNTL", (1 << 1) | (13 << 4) | (6 << 8) |
         0x00f00000 | (1 << 28) | (1 << 30))
    pkt0("DP_CNTL", 3)
    pkt0("DP_WRITE_MSK", 0xffffffff)
    for x, y, w, h in rects:
        pkt0("DP_BRUSH_FRGD_CLR", value)
        pkt0("DST_Y_X", (y << 16) | x)
        pkt0("DST_WIDTH_HEIGHT", (w << 16) | h)
    pkt0("DSTCACHE_CTLSTAT", 0xf)
    pkt0("WAIT_UNTIL", (1 << 9) | (1 << 16) | (1 << 18))
    return struct.pack("<%dI" % len(words), *words)


def decode_stream(data):
    """Walk the retained stream: relocation sites, pitch, base, and the
    launched rectangles, decoded from the packet words alone."""
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    sites, rects, pitch, base, origin = [], [], None, None, None
    i = 0
    while i < len(words):
        header = words[i]
        ptype = (header >> 30) & 3
        count = (header >> 16) & 0x3FFF
        if ptype == 0:
            reg = (header & 0x1FFF) << 2
            one = (header >> 15) & 1
            for k in range(count + 1):
                here = reg if one else reg + 4 * k
                v = words[i + 1 + k]
                if here == REG["DST_PITCH_OFFSET"]:
                    pitch = ((v >> 22) & 0xff) << 6
                    base = (v & 0x3fffff) << 10
                    sites.append(i + 1 + k)
                elif here == REG["DST_Y_X"]:
                    origin = (v & 0xffff, v >> 16)
                elif here == REG["DST_WIDTH_HEIGHT"]:
                    if origin is None:
                        fail("launch before DST_Y_X in the retained stream")
                    rects.append((origin[0], origin[1], v >> 16, v & 0xffff))
        elif ptype == 3 and ((header >> 8) & 0xff) == 0x10:
            pass
        i += count + 2
    return sites, rects, pitch, base


def byte_set(rects, pitch, cpp, base):
    covered = set()
    for x, y, w, h in rects:
        for row in range(y, y + h):
            start = base + row * pitch + x * cpp
            covered.update(range(start, start + w * cpp))
    return covered


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <loader-fill-application> "
              f"<rb2d-fill-arming-runner>", file=sys.stderr)
        return 2
    application, runner = sys.argv[1], sys.argv[2]
    import platform
    import tempfile
    kernel = platform.release()
    with tempfile.TemporaryDirectory() as work:
        sysfs = os.path.join(work, "sys")
        os.makedirs(os.path.join(sysfs, "module", "radeon"))
        with open(os.path.join(sysfs, "module", "radeon", "srcversion"),
                  "w", encoding="ascii") as f:
            f.write(loader.FIXTURE_SRCVERSION + "\n")
        scratch = os.path.join(work, "runner-scratch")
        os.mkdir(scratch)
        digest, identity, dwords, _sites = loader.runner_report(
            runner, sysfs, scratch, loader.DESTINATION_HANDLE)
        declaration = {
            "R3V_NATIVE_AUTHORIZED_IB_BLAKE3": digest,
            "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE": kernel,
            "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION":
                loader.FIXTURE_SRCVERSION,
            "R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3": identity,
        }
        leg = loader.Leg(application, work)

        # 1. Generic plan capture with the hazard gate open refuses the
        #    device, records nothing, and reaches no CS.
        plan = os.path.join(work, "plan-capture.json")
        result, evidence, present = leg.run("capture with gate open",
                                            "submitted", declare=declaration,
                                            plan=plan)
        if result.returncode == 0:
            fail("plan capture with the hazard gate open was admitted; the "
                 "device must refuse the capture session")
        if "vkCreateDevice: VK_ERROR_INITIALIZATION_FAILED" not in \
                result.stderr:
            fail(f"capture with gate open: the refusal was not "
                 f"VK_ERROR_INITIALIZATION_FAILED at vkCreateDevice\n"
                 f"{result.stderr}")
        if os.path.exists(plan):
            fail("capture with gate open: a plan file was written")
        if present:
            fail(f"capture with gate open: retained {present}")
        print("plan capture with the hazard gate open: REFUSED at "
              "vkCreateDevice, no plan file, directory unspent")

        # 2. The exact submit capture at the shim interception.
        result, evidence, present = leg.run("armed", "submitted",
                                            declare=declaration)
        if result.returncode != 0 or \
                loader.field(result.stdout, "shim_cs_ioctls") != "1":
            fail("armed: the shim did not intercept exactly one CS")
        for name in ("ib.bin", "submit_relocs.bin", "submit_manifest.json"):
            if name not in present:
                fail(f"armed: shim submit artifact {name} is absent")
        with open(os.path.join(evidence, "ib.bin"), "rb") as f:
            retained = f.read()
        print(f"exact submit capture: {len(retained) // 4} dwords retained "
              f"at the shim")

        # 3. The independent plan, assembled here from raw words.
        rects = linear_span_rects(FILL_OFFSET, FILL_BYTES, PITCH, CPP)
        independent = assemble_independent_plan(rects, FILL_VALUE, PITCH, 0)
        if independent != retained:
            fail(f"independent plan ({len(independent)} bytes) differs "
                 f"from the retained IB ({len(retained)} bytes)")
        if len(retained) != dwords * 4:
            fail("retained IB length differs from the runner's dword count")
        print(f"independent plan: {len(rects)} rectangles {rects}, "
              f"byte-identical to the retained IB")

        # 4. Semantic equality: relocation entries, buffer roles and
        #    domains, and the rectangle geometry's byte set.
        sites, decoded_rects, pitch, base = decode_stream(retained)
        if sites != [1]:
            fail(f"relocation sites {sites}, expected the DST_PITCH_OFFSET "
                 f"payload at dword 1 alone")
        if pitch != PITCH or base != 0:
            fail(f"decoded pitch {pitch} base {base}")
        if decoded_rects != rects:
            fail(f"decoded rectangles {decoded_rects} differ from the "
                 f"tiling {rects}")
        covered = byte_set(decoded_rects, pitch, CPP, base)
        expected = set(range(FILL_OFFSET, FILL_OFFSET + FILL_BYTES))
        if covered != expected:
            missing = sorted(expected - covered)[:4]
            extra = sorted(covered - expected)[:4]
            fail(f"byte set differs: missing {missing} extra {extra}")
        with open(os.path.join(evidence, "submit_relocs.bin"), "rb") as f:
            raw = f.read()
        entries = [struct.unpack_from("<4I", raw, 16 * k)
                   for k in range(len(raw) // 16)]
        if [(h, r, w) for h, r, w, _f in entries] != \
                [(loader.DESTINATION_HANDLE, 0, GTT),
                 (loader.DESTINATION_HANDLE + 1, 0, GTT)]:
            fail(f"relocation entries {entries} differ from destination "
                 f"and completion, both GTT write")
        with open(os.path.join(evidence, "submit_manifest.json"),
                  encoding="utf-8") as f:
            manifest = json.load(f)
        rows = manifest.get("bo_table")
        if not isinstance(rows, list) or len(rows) != 2:
            fail("submit_manifest.json carries no two-row bo_table")
        roles = [(row.get("role"), row.get("size"), row.get("write_domain"))
                 for row in rows]
        if roles[0][1:] != (ALLOCATION, GTT) or \
                roles[1] != ("completion", COMPLETION, GTT):
            fail(f"bo_table roles {roles} differ from the contract")
        print(f"semantic equality: 1 relocation site, {len(entries)} "
              f"relocation entries, {len(rows)} buffer references, byte set "
              f"[{FILL_OFFSET}, {FILL_OFFSET + FILL_BYTES}) covered exactly")
    print("r3v_native_rb2d_fill_capture_contract_check: capture refuses with "
          "the gate open; the shim submit object is retained; the "
          "independent plan and the semantic contract both match it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
