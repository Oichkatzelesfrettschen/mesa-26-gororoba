# SPDX-License-Identifier: MIT
#
# The canonical mutation table for the public RB2D fill cell.  One
# descriptor per mutation drives two legs: the in-process qualification leg
# (the non-submitting arming runner, which names the refusing gate in its
# report) and the loader-only public leg (libvulkan plus libc through
# vkQueueSubmit, where the common queue layer collapses every driver
# refusal to VK_ERROR_DEVICE_LOST).  The pairing is what makes the coarse
# public result legible: the same descriptor asserts the exact internal
# refusal, the public VkResult, the shim's CS count, and the attempt
# directory's state, so a public arm is never a hand-paired twin of an
# in-process test.
#
# Symbolic values resolve per leg through resolve(): "@stale_digest" is the
# armed digest with its first hex digit flipped, "@wrong_identity" the
# submission identity computed for the wrong destination handle, "@unset"
# removes the variable.  A leg that cannot express a field states so in
# its own column rather than skipping the row silently.

FILL_IDENTITY = "R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3"
IB_BLAKE3 = "R3V_NATIVE_AUTHORIZED_IB_BLAKE3"
KERNEL_RELEASE = "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE"
MODULE_SRCVERSION = "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION"
HAZARD_GATE = "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"
RUNNER_HANDLE = "R3V_NATIVE_RUNNER_DESTINATION_HANDLE"

PUBLIC_RESULT = "VK_ERROR_DEVICE_LOST"
SHIM_CS_COUNT = 0
DIRECTORY_STATE = "unspent"

# mutation_id, mutated field, runner environment change (the in-process
# leg), loader declaration change, loader application change, loader shim
# change, expected internal refusal marker in the runner's report.
MUTATIONS = (
    ("closed_gate", "hazard gate",
     {HAZARD_GATE: "@unset"}, {HAZARD_GATE: "@unset"}, {}, {},
     "hazard gate"),
    ("wrong_stream_digest", "authorized IB blake3",
     {IB_BLAKE3: "@stale_digest"}, {IB_BLAKE3: "@stale_digest"}, {}, {},
     "MISMATCH"),
    ("wrong_kernel_release", "authorized kernel release",
     {KERNEL_RELEASE: "0.0.0-fixture"}, {KERNEL_RELEASE: "0.0.0-fixture"},
     {}, {}, "MISMATCH"),
    ("wrong_module_srcversion", "authorized module srcversion",
     {MODULE_SRCVERSION: "WRONGSRCVERSION000000"},
     {MODULE_SRCVERSION: "WRONGSRCVERSION000000"}, {}, {}, "MISMATCH"),
    ("wrong_fill_identity", "authorized submission identity",
     {FILL_IDENTITY: "@stale_identity"}, {FILL_IDENTITY: "@stale_identity"},
     {}, {}, "different submission"),
    ("undeclared_fill_identity", "authorized submission identity absent",
     {FILL_IDENTITY: "@unset"}, {FILL_IDENTITY: "@unset"}, {}, {},
     "no submission identity"),
    ("wrong_destination_handle", "destination buffer-object name",
     {RUNNER_HANDLE: "@handle_plus_one"}, {FILL_IDENTITY: "@wrong_identity"},
     {}, {}, "different submission"),
    ("wrong_fill_value", "fill value (identity input)",
     {FILL_IDENTITY: "@stale_identity"}, {},
     {"R3V_LOADER_FILL_VALUE": "0x11223345"}, {}, "different submission"),
    ("wrong_rectangle", "fill byte count (identity input)",
     {FILL_IDENTITY: "@stale_identity"}, {},
     {"R3V_LOADER_FILL_BYTES": "4988"}, {}, "different submission"),
    ("wrong_platform_subsystem", "PCI subsystem pair",
     {"@sysfs": "wrong_subsystem"}, {}, {},
     {"R3V_DRM_SHIM_SUBSYSTEM_ID": "1028:0000"}, "no qualified platform"),
    ("wrong_dmi_product", "DMI product name",
     {"@sysfs": "wrong_dmi"}, {}, {},
     {"R3V_DRM_SHIM_DMI_PRODUCT_NAME": "Latitude D520"},
     "no qualified platform"),
)

# Rows the loader leg expresses through the evidence directory rather than
# the environment; the runner leg names the same state.
DIRECTORY_MUTATIONS = (
    ("absent_directory", "evidence directory absent", "ABSENT"),
    ("spent_token", "attempt.token present", "already attempted"),
)


def resolve(value, symbols):
    """Resolve one table value against a leg's symbol dictionary; None
    means the variable is removed."""
    if value == "@unset":
        return None
    if isinstance(value, str) and value.startswith("@"):
        if value not in symbols:
            raise KeyError(f"mutation table symbol {value} is not resolved "
                           f"by this leg")
        return symbols[value]
    return value


def stale(digest):
    return ("1" if digest[0] != "1" else "0") + digest[1:]
