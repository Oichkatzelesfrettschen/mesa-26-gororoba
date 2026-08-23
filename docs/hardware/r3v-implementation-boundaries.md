# R3V implementation boundaries and Vulkan completion criteria

## Status

The current-source statements in this document describe
`mesa-26-gororoba` at the commit that carries this document revision;
`git log -1 -- docs/hardware/r3v-implementation-boundaries.md` names it.

The R3V ICD, `libvulkan_r3v.so` under `driverName` `r3v`, is a Gallium-free
library with an owned Radeon DRM transport,
GEM-backed memory whose host coherency the driver maintains itself over the
unsnooped GART, queue and command-carrier objects, and one privately
injected fixed-cell PM4 lowering path carrying a compiler-produced fragment
program. Submission sits behind a multi-factor arming conjunction with
one-shot disarm, and both the semantic cell and the exact submit object
retain as digest-bound evidence. Its evidence stands at the host-unit,
build/link, no-submit PM4, offline kernel-parser, drm-shim host-model, and
one-shot silicon classes. Foundational native-cell records and the
procedure-linked R2VB records exist for their declared RS482 identities. The
first foundational cell submitted the bare inherited-state stream and left
the target at its sentinel fill; its kernel acceptance, retirement, and
oracle are
operator-observed because the runner output is not retained. The
direct-write 2D control's retained outcome proves the transport carries
device writes byte-exact through the same BO, cache, relocation, and
readback substrate; and the contract-prefixed 234-dword cell's retained
outcome reports the predicted triangle -- interior `0xff00ff00`,
exterior and canary at the sentinel. The first-run color-write cause
remains underdetermined. The gate matrix proves that
`US_OUT_FMT_0`, `RB3D_COLOR_CHANNEL_MASK`, and `SC_SCREENDOOR` each
independently suppress color writes on RS482, while its identical
readbacks cannot identify which predecessor state the first cell
inherited. The self-contained successor proves that establishing those
gates enables the witnessed raster output; it does not identify the
historical predecessor state. The direct-write control proves the shared
BO, cache, relocation, and readback transport carries device writes; it
does not identify a 3D color-write gate. The proven raster path is that
one fixed cell. The public recording surface now reaches it: a bounded
render-pass/pipeline/draw vocabulary -- the qualified linear color
target at any extent inside the 64x64 maximum, a pipeline admitted by
byte equality with the reference SPIR-V pair and the cell's fixed
state vector, and a draw that gathers
the bound vertex buffer through the CPU executor, applies the Vulkan
viewport transform over the pass target's extent inside a bounded
clip-volume domain (w exactly 1, NDC inside the clip volume, so
scissor and clip coincide and the identity perspective divide is
exact), and lands the window-space records in a
command-buffer-owned carrier -- records the byte-identical cell IB
through public `vkCmd*` entry points, at the drm-shim host-model class
under the `r3v-native-public-surface` harness, and every contract
deviation poisons or refuses.  The loader boundary is proven at the
same host-model class: the `r3v-native-loader-application` gate links a
standalone application against the installed Vulkan loader alone,
reaches the ICD only through its manifest, performs the complete
instance-to-submit sequence, and byte-compares the submit-retained IB
against the independently emitted reference cell, while its symbol
audit holds the binary free of every audited driver-symbol prefix in
any binding, the reference SPIR-V data header being the one driver
artifact the application compiles in.
The recorded IB equals the digest the
arming authority qualifies, so the command-stream grammar the silicon
witnessed is what the public route records; the witness's rendered
pixels are bound to the window-space payload the attended run carried,
which the public route reproduces byte-exactly from the NDC triangle
the viewport transform maps onto it; other in-domain records or a
nonzero `firstVertex` change the carrier bytes the same IB fetches, an
input set the silicon has not yet observed.  General vertex routes and the
complete Vulkan semantic/conformance sections remain implementation
contracts.

The native graphics family carries a bounded transfer surface in addition
to the fixed render cell: one linear `B8G8R8A8_UNORM` format. Its single
`linearTilingFeatures` mask contains
`VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT`,
`VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT`, and
`VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT`.

