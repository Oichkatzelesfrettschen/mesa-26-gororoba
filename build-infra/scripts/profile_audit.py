#!/usr/bin/env python3
"""Fail when a build profile enables a component its allowlist row forbids.

Each profile config under build-infra/configs/ (and configs/alternates/)
must have a row in build-infra/component-policy/radeon-minimal-allowlist.toml
naming its tag and its allowed gallium-drivers, vulkan-drivers, and
feature surfaces.  The audit parses the meson-format config, compares the
selected components against the row, and exits nonzero on any component
outside the allowlist or any config file without a row.  Policy lives in
the rows: the r300 release row omits zink, the terakan rows carry the
software rasterizers their x130e reference runs compare against.
"""

from __future__ import annotations

import argparse
import configparser
import glob
import os
import sys
import tomllib


def parse_profile(path: str) -> dict[str, list[str]]:
    parser = configparser.ConfigParser()
    parser.read(path)
    if "project options" in parser:
        section = parser["project options"]
    elif "options" in parser:
        section = parser["options"]
    else:
        return {}
    values: dict[str, list[str]] = {}
    for key in ("gallium-drivers", "vulkan-drivers"):
        raw = section.get(key, "")
        values[key] = [
            item.strip().strip("'\"")
            for item in raw.strip("[]").split(",")
            if item.strip().strip("'\"")
        ]
    return values


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
            allowed = set(row.get(key, []))
            for component in selected.get(key, []):
                if component not in allowed:
                    failures.append(
                        f"{base}: {key} selects '{component}' "
                        f"outside allowlist {sorted(allowed)}")

    for message in failures:
        print(f"profile-audit: FAIL: {message}")
    if failures:
        return 1
    print(f"profile-audit: ok ({len(config_paths)} profiles against "
          f"{len(profiles)} allowlist rows)")
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
