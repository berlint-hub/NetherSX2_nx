#!/usr/bin/env python3
"""Check every ini key the launcher writes against the strings of the core it ships.

The launcher stores settings as flat "Section/Key" strings and the core reads them
back through the SharedPreferences shim, so a key that is spelled differently on
the two sides fails silently: the row shows up in the menu, the value is written
to nethersx2.ini, the core never asks for it, and the setting does nothing. There
is no error, no warning, and no way to notice from a play session.

That is exactly what happened with FXAA. The launcher wrote "EmuCore/GS/FXAA";
the core binds the option as SettingsWrapBitBoolEx(FXAA, "fxaa"), i.e. the ini key
is lower-case "fxaa", and only that spelling appears in its strings. The row was
dead from the commit that added it. CI could not catch it because the existing
assertion checks that the launcher binary contains the key string -- which it did,
correctly, on the wrong side of the mismatch.

So this compares the two sides directly: every "EmuCore/..." literal in the
launcher must appear as an exact string in the core binary, unless it is in the
allowlist below with evidence that some other code path owns it.

Exit status is 0 only when every key is found or allowed.
"""

import argparse
import os
import re
import subprocess
import sys

# Keys that legitimately do not appear in the 4248 core. Each entry must point at
# a file that still contains the given evidence string, so an allowlist entry
# rots into a failure instead of silently covering a new mistake.
ALLOW = {
    "EmuCore/Speedhacks/vuCycleSteal": (
        os.path.join("source", "hooks", "patches.c"),
        'prefs_get_int("EmuCore/Speedhacks/vuCycleSteal"',
        "wrapper-owned: patches.c reads it to choose the VU-steal patch level; "
        "the core never sees this key"),
    "EmuCore/GS/ThreadedPresentation": (
        os.path.join("launcher", "source", "main.cpp"),
        'if(build=="3668")',
        "the 3668 core spells it positively; written only for build 3668 and "
        "removed for 4248, which is the only core this bundle ships"),
    "EmuCore/Patches/XGKickSkip": (
        os.path.join("launcher", "source", "main.cpp"),
        'strcmp(iniGet("Wrapper/CoreBuild","4248"),"3668")',
        "row is gated off unless Wrapper/CoreBuild is 3668 (see optEnabled)"),
}


def launcher_keys(root):
    """Every "EmuCore/..." literal the launcher can write, with a source line."""
    found = {}
    base = os.path.join(root, "launcher", "source")
    for name in sorted(os.listdir(base)):
        if not name.endswith((".cpp", ".h")):
            continue
        path = os.path.join(base, name)
        for lineno, line in enumerate(open(path, encoding="utf-8"), 1):
            for key in re.findall(r'"(EmuCore/[^"]+)"', line):
                found.setdefault(key, []).append("%s:%d" % (name, lineno))
    return found


def core_strings(core, strings_cmd):
    out = subprocess.run([strings_cmd, "-n", "4", core],
                         capture_output=True, text=True)
    if out.returncode != 0:
        print("cannot run %s on %s: %s" % (strings_cmd, core, out.stderr.strip()))
        sys.exit(2)
    return set(line.rstrip("\n") for line in out.stdout.split("\n"))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", default=None,
                    help="core .so to read strings from (default: the one the "
                         "launcher bundles, or the TEST3 checkout in /tmp)")
    ap.add_argument("--strings", default="strings",
                    help="strings binary; CI passes the devkitA64 one")
    args = ap.parse_args()

    core = args.core
    if not core:
        for cand in (os.path.join(root, "launcher", "romfs", "cores", "emucore_4248.so"),
                     "/tmp/T3n/4248/arm64-v8a/libemucore_tegrax1.so"):
            if os.path.exists(cand):
                core = cand
                break
    if not core or not os.path.exists(core):
        print("core .so not found; pass --core <path to libemucore*.so>")
        return 2

    keys = launcher_keys(root)
    strings = core_strings(core, args.strings)
    print("core: %s (%d distinct strings)" % (os.path.basename(core), len(strings)))
    print("launcher: %d distinct EmuCore/* keys" % len(keys))

    missing, stale = [], []
    for key in sorted(keys):
        leaf = key.rsplit("/", 1)[1]
        if leaf in strings:
            continue
        if key in ALLOW:
            path, needle, _ = ALLOW[key]
            full = os.path.join(root, path)
            if not os.path.exists(full) or needle not in open(full, encoding="utf-8").read():
                stale.append((key, path, needle))
            continue
        missing.append(key)

    for key, path, needle in stale:
        print("ALLOWLIST STALE %s: evidence %r no longer in %s" % (key, needle, path))
    for key in missing:
        print("MISSING %s  (core has no string %r)  written at %s"
              % (key, key.rsplit("/", 1)[1], ", ".join(keys[key])))
    for key, (_, _, reason) in sorted(ALLOW.items()):
        if key in keys:
            print("allowed %s -- %s" % (key, reason))
        else:
            print("ALLOWLIST STALE %s: the launcher no longer writes it" % key)
            stale.append((key, "-", "-"))

    if missing or stale:
        print("ini key check: FAIL (%d missing, %d stale allowlist entries)"
              % (len(missing), len(stale)))
        return 1
    print("ini key check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
