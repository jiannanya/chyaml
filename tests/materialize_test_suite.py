#!/usr/bin/env python3
"""Materialize the yaml-test-suite `src/` templates into the directory layout the
`chyaml_conformance` runner consumes.

Each `src/<ID>.yaml` file is a YAML list of cases carrying a `yaml:` input
block, and `fail: true` when the input is expected to be rejected. The layout
produced here is:

    <out>/<ID>/in.yaml     the input document
    <out>/<ID>/error       present exactly when the input must be rejected

usage: materialize_test_suite.py <suite-src-dir> <out-dir>
"""

import re
import sys
from pathlib import Path

import yaml

# The suite templates spell out characters that are invisible or ambiguous in a
# plain text file. These substitutions mirror bin/YAMLTestSuite.pm::unescape so
# the materialized `in.yaml` is byte-identical to what the official
# `suite-to-data` step produces.
UNESCAPE = (
    ("\u2423", " "),        # OPEN BOX      -> space
    ("\u2190", "\r"),       # LEFTWARDS ARROW -> carriage return
    ("\u21d4", "\ufeff"),   # LEFT RIGHT DOUBLE ARROW -> BOM
    ("\u21b5", ""),         # DOWNWARDS ARROW WITH CORNER -> dropped
    ("\u220e", ""),         # END OF PROOF -> end-of-input marker
)


def unescape(text: str) -> str:
    text = re.sub("\u2014*\u00bb", "\t", text)  # em-dash run + guillemet -> tab
    for source, replacement in UNESCAPE:
        text = text.replace(source, replacement)
    return re.sub("\u220e\\n\\Z", "", text)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src = Path(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    written = 0
    errors = 0
    skipped = 0
    for path in sorted(src.glob("*.yaml")):
        try:
            cases = yaml.safe_load(path.read_text(encoding="utf-8"))
        except yaml.YAMLError as exc:  # pragma: no cover - diagnostic path
            print(f"unparsable template {path.name}: {exc}", file=sys.stderr)
            skipped += 1
            continue
        if not isinstance(cases, list):
            skipped += 1
            continue
        for index, case in enumerate(cases):
            if not isinstance(case, dict) or "yaml" not in case:
                skipped += 1
                continue
            identifier = path.stem if len(cases) == 1 else f"{path.stem}-{index}"
            directory = out / identifier
            directory.mkdir(parents=True, exist_ok=True)
            text = case["yaml"]
            if not isinstance(text, str):
                skipped += 1
                continue
            (directory / "in.yaml").write_text(unescape(text), encoding="utf-8")
            marker = directory / "error"
            if case.get("fail"):
                marker.write_text("", encoding="utf-8")
                errors += 1
            elif marker.exists():
                marker.unlink()
            written += 1

    print(f"materialized {written} cases ({errors} expected to fail, {skipped} skipped)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
