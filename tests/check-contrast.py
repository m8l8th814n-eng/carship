#!/usr/bin/env python3
"""Flags prompt segments whose foreground equals their background.

Powerline separators legitimately sit at low contrast — their foreground is the
previous segment's colour and their background the next one's — so only an
exact match is treated as a fault. That is what happens when a palette maps two
adjacent roles in the format chain to the same colour, which makes the
separator disappear entirely.
"""

import os
import re
import subprocess
import sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/carship"
SGR = re.compile(r"(\x1b\[[0-9;]*m)")


def segments(text):
    """Yields (visible_text, fg, bg) for every styled run."""
    fg = bg = None
    for part in SGR.split(text):
        if part.startswith("\x1b["):
            codes = part[2:-1].split(";")
            i = 0
            while i < len(codes):
                code = codes[i]
                if code in ("0", ""):
                    fg = bg = None
                elif code == "38" and i + 4 < len(codes) and codes[i + 1] == "2":
                    fg = tuple(codes[i + 2:i + 5])
                    i += 4
                elif code == "48" and i + 4 < len(codes) and codes[i + 1] == "2":
                    bg = tuple(codes[i + 2:i + 5])
                    i += 4
                i += 1
        elif part.strip():
            yield part.strip(), fg, bg


def main():
    presets = subprocess.run([BIN, "preset", "--list"], capture_output=True,
                             text=True, check=True).stdout.split()
    failures = 0

    for name in presets:
        toml = subprocess.run([BIN, "preset", name], capture_output=True,
                              text=True, check=True).stdout
        with open("/tmp/carship-contrast.toml", "w") as f:
            f.write(toml)

        env = dict(os.environ, COLUMNS="100",
                   CARSHIP_CONFIG="/tmp/carship-contrast.toml")
        out = subprocess.run([BIN, "prompt"], capture_output=True, text=True,
                             env=env).stdout

        for text, fg, bg in segments(out):
            if fg and bg and fg == bg:
                print(f"FAIL invisible segment in {name}: "
                      f"'{text[:20]}' fg=bg=rgb({','.join(fg)})")
                failures += 1

    if failures:
        print(f"\n{failures} invisible segment(s)")
        return 1
    print(f"ok   no invisible segments in {len(presets)} presets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