Format capability and image-usage policy are separate contracts.
`r3v_GetPhysicalDeviceImageFormatProperties2` and `r3v_CreateImage`
admit color-attachment usage alone for the render family or a nonzero subset
of transfer usage bits for the transfer family; mixed usage is refused. The
transfer operations cover images up to 2048 texels per axis, region-validated
buffer/image copies, whole-image color clear, and an outside-render-pass
barrier no-op. They execute synchronously through host
mappings, publish destination memory, use no IB and issue no
`DRM_RADEON_CS` submission ioctl; a host mapping may issue
`DRM_RADEON_GEM_MMAP` through `radeon_drm_vk_bo_map()`. They carry host-unit
and drm-shim evidence rather than GPU-transfer or silicon evidence. Broader
transfer formats, usages, GPU copy packets, and target transfer observations
remain open. The [Vulkan queues rule](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#devsandqueues-physical-device-queue-properties)
requires an implementation that exposes graphics to expose at least one
family supporting both graphics and compute. The one family reports
`VK_QUEUE_GRAPHICS_BIT`, and `VK_QUEUE_COMPUTE_BIT` only behind the exact
`R3V_HYBRID_COMPUTE_EXPERIMENTAL=1` opt-in, under which
`r3v_native_compute.c` admits the identity-map kernel from SPIR-V words into
the common compute job and executes it on the CPU route at submission while
every other module refuses at pipeline creation; with the opt-in unset,
`r3v_CreateComputePipelines` returns `R3V_NATIVE_REFUSAL_RESULT`. This is an
experimental nonconformant surface rather than a Vulkan 1.0 capability claim,
and promotion stays blocked until the compute route stands without its
opt-in. The
extent gap is closed: `vkCreateImage` accepts every extent inside the
reported 64x64 maximum, and the cell family realizes it -- in TCL
bypass the extent reaches the hardware through the `SC_SCISSORS_BR`
and `SC_CLIPRECT_BR_0` payloads alone, `RB3D_COLORPITCH0` keeps the
64-pixel word because pitch is a memory-layout property, and at the
maximum extent the emission is byte-identical to the silicon-witnessed
reference cell, whose digest anchors the family.  The evidence classes
split at the reference: the 64x64 cell is host-model proven,
loader-boundary proven, and silicon witnessed; every other admitted
extent is host-model proven only.  A non-maximum extent differs from
the witnessed IB in the two scissor-family dwords, an input class the
silicon has not observed, and the checked-in attended submitter
records the 64x64 reference cell alone -- the arming runner's
`--extent` option is a no-submit digest and IB-generation facility, so
non-maximum silicon execution is blocked on a future parameterized
public-route runner with extent-aware retention.  Host support and
oracle eligibility are also separate sets: the extent oracle fails
closed when a target carries no margin-qualified samples, so an
API-admissible extent such as 1x1 is not thereby eligible for the
triangle raster oracle.

The bounded R300 R2VB `FLOAT_2` source transaction has one home:
`r300-r2vb-float2-source-contract.md`. This document owns the implementation
and conformance boundaries; that document owns the source-format transaction.

## Purpose

R3V has two distinct boundaries:

1. the R3V implementation: R3V-owned memory, queues, command lowering, PM4,
   and completion over the Radeon DRM transport;
2. complete Vulkan command semantics, synchronization, WSI, feature exposure,
   and conformance.

The Gallium-backed Vulkan lane that preceded the implementation is retired;
the retirement section below names the mechanism or fixture that carries each
of its former capabilities.

A result at one boundary never closes another. Source architecture, build and
link identity, runtime reachability, silicon execution, API semantics, and
conformance remain separate evidence classes.

The Linux Radeon driver owns memory placement, command validation, relocation,
submission, completion primitives, and hazard containment. Mesa userspace owns
Vulkan objects, R300 state construction, vertex semantics, route selection, and
execution planning. Kernel acceptance proves that a submitted tuple is safe; it
does not supply missing userspace semantics.

PALM and Terakan provide reusable direct-DRM engineering patterns. Their
Evergreen register values, packet semantics, cache rules, shader ISA, and
silicon results are not RS480 authority.

## Current-source authority

Each current-implementation claim binds to a named Mesa source location and a
structural query.

```sh
# Isolated-worktree and declared-source identity gate.
set -eu
expected_sha=${R3V_EXPECTED_SHA:?set R3V_EXPECTED_SHA to the audited commit}
canonical_root=${R3V_CANONICAL_ROOT:?set R3V_CANONICAL_ROOT to the canonical checkout}
canonical_root=$(realpath -e -- "$canonical_root")
worktree_root=$(realpath -e -- "$(git rev-parse --show-toplevel)")
test "$worktree_root" != "$canonical_root"
registered_worktree_matches() {
  expected_root=$1
  registered_worktree=false
  while IFS= read -r registered_root; do
    if test ! -e "$registered_root"; then
      continue
    fi
    registered_root=$(realpath -e -- "$registered_root")
    if test "$registered_root" = "$expected_root"; then
      registered_worktree=true
    fi
  done
  test "$registered_worktree" = true
}

registered_worktree_matches "$worktree_root" <<EOF
$(git worktree list --porcelain | sed -n 's/^worktree //p')
EOF
prunable_fixture="$worktree_root/.r3v-missing-worktree-fixture"
test ! -e "$prunable_fixture"
registered_worktree_matches "$worktree_root" <<EOF
$prunable_fixture
$worktree_root
EOF
test "$(git rev-parse --verify HEAD)" = "$expected_sha"
test -z "$(git status --porcelain=v2)"
git diff --quiet
git diff --cached --quiet

function_body() {
  sed -n "/^$1(/,/^}/p" "$2"
}

# Absence checks accept only ripgrep's status 1 no-match result.  A match
# fails the check, and an execution error propagates to the ledger.
assert_absent() {
  value=$1
  pattern=$2
  matches=
  if matches=$(printf '%s\n' "$value" | rg -F "$pattern"); then
    test -z "$matches"
  else
    status=$?
    case "$status" in
      1) test -z "$matches" ;;
      *) return "$status" ;;
    esac
  fi
}

rg_count() {
  value=$1
  pattern=$2
  count=0
  if count=$(printf '%s\n' "$value" | rg -F -c "$pattern"); then
    printf '%s\n' "$count"
  else
    status=$?
    case "$status" in
      1) printf '0\n' ;;
      *) return "$status" ;;
    esac
  fi
}

native_initializer() {
  sed -n "/$1 = {/,/^[[:space:]]*};/p" "$2"
}

# One ICD identity and its separation audit; the r300g source tree carries
# no Vulkan entry point.
rg -n --fixed-strings -e "'vulkan_r3v'" -e "output : 'r3v_icd." \
  -e 'separation' \
  src/amd/r300/vulkan/meson.build
test "$(rg -l --fixed-strings 'VKAPI_CALL' src/gallium/drivers/r300 | wc -l)" -eq 0

# r300g's R2VB source and delivery domains.
rg -n --fixed-strings -e 'r300_r2vb_producer_input_preflight' \
  -e 'r300_r2vb_delivery_element_preflight' \
  src/gallium/drivers/r300/r300_r2vb.c \
  src/amd/r300/common/r300_r2vb_source_contract.h

# Gallium-free Radeon DRM transport.
rg -n --fixed-strings -e 'radeon_drm_vk_cs_build' \
  -e 'radeon_drm_vk_completion' \
  src/amd/radeon/drm_vk/

# Native submission arming and one-shot disarm.
rg -n --fixed-strings -e 'r3v_native_arming_evaluate' \
  -e 'attempt.token' \
  src/amd/r300/vulkan/r3v_native_arming.c \
  src/amd/r300/vulkan/r3v_native_queue.c

# Native submit gate and digest-bound submission objects.
rg -n --fixed-strings \
  -e 'R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED' \
  -e 'R3V_NATIVE_MANIFEST_DIR' \
  -e 'submit_manifest' \
  src/amd/r300/vulkan/r3v_native_queue.c

# Native host coherency over the unsnooped GART.
rg -n --fixed-strings -e 'radeon_drm_vk_bo_cache_sync' \
  -e 'HOST_CACHED' \
  src/amd/radeon/drm_vk/radeon_drm_vk_bo.c \
  src/amd/r300/vulkan/r3v_physical_device.c

# Triangle-cell fragment program compiler output.
rg -n --fixed-strings -e 'r300_tcl_bypass_fs_tool' \
  -e 'r300_tcl_bypass_triangle_fs_block' \
  src/amd/r300/compiler/tests/r300_tcl_bypass_fs_tool.c \
  src/amd/r300/common/r300_tcl_bypass_triangle_fs_block.h

# Private fixed-cell recording outside the ICD export surface.
rg -n --fixed-strings 'r3v_native_record_tcl_bypass_triangle' \
  src/amd/r300/vulkan/r3v_native_cell.c \
  src/amd/r300/vulkan/r3v_native.h

# Public recording surface for image, view, pipeline, and draw.
rg -n --fixed-strings -e 'r3v_CmdDraw' -e 'r3v_CreateImage' \
  -e 'NATIVE_LIVE_CMDS' \
  src/amd/r300/vulkan/r3v_native_image.c \
  src/amd/r300/vulkan/r3v_native_pipeline.c \
  src/amd/r300/vulkan/r3v_native_draw.c

# Deferred draw execution at queue submission.
rg -n --fixed-strings 'execute_deferred_draw' \
  src/amd/r300/vulkan/r3v_native_cell.c \
  src/amd/r300/vulkan/r3v_native_queue.c

# Queue family and the gated compute surface, scoped to their functions.
native_queue=$(function_body \
  r3v_GetPhysicalDeviceQueueFamilyProperties2 \
  src/amd/r300/vulkan/r3v_physical_device.c)
printf '%s\n' "$native_queue" | rg -n --fixed-strings \
  -e 'VK_QUEUE_GRAPHICS_BIT' -e 'hybrid_compute_enabled'
rg -n --fixed-strings -e 'hybrid_compute_enabled' \
  -e 'R3V_NATIVE_REFUSAL_RESULT' -e 'r300_compute_job_from_spirv' \
  src/amd/r300/vulkan/r3v_native_compute.c

# Extension table and dispatch overlay.
native_extensions=$(native_initializer \
  r3v_native_device_extensions_supported \
  src/amd/r300/vulkan/r3v_physical_device.c)
printf '%s\n' "$native_extensions" | rg -n --fixed-strings \
  -e '.KHR_get_memory_requirements2 = true,' \
  -e '.KHR_bind_memory2 = true,' \
  -e '.KHR_dedicated_allocation = true,'
test "$(rg_count "$native_extensions" '.KHR_')" -eq 3
native_create=$(function_body r3v_CreateDevice \
  src/amd/r300/vulkan/r3v_native_device.c)
printf '%s\n' "$native_create" | rg -n --fixed-strings \
  -e 'r3v_device_entrypoints' -e 'vk_common_device_entrypoints'

# Linear B8G8R8A8 format mask and usage gates.
native_format=$(function_body r3v_get_format_properties \
  src/amd/r300/vulkan/r3v_physical_device.c)
printf '%s\n' "$native_format" | rg -n --fixed-strings \
  -e 'VK_FORMAT_B8G8R8A8_UNORM' \
  -e 'VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT' \
  -e 'VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT' \
  -e 'VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT'
native_image_query=$(function_body r3v_get_image_format_properties \
  src/amd/r300/vulkan/r3v_physical_device.c)
printf '%s\n' "$native_image_query" | rg -n --fixed-strings \
  -e 'VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT' \
  -e 'VK_IMAGE_USAGE_TRANSFER_SRC_BIT' \
  -e 'VK_IMAGE_USAGE_TRANSFER_DST_BIT'
native_image_create=$(function_body r3v_CreateImage \
  src/amd/r300/vulkan/r3v_native_image.c)
printf '%s\n' "$native_image_create" | rg -n --fixed-strings \
  -e 'VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT' \
  -e 'VK_IMAGE_USAGE_TRANSFER_SRC_BIT' \
  -e 'VK_IMAGE_USAGE_TRANSFER_DST_BIT'
assert_absent "$native_format" 'r3v_vk_format_to_pipe_format'
printf '%s\n' "$native_image_query" | rg -n --fixed-strings \
  -e 'r3v_native_transfer_query' -e 'r3v_image_usage_supported'

# Public query/create usage matrix: zero, color-only, each transfer bit,
# both transfer bits, and every mixed color/transfer combination all run
# through both entry points on the drm-shim fixture.  The Vulkan 1.0 KHR
# alias route is exercised by the real-loader sweep as well.
rg -n --fixed-strings \
  -e 'check_image_usage_surface' \
  -e 'query_image_properties' \
  -e 'VK_ERROR_FORMAT_NOT_SUPPORTED' \
  -e 'VK_IMAGE_USAGE_SAMPLED_BIT' \
  -e 'VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME' \
  -e 'vkGetPhysicalDeviceImageFormatProperties2KHR' \
  src/amd/r300/vulkan/tests/r3v_native_public_surface_harness.c \
  src/amd/r300/vulkan/tests/r3v_native_loader_sweep.c

# Usage-family predicates: source shape plus calibrated semantic mutants.
python3 - <<'PY'
from pathlib import Path
import re


def function_body(text, name):
    match = re.search(rf"(?m)^{re.escape(name)}\s*\(", text)
    if match is None:
        raise AssertionError(f"missing function: {name}")
    brace = text.find("{", match.end())
    if brace < 0:
        raise AssertionError(f"missing body: {name}")
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[match.start():index + 1]
    raise AssertionError(f"unterminated body: {name}")


def exact_usage_policy(body, prefix, color_operator, transfer_name):
    color = re.search(
        rf"{re.escape(prefix)}->usage\s*{color_operator}\s*"
        r"VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT",
        body,
    )
    transfer = re.search(
        rf"{re.escape(prefix)}->usage\s*!=\s*0\s*&&\s*"
        rf"\({re.escape(prefix)}->usage\s*&\s*~{transfer_name}\)"
        r"\s*==\s*0",
        body,
        re.S,
    )
    return color is not None and transfer is not None


def exact_transfer_mask(body, name):
    match = re.search(
        rf"const VkImageUsageFlags {re.escape(name)}\s*=\s*"
        r"(?P<left>VK_IMAGE_USAGE_TRANSFER_(?:SRC|DST)_BIT)\s*\|\s*"
        r"(?P<right>VK_IMAGE_USAGE_TRANSFER_(?:SRC|DST)_BIT)\s*;",
        body,
    )
    if match is None:
        return False
    return {
        match.group("left"),
        match.group("right"),
    } == {
        "VK_IMAGE_USAGE_TRANSFER_SRC_BIT",
        "VK_IMAGE_USAGE_TRANSFER_DST_BIT",
    }


physical = Path("src/amd/r300/vulkan/r3v_physical_device.c").read_text()
native_image = Path("src/amd/r300/vulkan/r3v_native_image.c").read_text()
query = function_body(physical, "r3v_get_image_format_properties")
create = function_body(native_image, "r3v_CreateImage")
assert exact_usage_policy(
    query, "info", "!=", "r3v_native_transfer_usage"
)
assert exact_usage_policy(create, "pCreateInfo", "==", "transfer_usage")
assert exact_transfer_mask(query, "r3v_native_transfer_usage")
assert exact_transfer_mask(create, "transfer_usage")

COLOR = 1
TRANSFER_SRC = 2
TRANSFER_DST = 4
TRANSFER = TRANSFER_SRC | TRANSFER_DST


def source_usage_policy(body, prefix, color_operator, transfer_name, usage):
    transfer = re.search(
        rf"{re.escape(prefix)}->usage\s*!=\s*0\s*"
        r"([&|]{2})\s*\("
        rf"{re.escape(prefix)}->usage\s*&\s*~{re.escape(transfer_name)}\)"
        r"\s*==\s*0",
        body,
        re.S,
    )
    mask = exact_transfer_mask(body, transfer_name)
    if transfer is None or not mask:
        return False
    if transfer.group(1) == "&&":
        transfer_accepts = usage != 0 and usage & ~TRANSFER == 0
    elif transfer.group(1) == "||":
        transfer_accepts = usage != 0 or usage & ~TRANSFER == 0
    else:
        return False

    if prefix == "info":
        rejection_name = "r3v_native_transfer_query"
        rejection = re.search(
            rf"\({re.escape(prefix)}->usage\s*!=\s*"
            r"VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT\s*"
            r"([&|]{2})\s*!"
            rf"{re.escape(rejection_name)}\)",
            body,
            re.S,
        )
        if rejection is None:
            return False
        color_is_not_render = usage != COLOR
        not_transfer = not transfer_accepts
        if rejection.group(1) == "&&":
            return not (color_is_not_render and not_transfer)
        if rejection.group(1) == "||":
            return not (color_is_not_render or not_transfer)
        return False

    if color_operator == "==":
        return usage == COLOR or transfer_accepts
    return False


known_good = (COLOR, TRANSFER_SRC, TRANSFER_DST, TRANSFER)
known_bad = (
    0,
    COLOR | TRANSFER_SRC,
    COLOR | TRANSFER_DST,
    COLOR | TRANSFER,
)
expected_good = (True,) * len(known_good)
expected_bad = (False,) * len(known_bad)
for body, prefix, color_operator, transfer_name in (
    (query, "info", "!=", "r3v_native_transfer_usage"),
    (create, "pCreateInfo", "==", "transfer_usage"),
):
    assert tuple(
        source_usage_policy(
            body, prefix, color_operator, transfer_name, usage
        )
        for usage in known_good
    ) == expected_good
    assert tuple(
        source_usage_policy(
            body, prefix, color_operator, transfer_name, usage
        )
        for usage in known_bad
    ) == expected_bad

mutants = (
    (
        "create-color-bit-test",
        create.replace(
            "pCreateInfo->usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT",
            "pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT",
            1,
        ),
        "pCreateInfo",
        "==",
        "transfer_usage",
    ),
    (
        "create-zero-transfer-admission",
        create.replace("pCreateInfo->usage != 0", "true", 1),
        "pCreateInfo",
        "==",
        "transfer_usage",
    ),
    (
        "query-zero-transfer-admission",
        query.replace("info->usage != 0", "true", 1),
        "info",
        "!=",
        "r3v_native_transfer_usage",
    ),
)
for name, mutant, prefix, color_operator, transfer_name in mutants:
    assert not exact_usage_policy(
        mutant, prefix, color_operator, transfer_name
    ), name

semantic_mutants = (
    (
        "query-or-transfer",
        query.replace("info->usage != 0 &&", "info->usage != 0 ||", 1),
        "info",
        "!=",
        "r3v_native_transfer_usage",
    ),
    (
        "query-or-rejection",
        query.replace(
            "info->usage != VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT &&\n"
            "        !r3v_native_transfer_query",
            "info->usage != VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ||\n"
            "        !r3v_native_transfer_query",
            1,
        ),
        "info",
        "!=",
        "r3v_native_transfer_usage",
    ),
    (
        "create-or-transfer",
        create.replace(
            "pCreateInfo->usage != 0 &&",
            "pCreateInfo->usage != 0 ||",
            1,
        ),
        "pCreateInfo",
        "==",
        "transfer_usage",
    ),
)
for name, mutant, prefix, color_operator, transfer_name in semantic_mutants:
    mutant_good = tuple(
        source_usage_policy(
            mutant, prefix, color_operator, transfer_name, usage
        )
        for usage in known_good
    )
    mutant_bad = tuple(
        source_usage_policy(
            mutant, prefix, color_operator, transfer_name, usage
        )
        for usage in known_bad
    )
    assert mutant_good != expected_good or mutant_bad != expected_bad, name

mask_source = re.compile(
    r"(?:VK_IMAGE_USAGE_TRANSFER_SRC_BIT\s*\|\s*"
    r"VK_IMAGE_USAGE_TRANSFER_DST_BIT|"
    r"VK_IMAGE_USAGE_TRANSFER_DST_BIT\s*\|\s*"
    r"VK_IMAGE_USAGE_TRANSFER_SRC_BIT)\s*;"
)


def expand_transfer_mask(body):
    expanded, replacements = mask_source.subn(
        "VK_IMAGE_USAGE_TRANSFER_SRC_BIT |\n"
        "      VK_IMAGE_USAGE_TRANSFER_DST_BIT |\n"
        "      VK_IMAGE_USAGE_SAMPLED_BIT;",
        body,
        count=1,
    )
    assert replacements == 1
    return expanded


def reverse_transfer_mask(body):
    reversed_body, replacements = re.subn(
        r"VK_IMAGE_USAGE_TRANSFER_SRC_BIT\s*\|\s*"
        r"VK_IMAGE_USAGE_TRANSFER_DST_BIT\s*;",
        "VK_IMAGE_USAGE_TRANSFER_DST_BIT |\n"
        "      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;",
        body,
        count=1,
    )
    assert replacements == 1
    return reversed_body


for body, mask_name in (
    (query, "r3v_native_transfer_usage"),
    (create, "transfer_usage"),
):
    assert exact_transfer_mask(reverse_transfer_mask(body), mask_name)


mask_mutants = (
    (
        "query-expanded-transfer-mask",
        expand_transfer_mask(query),
        "r3v_native_transfer_usage",
    ),
    (
        "create-expanded-transfer-mask",
        expand_transfer_mask(create),
        "transfer_usage",
    ),
)
for name, mutant, mask_name in mask_mutants:
    assert not exact_transfer_mask(mutant, mask_name), name

print("usage-policy known-good cases: PASS")
print("usage-policy known-bad mutants: PASS")
print("usage-policy exact transfer masks: PASS")
PY

# Reference SPIR-V admission pair and its generator.
rg -n --fixed-strings -e 'r3v_reference_vertex_spirv' \
  -e 'generate_reference_spirv' \
  src/amd/r300/vulkan/r3v_native_reference_spirv.h \
  src/amd/r300/vulkan/shaders/generate_reference_spirv.py

# Loader-boundary application gate and symbol audit.
rg -n --fixed-strings -e 'r3v-native-loader-application' \
  -e 'FORBIDDEN_PREFIXES' -e 'R3V_EXPECTED_ICD_DSO' \
  src/amd/r300/vulkan/tests/r3v_native_loader_application.c \
  src/amd/r300/vulkan/tests/r3v_native_loader_application_symbol_audit.py \
  src/amd/r300/vulkan/meson.build

# Vulkan 1.0 KHR physical-device-properties2 alias through the real loader.
rg -n --fixed-strings \
  -e 'r3v-native-loader-sweep' \
  -e 'VK_API_VERSION_1_0' \
  -e 'VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME' \
  -e 'vkGetPhysicalDeviceImageFormatProperties2KHR' \
  src/amd/r300/vulkan/tests/r3v_native_loader_sweep.c \
  src/amd/r300/vulkan/meson.build

# Native transfer image family and deferred host copies.
rg -n --fixed-strings \
  -e 'R3V_NATIVE_TRANSFER_DIMENSION_MAX' \
  -e 'r3v_CmdCopyBufferToImage' \
  -e 'r3v_CmdClearColorImage' \
  -e 'r3v_native_cmd_buffer_execute_deferred_copies' \
  src/amd/r300/vulkan/r3v_native_image.c \
  src/amd/r300/vulkan/r3v_native_recording.c \
  src/amd/r300/vulkan/r3v_native_transfer.c \
  src/amd/r300/vulkan/r3v_physical_device.c \
  src/amd/r300/vulkan/tests/r3v_native_public_surface_harness.c

# Native WSI surface and presentation boundary.
rg -n --fixed-strings \
  -e 'r3v_native_device_extensions_supported' \
  -e 'r3v_init_wsi' \
  -e 'r3v-native-wsi-surface-contract' \
  src/amd/r300/vulkan/r3v_physical_device.c \
  src/amd/r300/vulkan/tests/r3v_native_wsi_surface_contract.c \
  src/amd/r300/vulkan/meson.build

# Present-support, entrypoint-closure, and loader-boundary audit contracts.
rg -n --fixed-strings -e 'supported == VK_FALSE' \
  -e 'r3v-native-wsi-surface-contract' \
  src/amd/r300/vulkan/tests/r3v_native_wsi_surface_contract.c \
  src/amd/r300/vulkan/meson.build
rg -n --fixed-strings -e 'r3v-native-entrypoint-closure' \
  -e 'r3v-native-public-surface-policy' \
  src/amd/r300/vulkan/meson.build
rg -n --fixed-strings -e 'r3v-native-loader-application' \
  -e 'r3v-native-loader-application-symbols' \
  src/amd/r300/vulkan/meson.build
python3 src/amd/r300/vulkan/tests/r3v_native_entrypoint_audit.py --selftest
```

## Retired Gallium-backed lane

A Gallium-backed Vulkan lane preceded the implementation: it recorded
Vulkan commands and replayed them through r300g's `pipe_context`, with
Gallium resources, CSOs, winsys command buffers, and fences as its execution
substrate.  That lane is deleted from the tree, with its Meson option, its
conditionals, its DSO, and r300g's compute-admission classifier whose only
production caller it was.  The differential value its cross-lane tests
carried lives on in references that run without it:

- retained manifests: the submit manifests and digest-bound submit objects
  the queue retains under `R3V_NATIVE_MANIFEST_DIR`, the typed-carry corpus
  sha256 manifest (`src/gallium/drivers/r300/tests/typed_carry_corpus/
  r300_typed_carry_reference.sha256`, three rows equal to the retained RS482
  bundle digests), and the vertex front-end parity manifest
  (`src/amd/r300/compiler/tests/r300_vertex_front_end_parity.manifest`);
- common-IR parity: `r300-vertex-front-end-parity` holds the NIR front end
  (r300g's) and the direct SPIR-V admitter (the ICD's) to identical jobs,
  carriers, and color bits over the one job IR;
- exact fixtures: `r3v-native-format-features`, `r3v-native-descriptor`,
  the native burst harness's submit-lifetime legs, and the r300g-owned
  `r300-r2vb-typed-route-oracle` and `r300-r2vb-producer-census` over the
  corpus.

The table below maps each former lane capability to the mechanism that
carries it and the evidence class that mechanism holds; a row reading
`silicon` for one payload closes only that payload, and widening a row is
the capability ladder's work.

| Former lane capability | Surviving mechanism | Evidence class |
|---|---|---|
| Vulkan commands lowered into Gallium CSOs and `pipe_context` replay | one fixed render cell, recorded through public `vkCmd*` over a bounded render-pass/pipeline/draw vocabulary | silicon, one qualified payload |
| NIR ingress through `nir_to_rc` | `r300_vertex_job_from_spirv` and `r300_fragment_constant_color_from_spirv` admit the straight-line vec4 shapes from SPIR-V words directly (no NIR); the NIR front end `r300_vertex_job_from_nir` remains in `compiler/` as the Gallium consumer's path over the same common job IR, held to parity by `r300-vertex-front-end-parity` | host unit |
| NIR compatibility through `nir_to_tgsi` for Draw shapes | covered by the `nir_to_rc` and CPU-vertex-execution rows: `nir_to_tgsi` is r300g's internal bridge into the Draw module (`draw_vs_exec.c`), the ICD never calls it, and no Vulkan-reachable shape resolves only through it | host unit, through the covering rows |
| CPU vertex execution over Gallium Draw SW TCL | `r300_cpu_vertex_job_execute` over the job IR | host unit, and silicon through the cell's carrier |
| R300 graphics state: VAP, PSC, RS, US, TX, CB, ZB, ROP, viewport, raster | the cell's fixed state vector alone | silicon, one state vector |
| R2VB producer, CB export, cache publication, TCL-bypass re-ingest | public GPU-producer route, one payload, F32_4 position stream, three vertices | silicon, one payload |
| Graphics-as-compute: `VK_QUEUE_COMPUTE_BIT` and `vkCmdDispatch` over raster kernels and multipass carriers, behind the `R3V_HYBRID_COMPUTE_EXPERIMENTAL` gate | native compute surface behind the same exact gate: `r300_compute_job_from_spirv` admits the identity-map kernel from SPIR-V words directly (no NIR), storage-buffer descriptors on set 0, one recorded dispatch per command buffer, `r300_cpu_compute_job_execute` at submission; every out-of-subset module refuses at pipeline creation, closing the Gallium lane's admitted-but-unmatched no-op | host unit and end-to-end shim dispatch; raster-carrier execution and the wider verb corpus are the open widening axes |
| Video: Gallium VL MPEG-1/MPEG-2 shader decode | reclassified out of the Vulkan surface: `libgalliumvl` decode reaches applications through the GL state trackers alone, neither ICD names a `VK_KHR_video_*` symbol, so it retires with the GL carve-out above | not an R3V capability |
| Memory and transfers: maps, uploads, copies, blits, clears | GEM-backed memory, one linear `B8G8R8A8_UNORM` transfer surface | host model, and silicon for the cell's carrier |
| Queue and completion | owned Radeon DRM submission with fence retirement | silicon |
| WSI over Gallium-exported resources | surface-query behavior alone; `VK_KHR_swapchain` and external-memory handles stay outside the native ICD | host model, and no presentation |
| Host modeling: drm-shim identity, BO-domain, ioctl models | the same drm-shim models, exercised by the native harnesses | host model |

Eleven rows carry a surviving mechanism, several bounded to one payload,
state vector, kernel, or modeled surface; the video row leaves the Vulkan
comparison the way GL and GLES did; WSI stops before presentation. The
graphics-as-compute mechanism admits the identity-map kernel where the lane
admitted a raster-verb corpus, so that row's widening -- more admitted
kernel shapes, and GPU raster-carrier execution beside the CPU route --
plus the driver's general surface is the remaining gap, and the closing
order is the one `Ordered development` names.

The video scope for the native ICD is fixed by silicon:
`VK_KHR_video_queue` and every decode or encode profile need a decode
engine, a queue with `VK_QUEUE_VIDEO_DECODE_BIT_KHR`, planar 4:2:0 DPB
formats, and entropy decode, and RS482 supplies none of them -- the
texture and render-target tables carry packed 4:2:2 (`YUYV`/`UYVY`
through `R300_EASY_TX_FORMAT(..., YVYU422)`) alone, and CABAC/CAVLC is
serial integer work the FP24 fragment ALU cannot express. The one
implementable video-adjacent surface is
`VK_KHR_sampler_ycbcr_conversion` scoped to packed 4:2:2 presentation
of externally decoded content; planar 4:2:0 stays outside it.

## R3V implementation

### Required ownership

The R3V ICD owns its Vulkan objects, BOs, memory bindings, command buffers,
execution graph, queues, PM4, synchronization, and completion. Its build
omits runtime ownership by `driver_r300`, `libgallium`, `libgalliumvl`,
`pipe_context`, `pipe_screen`, `pipe_resource`, and Gallium CSOs, and the
separation audit proves it at the source, compile-command, object, and DSO
layers.

### Current state

`libvulkan_r3v.so` is the ICD (`r3v_icd.<cpu>.json`, `driverName` `r3v`);
the separation audit holds its exports to the three `vk_icd*` symbols and
its dependency list free of Gallium runtime libraries.  The landed
mechanisms are:

- the Gallium-free transport `src/amd/radeon/drm_vk/` (ioctl vtable seam,
  BO/PRIME refcount, relocation dedupe, three-chunk CS build/submit split,
  finite completion via a write-domain BO plus bounded `GEM_WAIT_IDLE`);
- deep-copied fragment binaries (`r300_fragment_binary`) with content hash
  and structural validator;
- native device, memory (one owned GEM BO per `VkDeviceMemory`), buffer,
  image, image-view, pipeline, queue, and command-carrier objects;
  reporting narrowed to executable source routes: the queue family reports
  `VK_QUEUE_GRAPHICS_BIT`, and `VK_QUEUE_COMPUTE_BIT` only under the exact
  `R3V_HYBRID_COMPUTE_EXPERIMENTAL=1` opt-in that admits the identity-map
  kernel (`r3v_native_compute.c`). The Vulkan 1.0 queue-family rule keeps
  promotion open until the compute route stands without its opt-in.
  Format properties advertise one linear `B8G8R8A8_UNORM`
  `linearTilingFeatures` mask containing
  `VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT`,
  `VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT`, and
  `VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT`; image format query and creation
  split those bits into color-attachment usage alone or a nonzero
  transfer-bit subset, and reject mixed usage. The F32-
  family vertex formats and one UMA heap size from `DRM_RADEON_GEM_INFO`
  remain advertised;
- the public graphics recording surface: the qualified linear image
  family at any extent inside the 64x64 maximum over the fixed
  64-pixel pitch (larger extents refuse at creation), its identity
  view, the pipeline admitted by byte equality with the reference
  SPIR-V pair and the cell's fixed state vector with the
  viewport/scissor pair as its target-extent claim, and the
  render-pass/bind/draw command subset whose draw lowers through the
  CPU vertex carrier -- viewport-transformed inside the bounded
  clip-volume domain -- into the extent-resolved cell, with the vertex
  gather and load-op clear executing at queue submission;
- the fixed TCL-bypass triangle lowered into a native command buffer by
  `r3v_native_record_tcl_bypass_triangle`, a private entry linked directly
  by the pre-hardware harness; the recording opens with the neutral
  first-draw state contract (`src/amd/r300/common/r300_first_draw_state.c`),
  emitted in pipeline order and proven self-establishing by the
  poison-model checker; `vkBeginCommandBuffer` and
  `vkEndCommandBuffer` record nothing themselves;
- the exact-value submit gate `R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1`; the
  closed gate retains the IB, relocation list, and manifest under
  `R3V_NATIVE_MANIFEST_DIR` and fails closed with `VK_ERROR_DEVICE_LOST`;
- the drm-shim triangle-cell harness driving both gate states, with the
  closed-gate retained IB byte-identical to the direct emitter;
- the R2VB identity delivery route
  (`r300_r2vb_carrier_delivery`): on the exact opt-in
  `R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL=1` and the F32_4, F32_3, and
  F32_2 formats, the deferred draw delivers the vertex stream through
  the host model of the producer's passthrough copy instead of the CPU
  gather; F32_3 and F32_2 synthesize the lanes past the source record
  exactly as the gather does -- Z as 0.0, W as 1.0, values the producer
  embeds host-side, both FP24 fixed points by construction, so the
  synthesis needs no admission scan.  F32_1's synthesized Y stays a
  CPU-route shape until its identity control exists.  The
  R2VB producer routes every attribute through the US fragment
  datapath -- s1e7m16 (FP24) registers and interpolators into the
  C4_32_FP color container -- so a binary32 value survives delivery
  byte-exact only as a fixed point of the FP24 round trip; the model
  admits non-negative binary32 encodings that equal the shared FP24
  quantizer: positive zero and normal values from the measured minimum
  through the measured finite maximum with the low 7 mantissa bits clear.
  The RS48x source-read model canonicalizes negative zero and steps
  negative nonzero values toward zero, so negative values refuse along
  with infinities, NaNs, denormals, off-grid values, and values outside
  the measured range.  Stream components use little-endian binary32
  bytes, and the host model decodes those bytes explicitly before
  admission.  On the admitted domain the round trip is the identity, so
  delivery is a verbatim copy there, the CPU gather re-derives the carrier
  as the semantic oracle at every R2VB execution, and a byte divergence
  refuses the draw.  The CPU route is the default, the delivered PM4 is
  unchanged, and the route is host-model evidence only.
- the linear transfer image family and its synchronous copies:
  linear `B8G8R8A8_UNORM` 2D images whose usage is a nonzero subset of
  `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` and `VK_IMAGE_USAGE_TRANSFER_DST_BIT`
  at any extent inside 2048 per axis, a deliberately conservative policy
  bound taken from the RS48x single-tile texture ceiling. The image-format
  query and `r3v_CreateImage` admit this transfer subset, admit the render
  family only for color-attachment usage alone, and refuse mixed usage. The
  row pitch aligns each row to 64 bytes -- the 2D engine's DST_PITCH_OFFSET
  word carries pitch in 64-byte units, so every family row layout stays
  addressable by the qualified direct-write 2D path -- and the footprint is
  the rows alone; the oracle-headroom row is the render family's contract.
  `vkCmdCopyBufferToImage`,
  `vkCmdCopyImageToBuffer`, `vkCmdCopyImage`, and
  `vkCmdClearColorImage` (whole-subresource, clamp-then-round unorm
  pack with NaN converting as zero) record region-admitted deferred
  copies -- subresource, bounds, usage bits, and the buffer
  byte footprint all prove at record time -- and execute them in order
  at queue submission through host mappings, with each destination
  published for the unsnooped GART; a copy-carrying command buffer
  holds no IB and issues no `DRM_RADEON_CS` submission ioctl; a host
  mapping may issue `DRM_RADEON_GEM_MMAP` through
  `radeon_drm_vk_bo_map()`.  The 64-byte row layout remains compatible
  with the qualified direct-write 2D packet, but these transfer commands
  execute as host copies rather than GPU 2D packets.
  Transfer images report 4096-byte alignment and accept an aligned nonzero
  binding offset when the remaining allocation covers the footprint; copy
  and clear addresses include that stored offset.  Render images retain
  dedicated offset-zero binding for the color reference.
  The format query, the advertised
  transfer features, and creation admit the same family.
  `vkCmdPipelineBarrier` outside a render pass records as an admitted
  no-op -- the deferred ops execute in recorded order on one host
  thread and every destination publishes before the submission
  retires, so execution dependencies, availability, and visibility
  hold by construction -- while an ownership-transferring barrier
  refuses (the device exposes one queue family) and an in-pass
  barrier refuses (the pass's one draw has no self-dependency
  lowering).  Usage mixing the families refuses, no view admits the
  family, and a command buffer carries either the qualified render
  pass or copies, so the attachment path and the qualified render
  cell never see a transfer image.
- the delivery route resolver (`r300_delivery_route_resolve`): the one
  home of the route policy the deferred draw consults -- CPU gather by
  default, the R2VB host model on the exact gate value and the three
  modeled formats, every refused input landing on the CPU route with a
  naming reason.  A production R2VB promotion is a measurement decision
  the resolver holds closed: it requires total draw latency measured on
  silicon with public-route live delivery -- producer submission, cache
  publication, and the re-ingest stall included, gather time alone deciding
  nothing.  Total-latency qualification remains open for the landed public
  F32_4 route.
- the R2VB producer-pass PM4 emitter (`r300_r2vb_producer_pass`): the
  raster pass that writes an F32_4 carrier through the color backend --
  first-draw contract prefix, target prologue (destination-cache barrier,
  carrier retarget to C4_32_FP with one read-write GTT relocation, BGRA
  output select canceling the body's (z, y, x, w) pre-swizzle, full-write
  blend state, one-pixel point raster), the embedded `3D_DRAW_IMMD_2`
  POINTS body (`VAP_VTX_SIZE` = 8, one slot center plus one pre-swizzled
  record per vertex, 1024-vertex admission ceiling from
  `R300_R2VB_PRODUCER_MAX_COUNT`), and the publication tail whose closing
  `VAP_PVS_STATE_FLUSH_REG = 0` syncs the vertex engine so a later fetch
  of the same BO cannot read stale vertex-cache content.  The emission
  refuses records outside the FP24 fixed-point domain with -EDOM.  The
  reference pass replays clean through the kernel CS-track model (accept,
  one relocation; truncation, undersized `VAP_VTX_SIZE`, and an
  undersized carrier each reject), and the TCL-bypass width predicate
  declines its PRIM_WALK-3 draw by declared scope.  No-submit structural
  evidence only.

Compute pipelines, descriptors, transfer images and copies beyond the
bounded linear `B8G8R8A8_UNORM` family, native WSI presentation and
external-memory handles, formats outside the accepted render and transfer
families, silicon witnesses for non-maximum render extents or any native
transfer operation, and public R2VB producer dispatch for payloads beyond the
authorized F32_4 triangle remain outside the landed surface. Live
`DRM_RADEON_CS` evidence is split between
the foundational records below and the R2VB records linked from their
procedure documents. The first-submission record carries the bare
inherited-state cell and its all-sentinel target;
its kernel acceptance, retirement, and oracle are operator-observed because
the runner output is not retained. The direct-write 2D control retains its
byte-exact probe result, and the contract-prefixed successor retains its
accepted and retired raster result. The retained records are repository-
relative paths in the sibling
`steinmarder-r300` checkout:

- `results/r3v-native-attended-cell-rs482-first-submission/` records the
  inherited-state cell and all-sentinel target; acceptance and retirement
  are operator-observed;
- `results/rs482_native_direct_write_transport_visibility_pass_20260808T065343Z/`
  records the byte-exact direct-write transport control;
- `results/rs482_native_triangle_first_correct_pixel_witness_20260808T070427Z/`
  records the accepted and retired self-contained cell with the green
  interior and sentinel exterior.

The separate gate matrix at
`results/rs482-first-draw-color-write-gate-discrimination/` records the
single-register controls that leave the historical first-run cause
underdetermined. Its mutation logs, readbacks, manifests, and hash file
are retained, but it retains no same-run kernel window or mapped-DSO
capture; its manifest contract records both gaps. The
`results/rs482-post-attended-cell-fence-wedge/` record retains the
recovered wedged-boot kernel log, journal tail, boot and module identity,
and wedge classification, but no dmesg or serial capture covers the
06:11:14-06:13:31 interval, so attribution between the native submission
and an unrelated desktop draw remains unresolved.

The first-submission bundle retains the semantic cell (`ib.bin`,
`relocs.bin`, `manifest.json`), exact submit object (`submit_relocs.bin`,
`submit_manifest.json`), one-shot token, color target and census,
`dmesg-before.txt`, `dmesg-after.txt`, boot continuity, procedure, README,
bundle/run manifests, and `bundle_hashes.sha256`. Its acceptance,
retirement, and oracle result are operator-observed because stdout, stderr,
and process exit status are not retained. The direct-write bundle retains
the semantic cell (`ib.bin`, `relocs.bin`, `manifest.json`), exact submit
object (`submit_relocs.bin`, `submit_manifest.json`), one-shot token, color
target, `direct_write_outcome.json`, `attended_run_stdout.txt`,
`dmesg_before.txt`, `dmesg_after.txt`, `netconsole.log`,
`host_identity.txt`, `mesa_sha.txt`, `elf_identity.sha256`,
`arming_report_armed.txt`, `arming_report_undeclared.txt`, `README.md`, and
`SHA256SUMS`. The triangle
bundle retains the semantic cell (`ib.bin`, `relocs.bin`, `manifest.json`),
exact submit object (`submit_relocs.bin`, `submit_manifest.json`), one-shot
token, `color_target.bin`, `attended_run_stdout.txt`, `preflight.txt`,
`dmesg_before.txt`, `dmesg_after.txt`, `netconsole.log`,
`host_identity.txt`, `mesa_sha.txt`, `elf_identity.sha256`,
`arming_report_armed.txt`, `arming_report_undeclared.txt`, `README.md`, and
`SHA256SUMS`. These retention
shapes differ; each result remains evidence for its recorded Mesa source
and target boot, not current-head runtime or conformance proof.

### Source-layer split

| Layer | Authority |
|---|---|
| `src/amd/radeon/drm_vk/` | Radeon DRM BO, map, PRIME, relocation, submission, and finite completion transport |
| `src/amd/r300/common/` | RS480/R300 device facts, formats, packet fields, state packs, barriers, and validators |
| `src/amd/r300/cpu/` | portable byte-defined vertex execution baseline plus measured per-target tuned paths |
| `src/amd/r300/vulkan/` | Vulkan objects, command lowering, execution graph, queue policy, images, WSI, and entry points |

The shared Radeon DRM layer contains no R300 or Evergreen graphics state.
R300 packet values, registers, tiling, cache operations, shader metadata,
vertex tuples, and R2VB semantics stay in the R300 layers.

### Native object graph

```text
r3v_instance
  -> r3v_physical_device
       -> render-node fd
       -> RS480 device information
       -> one UMA budget model
  -> r3v_device
       -> radeon_drm_vk_device
       -> completion service
       -> native queues and object registries
  -> r3v_device_memory
       -> one owned r3v_bo
  -> r3v_buffer / r3v_image
       -> bound range or layout views of memory BOs
  -> r3v_pipeline
       -> owned shader binaries and immutable R300 state packs
  -> r3v_cmd_buffer
       -> Vulkan-semantic command records
  -> r3v_exec_graph
       -> resource-scoped execution nodes
```

`VkBuffer` and `VkImage` create metadata. Memory binding installs a range or
layout view into an already allocated `VkDeviceMemory` BO. Host mapping maps the
owned BO under an explicit unsnooped-UMA visibility contract.

### Owned pipeline binaries

The pipeline owns its fragment binary: `r300_fragment_binary` holds every
consumed word and subordinate table in R3V-owned storage, produced by the
compiler tool (`r300_tcl_bypass_fs_tool`) for the fixed cell, with no
Gallium CSO in the path.

The first owned fragment binary carries:

- the US/FG PM4 block;
- immutable validation metadata;
- external and state-constant layout;
- a content hash;
- source compiler identity.

A build-time compiler tool may produce the binary; the runtime artifact and
queue path depend on no Gallium object.

### Native execution and first hardware witness

A native command buffer lowers to explicit resource-scoped nodes such as
`CPU_VERTEX`, `R2VB_PRODUCER`, `PM4_DRAW`, `COPY`, `CLEAR`, `BARRIER`, and
`PRESENT`. Each node names BO reads and writes, byte ranges, domains,
coordinate space, precision contract, and predecessors.

The first native hardware witness is one pretransformed TCL-bypass triangle,
not PVS and not R2VB. It uses:

- one GTT vertex BO and one color BO;
- one `FLOAT_4` position stream;
- identity `XYZW` PSC selectors;
- `VAP_VTX_SIZE = 4`;
- position-only VAP output;
- no index buffer, instancing, user clip planes, query, texture, or external
  shader constants;
- one owned fragment binary;
- explicit cache/VAP publication;
- one complete relocation list;
- one finite completion object.

The no-submit form first fixes PM4, relocation identity, state coverage, and
command size. Radeon shim results remain host-model evidence. Offline kernel
replay proves parser acceptance and calibrated malformed rejection. Only then
does an attended RS480-family target submit the known-good cell.

### CPU vertex execution and R2VB migration

The general vertex route admits SPIR-V words directly into the common job IR
(no NIR) and is independent of Gallium Draw:

```text
Vulkan vertex and index state
-> byte-defined portable baseline (any host endianness; R300-era hosts
   span x86, x86-64, and PowerPC)
-> per-target tuned path only where a measurement on that target
   justifies it (K8 is the primary measured target; general code speed
   rides the build profile's compiler flags)
-> direct writes into the final mapped GTT carrier
-> TCL-bypass delivery
```

R2VB migration follows the fixed triangle and CPU route:

1. migrate the `FLOAT_4` identity source control;
2. migrate the qualified `FLOAT_3` producer source with `XYZ1` reconstruction;
3. add `FLOAT_2` only after `XY01` userspace and kernel validators agree;
4. migrate qualified count, grid, and topology cells;
5. add computed varyings one measured shape at a time: the first shape is
   one location-0 vec4 varying the vertex job computes, carried as the
   record's second FLOAT_4 into the texture-coordinate-0 vector and routed
   through RS_IP_0 / RS_INST_0 to the pass-through fragment program (the
   varying triangle cell on the CPU route, runner
   `r3v_native_attended_varying_triangle`, procedure
   `r3v-native-attended-varying-triangle-procedure.md`; silicon-delivered
   on RS482 with the gradient exact at every interior sample, retained as
   steinmarder-r300 bundle
   `r3v-native-varying-triangle-cell-first-delivery-rs482`);
6. read several vertex attributes, one job slot per shader input
   location over one or more per-vertex bindings, through the CPU
   executor into the same varying cell (the IB identity is unchanged;
   only the host-gathered records differ); the pipeline refuses an
   attribute that crosses its binding's stride, the draw refuses an
   unbound binding and a stream sharing bytes with the pass target, and
   the composed GPU-producer routes keep one source relocation role, so
   they admit the slot-0 identity job alone;
7. draw indexed: the three UINT16 or UINT32 indices are read from the
   bound index buffer at execution, each summed with the base vertex in
   32-bit wrapping arithmetic, and the CPU executor gathers the named
   vertices into the same carrier (the de-indexed stream the consumer
   draws linearly, so the IB identity is unchanged); the record proves
   the index range inside the index buffer and outside the pass target,
   the vertex numbers fall under the robust rule at execution (zeros
   with robustBufferAccess, refusal before any write without it), the
   pipeline holds primitive restart disabled so the restart value is an
   ordinary index, a UINT8 index type refuses at the bind, and the
   producer routes refuse an indexed draw, fetching one linear source
   range;
8. add hybrid carriers only with an explicit final VAP join.

The route owns its BOs, PM4, packers, barriers, and completion; r300g's R2VB
planner belongs to the GL lane and is never an R3V execution path.

### Public GPU-producer route scope

The public GPU-producer route admits through `vkCmdDraw` plus `vkQueueSubmit`
on the exact double opt-in, and its admission composes the producer pass over
the application's gathered records ahead of the recorded triangle consumer in
one IB. The producer embeds those records as literal `DRAW_IMMD_2` body
dwords, so the composed stream's digest is a function of the vertex payload:
one authorized payload per arming, and a second submission carrying different
vertex bytes produces a digest the operator's declaration no longer matches.
The route is payload-specific by construction, not payload-agnostic, and a
widened payload set means a widened set of declared digests.

The admission's three predicates are structural (identity vertex job over the
`F32_4` position stream on the recorded triangle consumer), transport
(`r300_r2vb_producer_pass_semantic_equal` against a fresh reference emission,
which compares every dword outside the embedded record payloads), and numeric
(the emitter's `-EDOM` refusal holds every record inside the FP24 fixed-point
domain, and the gathered records become the post-completion read-back oracle).
A completed submission whose carrier diverges from that oracle retains the
observed bytes, quarantines the capability on the device, and reports device
loss; a completed submission that agrees retains the same bytes beside the
expectation, so a delivery is auditable from the bundle rather than from the
exit status.

