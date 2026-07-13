#!/usr/bin/env python3
"""Emit meson -D flags from a build-infra profile native file.

Meson treats options in a --native-file as defaults only.  On reconfigure,
stale coredata wins: an option value that is no longer a valid choice is
reset to the option's default (for vulkan-drivers that is ['auto']), and
the native-file default is not re-applied.  That is the option-drift
failure mode for r300 profiles that store the retired 'amd_r300' selector
while the profile file carries 'ati_r300' and llvm=disabled -- auto expands
to lavapipe and configure aborts.

Command-line -D flags override coredata on every setup/reconfigure.  This
script walks the profile's [project options] (and optionally [built-in
options]) and prints one -Dkey=value per option so Make can append them to
`meson setup` and heal drift without a wipe.

Usage:
  meson_profile_dflags.py PATH/TO/profile.meson
  meson_profile_dflags.py --builtin PATH/TO/profile.meson
"""

from __future__ import annotations

import argparse
import configparser
import re
import sys


def _strip_comment(value: str) -> str:
    """Drop a trailing # comment that is outside quotes."""
    out: list[str] = []
    in_single = False
    in_double = False
    i = 0
    while i < len(value):
        ch = value[i]
        if ch == "'" and not in_double:
            in_single = not in_single
            out.append(ch)
        elif ch == '"' and not in_single:
            in_double = not in_double
            out.append(ch)
        elif ch == "#" and not in_single and not in_double:
            break
        else:
            out.append(ch)
        i += 1
    return "".join(out).rstrip()


def _normalize_value(raw: str) -> str:
    """Convert a Meson native-file value to a meson -D CLI value."""
    text = _strip_comment(raw).strip()
    if not text:
        return ""

    # Array: ['a', 'b'] or ["a", "b"] -> a,b ; explicit [] stays [] for Meson.
    if text.startswith("[") and text.endswith("]"):
        inner = text[1:-1].strip()
        if not inner:
            return "[]"
        parts = re.findall(r"'([^']*)'|\"([^\"]*)\"|([^,\s\]]+)", inner)
        items = [a or b or c for a, b, c in parts if (a or b or c)]
        return ",".join(items)

    # Quoted scalar.
    if (len(text) >= 2 and text[0] == text[-1] and text[0] in "'\""):
        return text[1:-1]

    return text


def profile_dflags(path: str, include_builtin: bool) -> list[str]:
    """Return -Dkey=value flags from the profile native file."""
    parser = configparser.ConfigParser()
    # Keep option names case-sensitive (Meson option names are fixed).
    parser.optionxform = str  # type: ignore[method-assign]
    read = parser.read(path)
    if not read:
        raise FileNotFoundError(path)

    sections: list[str] = []
    if include_builtin and "built-in options" in parser:
        sections.append("built-in options")
    if "project options" in parser:
        sections.append("project options")
    elif "options" in parser:
        sections.append("options")

    if not sections:
        raise ValueError(f"{path}: no [project options] (or [options]) section")

    flags: list[str] = []
    for section in sections:
        for key, raw in parser[section].items():
            # Skip multi-line / empty noise; c_args etc. stay native-file only
            # when they are long arrays -- still emit them so reconfigure
            # keeps the profile's compiler flags if they were in project
            # options.  Built-in c_args live under [built-in options].
            value = _normalize_value(raw)
            # Meson CLI rejects empty -Dkey= for some types; skip empties.
            # Explicit [] normalizes to "[]" and is emitted as -Dkey=[].
            if value == "":
                continue
            flags.append(f"-D{key}={value}")
    return flags


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("profile", help="path to a build-infra profile .meson file")
    ap.add_argument(
        "--builtin",
        action="store_true",
        help="also emit [built-in options] (buildtype, c_args, ...)",
    )
    args = ap.parse_args(argv)
    try:
        flags = profile_dflags(args.profile, include_builtin=args.builtin)
    except (OSError, ValueError, configparser.Error) as exc:
        print(f"meson_profile_dflags: {exc}", file=sys.stderr)
        return 2
    # Space-separated on one line for easy Make $(shell) capture.
    print(" ".join(flags))
    return 0


if __name__ == "__main__":
    sys.exit(main())
