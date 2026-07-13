#!/usr/bin/env bash
# Hermetic integrity checks for run.sh with fake Vulkan and dEQP tools.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd -P)"
HARNESS="$HERE/run.sh"
WORKDIR="$(mktemp -d "${TMPDIR:-/var/tmp}/r3v-vulkan-surface-test.XXXXXX")"

cleanup() {
  case "$WORKDIR" in
    "${TMPDIR:-/var/tmp}"/r3v-vulkan-surface-test.*) rm -rf -- "$WORKDIR" ;;
    *) echo "refusing to remove unexpected test directory: $WORKDIR" >&2; exit 1 ;;
  esac
}
trap cleanup EXIT

EXTS=(VK_KHR_bind_memory2 VK_KHR_get_memory_requirements2 VK_KHR_dedicated_allocation
      VK_KHR_driver_properties VK_KHR_format_feature_flags2
      VK_KHR_uniform_buffer_standard_layout VK_KHR_relaxed_block_layout
      VK_KHR_storage_buffer_storage_class VK_KHR_sampler_mirror_clamp_to_edge)

mkdir -p "$WORKDIR/bin" "$WORKDIR/out"
printf '{}\n' > "$WORKDIR/r3v_icd.json"
printf 'dEQP-VK.api.info.fixture\n' > "$WORKDIR/caselist.txt"

cat > "$WORKDIR/bin/vulkaninfo" <<'EOF'
#!/usr/bin/env sh
printf 'driverName = %s\n' "${VULKANINFO_DRIVER:-r3v}"
printf '%s\n' \
  VK_KHR_bind_memory2 \
  VK_KHR_get_memory_requirements2 \
  VK_KHR_dedicated_allocation \
  VK_KHR_driver_properties \
  VK_KHR_format_feature_flags2 \
  VK_KHR_uniform_buffer_standard_layout \
  VK_KHR_relaxed_block_layout \
  VK_KHR_storage_buffer_storage_class \
  VK_KHR_sampler_mirror_clamp_to_edge
EOF
cat > "$WORKDIR/bin/vkcube" <<'EOF'
#!/usr/bin/env sh
exit "${VKCUBE_EXIT:-0}"
EOF
cat > "$WORKDIR/bin/deqp-vk" <<'EOF'
#!/usr/bin/env sh
printf "Test case 'dEQP-VK.api.info.fixture'..\n"
printf '  %s (fixture)\n' "${DEQP_STATUS:-Pass}"
exit "${DEQP_EXIT:-0}"
EOF
chmod +x "$WORKDIR/bin/vulkaninfo" "$WORKDIR/bin/vkcube" "$WORKDIR/bin/deqp-vk"

write_baseline() {
  : > "$WORKDIR/baseline.tsv"
  for extension in "${EXTS[@]}"; do
    printf 'Pass\tsmoke.device_extension.%s\n' "$extension" >> "$WORKDIR/baseline.tsv"
  done
  printf 'Pass\tsmoke.vkcube.no_crash\n' >> "$WORKDIR/baseline.tsv"
  printf 'Pass\tdEQP-VK.api.info.fixture\n' >> "$WORKDIR/baseline.tsv"
}

run_harness() {
  local label="$1"
  local deqp="$2"
  shift 2
  local output_root="${TEST_OUT_ROOT:-$WORKDIR/out/$label}"
  local baseline="${TEST_BASELINE:-$WORKDIR/baseline.tsv}"

  env PATH="$WORKDIR/bin:$PATH" \
      DISPLAY=:0 \
      VK_ICD_FILENAMES="$WORKDIR/r3v_icd.json" \
      DEQP_VK="$deqp" \
      OUT="$output_root" \
      CASELIST="$WORKDIR/caselist.txt" \
      BASELINE="$baseline" \
      VULKANINFO_DRIVER="${VULKANINFO_DRIVER:-r3v}" \
      VKCUBE_EXIT="${VKCUBE_EXIT:-0}" \
      DEQP_STATUS="${DEQP_STATUS:-Pass}" \
      DEQP_EXIT="${DEQP_EXIT:-0}" \
      bash "$HARNESS" "$@"
}

expect_rc() {
  local expected="$1"
  shift
  set +e
  "$@"
  local actual=$?
  set -e
  if [ "$actual" -ne "$expected" ]; then
    echo "expected exit $expected, got $actual" >&2
    exit 1
  fi
}

write_baseline
expect_rc 0 run_harness success "$WORKDIR/bin/deqp-vk" --check

cp "$WORKDIR/baseline.tsv" "$WORKDIR/baseline-before.tsv"
expect_rc 2 run_harness missing-deqp "$WORKDIR/bin/missing-deqp-vk" --record
cmp "$WORKDIR/baseline-before.tsv" "$WORKDIR/baseline.tsv"

VULKANINFO_DRIVER=other
expect_rc 2 run_harness wrong-driver "$WORKDIR/bin/deqp-vk" --check
unset VULKANINFO_DRIVER

VKCUBE_EXIT=9
expect_rc 1 run_harness failing-vkcube "$WORKDIR/bin/deqp-vk" --check
unset VKCUBE_EXIT
grep -Fqx $'Fail\tsmoke.vkcube.no_crash' \
  "$WORKDIR/out/failing-vkcube"/r3v-vulkan-surface.*/status.tsv

DEQP_STATUS=UnknownStatus
expect_rc 2 run_harness unknown-deqp "$WORKDIR/bin/deqp-vk" --check
unset DEQP_STATUS

TEST_BASELINE="$WORKDIR/no-baseline-parent/baseline.tsv"
expect_rc 2 run_harness baseline-copy-failure "$WORKDIR/bin/deqp-vk" --record
unset TEST_BASELINE

mkdir -p "$WORKDIR/out/sentinel"
touch "$WORKDIR/out/sentinel/keep"
expect_rc 0 run_harness sentinel "$WORKDIR/bin/deqp-vk" --check
[ -f "$WORKDIR/out/sentinel/keep" ]

TEST_OUT_ROOT=/
expect_rc 2 run_harness unsafe-output "$WORKDIR/bin/deqp-vk" --check
unset TEST_OUT_ROOT

echo "r3v-vulkan-surface harness fixtures: PASS"