`docs/hardware/r3v-native-attended-public-gpu-producer-procedure.md` carries
the silicon cell: the arming runner that composes the route offline and
reports the digest, the declarations the run sets, the predictions and
falsifiers, and the two fail-closed oracles -- the driver's carrier read-back
and the runner's color-target compare -- that a `TARGET_DELIVERED` verdict
requires together. The color target is downstream of the carrier, so the pair
is serially dependent rather than independent, and each one's verdict rests on
a pre-seeded value the correct path cannot manufacture;
`rs482-native-delivery-route-admission-and-oracle-model.md` carries that
model.

### Native WSI

The native build initializes common WSI surface plumbing, but its extension
table advertises only memory-requirements, batched-bind, and dedicated-
allocation contracts. Native `VK_KHR_swapchain` and external-memory handle
entry points remain outside the native ICD. The source test
`r3v-native-wsi-surface-contract` defines XCB surface construction and
surface capability, format, and present-mode queries at the host-model
class; its queue-family assertion requires no present support, and its path
stops before swapchain creation. The source contract covers surface-query
behavior. Native presentation, PRIME export, and silicon WSI require
separate evidence.

Future native WSI qualification begins after native BO ownership and export
identity are established:

```text
native image-memory BO
-> PRIME dma-buf export
-> identical BO at common WSI
-> same-GPU presentation
-> retirement and reuse
```

