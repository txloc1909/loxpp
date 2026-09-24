#!/usr/bin/env python3
"""gen_fixtures.py — regenerates this example's test databases.

Both fixtures are committed binaries (small and fully deterministic given
this script), so running this is only needed after changing the schema or
row data below. Uses Python's stdlib sqlite3 module, which also serves as
the second oracle in diff_sqlite.py.

  sample.db      - two small single-page tables (base stages 1-7).
  superheroes.db - one table spanning many pages, forcing interior table
                   b-tree traversal (base stage 8).
"""
import os
import random
import sqlite3

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")


def make_sample_db():
    path = os.path.join(FIXTURES, "sample.db")
    if os.path.exists(path):
        os.remove(path)
    con = sqlite3.connect(path)
    cur = con.cursor()
    cur.execute(
        "CREATE TABLE apples (id integer primary key autoincrement, name text, color text)"
    )
    cur.execute(
        "CREATE TABLE oranges (id integer primary key autoincrement, name text, description text)"
    )
    cur.executemany(
        "INSERT INTO apples (name, color) VALUES (?, ?)",
        [
            ("Granny Smith", "Light Green"),
            ("Fuji", "Red"),
            ("Honeycrisp", "Blush Red"),
            ("Golden Delicious", "Yellow"),
        ],
    )
    cur.executemany(
        "INSERT INTO oranges (name, description) VALUES (?, ?)",
        [
            ("Mandarin", "Sweet and small"),
            ("Navel", "Seedless and juicy"),
            ("Blood Orange", "Deep red flesh"),
        ],
    )
    con.commit()
    con.close()
    print("wrote", path, os.path.getsize(path))


def make_superheroes_db():
    path = os.path.join(FIXTURES, "superheroes.db")
    if os.path.exists(path):
        os.remove(path)
    con = sqlite3.connect(path)
    cur = con.cursor()
    cur.execute(
        "CREATE TABLE superheroes (id integer primary key autoincrement, name text, eye_color text)"
    )
    random.seed(42)
    colors = ["Blue Eyes", "Brown Eyes", "Green Eyes", "Pink Eyes", "Red Eyes", "Hazel Eyes"]
    rows = []
    for i in range(3000):
        name = "Hero %d (New Earth)" % i
        color = "Pink Eyes" if i % 500 == 0 and i > 0 else random.choice(colors)
        rows.append((name, color))
    cur.executemany(
        "INSERT INTO superheroes (name, eye_color) VALUES (?, ?)", rows
    )
    con.commit()
    con.close()
    print("wrote", path, os.path.getsize(path))


if __name__ == "__main__":
    os.makedirs(FIXTURES, exist_ok=True)
    make_sample_db()
    make_superheroes_db()
