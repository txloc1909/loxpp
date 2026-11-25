#!/usr/bin/env python3
"""Check the table of write-barrier mutators.

docs/reference/gc-runtime-contract.md, section 7, has a table. Each row names
a heap object type, one of its members, the mutators that change it, the
barrier hooks that those mutators run, and a status. This script reads the
headers of the object types (src/*_objects.h, src/object.h) and checks that:

  1. each object type has a row;
  2. each member that can hold a reference (a Value, an object pointer, a
     Table, a container of those) and that is not const has a row;
  3. a public member of that kind has the status exempt or unprotected, with a
     note, because only a private member forces code to use a mutator;
  4. each row names a member that the type has;
  5. each mutator of a protected row exists in the type, and its body reaches
     each hook of the row (directly, or through a helper or another mutator
     of the type);
  6. a status "immutable" names a const member;
  7. no object type has a friend, and no public function of an object type
     returns a non-const reference or pointer to a private member, except the
     functions in RAW_ACCESSORS (they have a row with the status exempt);
  8. each RAW_ACCESSORS function is called only in its allowed files;
  9. const_cast does not appear in src/ outside the allowed files;
 10. isUnprotectedType (src/barrier.h) lists exactly the types that have a row
     with the status unprotected.

Usage: check_gc_barrier_table.py [--root DIR]

Exit status: 0 pass, 1 fail, 2 usage error or a file that is missing.
"""

import argparse
import os
import re
import sys

HEADERS = [
    "src/object.h",
    "src/class_objects.h",
    "src/container_objects.h",
    "src/exec_objects.h",
]
# Out-of-line bodies of mutators.
BODY_SOURCES = ["src/container_objects.cpp"]
BARRIER_HEADER = "src/barrier.h"
CONTRACT = "docs/reference/gc-runtime-contract.md"

HOOKS = {
    "barrierField",
    "barrierElement",
    "barrierEntry",
    "barrierUpvalue",
    "barrierSnapshot",
}
STATUSES = {"protected", "immutable", "exempt", "unprotected"}

# A function that gives raw write access to a member, with the files that may
# call it.
RAW_ACCESSORS = {
    "ObjTask::ioProgress": ["src/stdlib/net_api.cpp"],
}
# Each use here casts a char buffer or an object pointer, and writes no member
# of a heap object.
CONST_CAST_ALLOWED = {
    "src/memory_manager.cpp",
    "src/backend/qbe_frontend.cpp",
    "src/stdlib/net_api.cpp",
}