X11 and Wayland are independent qualification cells. A resource exported by
the GL lane does not close presentation.

### Completion criteria

The native implementation is complete only when:

- one ICD identity: `libvulkan_r3v.so`, `r3v_icd.<cpu>.json`, `driverName`
  `r3v` (landed);
- the functional target links without Gallium runtime ownership (landed);
- dependency, symbol, and include audits prove that separation;
- memory owns real BOs and bound objects are views;
- queues submit bounded direct PM4 and complete finitely;
- the first valid PM4 cell retires and a malformed control fails closed;
- CPU vertex and migrated R2VB paths retain exact output oracles;
- every advertised native capability has current kernel and target evidence.

## Complete Vulkan semantics and conformance

Complete Vulkan support begins from a working native implementation and closes
the API contract. It requires:

- authoritative image-format, creation, and memory-binding rules;
- complete aliasing, mapping, flush, invalidate, and external-memory semantics;
- resource-scoped queue ordering, fences, semaphores, events, and barriers;
- native transfers, clears, render passes, dynamic rendering, queries, and
  presentation;
- complete graphics pipeline and vertex-interface semantics;
- compute only when workgroups, descriptors, storage access, atomics, barriers,
  and dispatch semantics exist;
- no successful no-op command;
- each advertised X11 and Wayland WSI surface;
- feature and extension tables generated from implemented behavior;
- CTS, dEQP, Piglit, and workload evidence appropriate to every exposed
  surface;
