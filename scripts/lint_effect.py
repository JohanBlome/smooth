#!/usr/bin/env python3
"""Catch constructs known to fail in OBS's conservative .effect parser."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


CAST_TYPES = (
    r"float|int|uint|bool|"
    r"float[234]|int[234]|uint[234]|bool[234]|"
    r"float[234]x[234]"
)

RULES = [
    (
        re.compile(r"\[\s*(unroll|loop|branch|flatten|fastopt|allow_uav_condition)\s*\]"),
        "HLSL control-flow attributes are rejected by OBS's shader translator",
    ),
    (
        re.compile(rf"\((?:{CAST_TYPES})\)\s*[A-Za-z_(\-+]"),
        "C-style casts are rejected; use constructor syntax such as float(x)",
    ),
    (
        re.compile(r":\s*COLOR(?![A-Za-z0-9_])"),
        "COLOR semantics are unreliable between stages; use TEXCOORDn",
    ),
]

TECH_BIND_RE = re.compile(
    r"(?P<stage>vertex_shader|pixel_shader)\s*=\s*"
    r"(?P<func>[A-Za-z_][A-Za-z0-9_]*)\s*\(\s*"
    r"(?P<placeholder>[A-Za-z_][A-Za-z0-9_]*)\s*\)\s*;"
)
LINE_COMMENT_RE = re.compile(r"//.*")
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    text = BLOCK_COMMENT_RE.sub("", text)
    return "\n".join(LINE_COMMENT_RE.sub("", line) for line in text.splitlines())


def function_parameter_pattern(name: str) -> re.Pattern[str]:
    return re.compile(
        rf"\b{re.escape(name)}\s*\(\s*"
        rf"[A-Za-z_][A-Za-z0-9_<>:.\s*]*?\s+"
        rf"(?P<param>[A-Za-z_][A-Za-z0-9_]*)\s*[,)]"
    )


def lint(path: Path) -> list[tuple[int, str, str]]:
    raw = path.read_text(encoding="utf-8")
    raw_lines = raw.splitlines()
    clean = strip_comments(raw)
    clean_lines = clean.splitlines()
    findings: list[tuple[int, str, str]] = []

    for line_number, line in enumerate(clean_lines, start=1):
        for rule, message in RULES:
            if rule.search(line):
                findings.append((line_number, message, raw_lines[line_number - 1]))

        binding = TECH_BIND_RE.search(line)
        if not binding:
            continue
        function = binding.group("func")
        placeholder = binding.group("placeholder")
        definition = function_parameter_pattern(function).search(clean)
        if not definition:
            findings.append((line_number, f"shader function {function} is not defined", raw_lines[line_number - 1]))
        elif definition.group("param") != placeholder:
            findings.append(
                (
                    line_number,
                    f"technique placeholder {placeholder!r} does not match {function}'s parameter",
                    raw_lines[line_number - 1],
                )
            )
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    count = 0
    for path in args.files:
        try:
            findings = lint(path)
        except OSError as error:
            print(f"lint_effect: {path}: {error}", file=sys.stderr)
            return 2
        for line_number, message, source in findings:
            count += 1
            print(f"{path}:{line_number}: error: {message}")
            print(f"  | {source.rstrip()}")
    if count:
        print(f"lint_effect: {count} issue(s) found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
