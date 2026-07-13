#!/usr/bin/env bash
# r3v Vulkan extension-surface conformance regression harness (deqp-vk).
#
# The harness verifies the selected r3v ICD, records the promoted-extension
# surface, exercises vkcube when its display prerequisites are available, and
# compares the dEQP result domain with baseline.tsv. A baseline Pass that no
# longer passes is a regression.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd -P)"
MODE="${1:---check}"
DEQP_VK="${DEQP_VK:-}"
OUT_ROOT="${OUT:-${TMPDIR:-/var/tmp}/r3v-vulkan-surface}"
CASELIST="${CASELIST:-$HERE/caselist.txt}"
BASELINE="${BASELINE:-$HERE/baseline.tsv}"

EXTS=(VK_KHR_bind_memory2 VK_KHR_get_memory_requirements2 VK_KHR_dedicated_allocation
      VK_KHR_driver_properties VK_KHR_format_feature_flags2
      VK_KHR_uniform_buffer_standard_layout VK_KHR_relaxed_block_layout
      VK_KHR_storage_buffer_storage_class VK_KHR_sampler_mirror_clamp_to_edge)

fatal() {
  echo "FATAL: $*" >&2
  exit 2
}

case "$MODE" in
  --check|--record) ;;
  *) fatal "usage: $0 [--check|--record]" ;;
esac
[ "$#" -le 1 ] || fatal "usage: $0 [--check|--record]"

if [ -z "$DEQP_VK" ]; then
  DEQP_VK="$(command -v deqp-vk || true)"
fi
[ -n "$DEQP_VK" ] && [ -x "$DEQP_VK" ] ||
  fatal "set DEQP_VK to an executable deqp-vk or provide deqp-vk on PATH"
DEQP_DIR="$(cd -- "$(dirname -- "$DEQP_VK")" && pwd -P)" ||
  fatal "cannot resolve dEQP directory for $DEQP_VK"
DEQP_VK="$DEQP_DIR/$(basename -- "$DEQP_VK")"

[ -n "${VK_ICD_FILENAMES:-}" ] ||
  fatal "set VK_ICD_FILENAMES to one r3v ICD JSON file"
case "$VK_ICD_FILENAMES" in
  *:*) fatal "VK_ICD_FILENAMES must name one ICD JSON file" ;;
esac
[ -f "$VK_ICD_FILENAMES" ] ||
  fatal "VK_ICD_FILENAMES does not name a regular file: $VK_ICD_FILENAMES"
# Absolute path: dEQP later cds into DEQP_DIR; a relative ICD would re-resolve
# against that directory and fail closed or load the wrong JSON.
VK_ICD_FILENAMES="$(cd -- "$(dirname -- "$VK_ICD_FILENAMES")" && pwd -P)/$(basename -- "$VK_ICD_FILENAMES")" ||
  fatal "cannot canonicalize VK_ICD_FILENAMES"
unset VK_DRIVER_FILES
export VK_ICD_FILENAMES

case "$OUT_ROOT" in
  ""|/|.|..) fatal "OUT must name a dedicated output parent" ;;
esac
mkdir -p -- "$OUT_ROOT" || fatal "cannot create OUT parent: $OUT_ROOT"
OUT_ROOT="$(cd -- "$OUT_ROOT" && pwd -P)" ||
  fatal "cannot canonicalize OUT parent: $OUT_ROOT"