- default promotion only after image, queue, memory, execution, and WSI
  identities are current and replayable.

Complete Vulkan semantics never follow merely because r300g's GL lane
performs an operation or because the queue can submit one direct draw.

## R300 extraction boundary

Suitable common value-type mechanisms include:

- neutral vertex format records;
- DATA_TYPE and component-selector packing;
- checked source and destination extents;
- FP24 constant packing;
- shader admission cost records;
- deep-copied fragment binary descriptors;
- immutable blend, DSA, raster, viewport, scissor, and output-format packs;
- VAP, PSC, and RS tuple construction and validation;
- R2VB slot layout and source contracts;
- PM4 packet and relocation writers;
- cache and role-transition barrier packs;
- texture and image layout arithmetic with value-type inputs.

R300-specific mechanisms remain under R300 common or Vulkan code:

- RS480 family capabilities and quirks;
- invariant and VAP-invariant register values;
- R300 shader ISA and RC metadata;
- VAP, PSC, RS, GA, US, TX, CB, ZB, and ROP registers;
- R300 texture layout and tiling rules;
- R300 cache publication and flush sequences;
- R300 draw packet construction;
- R2VB producer and delivery semantics.

Gallium-owned objects stay in r300g: `pipe_context`, `pipe_screen`,
`pipe_resource`, dirty atoms, CSO lifetime, Draw/vbuf ownership, `u_upload`,
`u_blitter`, transfer helpers, VL decoder ownership, and Gallium winsys
command buffers and fences.

