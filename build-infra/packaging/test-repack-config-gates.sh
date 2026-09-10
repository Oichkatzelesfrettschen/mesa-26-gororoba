#!/bin/sh
# Calibrate shared package metadata against complete and incompatible Meson options.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest -q \
  "$here/../tests/test_mesa_package_layout.py" -k 'configuration or profile_mismatch or complete_stock'
