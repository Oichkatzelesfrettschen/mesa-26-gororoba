#!/bin/sh
# Calibrate shared package metadata against complete and incompatible Meson options.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
PYTHON=$(MESA_PYTHON_INPUT=${PYTHON:-} \
  sh "$here/../scripts/resolve-python-interpreter.sh") || exit 1
export PYTHON
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 "$PYTHON" -m pytest -q \
  "$here/../tests/test_mesa_package_layout.py" -k 'configuration or profile_mismatch or complete_stock'
