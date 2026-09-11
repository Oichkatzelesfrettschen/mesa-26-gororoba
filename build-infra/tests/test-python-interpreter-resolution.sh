#!/bin/sh
# Calibrate build-infrastructure interpreter selection and version admission.
set -eu

resolver=$1
selected_python=$2
build_infra_root=$3
temporary_directory=$(mktemp -d)
trap 'rm -rf -- "$temporary_directory"' EXIT HUP INT TERM

fail()
{
   printf 'FAIL  python interpreter resolution: %s\n' "$*" >&2
   exit 1
}

mkdir "$temporary_directory/bin"
ln -s "$selected_python" "$temporary_directory/bin/mesa-python"
resolved=$(PATH="$temporary_directory/bin:$PATH" \
   MESA_PYTHON_INPUT=mesa-python sh "$resolver") || \
   fail 'a supported interpreter with a non-python3 name was refused'
[ "$resolved" = "$temporary_directory/bin/mesa-python" ] || \
   fail 'the named interpreter did not remain the selected executable'
printf '%s\n' 'OK    non-python3 executable name resolves'
PATH="$temporary_directory/bin:$PATH" \
   make -C "$build_infra_root" --no-print-directory \
   PYTHON=mesa-python python-interpreter-recursive-export-check >/dev/null || \
   fail 'GNU Make did not accept and export the non-python3 executable name'
printf '%s\n' 'OK    GNU Make accepts the non-python3 executable name'

mkdir "$temporary_directory/versioned-bin"
for versioned_name in python3.12 python3.13 python3.14; do
   ln -s "$selected_python" "$temporary_directory/versioned-bin/$versioned_name"
done
resolved=$(PATH="$temporary_directory/versioned-bin" \
   /bin/sh "$resolver") || \
   fail 'a PATH containing only versioned Python names was refused'
[ "$resolved" = "$temporary_directory/versioned-bin/python3.14" ] || \
   fail 'versioned Python names did not resolve in newest-to-oldest order'
printf '%s\n' 'OK    versioned-only PATH resolves newest supported name first'

missing_output=$temporary_directory/missing.stderr
if MESA_PYTHON_INPUT=missing-mesa-python sh "$resolver" \
   >"$temporary_directory/missing.stdout" 2>"$missing_output"; then
   fail 'a missing interpreter was accepted'
fi
grep -F 'missing or outside CPython 3.12 through 3.14' "$missing_output" \
   >/dev/null || fail 'the missing-interpreter refusal lacks its supported range'
printf '%s\n' 'OK    missing interpreter fails closed'
if make -C "$build_infra_root" --no-print-directory \
   PYTHON=missing-mesa-python help \
   >"$temporary_directory/missing-make.stdout" \
   2>"$temporary_directory/missing-make.stderr"; then
   fail 'GNU Make accepted a missing interpreter'
fi
grep -F 'Python interpreter resolution failed' \
   "$temporary_directory/missing-make.stderr" >/dev/null || \
   fail 'GNU Make did not report interpreter resolution failure'
printf '%s\n' 'OK    GNU Make rejects a missing interpreter during parsing'

old_python=$temporary_directory/bin/old-python
cat >"$old_python" <<'PYTHON_WRAPPER'
#!/bin/sh
printf '%s\n' 'CPython 3 11'
PYTHON_WRAPPER
chmod +x "$old_python"
version_output=$temporary_directory/version.stderr
if MESA_PYTHON_INPUT="$old_python" sh "$resolver" \
   >"$temporary_directory/version.stdout" 2>"$version_output"; then
   fail 'CPython 3.11 was accepted'
fi
grep -F 'missing or outside CPython 3.12 through 3.14' "$version_output" \
   >/dev/null || fail 'the version refusal lacks its supported range'
printf '%s\n' 'OK    unsupported CPython version fails closed'
if make -C "$build_infra_root" --no-print-directory \
   PYTHON="$old_python" help \
   >"$temporary_directory/version-make.stdout" \
   2>"$temporary_directory/version-make.stderr"; then
   fail 'GNU Make accepted CPython 3.11'
fi
grep -F 'Python interpreter resolution failed' \
   "$temporary_directory/version-make.stderr" >/dev/null || \
   fail 'GNU Make did not report the version resolution failure'
printf '%s\n' 'OK    GNU Make rejects an unsupported version during parsing'
