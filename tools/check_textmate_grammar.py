#!/usr/bin/env python3
"""Validate editors/loxpp.tmbundle against the VS Code grammar and the spec.

The bundle JSON is a byte-for-byte copy of
editors/loxpp-vscode/syntaxes/lox.tmLanguage.json, which the
vscode-textmate tests cover. This script keeps the copy honest: it fails
when the two files drift, when the XML plist stops matching the JSON, or
when a keyword from spec/01-lexical.md leaves the grammar.
"""

import json
import plistlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUNDLE = ROOT / "editors" / "loxpp.tmbundle"
BUNDLE_JSON = BUNDLE / "Syntaxes" / "Lox++.tmLanguage.json"
BUNDLE_PLIST = BUNDLE / "Syntaxes" / "Lox++.tmLanguage"
BUNDLE_INFO = BUNDLE / "info.plist"
VSCODE_JSON = ROOT / "editors" / "loxpp-vscode" / "syntaxes" / "lox.tmLanguage.json"

# The reserved words from spec/01-lexical.md. `true`, `false`, and `nil`
# scope as constants; the rest scope as keywords or storage types.
KEYWORDS = [
    "and", "break", "case", "catch", "class", "continue", "default",
    "defer", "else", "enum", "false", "for", "fun", "if", "in", "match",
    "nil", "or", "print", "return", "super", "this", "throw", "true",
    "try", "var", "while",
]

REQUIRED_RULES = [
    "comments", "strings", "numbers", "keywords", "declarations",
    "operators", "operators-patterns", "punctuation", "identifiers",
]

UUID_RE = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
)


def fail(message):
    print(f"FAIL: {message}")
    return False


def check_json_loads(text):
    try:
        return True, json.loads(text)
    except json.JSONDecodeError as exc:
        return False, fail(f"bundle JSON does not parse: {exc}")


def collect_patterns(node, out):
    if isinstance(node, dict):
        for key in ("match", "begin", "end", "name"):
            value = node.get(key)
            if isinstance(value, str):
                out.append(value)
        for value in node.values():
            collect_patterns(value, out)
    elif isinstance(node, list):
        for value in node:
            collect_patterns(value, out)


def main():
    ok = True

    for path in (BUNDLE_JSON, BUNDLE_PLIST, BUNDLE_INFO, VSCODE_JSON):
        if not path.is_file():
            ok = fail(f"missing file: {path.relative_to(ROOT)}") and ok

    if not ok:
        return 1

    bundle_bytes = BUNDLE_JSON.read_bytes()
    vscode_bytes = VSCODE_JSON.read_bytes()
    if bundle_bytes != vscode_bytes:
        ok = fail(
            "bundle JSON differs from editors/loxpp-vscode/syntaxes/"
            "lox.tmLanguage.json; copy the tested file over"
        ) and ok

    parsed, grammar = check_json_loads(bundle_bytes.decode("utf-8"))
    if not parsed:
        return 1

    if grammar.get("name") != "Lox++":
        ok = fail(f"bundle JSON name is {grammar.get('name')!r}, want 'Lox++'") and ok
    if grammar.get("scopeName") != "source.lox":
        ok = fail(
            f"bundle JSON scopeName is {grammar.get('scopeName')!r}, "
            "want 'source.lox'"
        ) and ok

    repository = grammar.get("repository", {})
    for rule in REQUIRED_RULES:
        if rule not in repository:
            ok = fail(f"bundle JSON repository lacks #{rule}") and ok

    pattern_text = []
    collect_patterns(grammar, pattern_text)
    blob = "\n".join(pattern_text)
    for keyword in KEYWORDS:
        if keyword not in blob:
            ok = fail(f"keyword {keyword!r} is missing from the grammar") and ok

    # The keyword rules must name every reserved word. A plain substring
    # check cannot prove this: the declaration lookahead repeats the words,
    # so a dropped rule still leaves the substring behind. Read the words
    # out of the keyword alternations instead.
    alternation = re.compile(r"\(\?:([a-z]+(?:\|[a-z]+)*)\)")
    scoped = set()
    for rule in grammar.get("repository", {}).get("keywords", {}).get("patterns", []):
        for group in alternation.findall(rule.get("match", "")):
            scoped.update(group.split("|"))
    missing = set(KEYWORDS) - scoped
    if missing:
        ok = fail(
            "keyword rules lack: " + ", ".join(sorted(missing))
        ) and ok

    if "TODO|FIXME" not in blob:
        ok = fail("comment TODO/FIXME marker is missing from the grammar") and ok
    if "constant.character.escape" not in blob:
        ok = fail("string escape rule is missing from the grammar") and ok
    if "invalid.illegal.bad-escape" not in blob:
        ok = fail("bad-escape rule is missing from the grammar") and ok

    try:
        with BUNDLE_PLIST.open("rb") as handle:
            plist_grammar = plistlib.load(handle)
    except Exception as exc:
        fail(f"bundle plist does not parse: {exc}")
        return 1

    for key in ("name", "scopeName", "patterns", "repository"):
        if plist_grammar.get(key) != grammar.get(key):
            ok = fail(f"plist {key!r} differs from the JSON grammar") and ok
    if "lox" not in plist_grammar.get("fileTypes", []):
        ok = fail("plist fileTypes lacks 'lox'") and ok
    if not UUID_RE.match(str(plist_grammar.get("uuid", ""))):
        ok = fail("plist uuid is missing or not a UUID") and ok

    try:
        with BUNDLE_INFO.open("rb") as handle:
            info = plistlib.load(handle)
    except Exception as exc:
        fail(f"info.plist does not parse: {exc}")
        return 1

    if info.get("name") != "Lox++":
        ok = fail(f"info.plist name is {info.get('name')!r}, want 'Lox++'") and ok
    if not UUID_RE.match(str(info.get("uuid", ""))):
        ok = fail("info.plist uuid is missing or not a UUID") and ok

    if not ok:
        return 1
    print(
        f"OK: loxpp.tmbundle matches the VS Code grammar "
        f"({len(KEYWORDS)} keywords, {len(REQUIRED_RULES)} rules, plist in sync)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
