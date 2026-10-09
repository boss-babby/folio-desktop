#!/usr/bin/env python3
"""tools/check-header-includes.py - find standard includes a header relies on
but does not ask for.

Why this exists
---------------
`folio-ui/include/folio/ui/Draw.h` declared a function returning
`std::string` while including only `<string_view>`. Every Linux build
passed for months, because libstdc++ drags `<string>` in through other
headers. MSVC's standard library does not, so the first Windows CI run
failed with "'string': is not a member of 'std'".

A compiler cannot find this for us: the only compiler that would complain
is the one we do not have. So this is a static check instead - it reads
the text, not the semantics, and is deliberately conservative.

It is a lint, not a proof. It reports a symbol only when the header that
owns it is not included directly, and it never guesses about transitive
includes, because guessing is the thing that caused the bug.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

# std symbol -> the one header the standard says provides it.
# Kept small and certain on purpose. Anything ambiguous is left out: a
# lint that cries wolf gets switched off, and then it protects nothing.
OWNERS: dict[str, str] = {
    "string": "string",
    "to_string": "string",
    "stoi": "string",
    "stof": "string",
    "vector": "vector",
    "array": "array",
    "deque": "deque",
    "map": "map",
    "set": "set",
    "unordered_map": "unordered_map",
    "unordered_set": "unordered_set",
    "pair": "utility",
    "move": "utility",
    "forward": "utility",
    "swap": "utility",
    "unique_ptr": "memory",
    "shared_ptr": "memory",
    "weak_ptr": "memory",
    "make_unique": "memory",
    "make_shared": "memory",
    "enable_shared_from_this": "memory",
    "function": "functional",
    "optional": "optional",
    "nullopt": "optional",
    "variant": "variant",
    "string_view": "string_view",
    "min": "algorithm",
    "max": "algorithm",
    "clamp": "algorithm",
    "sort": "algorithm",
    "find": "algorithm",
    "atomic": "atomic",
    "mutex": "mutex",
    "lock_guard": "mutex",
    "unique_lock": "mutex",
    "thread": "thread",
    "condition_variable": "condition_variable",
    "size_t": "cstddef",
    "ptrdiff_t": "cstddef",
    "byte": "cstddef",
    "uint8_t": "cstdint",
    "uint16_t": "cstdint",
    "uint32_t": "cstdint",
    "uint64_t": "cstdint",
    "int8_t": "cstdint",
    "int16_t": "cstdint",
    "int32_t": "cstdint",
    "int64_t": "cstdint",
    "uintptr_t": "cstdint",
}

# Headers that genuinely re-export another, per the standard.
SATISFIES: dict[str, set[str]] = {
    "string": {"string_view"},          # <string> provides std::string_view
    "memory": {"cstddef"},
    "vector": {"cstddef"},
    "string_view": {"cstddef"},
    "array": {"cstddef"},
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*<([^>]+)>', re.M)
# `std::foo` with no leading identifier char, so `folio::std::x` or a
# member named `...std::` cannot match.
USE_RE = re.compile(r'(?<![\w:])std::([a-z_][a-z0-9_]*)')
# Strip comments and string literals before scanning, so prose in a
# doc-comment ("returns a std::string") is not treated as code.
BLOCK_COMMENT_RE = re.compile(r'/\*.*?\*/', re.S)
LINE_COMMENT_RE = re.compile(r'//[^\n]*')
STRING_RE = re.compile(r'"(?:[^"\\]|\\.)*"')


def strip_noise(text: str) -> str:
    text = BLOCK_COMMENT_RE.sub(" ", text)
    text = LINE_COMMENT_RE.sub(" ", text)
    return STRING_RE.sub('""', text)


def check(path: Path) -> list[str]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    included = set(INCLUDE_RE.findall(raw))

    # Expand the handful of documented re-exports.
    effective = set(included)
    for inc in included:
        effective |= SATISFIES.get(inc, set())

    problems = []
    seen = set()
    for sym in USE_RE.findall(strip_noise(raw)):
        owner = OWNERS.get(sym)
        if owner is None or owner in effective or sym in seen:
            continue
        seen.add(sym)
        problems.append(f"std::{sym} used but <{owner}> not included")
    return problems


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(".")
    headers = sorted(
        p for p in root.glob("folio-*/**/*.h")
        if "build" not in p.parts and "third_party" not in p.parts
    )
    total = 0
    for h in headers:
        for msg in check(h):
            print(f"{h}: {msg}")
            total += 1
    print(f"\n-- header include check: {len(headers)} headers, "
          f"{total} missing include(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
