# SPDX-License-Identifier: MIT
"""Binary-role matrix for the public RB2D fill cell.

Three executables carry three roles, and a qualification that binds only
two of them proves transport and arming while owning nothing that can
write the destination on silicon:

  binary                              shim      without shim  writes dest
  r3v_native_rb2d_fill_arming_runner  no        yes           no
  r3v_native_loader_fill_application  required  refuses       no
  r3v_native_attended_rb2d_fill       refuses   yes           yes

Each row is exercised as an executable fact: the attended binary is
present, distinct by digest, loader-only by its dynamic section and
symbol table, refuses a preloaded shim before vkCreateInstance, and its
oracle classifies an unchanged destination as NO_DEVICE_WRITE with an
expected change of 4992 bytes; the shim binary refuses a missing shim;
the arming runner links neither libvulkan nor libdrm and names no DRM
node.  The host-write discriminator runs the shim binary's host executor
over its protected mapping and requires SIGSEGV.  A role table that
listed only the first two rows is the state that let the v2 prediction
seal without an attended executable; this gate fails on that state.
"""

import hashlib
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile

ATTENDED_FORBIDDEN_PREFIXES = ("r3v_native_", "r300_", "radeon_drm_vk_",
                               "drm_shim_")
ATTENDED_ALLOWED_NEEDED = {"libvulkan.so.1", "libc.so.6"}
RUNNER_FORBIDDEN_NEEDED_PREFIXES = ("libvulkan", "libdrm")
SPECIMEN_SUBSYSTEM = "1028:022a"
SPECIMEN_DMI = "Vostro   1000 "
FIXTURE_SRCVERSION = "FIXTURESRCVERSION0000000"


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def elf_needed(path):
    """DT_NEEDED entries of a 64-bit little-endian ELF, read directly."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        fail(f"{path} is not a 64-bit little-endian ELF")
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x3a)
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from("<I", data, off + 4)[0]
        sh_offset, sh_size = struct.unpack_from("<QQ", data, off + 0x18)
        sh_link = struct.unpack_from("<I", data, off + 0x28)[0]
        sections.append((sh_type, sh_offset, sh_size, sh_link))
    needed = []
    for sh_type, sh_offset, sh_size, sh_link in sections:
        if sh_type != 6:  # SHT_DYNAMIC
            continue
        _, str_off, str_size, _ = sections[sh_link]
        strtab = data[str_off:str_off + str_size]
        for pos in range(sh_offset, sh_offset + sh_size, 16):
            d_tag, d_val = struct.unpack_from("<qQ", data, pos)
            if d_tag == 1:  # DT_NEEDED
                end = strtab.index(b"\0", d_val)
                needed.append(strtab[d_val:end].decode("ascii"))
    return needed


def nm_table(nm, path):
    result = subprocess.run([nm, path], capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"nm {path}: {result.stderr}")
    return [line.split() for line in result.stdout.splitlines() if line.split()]


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def clean_env():
    env = dict(os.environ)
    for key in list(env):
        if key.startswith(("R3V_NATIVE_", "R3V_DRM_SHIM_", "R3V_LOADER_")) or \
                key == "LD_PRELOAD":
            del env[key]
    return env


def main():
    if len(sys.argv) != 8:
        print(f"usage: {sys.argv[0]} <nm> <attended> <shim-application> "
              f"<arming-runner> <drm-shim.so> <oracle-tool> <icd-dso>",
              file=sys.stderr)
        return 2
    nm, attended, shim_app, runner, shim, oracle, icd_dso = sys.argv[1:]
    rows = []

    # Presence and distinctness.
    for path in (attended, shim_app, runner):
        if not os.access(path, os.X_OK):
            fail(f"the executable {path} is absent")
    digests = {os.path.basename(p): sha256(p) for p in (attended, shim_app, runner)}
    if len(set(digests.values())) != 3:
        fail(f"the three roles are not three distinct executables: {digests}")
    rows.append("attended executable present and distinct: PASS")

    # The attended binary's link surface.
    needed = elf_needed(attended)
    if not set(needed) <= ATTENDED_ALLOWED_NEEDED or "libvulkan.so.1" not in needed:
        fail(f"the attended executable needs {needed}; admitted "
             f"{sorted(ATTENDED_ALLOWED_NEEDED)}")
    forbidden = sorted({f[-1] for f in nm_table(nm, attended)
                        if f[-1].startswith(ATTENDED_FORBIDDEN_PREFIXES)})
    if forbidden:
        fail(f"the attended executable carries {forbidden}")
    if not any(len(f) >= 2 and f[-2] == "U" and f[-1] == "vkCreateInstance"
               for f in nm_table(nm, attended)):
        fail("the attended executable does not import vkCreateInstance")
    rows.append(f"attended loader-only link surface {needed}: PASS")

    with tempfile.TemporaryDirectory() as work:
        # The attended binary refuses a preloaded shim before any Vulkan
        # call; the same run against the shim binary cannot satisfy this
        # row, which is what stops a substitution.
        receipt = os.path.join(work, "receipt-preload")
        os.mkdir(receipt)
        env = clean_env()
        env["LD_PRELOAD"] = shim
        env["RADEON_GPU_ID"] = "0x5974"
        env["DRM_SHIM_EXPECTED_DSO"] = shim
        result = subprocess.run([attended, os.devnull, receipt], env=env,
                                capture_output=True, text=True)
        if result.returncode != 2 or \
                "INFRASTRUCTURE_REFUSAL: LD_PRELOAD is set" not in result.stderr \
                or "phase=instance" in result.stdout or \
                "phase=preflight" not in result.stdout:
            fail(f"attended under a preloaded shim: status {result.returncode}\n"
                 f"{result.stdout}\n{result.stderr}")
        if sorted(os.listdir(receipt)) != ["outcome.json"]:
            fail(f"the refusal left {os.listdir(receipt)} in the receipt dir")
        with open(os.path.join(receipt, "outcome.json"), encoding="utf-8") as f:
            if '"outcome": "INFRASTRUCTURE_REFUSAL"' not in f.read():
                fail("the refusal record does not name INFRASTRUCTURE_REFUSAL")
        rows.append("attended rejects LD_PRELOAD before vkCreateInstance: PASS")

        # The shim binary refuses a missing shim.
        env = clean_env()
        env["R3V_EXPECTED_ICD_DSO"] = icd_dso
        env["R3V_LOADER_FILL_EXPECT"] = "submitted"
        result = subprocess.run([shim_app], env=env, capture_output=True,
                                text=True)
        if result.returncode != 2 or \
                "drm-shim is not preloaded" not in result.stderr:
            fail(f"shim application without the shim: status "
                 f"{result.returncode}\n{result.stderr}")
        rows.append("shim application refuses a missing shim: PASS")

        # The attended oracle expects a device write of exactly the
        # interval: the initialized image, which is byte-identical to the
        # destination the shim binary verifies unchanged after its routed
        # submission, classifies as NO_DEVICE_WRITE.
        initial = os.path.join(work, "initial.bin")
        subprocess.run([oracle, "--emit-initial", initial], check=True)
        result = subprocess.run([oracle, "--classify", initial],
                                capture_output=True, text=True)
        fields = dict(re.findall(r"^(\w+)=(\S+)$", result.stdout, re.MULTILINE))
        if result.returncode != 1 or fields.get("outcome") != "NO_DEVICE_WRITE" \
                or fields.get("expected_changed_bytes") != "4992" or \
                fields.get("expected_changed_dwords") != "1248":
            fail(f"oracle over the unchanged shim result: status "
                 f"{result.returncode}\n{result.stdout}")
        rows.append("attended oracle expects 4992 changed bytes; unchanged "
                    "destination is NO_DEVICE_WRITE: PASS")

        # The arming runner creates no Vulkan object and opens no DRM
        # node: its dynamic section names neither libvulkan nor libdrm,
        # its symbol table imports no vk entry point, its image carries
        # no DRM node path, and a run leaves the preview directory as it
        # found it.
        runner_needed = elf_needed(runner)
        bad = [n for n in runner_needed
               if n.startswith(RUNNER_FORBIDDEN_NEEDED_PREFIXES)]
        if bad:
            fail(f"the arming runner links {bad}")
        if any(len(f) >= 2 and f[-2] == "U" and f[-1].startswith("vk")
               for f in nm_table(nm, runner)):
            fail("the arming runner imports a Vulkan entry point")
        with open(runner, "rb") as f:
            if b"/dev/dri" in f.read():
                fail("the arming runner names a DRM node")
        sysfs = os.path.join(work, "sys")
        os.makedirs(os.path.join(sysfs, "module", "radeon"))
        with open(os.path.join(sysfs, "module", "radeon", "srcversion"), "w",
                  encoding="ascii") as f:
            f.write(FIXTURE_SRCVERSION + "\n")
        preview = os.path.join(work, "preview")
        os.mkdir(preview)
        env = clean_env()
        env["R3V_NATIVE_RUNNER_SYSFS_ROOT"] = sysfs
        env["R3V_NATIVE_RUNNER_DESTINATION_HANDLE"] = "1"
        result = subprocess.run([runner, preview], env=env,
                                capture_output=True, text=True)
        if os.listdir(preview) or "no submission attempted" not in result.stdout:
            fail(f"the arming runner wrote {os.listdir(preview)} or omitted "
                 f"the no-submission statement")
        rows.append(f"arming runner links {runner_needed}, imports no vk "
                    f"entry point, names no DRM node, preview untouched: PASS")

        # Host-write discriminator: the shim binary's host executor over
        # the protected mapping terminates by SIGSEGV.  The routed leg
        # that leaves the same mapping untouched is the armed leg of
        # r3v-native-loader-fill-application.
        env = clean_env()
        env.update({
            "LD_PRELOAD": shim,
            "RADEON_GPU_ID": "0x5974",
            "DRM_SHIM_EXPECTED_DSO": shim,
            "R3V_EXPECTED_ICD_DSO": icd_dso,
            "R3V_DRM_SHIM_SUBSYSTEM_ID": SPECIMEN_SUBSYSTEM,
            "R3V_DRM_SHIM_DMI_PRODUCT_NAME": SPECIMEN_DMI,
            "R3V_DRM_SHIM_MODULE_SRCVERSION": FIXTURE_SRCVERSION,
            "R3V_NATIVE_MANIFEST_DIR": os.path.join(work, "host-evidence"),
            "R3V_NATIVE_EXECUTION_POLICY": "auto",
            "R3V_LOADER_FILL_EXPECT": "host-filled",
            "R3V_LOADER_FILL_PROTECT": "1",
        })
        os.mkdir(env["R3V_NATIVE_MANIFEST_DIR"])
        result = subprocess.run([shim_app], env=env, capture_output=True,
                                text=True)
        if result.returncode != -signal.SIGSEGV:
            fail(f"host executor over the protected mapping: status "
                 f"{result.returncode}, expected SIGSEGV\n{result.stdout}\n"
                 f"{result.stderr}")
        rows.append("host store over the protected mapping: SIGSEGV")

    for row in rows:
        print(row)
    print("r3v_native_rb2d_fill_role_matrix_check: three roles, three "
          "executables, each row held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