case "$OUT_ROOT" in
  /|/tmp|/var/tmp|"$HERE"|"$HERE"/*)
    fatal "OUT must not be a shared temporary root or the source directory"
    ;;
esac
OUT="$(mktemp -d "$OUT_ROOT/r3v-vulkan-surface.XXXXXX")" ||
  fatal "cannot create run directory under $OUT_ROOT"
RESULTS="$OUT/results.tsv"
: > "$RESULTS"

echo "=== r3v Vulkan surface conformance run $(date -u +%FT%TZ) ==="
echo "ICD: $VK_ICD_FILENAMES"
echo "Artifacts: $OUT"

# --- check 1: selected r3v ICD and extension presence -------------------------
command -v vulkaninfo >/dev/null 2>&1 || fatal "vulkaninfo is required"
VI="$OUT/vulkaninfo.txt"
vulkaninfo > "$VI" 2>&1 || fatal "vulkaninfo failed for $VK_ICD_FILENAMES"
grep -Eq '^[[:space:]]*driverName[[:space:]]*=[[:space:]]*r3v[[:space:]]*$' "$VI" ||
  fatal "vulkaninfo did not select driverName = r3v"
for extension in "${EXTS[@]}"; do
  if grep -Fq "$extension" "$VI"; then
    status=Pass
  else
    status=Fail
  fi
  printf '%s\tsmoke.device_extension.%s\n' "$status" "$extension" >> "$RESULTS"
done

# --- check 2: vkcube no-crash (null-pipeline replay guard) --------------------
vkcube_status=NotSupported
if command -v vkcube >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1 &&
   [ -n "${DISPLAY:-}" ]; then
  if timeout 15 vkcube --c 3 > "$OUT/vkcube.txt" 2>&1; then
    vkcube_status=Pass
  else
    # Display-connection failures are environment, not driver regressions.
    if grep -Eiq 'cannot open display|No protocol specified|Authorization required|Connection refused|X11 connection|unable to open display'          "$OUT/vkcube.txt" 2>/dev/null; then
      vkcube_status=NotSupported
    else
      vkcube_status=Fail
    fi
  fi
fi
printf '%s\tsmoke.vkcube.no_crash\n' "$vkcube_status" >> "$RESULTS"
if [ "$MODE" = "--record" ] && [ "$vkcube_status" != "Pass" ]; then
  fatal "--record requires DISPLAY, vkcube, timeout, and a successful vkcube run"
fi

# --- check 3: deqp-vk api.info domain ------------------------------------------
parse() {
  awk '
    BEGIN {
      rank["Pass"] = 0
      rank["NotSupported"] = 1
      rank["QualityWarning"] = 1
      rank["CompatibilityWarning"] = 1
      rank["Fail"] = 3
      rank["Timeout"] = 3
      rank["Crash"] = 4
      rank["InternalError"] = 4
      rank["ResourceError"] = 4
      parsed = 0
      invalid = 0
    }
    /^Test case '\''/ {
      case_name = $0
      sub(/^Test case '\''/, "", case_name)
      sub(/'\''\.\.[[:space:]]*$/, "", case_name)
      next
    }
    /^  [[:alnum:]_]+[[:space:]]*\(/ {
      status = $1
      sub(/\(.*/, "", status)
      if (case_name == "" || !(status in rank)) {
        invalid = 1
        next
      }
      print status "\t" case_name
      parsed++
    }
    END { exit invalid || parsed == 0 }
  ' "$1"
}

[ -r "$CASELIST" ] || fatal "cannot read case list: $CASELIST"
group_index=0
while IFS= read -r case_glob; do
  case "$case_glob" in ''|\#*) continue ;; esac
  group_index=$((group_index + 1))
  group_log="$OUT/group_${group_index}.txt"
  group_results="$OUT/group_${group_index}.tsv"
  if ( cd "$DEQP_DIR" && "$DEQP_VK" --deqp-case="$case_glob" \
       --deqp-log-filename="$OUT/group_${group_index}.qpa" ) > "$group_log" 2>&1; then
    deqp_status=0
  else
    deqp_status=$?
  fi
  parse "$group_log" > "$group_results" ||
    fatal "dEQP group $case_glob emitted an unknown or no recognized verdict"
  group_count="$(awk 'END { print NR + 0 }' "$group_results")"
  [ "$group_count" -gt 0 ] ||
    fatal "dEQP group $case_glob produced no parsed verdicts"
  cat "$group_results" >> "$RESULTS"
  if [ "$deqp_status" -ne 0 ]; then
    echo "WARN: deqp-vk exited $deqp_status for $case_glob; parsed verdicts retained" >&2
  fi
  printf '  [%d] %-52s %s cases\n' "$group_index" "$case_glob" "$group_count"