A helper becomes common code only after its inputs and outputs are independent
value types and its tests run without a Gallium object.

## R2VB source-format transition

The neutral source contract defines:

```text
F32_2 -> 2 physical dwords -> XY01 logical vec4
F32_3 -> 3 physical dwords -> XYZ1 logical vec4
F32_4 -> 4 physical dwords -> XYZW logical vec4
```

r300g's live automatic R2VB producer admits `F32_3` and `F32_4`, and its
live automatic final delivery admits FP32x4 only. The R3V
identity-delivery host model covers `F32_4`, `F32_3`, and `F32_2` under its
exact opt-in. Operator-armed producer, re-ingest, and exact F32
`FLOAT_2 + XY01` cells hold on RS482. The public route composes and
submits one payload-specific F32_4 producer plus triangle consumer through
`vkCmdDraw` and `vkQueueSubmit` under its exact double opt-in.

The integration order separates landed no-submit source transactions from
remaining validator, live-delivery, and silicon work:

1. keep the neutral source and Gallium-adapter tests in the normal build
   (landed);
2. route existing `F32_3` and `F32_4` construction through the neutral contract
   with byte-identical PM4 controls (landed; pinned by
   `r300_r2vb_psc_byte_identity_test`);
3. add an exact `F32_2` source gate that never rides `R300_R2VB_STANDING`
   (landed as `R300_R2VB_FLOAT2_SOURCE`);
