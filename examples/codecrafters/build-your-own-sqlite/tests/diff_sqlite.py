#!/usr/bin/env python3
"""diff_sqlite.py — differential oracle for the Lox++ SQLite reader.

Runs a battery of queries through your_sqlite.sh and through Python's
stdlib sqlite3 module (an independent, real SQLite implementation), and
diffs the two, line for line (order-independent — the challenge and our
engine both do unordered full/index scans). Complements run_stages.sh,
which checks only the handful of cases transcribed from the public
stage_descriptions/*.md files.

Usage: tests/diff_sqlite.py [your_sqlite.sh]
"""
import os
import sqlite3
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROG = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "your_sqlite.sh")
SAMPLE = os.path.join(HERE, "fixtures", "sample.db")
SUPERHEROES = os.path.join(HERE, "fixtures", "superheroes.db")
COMPANIES = os.path.join(HERE, "fixtures", "companies.db")

CASES = [
    (SAMPLE, "SELECT COUNT(*) FROM apples"),
    (SAMPLE, "SELECT COUNT(*) FROM oranges"),
    (SAMPLE, "SELECT name FROM apples"),
    (SAMPLE, "SELECT id, name FROM apples"),
    (SAMPLE, "SELECT name, color FROM apples"),
    (SAMPLE, "SELECT name FROM apples WHERE color = 'Red'"),
    (SAMPLE, "SELECT name FROM apples WHERE color = 'Light Green'"),
    (SAMPLE, "SELECT name FROM apples WHERE color = 'no such color'"),
    (SAMPLE, "SELECT description FROM oranges WHERE name = 'Navel'"),
    (SUPERHEROES, "SELECT COUNT(*) FROM superheroes"),
    (SUPERHEROES, "SELECT id, name FROM superheroes WHERE eye_color = 'Pink Eyes'"),
    (SUPERHEROES, "SELECT id, name FROM superheroes WHERE eye_color = 'Blue Eyes'"),
    (SUPERHEROES, "SELECT name FROM superheroes WHERE eye_color = 'no such color'"),
    # companies.db has idx_companies_country on country: these exercise the
    # index-scan path (walkIndex + findRowByRowid), not the full-scan
    # fallback the other cases above use.
    (COMPANIES, "SELECT COUNT(*) FROM companies"),
    (COMPANIES, "SELECT id, name FROM companies WHERE country = 'eritrea'"),
    (COMPANIES, "SELECT id, name FROM companies WHERE country = 'france'"),
    (COMPANIES, "SELECT id FROM companies WHERE country = 'no such country'"),
]


def oracle_rows(db, query):
    con = sqlite3.connect(db)
    cur = con.cursor()
    cur.execute(query)
    rows = cur.fetchall()
    con.close()
    lines = []
    for row in rows:
        lines.append("|".join("" if v is None else str(v) for v in row))
    return sorted(lines)


def engine_rows(db, query):
    out = subprocess.run(
        [PROG, db, query], capture_output=True, text=True, check=False
    )
    if out.returncode != 0:
        raise RuntimeError(
            "your_sqlite.sh exited %d: %s" % (out.returncode, out.stderr)
        )
    lines = out.stdout.rstrip("\n").split("\n")
    lines = [l for l in lines if l != ""]
    return sorted(lines)


def main():
    fails = 0
    for db, query in CASES:
        want = oracle_rows(db, query)
        try:
            got = engine_rows(db, query)
        except RuntimeError as e:
            print("FAIL %s | %s\n  %s" % (os.path.basename(db), query, e))
            fails += 1
            continue
        if want != got:
            print("FAIL %s | %s" % (os.path.basename(db), query))
            print("  want:", want[:5], "..." if len(want) > 5 else "")
            print("  got: ", got[:5], "..." if len(got) > 5 else "")
            fails += 1
        else:
            print("ok   %s | %s (%d rows)" % (os.path.basename(db), query, len(want)))

    print("\n%d/%d cases passed" % (len(CASES) - fails, len(CASES)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
