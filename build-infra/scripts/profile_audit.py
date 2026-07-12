#!/usr/bin/env python3
"""Fail when a build profile enables a component its allowlist row forbids.

Each profile config under build-infra/configs/ (and configs/alternates/)
must have a row in build-infra/component-policy/radeon-minimal-allowlist.toml
naming its allowed gallium-drivers and vulkan-drivers.  The audit parses
the meson-format config, compares the selected drivers against the row,
and exits nonzero on any driver outside the allowlist, any config file
without a row, or any config that omits an explicit driver selector.

Meson treats a missing gallium-drivers/vulkan-drivers option as `auto`,
which expands to a host-dependent surface larger than these allowlist
rows.  The audit therefore fails closed on an absent selector instead of
treating the empty list as "nothing selected".

Driver identity is compared after alias canonicalization: the retired
`amd_r300` Meson token maps to `ati_r300` so a non-r300 row cannot slip
past the gate by spelling the deprecated alias.

This gate enforces driver lists only.  Feature surfaces (Rusticl, video
codecs, debug layers, and similar options) stay outside its contract;
document or extend the allowlist rows before claiming feature coverage.
Policy ownership is the Make target `profile-audit` (also wired into
`make audit`); this script is the implementation body Make invokes.
"""

from __future__ import annotations

import argparse
import configparser
import glob
import os
import sys
import tomllib

# Retired Meson vulkan-drivers token -> current ICD selector.
_VULKAN_ALIASES = {
    "amd_r300": "ati_r300",
}


def parse_driver_list(raw: str) -> list[str]:
    """Parse a Meson array option value into stripped driver names."""
    text = raw.strip()
    if text.startswith("["):
        text = text[1:]
    if text.endswith("]"):
        text = text[:-1]
    values: list[str] = []
    for item in text.split(","):
        name = item.strip().strip("'\"")
        if name:
            values.append(name)
    return values


def parse_profile(path: str) -> dict[str, list[str] | None]:
    """Return driver selectors; None means the option is absent from the file."""
    parser = configparser.ConfigParser()
    parser.read(path)
    if "project options" in parser:
        section = parser["project options"]
    elif "options" in parser:
        section = parser["options"]
    else:
        return {"gallium-drivers": None, "vulkan-drivers": None}

    values: dict[str, list[str] | None] = {}
    for key in ("gallium-drivers", "vulkan-drivers"):
        if key not in section:
            values[key] = None
            continue
        values[key] = parse_driver_list(section.get(key, ""))
    return values


def canonicalize_drivers(key: str, drivers: list[str]) -> tuple[list[str], list[str]]:
    """Return (canonical names, deprecation warnings)."""
    warnings: list[str] = []
    out: list[str] = []
    for name in drivers:
        if key == "vulkan-drivers" and name in _VULKAN_ALIASES:
            canon = _VULKAN_ALIASES[name]
            warnings.append(
                f"deprecated vulkan-drivers token '{name}' "
                f"canonicalizes to '{canon}'")
            out.append(canon)
        else:
            out.append(name)
    return out, warnings


def audit(repo_root: str) -> int:
    policy_path = os.path.join(
        repo_root, "build-infra/component-policy/radeon-minimal-allowlist.toml")
    with open(policy_path, "rb") as handle:
        policy = tomllib.load(handle)
    profiles = policy.get("profile", {})

    config_paths = sorted(
        glob.glob(os.path.join(repo_root, "build-infra/configs/*.meson"))
        + glob.glob(os.path.join(repo_root, "build-infra/configs/alternates/*.meson")))

    failures: list[str] = []
    for path in config_paths:
        base = os.path.splitext(os.path.basename(path))[0]
        row = profiles.get(base)
        if row is None:
            failures.append(f"{base}: no allowlist row")
            continue
        selected = parse_profile(path)
        for key in ("gallium-drivers", "vulkan-drivers"):
            chosen = selected.get(key)
            if chosen is None:
                failures.append(
                    f"{base}: {key} omitted; Meson defaults to 'auto' and "
                    f"would expand outside allowlist {sorted(row.get(key, []))}")
                continue
            allowed = set(row.get(key, []))
            canon, warnings = canonicalize_drivers(key, chosen)
            for message in warnings:
                print(f"profile-audit: WARN: {base}: {message}", file=sys.stderr)
            for component in canon:
                if component not in allowed:
                    failures.append(
                        f"{base}: {key} selects '{component}' "
                        f"outside allowlist {sorted(allowed)}")

    for message in failures:
        print(f"profile-audit: FAIL: {message}")
    if failures:
        return 1
    print(f"profile-audit: ok ({len(config_paths)} profiles against "
          f"{len(profiles)} allowlist rows; driver lists only)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=None)
    args = parser.parse_args()
    root = args.repo_root or os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    return audit(root)


if __name__ == "__main__":
    sys.exit(main())
