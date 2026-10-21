#!/usr/bin/env python3
"""
Rewrite each bare enum constructor name in a Lox++ program to the qualified
form E.A (docs/decisions/enum-constructor-namespaces.md).

The script reads each file alone. The enum declarations of a file tell it
which names are constructors. It rewrites a constructor name:

  - in an expression, where the name is not a member name after '.';
  - in a match arm pattern, where the name is the head of an alternative:
    the first pattern of the arm, a pattern after 'or' or after '@'.
    A name inside the parentheses, braces or brackets of a pattern is a
    binding or a field name, so the script leaves it.

It never rewrites an enum name, which means the enum also where a
constructor has that name, or a bare "_", which is the wildcard.

It changes no other token, and keeps all spacing and comments.

Some programs need a person to decide. The script then rewrites nothing in
the file and reports the reason:

  - a constructor name is also declared as a variable, a parameter, a
    function, a class, a method or a binding in the file, so a use of the
    name can mean the declaration and not the constructor;
  - a constructor name is used before the declaration of its enum. E.A
    before the declaration of E is a compile error, so the declaration must
    move first;
  - a constructor name is a pattern head before the declaration of its
    enum. Today that name is a binding, not a constructor test.

Usage:
    tools/qualify_enum_constructors.py [--check] <path>...

<path> is a .lox file or a directory (every *.lox file under it). Without
--check, the script rewrites the files in place. With --check, it changes
nothing and lists each bare constructor name that it would rewrite. Exits 1
when --check finds one, or when a file needs a person to decide; 0
otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

TOKEN_RE = re.compile(
    r"""
      (?P<ws>[ \t\r\n]+)
    | (?P<comment>//[^\n]*)
    | (?P<string>"(?:\\.|[^"\\])*"?)
    | (?P<number>[0-9]+(?:\.[0-9]+)?)
    | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
    | (?P<op>\.\.\.|=>|==|!=|<=|>=|[-+*/%;,.(){}\[\]<>=!@:?&|^~])
    | (?P<other>.)
    """,
    re.VERBOSE | re.DOTALL,
)


@dataclass
class Tok:
    kind: str
    text: str
    start: int
    line: int


def tokenize(src: str) -> list[Tok]:
    toks: list[Tok] = []
    line = 1
    for m in TOKEN_RE.finditer(src):
        kind = m.lastgroup or "other"
        text = m.group()
        if kind not in ("ws", "comment"):
            toks.append(Tok(kind, text, m.start(), line))
        line += text.count("\n")
    return toks


DECL_KEYWORDS = {"var", "fun", "class"}


@dataclass
class Edit:
    tok: Tok
    enum: str
    where: str


class FileRewrite:
    def __init__(self, path: Path, src: str) -> None:
        self.path = path
        self.src = src
        self.toks = tokenize(src)
        # constructor name -> (enum name, index of the enum keyword)
        self.ctors: dict[str, tuple[str, int]] = {}
        # token indexes inside enum declarations
        self.enum_body: set[int] = set()
        self.problems: list[str] = []
        self.edits: list[Edit] = []

    def t(self, i: int) -> Tok | None:
        return self.toks[i] if 0 <= i < len(self.toks) else None

    def text(self, i: int) -> str:
        tok = self.t(i)
        return tok.text if tok else ""

    def closing(self, i: int) -> int:
        """Index of the bracket that closes the one at i."""
        pairs = {"(": ")", "{": "}", "[": "]"}
        open_text = self.text(i)
        close_text = pairs[open_text]
        depth = 0
        j = i
        while j < len(self.toks):
            if self.toks[j].kind == "op":
                if self.toks[j].text == open_text:
                    depth += 1
                elif self.toks[j].text == close_text:
                    depth -= 1
                    if depth == 0:
                        return j
            j += 1
        return len(self.toks) - 1

    def collect_enums(self) -> None:
        for i, tok in enumerate(self.toks):
            if tok.kind != "ident" or tok.text != "enum":
                continue
            name = self.t(i + 1)
            if name is None or name.kind != "ident" or self.text(i + 2) != "{":
                continue
            end = self.closing(i + 2)
            self.enum_body.update(range(i, end + 1))
            j = i + 3
            while j < end:
                if self.toks[j].kind == "ident":
                    self.ctors.setdefault(self.toks[j].text, (name.text, i))
                    if self.text(j + 1) == "(":
                        j = self.closing(j + 1)
                j += 1

    def declared_names(self) -> dict[str, int]:
        """Each name the file declares, other than as an enum or a
        constructor, with the line of one such declaration."""
        names: dict[str, int] = {}

        def add(i: int) -> None:
            tok = self.t(i)
            if tok is not None and tok.kind == "ident":
                names.setdefault(tok.text, tok.line)

        def add_list(open_i: int) -> None:
            for j in range(open_i + 1, self.closing(open_i)):
                if self.toks[j].kind == "ident":
                    add(j)

        class_depths: list[int] = []
        depth = 0
        for i, tok in enumerate(self.toks):
            if i in self.enum_body:
                continue
            if tok.kind == "op" and tok.text in "{(":
                depth += 1
            elif tok.kind == "op" and tok.text in "})":
                depth -= 1
                if class_depths and depth == class_depths[-1]:
                    class_depths.pop()
            if tok.kind != "ident":
                continue
            if tok.text in DECL_KEYWORDS:
                nxt = self.text(i + 1)
                if nxt in ("[", "{"):
                    add_list(i + 1)
                else:
                    add(i + 1)
                if tok.text == "fun" and self.text(i + 2) == "(":
                    add_list(i + 2)
                if tok.text == "fun" and self.text(i + 1) == "(":
                    add_list(i + 1)
                if tok.text == "class":
                    j = i + 2
                    while j < len(self.toks) and self.text(j) != "{":
                        j += 1
                    class_depths.append(depth)
            elif tok.text == "catch" and self.text(i + 1) == "(":
                add(i + 2)
            elif (
                class_depths
                and depth == class_depths[-1] + 1
                and self.text(i + 1) == "("
                and self.text(i - 1) in ("{", "}", ";")
            ):
                # A method: its name and its parameters.
                add(i)
                add_list(i + 1)
            elif self.text(i + 1) == "@":
                add(i)
        # A name inside the brackets of a pattern is a binding.
        for i in self.pattern_inner():
            if self.toks[i].kind == "ident":
                add(i)
        return names

    def pattern_heads(self) -> dict[int, str]:
        """Token indexes of the pattern heads of every match arm, with
        'head' or 'tag' (a head with no '(' or '{' after it)."""
        heads: dict[int, str] = {}
        for i, tok in enumerate(self.toks):
            if tok.kind != "ident" or tok.text != "case" or i in self.enum_body:
                continue
            j = i + 1
            expect_head = True
            while j < len(self.toks):
                cur = self.toks[j]
                if cur.kind == "op" and cur.text == "=>":
                    break
                if cur.kind == "ident" and cur.text == "if":
                    break
                if cur.kind == "op" and cur.text in "([{":
                    j = self.closing(j) + 1
                    expect_head = False
                    continue
                if cur.kind == "ident" and cur.text == "or":
                    expect_head = True
                elif cur.kind == "op" and cur.text in ("@", ","):
                    expect_head = True
                elif cur.kind == "ident" and expect_head:
                    if self.text(j + 1) == ".":
                        # Already qualified: E.A
                        j += 2
                    else:
                        heads[j] = "head"
                    expect_head = False
                else:
                    expect_head = False
                j += 1
        return heads

    def pattern_inner(self) -> set[int]:
        """Token indexes inside the brackets of a pattern."""
        inner: set[int] = set()
        for i, tok in enumerate(self.toks):
            if tok.kind != "ident" or tok.text != "case" or i in self.enum_body:
                continue
            j = i + 1
            while j < len(self.toks):
                cur = self.toks[j]
                if (cur.kind == "op" and cur.text == "=>") or (cur.kind == "ident" and cur.text == "if"):
                    break
                if cur.kind == "op" and cur.text in "([{":
                    end = self.closing(j)
                    inner.update(range(j, end + 1))
                    j = end
                j += 1
        return inner

    def plan(self) -> None:
        self.collect_enums()
        if not self.ctors:
            return
        declared = self.declared_names()
        clash = sorted(n for n in self.ctors if n in declared and n not in self.enum_names())
        for name in clash:
            self.problems.append(
                f"the constructor name '{name}' is also declared at line {declared[name]}"
            )
        heads = self.pattern_heads()
        inner = self.pattern_inner()
        for i, tok in enumerate(self.toks):
            if tok.kind != "ident" or tok.text not in self.ctors or i in self.enum_body:
                continue
            if self.text(i - 1) == ".":
                continue
            # An enum name means the enum, also where a constructor has
            # that name.
            if tok.text in self.enum_names():
                continue
            # A bare "_" is the wildcard; only "_(" and "_{" name a
            # constructor.
            if tok.text == "_" and self.text(i + 1) not in ("(", "{"):
                continue
            enum_name, enum_at = self.ctors[tok.text]
            if i in inner:
                continue
            in_pattern = self.in_pattern(i)
            if in_pattern and i not in heads:
                continue
            if i < enum_at:
                kind = "pattern head" if in_pattern else "use"
                self.problems.append(
                    f"line {tok.line}: {kind} of '{tok.text}' before the declaration of enum '{enum_name}'"
                )
                continue
            if self.text(i + 1) == ":" and self.text(i - 1) in ("{", ","):
                # A map key name is not an expression.
                continue
            self.edits.append(Edit(tok, enum_name, "pattern" if in_pattern else "expression"))

    def enum_names(self) -> set[str]:
        return {enum for enum, _ in self.ctors.values()}

    def in_pattern(self, i: int) -> bool:
        """Whether token i is between a 'case' and its '=>' or guard."""
        j = i - 1
        depth = 0
        while j >= 0:
            cur = self.toks[j]
            if cur.kind == "op" and cur.text in ")]}":
                depth += 1
            elif cur.kind == "op" and cur.text in "([{":
                if depth == 0:
                    return False
                depth -= 1
            elif depth == 0 and cur.kind == "op" and cur.text in ("=>", ";"):
                return False
            elif depth == 0 and cur.kind == "ident" and cur.text == "if":
                return False
            elif depth == 0 and cur.kind == "ident" and cur.text == "case":
                return True
            j -= 1
        return False

    def rewritten(self) -> str:
        out = []
        last = 0
        for edit in sorted(self.edits, key=lambda e: e.tok.start):
            out.append(self.src[last : edit.tok.start])
            out.append(f"{edit.enum}.{edit.tok.text}")
            last = edit.tok.start + len(edit.tok.text)
        out.append(self.src[last:])
        return "".join(out)


def collect(paths: list[str]) -> list[Path]:
    files: list[Path] = []
    for p in paths:
        path = Path(p)
        if path.is_dir():
            files.extend(sorted(path.rglob("*.lox")))
        else:
            files.append(path)
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="change nothing; list each name to rewrite")
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()

    status = 0
    changed = 0
    for path in collect(args.paths):
        src = path.read_text(encoding="utf-8")
        job = FileRewrite(path, src)
        job.plan()
        if job.problems:
            status = 1
            for problem in job.problems:
                print(f"DECIDE   {path}: {problem}")
            continue
        if not job.edits:
            continue
        if args.check:
            status = 1
            for edit in job.edits:
                print(f"BARE     {path}:{edit.tok.line}: {edit.tok.text} ({edit.where}, enum {edit.enum})")
            continue
        path.write_text(job.rewritten(), encoding="utf-8")
        changed += 1
        print(f"REWROTE  {path} ({len(job.edits)} name(s))")
    if not args.check:
        print(f"{changed} file(s) rewritten")
    return status


if __name__ == "__main__":
    sys.exit(main())