4. capture the six-dword `FLOAT_4 + FLOAT_2` producer tuple without submit
   (landed; pinned by `r300_r2vb_float2_tuple_test`);
5. extend userspace and kernel validators from identity-only PSC to explicit
   synthesized-lane contracts (landed; pinned by the tuple replay);
6. run the bounded F32 `FLOAT_2 + XY01` silicon cell (landed; scoped by
   `r3v-native-attended-float2-tuple-procedure.md`);
7. decide standing promotion in a separate change.

The native implementation migrates `F32_3` before `F32_2`. Producer support for
a narrow source never implies narrow final-delivery support.

## Repository authority

| Repository | Authority |
|---|---|
| `mesa-26-gororoba` | the R3V ICD, r300g, compilers, state packs, R2VB, WSI, and tests |
| `steinmarder-r300` (separate repository; `src/re/r300/` and root `results/`) | RS480 frontier, probes, falsifiers, findings, manifests, and target result bundles |
| `vostro1000-re` | K8 and platform behavior plus CPU-executor qualification |
| `linux-radeon-gororoba` | Radeon parser, GEM, GART, faults, completion, recovery, and containment |
| `radeon-custom` | source pin, package construction, deployment transition, rollback, and installed runtime identity |
| Xserver and Radeon DDX repositories | X11 source, package, and installed-image authority |
| `steinmarder-r600-terakan` | reusable process patterns and PALM evidence, never RS480 hardware facts |

