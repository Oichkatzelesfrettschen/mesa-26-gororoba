#!/bin/sh
# Resolve one CPython interpreter for every build-infrastructure Python call.
set -eu

resolve_executable()
{
   candidate=$1

   case "$candidate" in
      ''|-*|*[!A-Za-z0-9_./+-]*)
         return 1
         ;;
   esac

   executable=$(command -v "$candidate" 2>/dev/null) || return 1
   case "$executable" in
      /*) ;;
      */*)
         executable_directory=$(dirname "$executable")
         executable_name=$(basename "$executable")
         case "$executable_directory" in
            /*|./*|../*) ;;
            *) executable_directory=./$executable_directory ;;
         esac
         executable_directory=$(CDPATH='' cd "$executable_directory" && pwd -P) || return 1
         executable=$executable_directory/$executable_name
         ;;
      *)
         return 1
         ;;
   esac

   [ -x "$executable" ] || return 1
   interpreter_identity=$("$executable" -c '
import platform
import sys

print(platform.python_implementation(), sys.version_info.major, sys.version_info.minor)
' 2>/dev/null) || return 1
   case "$interpreter_identity" in
      'CPython 3 12'|'CPython 3 13'|'CPython 3 14') ;;
      *) return 1 ;;
   esac

   printf '%s\n' "$executable"
}

if [ -z "${MESA_PYTHON_INPUT:-}" ]; then
   printf '%s\n' \
      'python-interpreter: PYTHON must name a CPython 3.12 through 3.14 executable' >&2
   exit 1
fi

if resolve_executable "$MESA_PYTHON_INPUT"; then
   exit 0
fi

printf 'python-interpreter: %s is missing or outside CPython 3.12 through 3.14\n' \
   "$MESA_PYTHON_INPUT" >&2
exit 1