def fail(problems):
    for p in problems:
        print(f"FAIL: {p}")
    sys.exit(1)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def match_brace(text, start):
    """Index of the brace that closes the one at `start`."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unbalanced braces")


class Member:
    def __init__(self, name, type_, access):
        self.name = name
        self.type = type_
        self.access = access

    @property
    def is_const(self):
        t = self.type
        return bool(re.match(r"\s*const\b", t)) or bool(re.search(r"\*\s*const\b", t))

    @property
    def holds_reference(self):
        t = self.type
        if re.search(r"\bValue\b(?!\s*\*)", t):
            return True
        if re.search(r"\bObj\w*\s*\*", t):
            return True
        for word in ("Table", "Entries", "FrameSnapshot", "WaitingCall"):
            if word in t:
                return True
        return False


class Struct:
    def __init__(self, name, body):
        self.name = name
        self.body = body
        self.members = {}
        self.methods = {}  # name -> body text (may be "")
        self.has_friend = False
        self.nonconst_accessors = []
        self._parse()

    def _parse(self):
        body = self.body
        access = "public"
        stmt = ""
        i = 0
        depth = 0
        raw = ""
        while i < len(body):
            ch = body[i]
            if ch == "{":
                end = match_brace(body, i)
                inner = body[i : end + 1]
                if "(" in stmt and not stmt.rstrip().endswith("="):
                    # A function definition (or a constructor).
                    name = re.findall(r"(\w+)\s*\(", stmt)[0]
                    sig = stmt
                    self.methods.setdefault(name, "")
                    self.methods[name] += sig + inner
                    if (
                        re.search(r"[&*]\s*\w+\s*\(\s*\)\s*(?!const)\{?$", sig.strip())
                        and "return" in inner
                        and re.search(r"return\s+m_", inner)
                        and not re.search(r"\)\s*const\s*$", sig.strip())
                    ):
                        self.nonconst_accessors.append(name)
                    stmt = ""
                    i = end + 1
                    # A function ends its statement.
                    continue
                # An initializer: keep a marker and go on to the `;`.
                stmt += "{}"
                i = end + 1
                continue
            if ch == ";":
                self._statement(stmt, access)
                stmt = ""
                i += 1
                continue
            m = re.match(r"\s*(public|private|protected)\s*:", body[i:]) if ch in "ppP \n" else None
            if m and stmt.strip() == "":
                access = m.group(1)
                i += m.end()
                continue
            stmt += ch
            i += 1
        _ = depth, raw

    def _statement(self, stmt, access):
        s = " ".join(stmt.split())
        if not s:
            return
        if s.startswith("friend"):
            self.has_friend = True
            return
        if s.startswith(("using", "static", "enum", "struct", "template", "typedef")):
            return
        if "(" in s:
            # A declaration of a function, or a member with a call in its
            # initializer. A declaration has no body to check.
            name = re.findall(r"(\w+)\s*\(", s)[0]
            self.methods.setdefault(name, "")
            return
        s = re.sub(r"\s*=.*$", "", s)
        s = s.replace("{}", "")
        m = re.match(r"(.+?)\s*(\w+)\s*$", s)
        if not m:
            return
        type_, name = m.group(1), m.group(2)
        # `ObjClass* const klass` has the const in the type part.
        self.members[name] = Member(name, type_.strip(), access)


def parse_structs(path, text):
    text = strip_comments(text)
    out = {}
    for m in re.finditer(r"struct\s+(Obj\w+)\s*:\s*public\s+Obj\s*\{", text):
        start = m.end() - 1
        end = match_brace(text, start)
        out[m.group(1)] = Struct(m.group(1), text[start + 1 : end])
    return out


def out_of_line_bodies(text, struct_names):
    """{(Struct, method): body} for `Struct::method(...) {...}`."""
    text = strip_comments(text)
    bodies = {}
    for m in re.finditer(r"\b(Obj\w+)::(\w+)\s*\(", text):
        if m.group(1) not in struct_names:
            continue
        brace = text.find("{", m.end())
        semi = text.find(";", m.end())
        if brace == -1 or (semi != -1 and semi < brace):
            continue
        end = match_brace(text, brace)
        bodies.setdefault((m.group(1), m.group(2)), "")
        bodies[(m.group(1), m.group(2))] += text[brace : end + 1]
    return bodies


def free_functions(text):
    """{name: body} of the inline functions of a header."""
    text = strip_comments(text)
    out = {}
    for m in re.finditer(r"\binline\s+[\w:<>\*&\s]+?\b(\w+)\s*\([^;{]*\)\s*(?:const\s*)?\{", text):
        brace = m.end() - 1
        out[m.group(1)] = text[brace : match_brace(text, brace) + 1]
    return out


def reaches(body, hook, scope, seen):
    if re.search(r"\b%s\b" % hook, body):
        return True
    for name in set(re.findall(r"\b(\w+)\s*\(", body)):
        if name in seen or name not in scope:
            continue
        seen.add(name)
        if reaches(scope[name], hook, scope, seen):
            return True
    return False


def parse_table(text):
    """The rows of the table in section 7: a list of dicts."""
    m = re.search(r"^## 7\..*?$(.*?)^## 8\.", text, flags=re.S | re.M)
    if not m:
        fail(["section 7 of the contract was not found"])
    rows = []
    header = None
    for line in m.group(1).splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if cells and cells[0] == "Object" and "Status" in cells:
            header = cells
            continue
        if header is None or set("".join(cells)) <= set("-: "):
            continue
        if len(cells) != len(header):
            fail([f"a row of the table has {len(cells)} cells, not {len(header)}: {line}"])
        row = dict(zip(header, cells))
        rows.append(row)
    if not rows:
        fail(["the table of section 7 has no rows"])
    return rows


def split_names(cell):
    cell = cell.replace("`", "").strip()
    if cell in ("", "-"):
        return []
    return [c.strip() for c in cell.split(",") if c.strip()]


def unprotected_types(barrier_text):
    text = strip_comments(barrier_text)
    m = re.search(r"isUnprotectedType\s*\([^)]*\)\s*\{", text)
    if not m:
        fail(["isUnprotectedType was not found in " + BARRIER_HEADER])
    body = text[m.end() - 1 : match_brace(text, m.end() - 1) + 1]
    return set(re.findall(r"case\s+ObjType::(\w+)\s*:", body))


def camel(enum_name):
    return "Obj" + "".join(p.capitalize() for p in enum_name.split("_"))


def check(root):
    problems = []
    structs = {}
    texts = {}
    for h in HEADERS:
        path = os.path.join(root, h)
        if not os.path.exists(path):
            print(f"{h} does not exist")
            sys.exit(2)
        texts[h] = open(path).read()
        structs.update(parse_structs(h, texts[h]))
    barrier_text = open(os.path.join(root, BARRIER_HEADER)).read()
    scope_free = free_functions(barrier_text)
    bodies = {}
    for src in BODY_SOURCES:
        bodies.update(out_of_line_bodies(open(os.path.join(root, src)).read(), set(structs)))
    rows = parse_table(open(os.path.join(root, CONTRACT)).read())

    by_type = {}
    for r in rows:
        by_type.setdefault(r["Object"].replace("`", ""), []).append(r)

    # 1. each type has a row
    for name in sorted(structs):
        if name not in by_type:
            problems.append(f"{name} has no row in the table")
    for name in by_type:
        if name not in structs:
            problems.append(f"the table has a row for {name}, which is not an object type")

    # 2, 3. each reference member has a row
    for name, st in sorted(structs.items()):
        rows_of = by_type.get(name, [])
        named = {r["Member"].replace("`", ""): r for r in rows_of}
        for mem in st.members.values():
            if not mem.holds_reference or mem.is_const:
                continue
            row = named.get(mem.name)
            if row is None:
                problems.append(f"{name}::{mem.name} can hold a reference and has no row")
                continue
            if mem.access != "private" and row["Status"] not in ("exempt", "unprotected"):
                problems.append(
                    f"{name}::{mem.name} is public, so its row must be exempt or unprotected"
                )

    # 4-6. each row is true
    for r in rows:
        name = r["Object"].replace("`", "")
        st = structs.get(name)
        if st is None:
            continue
        member = r["Member"].replace("`", "")
        status = r["Status"].replace("`", "")
        note = r.get("Note", "")
        if status not in STATUSES:
            problems.append(f"{name}::{member}: the status '{status}' is not one of {sorted(STATUSES)}")
            continue
        if member != "-" and member not in st.members:
            problems.append(f"{name}: the table names member '{member}', which the type does not have")
            continue
        if status in ("exempt", "unprotected") and (not note or note == "-"):
            problems.append(f"{name}::{member}: a {status} row needs a note with the reason")
        if status == "immutable" and member != "-" and not st.members[member].is_const:
            problems.append(f"{name}::{member}: the row says immutable, and the member is not const")
        muts = split_names(r["Mutators"])
        hooks = split_names(r["Barrier"])
        for h in hooks:
            if h not in HOOKS:
                problems.append(f"{name}::{member}: '{h}' is not a barrier hook")
        if status == "protected":
            if not muts or not hooks:
                problems.append(f"{name}::{member}: a protected row needs mutators and hooks")
            scope = dict(scope_free)
            for mname, body in st.methods.items():
                scope[mname] = body
            for (sname, mname), body in bodies.items():
                if sname == name:
                    scope[mname] = scope.get(mname, "") + body
            for mut in muts:
                if mut not in scope or not scope[mut]:
                    problems.append(f"{name}::{member}: mutator '{mut}' has no body in the type")
                    continue
                for h in hooks:
                    if not reaches(scope[mut], h, scope, {mut}):
                        problems.append(f"{name}::{mut} does not reach {h}")
        elif muts and status == "immutable":
            problems.append(f"{name}::{member}: an immutable row has mutators")

    # 7. no friend, no raw accessor
    for name, st in sorted(structs.items()):
        if st.has_friend:
            problems.append(f"{name} has a friend, which can write a private member")
        for acc in st.nonconst_accessors:
            if f"{name}::{acc}" not in RAW_ACCESSORS:
                problems.append(
                    f"{name}::{acc} returns a non-const reference or pointer to a private member"
                )
    for acc, files in RAW_ACCESSORS.items():
        tname, fn = acc.split("::")
        if tname not in structs or fn not in structs[tname].methods:
            problems.append(f"RAW_ACCESSORS names {acc}, which does not exist")
            continue
        rows_of = [r for r in by_type.get(tname, []) if r["Member"].replace("`", "") == fn
                   or fn in split_names(r["Mutators"])]
        if not rows_of or rows_of[0]["Status"] != "exempt":
            problems.append(f"{acc} needs a row with the status exempt")

    # 8, 9. scan the sources
    for dirpath, _, files in os.walk(os.path.join(root, "src")):
        for f in files:
            if not f.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, root)
            text = strip_comments(open(path, errors="replace").read())
            for acc, allowed in RAW_ACCESSORS.items():
                fn = acc.split("::")[1]
                if re.search(r"[>.]%s\s*\(" % fn, text) and rel not in allowed:
                    problems.append(f"{rel} calls {fn}, and only {allowed} may")
            if "const_cast" in text and rel not in CONST_CAST_ALLOWED:
                problems.append(f"{rel} uses const_cast")

    # 10. the unprotected set
    in_code = unprotected_types(barrier_text)
    in_table = set()
    for r in rows:
        if r["Status"].replace("`", "") == "unprotected":
            in_table.add(r["Object"].replace("`", ""))
    in_code_names = {camel(n) for n in in_code}
    if in_code_names != in_table:
        problems.append(
            f"isUnprotectedType lists {sorted(in_code_names)}, the table says {sorted(in_table)}"
        )
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--root",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."),
    )
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    if not os.path.exists(os.path.join(root, CONTRACT)):
        print(f"{CONTRACT} does not exist under {root}")
        sys.exit(2)
    problems = check(root)
    if problems:
        fail(problems)
    print("OK: the table of barrier mutators is complete")


if __name__ == "__main__":
    main()
