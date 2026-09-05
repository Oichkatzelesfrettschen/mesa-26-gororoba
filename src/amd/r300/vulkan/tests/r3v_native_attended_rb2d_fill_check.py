# SPDX-License-Identifier: MIT
"""Declaration ladder of the attended RB2D fill application.

Every refusal the attended binary can make ahead of vkCreateInstance is
driven here with one declared fact wrong at a time, and each must be
named by its own message with the receipt directory holding the refusal
record alone.  The complete, self-consistent declaration then runs on
this host: its manifest, ICD, and self digests, its environment
equalities, and its cell numbers all hold, so the run reaches the host
facts and is refused by whichever differs (on a host without the radeon
module, the srcversion).  No leg reaches vkCreateInstance; the silicon
leg is the attended run itself, under its token.
"""

import hashlib
import os
import platform
import subprocess
import sys
import tempfile

CELL = {"fill_offset": "12", "fill_bytes": "4992", "fill_value": "0x11223344",
        "memory_type_index": "0", "wait_bound_ns": "30000000000"}


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def read(path, default):
    try:
        with open(path, encoding="ascii") as f:
            return f.readline().strip()
    except OSError:
        return default


def main():
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <attended> <icd-manifest> <icd-dso>",
              file=sys.stderr)
        return 2
    attended, manifest, icd_dso = sys.argv[1:]
    with tempfile.TemporaryDirectory() as work:
        manifest_dir = os.path.join(work, "manifest-dir")
        os.mkdir(manifest_dir)
        declaration = {
            "vk_driver_files": manifest,
            "icd_manifest_sha256": sha256(manifest),
            "icd_dso": icd_dso,
            "icd_sha256": sha256(icd_dso),
            "attended_application_sha256": sha256(attended),
            "kernel_release": platform.release(),
            "module_srcversion": read("/sys/module/radeon/srcversion",
                                      "NOMODULE0000000000000000"),
            "boot_id": read("/proc/sys/kernel/random/boot_id", "no-boot-id"),
            "authorized_ib_blake3": "0" * 64,
            "authorized_fill_identity_blake3": "1" * 64,
            "manifest_dir": manifest_dir,
            **CELL,
        }
        base_env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("R3V_", "LD_PRELOAD"))}
        base_env.update({
            "VK_DRIVER_FILES": manifest,
            "R3V_NATIVE_AUTHORIZED_IB_BLAKE3": declaration["authorized_ib_blake3"],
            "R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3":
                declaration["authorized_fill_identity_blake3"],
            "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE": declaration["kernel_release"],
            "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION":
                declaration["module_srcversion"],
            "R3V_NATIVE_MANIFEST_DIR": manifest_dir,
            "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED": "1",
            "R3V_NATIVE_EXECUTION_POLICY": "gpu_only",
            "R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL": "1",
        })
        count = 0

        def run(label, decl, env_change=None, expect_marker=None,
                expect_status=2):
            nonlocal count
            count += 1
            path = os.path.join(work, f"declaration-{count}.txt")
            with open(path, "w", encoding="ascii") as f:
                for key, value in decl.items():
                    f.write(f"{key}={value}\n")
            receipt = os.path.join(work, f"receipt-{count}")
            os.mkdir(receipt)
            env = dict(base_env)
            if env_change:
                for key, value in env_change.items():
                    if value is None:
                        env.pop(key, None)
                    else:
                        env[key] = value
            result = subprocess.run([attended, path, receipt], env=env,
                                    capture_output=True, text=True)
            print(f"{label}: status {result.returncode}, "
                  f"{result.stderr.strip().splitlines()[-1] if result.stderr.strip() else '(no stderr)'}")
            if result.returncode != expect_status:
                fail(f"{label}: status {result.returncode}\n{result.stdout}\n"
                     f"{result.stderr}")
            if "phase=instance" in result.stdout:
                fail(f"{label}: reached vkCreateInstance")
            if expect_marker and expect_marker not in result.stderr:
                fail(f"{label}: refusal does not name '{expect_marker}':\n"
                     f"{result.stderr}")
            if os.listdir(receipt) != ["outcome.json"]:
                fail(f"{label}: receipt dir holds {os.listdir(receipt)}")
            return result

        # Absent and malformed declarations.
        run("absent declaration", {}, expect_marker="declaration lacks")
        bad = dict(declaration)
        bad["mystery"] = "1"
        run("unknown key", bad, expect_marker="unknown key")

        # Each declared fact wrong alone.
        for key, marker in (
                ("vk_driver_files", "VK_DRIVER_FILES is"),
                ("icd_manifest_sha256", "the manifest"),
                ("icd_sha256", "the ICD"),
                ("attended_application_sha256", "this executable hashes"),
                ("authorized_ib_blake3", "R3V_NATIVE_AUTHORIZED_IB_BLAKE3"),
                ("authorized_fill_identity_blake3",
                 "R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3"),
                ("manifest_dir", "R3V_NATIVE_MANIFEST_DIR"),
                ("fill_offset", "differs from the sealed cell"),
                ("fill_bytes", "differs from the sealed cell"),
                ("fill_value", "differs from the sealed cell"),
                ("wait_bound_ns", "wait bound"),
        ):
            bad = dict(declaration)
            bad[key] = "0" if key != "wait_bound_ns" else "0"
            if key in ("vk_driver_files", "icd_dso", "manifest_dir"):
                bad[key] = os.path.join(work, "no-such-path")
            run(f"wrong {key}", bad, expect_marker=marker)
        bad = dict(declaration)
        bad["memory_type_index"] = "x"
        run("malformed memory_type_index", bad, expect_marker="malformed")

        # Each environment gate absent or wrong alone.
        for key in ("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
                    "R3V_NATIVE_EXECUTION_POLICY",
                    "R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL",
                    "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
                    "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
                    "VK_DRIVER_FILES"):
            run(f"unset {key}", declaration, env_change={key: None},
                expect_marker=key)
        run("policy auto", declaration,
            env_change={"R3V_NATIVE_EXECUTION_POLICY": "auto"},
            expect_marker="R3V_NATIVE_EXECUTION_POLICY")

        # The complete declaration reaches the host facts.  On the board
        # every fact holds and the next phase is vkCreateInstance; on a
        # host without the module, or with a different kernel or boot,
        # the first host fact that differs refuses.
        bad = dict(declaration)
        bad["kernel_release"] = "0.0.0-none"
        run("wrong kernel_release", bad, env_change={
            "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE": "0.0.0-none"},
            expect_marker="the kernel release")
        bad = dict(declaration)
        bad["boot_id"] = "00000000-0000-0000-0000-000000000000"
        result = run("wrong boot_id", bad, expect_marker="the ")
        if "the boot id" not in result.stderr and \
                "srcversion is unreadable" not in result.stderr:
            fail("wrong boot id: refused by neither the boot id nor the "
                 "absent module")

    print(f"r3v_native_attended_rb2d_fill_check: {count} refusal legs, none "
          f"reached vkCreateInstance")
    return 0


if __name__ == "__main__":
    sys.exit(main())