done < "$CASELIST"
[ "$group_index" -gt 0 ] || fatal "case list has no runnable groups"

# Deduplicate by case and retain its worst status.
if ! sort -u "$RESULTS" | awk -F'\t' '
  BEGIN {
    rank["Pass"] = 0
    rank["NotSupported"] = 1
    rank["QualityWarning"] = 1
    rank["CompatibilityWarning"] = 1
    rank["Fail"] = 3
    rank["Timeout"] = 3
    rank["Crash"] = 4
    rank["InternalError"] = 4
    rank["ResourceError"] = 4
  }
  {
    status = $1
    case_name = $2
    if (!(status in rank) || case_name == "") {
      invalid = 1
      next
    }
    if (!(case_name in best) || rank[status] > best_rank[case_name]) {
      best[case_name] = status
      best_rank[case_name] = rank[status]
    }
  }
  END {
    if (invalid)
      exit 2
    for (case_name in best)
      print best[case_name] "\t" case_name
  }
' | sort -t $'\t' -k2 > "$OUT/status.tsv"; then
  fatal "result stream contains an invalid status"
fi

total="$(awk 'END { print NR + 0 }' "$OUT/status.tsv")"
npass="$(awk -F'\t' '$1 == "Pass" { count++ } END { print count + 0 }' "$OUT/status.tsv")"
echo "=== executed $total cases, $npass Pass ==="

if [ "$MODE" = "--record" ]; then
  if ! cp -- "$OUT/status.tsv" "$BASELINE"; then
    fatal "cannot write baseline: $BASELINE"
  fi
  echo "=== baseline written: $BASELINE ($total cases) ==="
  exit 0
fi

[ -r "$BASELINE" ] || fatal "no readable baseline at $BASELINE (run --record first)"
if ! join -t $'\t' -1 2 -2 2 -a1 -a2 -e MISSING -o '0,1.1,2.1' \
     <(sort -t $'\t' -k2 "$BASELINE") <(sort -t $'\t' -k2 "$OUT/status.tsv") \
     > "$OUT/joined.tsv"; then
  fatal "cannot compare baseline and current status"
fi

if awk -F'\t' '
  {
    case_name = $1
    baseline = $2
    current = $3
    if (baseline == "Pass" && current != "Pass") {
      regression[case_name] = current
      regression_count++
    } else if (baseline != "Pass" && baseline != "MISSING" && current == "Pass") {
      progression[case_name] = baseline
      progression_count++
    } else if (baseline == "MISSING" && current != "MISSING") {
      added[case_name] = current
      added_count++
    } else if (current == "MISSING" && baseline != "MISSING") {
      removed[case_name] = baseline
      removed_count++
    }
  }
  END {
    if (regression_count) {
      print "\n!!! REGRESSIONS (" regression_count ") -- baseline Pass, now not:"
      for (case_name in regression)
        print "  REGRESS  " regression[case_name] "\t" case_name
    }
    if (progression_count) {
      print "\n+++ PROGRESSIONS (" progression_count "):"
      for (case_name in progression)
        print "  FIXED    " progression[case_name] "->Pass\t" case_name
    }
    if (added_count) {
      print "\n... NEW cases (" added_count "):"
      for (case_name in added)
        print "  NEW      " added[case_name] "\t" case_name
    }
    if (removed_count) {
      print "\n... GONE cases (" removed_count "):"
      for (case_name in removed)
        print "  GONE     " removed[case_name] "\t" case_name
    }
    print "\n=== verdict: " regression_count + 0 " regressions, " \
          progression_count + 0 " progressions, " added_count + 0 " new, " \
          removed_count + 0 " gone ==="
    exit (regression_count > 0 ? 1 : 0)
  }
' "$OUT/joined.tsv"; then
  verdict=0
else
  verdict=$?
fi
echo "=== DONE_SENTINEL r3v_vk_conformance rc=$verdict ==="
exit "$verdict"
