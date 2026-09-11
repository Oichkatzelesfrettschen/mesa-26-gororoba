#!/bin/sh
# Calibrate each required GLX/EGL/GBM/DRI artifact and the qualified package payload.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
PYTHON=$(MESA_PYTHON_INPUT=${PYTHON:-} \
  sh "$here/../scripts/resolve-python-interpreter.sh") || exit 1
export PYTHON
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 "$PYTHON" -m pytest -q \
  "$here/../tests/test_mesa_package_layout.py" -k 'loader_artifact or complete_stock or payload_mutation'
