#!/usr/bin/env python3
"""tools/fix-header-includes.py - add the standard includes that
check-header-includes.py reports as missing.

Inserts each missing <header> into the file's existing block of angle-bracket
includes, keeping that block sorted. If a file has no such block, one is
started after the last project include (or after #pragma once), which
matches the convention used throughout this repository: project includes
first, blank line, then standard headers.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from importlib import import_module

checker = import_module("check-header-includes".replace("-", "_")) \
    if False else None  # noqa: E501  (module name has dashes; load manually)

import importlib.util

_spec = importlib.util.spec_from_file_location(
    "chk", Path(__file__).parent / "check-header-includes.py")
chk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(chk)

ANGLE_RE = re.compile(r'^#include <([^>]+)>$', re.M)
PROJECT_INC_RE = re.compile(r'^#include "[^"]+"$', re.M)
PRAGMA_RE = re.compile(r'^#pragma once$', re.M)


def fix(path: Path) -> list[str]:
    problems = chk.check(path)
    if not problems:
        return []
    needed = sorted({m.split("<")[1].split(">")[0] for m in problems})

    text = path.read_text(encoding="utf-8")
    angles = list(ANGLE_RE.finditer(text))

    if angles:
        existing = [m.group(1) for m in angles]
        merged = sorted(set(existing) | set(needed))
        block = "\n".join(f"#include <{h}>" for h in merged)
        start, end = angles[0].start(), angles[-1].end()
        # Only rewrite a contiguous run; a split block (e.g. platform
        # includes under an #ifdef) must be left alone.
        if text[start:end].strip().splitlines() == [
                l for l in text[start:end].strip().splitlines()
                if l.startswith("#include <")]:
            text = text[:start] + block + text[end:]
        else:
            anchor = angles[0].start()
            text = (text[:anchor]
                    + "\n".join(f"#include <{h}>" for h in needed)
                    + "\n" + text[anchor:])
    else:
        proj = list(PROJECT_INC_RE.finditer(text))
        pragma = PRAGMA_RE.search(text)
        anchor = proj[-1].end() if proj else (pragma.end() if pragma else 0)
        block = "\n\n" + "\n".join(f"#include <{h}>" for h in needed)
        text = text[:anchor] + block + text[anchor:]

    path.write_text(text, encoding="utf-8")
    return needed


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(".")
    headers = sorted(
        p for p in root.glob("folio-*/**/*.h")
        if "build" not in p.parts and "third_party" not in p.parts)
    changed = 0
    for h in headers:
        added = fix(h)
        if added:
            print(f"{h}: +{', '.join('<'+a+'>' for a in added)}")
            changed += 1
    print(f"\n-- fixed {changed} header(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