Mesa behavior changes land in Mesa. Kernel changes land in the kernel source
repository. Package policy lands in the package repository. Target evidence and
findings remain in the evidence repository.

`rs482-native-delivery-route-admission-and-oracle-model.md` carries the native
lane's route topology, admission calculus, host-coherency model, oracle
structure, evidence ladder, and artifact profile; this document keeps the
ownership boundary and the completion criteria.

## Ordered development

The landed surfaces carry source, host-unit, build/link, no-submit, drm-shim,
offline kernel-parser, and bounded attended-silicon evidence classes as
stated above. The ordered list marks remaining mechanisms; one evidence class
does not promote another.

1. Keep the retired lane's differential references current: the retained
   manifests, the common-IR parity test, and the rebound fixtures (landed).
2. Refactor existing `F32_3` and `F32_4` R2VB construction through the neutral
   source contract (landed).
3. Land the gated `F32_2` no-submit producer transaction and synthesized-lane
   validators (landed).
4. Extract a generic Radeon DRM transport layer with host tests (landed).
5. Deep-copy R300 fragment binaries into R3V-owned storage (landed).
6. Collapse to one ICD identity, `libvulkan_r3v.so` under `r3v` (landed).
7. Build native BO, memory, command, queue, and completion ownership (landed;
   native render and transfer image families, host-mapped transfer records,
   and private fixed-cell recording each keep a bounded ownership contract).
8. Emit and offline-validate the fixed identity-bypass triangle (landed).
9. Run the attended native triangle cell (landed: the contract-prefixed
   cell rendered as predicted on RS482; procedure and arming live in
   `docs/hardware/r3v-native-attended-cell-procedure.md`, while the
   retained record in the sibling `steinmarder-r300` repository is
   `results/rs482_native_triangle_first_correct_pixel_witness_20260808T070427Z/`).
10. Build and qualify the native CPU vertex executor (gather stage and
    carrier delivery landed: `src/amd/r300/cpu/` carries the portable
    byte-defined baseline and the SSE2/SSE3 tuned candidates under the
    `r300-cpu-vertex` oracle at the host-unit class, and the stream-fed
    recorder delivers through `r300_cpu_vertex_gather`; the
    `r300_cpu_vertex_bench` three-way measurement -- baseline versus
    SSE2 versus SSE3 against the memcpy copy ceiling, on the K8 target
    under the `k8-sse3` profile flags -- remains open and decides which
    candidate the auto dispatch keeps). The job benchmark reuses heap
    storage. Native dispatch timing remains open over the command-buffer-owned
    mapped GTT carrier, which each submission maps, rewrites, publishes, and
    unmaps.
11. Migrate native R2VB producer and live delivery (`F32_4` control, then
    `F32_3`, then `F32_2`).
    The identity-delivery host model and no-submit producer emitter are
    landed. Operator-armed producer, FP24 boundary, same-IB re-ingest, and
    exact F32 `FLOAT_2 + XY01` cells carry retained RS482 results; their
    procedure documents own the contracts and point to the result bundles:
    `r3v-native-attended-producer-procedure.md`,
    `r3v-native-attended-reingest-procedure.md`, and
    `r3v-native-attended-float2-tuple-procedure.md`. Public live GPU-route
    dispatch is landed for one authorized F32_4 triangle payload under the
    exact double opt-in; F32_3 and F32_2 public-route migration remains open.
    The supporting contracts are landed: the kernel synthesized-lane validator
    decodes the PSC element list and accepts
    the `FLOAT_2 + XY01 + vtx_size 6` tuple
    (linux-radeon-gororoba `r300_tcl_bypass_vtx_check.h`), the fetched
    tuple pass and its manifest replay prove the userspace/kernel
    agreement offline (`r300-r2vb-float2-tuple-replay`, per
    `docs/hardware/r300-r2vb-float2-source-contract.md`), and
    `r300_delivery_route_resolve` carries the
    `R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER` value behind the exact
    double opt-in. Each staged cell runs under its own operator-armed digest.
12. Extend native image, transfer, and resource-scoped synchronization
    semantics; the bounded linear transfer family and its host-order barrier
    contract are landed, while broader GPU-backed transfer semantics remain
    open.
13. Prove native same-GPU WSI.
14. Complete Vulkan semantics and conformance before default promotion.

The retained manifests, the common-IR parity test, and the rebound fixtures
are the differential reference; r300g's GL lane is the state-tracker
neighbor, not an R3V execution path.

## Evidence classes

Every result names one class:

- source proof;
- host unit proof;
- build and link proof;
- no-submit PM4 proof;
- offline kernel-parser proof;
- attended kernel-submission proof;
- silicon-output proof;
- conformance proof;
- deployment proof.

A higher class never appears by implication. A Radeon shim result is a host
model. Parser acceptance is not execution. Fence retirement is not output
correctness. An output hash from an old Mesa image is not evidence for the
current head.
