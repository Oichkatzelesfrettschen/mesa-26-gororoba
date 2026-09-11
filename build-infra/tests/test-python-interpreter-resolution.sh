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
   MESA_PYTHON_INPUT=python3.13 /bin/sh "$resolver") || \
   fail 'an explicitly selected versioned interpreter was refused'
[ "$resolved" = "$temporary_directory/versioned-bin/python3.13" ] || \
   fail 'the explicitly selected versioned interpreter changed'
printf '%s\n' 'OK    explicitly selected versioned interpreter resolves'

absent_output=$temporary_directory/absent.stderr
if PATH="$temporary_directory/versioned-bin" /bin/sh "$resolver" \
   >"$temporary_directory/absent.stdout" 2>"$absent_output"; then
   fail 'an absent caller selection searched versioned Python names'
fi
grep -F 'PYTHON must name a CPython 3.12 through 3.14 executable' \
   "$absent_output" >/dev/null || \
   fail 'the absent-interpreter refusal lacks the caller contract'
printf '%s\n' 'OK    absent caller selection fails without PATH probing'

if env -u PYTHON MAKEFLAGS= MFLAGS= \
   make -C "$build_infra_root" --no-print-directory help \
   >"$temporary_directory/absent-make.stdout" \
   2>"$temporary_directory/absent-make.stderr"; then
   fail 'GNU Make accepted an absent caller selection'
fi
grep -F 'Python interpreter resolution failed' \
   "$temporary_directory/absent-make.stderr" >/dev/null || \
   fail 'GNU Make did not report absent interpreter selection'
printf '%s\n' 'OK    GNU Make requires caller-supplied PYTHON'

empty_output=$temporary_directory/empty.stderr
if MESA_PYTHON_INPUT= sh "$resolver" \
   >"$temporary_directory/empty.stdout" 2>"$empty_output"; then
   fail 'an empty caller selection was accepted'
fi
grep -F 'PYTHON must name a CPython 3.12 through 3.14 executable' \
   "$empty_output" >/dev/null || \
   fail 'the empty-interpreter refusal lacks the caller contract'
if MAKEFLAGS= MFLAGS= make -C "$build_infra_root" --no-print-directory \
   PYTHON= help >"$temporary_directory/empty-make.stdout" \
   2>"$temporary_directory/empty-make.stderr"; then
   fail 'GNU Make accepted an empty caller selection'
fi
grep -F 'Python interpreter resolution failed' \
   "$temporary_directory/empty-make.stderr" >/dev/null || \
   fail 'GNU Make did not report empty interpreter selection'
printf '%s\n' 'OK    empty caller selection fails closed'

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

non_cpython=$temporary_directory/bin/non-cpython
cat >"$non_cpython" <<'PYTHON_WRAPPER'
#!/bin/sh
printf '%s\n' 'PyPy 3 14'
PYTHON_WRAPPER
chmod +x "$non_cpython"
if MESA_PYTHON_INPUT="$non_cpython" sh "$resolver" \
   >"$temporary_directory/non-cpython.stdout" \
   2>"$temporary_directory/non-cpython.stderr"; then
   fail 'a non-CPython implementation was accepted'
fi
grep -F 'missing or outside CPython 3.12 through 3.14' \
   "$temporary_directory/non-cpython.stderr" >/dev/null || \
   fail 'the implementation refusal lacks its supported range'
printf '%s\n' 'OK    non-CPython implementation fails closed'

for packaging_test in test-repack-config-gates.sh test-gbm-backend-stage-guard.sh; do
   if PYTHON=/bin/true sh "$build_infra_root/packaging/$packaging_test" \
      >"$temporary_directory/$packaging_test.stdout" \
      2>"$temporary_directory/$packaging_test.stderr"; then
      fail "$packaging_test accepted a non-Python executable"
   fi
done
printf '%s\n' 'OK    packaging gates validate the caller-selected interpreter'
