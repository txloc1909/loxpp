# Build your own SQLite (Codecrafters) in Lox++

A Lox++ implementation of the public
[Build your own SQLite](https://github.com/codecrafters-io/build-your-own-sqlite)
challenge's base stages (1-9: `.dbinfo`, `.tables`, `SELECT COUNT(*)`,
single/multi-column `SELECT`, `WHERE` equality, multi-page table b-tree
traversal, and an index-accelerated `WHERE`), in pure Lox++, with a thin
entrypoint wrapper that resolves the `loxpp` binary.

## Layout

| File | Role |
|---|---|
| `sqlite.lox` | Varint/record/b-tree parsing + a small SQL subset, pure Lox++ |
| `your_sqlite.sh` | Challenge entrypoint: resolves `loxpp` and runs `sqlite.lox` |
| `tests/gen_fixtures.py` | Regenerates the three test databases via Python's stdlib `sqlite3` |
| `tests/run_stages.sh` | Cases transcribed from the public `stage_descriptions/base-*.md` files |
| `tests/diff_sqlite.py` | Differential battery vs. Python's `sqlite3` as a second, independent oracle |

## Index-accelerated `WHERE` (stage 9)

`WHERE col = 'literal'` uses a single-column index on `col`, when the
schema has one, instead of a full table scan:

1. `walkIndex` descends the index b-tree (page types 2/10), pruning by
   comparing the target value against each interior cell's key with the
   built-in String order. It keeps scanning right past an
   equal key rather than stopping, because a non-unique index breaks ties
   by rowid, so a run of equal keys can span more than one cell or page.
2. Each matching rowid is then fetched with `findRowByRowid`, a point
   lookup that descends the table b-tree via each interior cell's routing
   key, instead of `collectRows`'s full scan.

Both are O(tree depth + matches) page visits rather than O(table size).
On the committed `companies.db` fixture (50,000 rows, 6 matching an
indexed column), that is the difference between roughly 10ms and several
seconds for the same query answered by a full scan of an unindexed
column — see `tests/run_stages.sh`'s `index-where-speed` case, which
asserts the fast path stays fast as a regression guard.

Only a single-column `CREATE INDEX ... ON t (col)` is recognised; a
composite index, or a `WHERE` on a column no index covers, falls back to
the full-scan path from the earlier stages.

## Known gaps

- **Cell payload overflow pages are not implemented.** A row whose record
  is too large for one page has its payload silently truncated by Lox++'s
  clamped string slicing rather than followed onto an overflow page. The
  challenge's own stage-3 notes say overflow handling is out of scope, and
  no base-stage sample database triggers it.
- **Serial type 7 (IEEE-754 float column) decodes as `nil`.** No base-stage
  query touches a float column, and reconstructing a 64-bit float from
  bytes without bitwise operators was not worth the complexity here.
- **8-byte signed integers (serial type 6) lose precision near the top of
  the 64-bit range**, because Lox++ Numbers are IEEE-754 doubles (53-bit
  mantissa) with no separate integer type. Every rowid and every integer
  value in these fixtures stays far below that range.
- **The whole database file is read into memory up front.** Lox++'s
  `File` type has no seek/pread — only sequential `read()`/`readline()` —
  so random access to a page requires the file to already be in memory as
  one String; there is no way to fetch only the pages a b-tree descent
  visits. Measured on a 91MB synthetic fixture, `open()` + `read()` +
  parsing the file header takes about 0.25s (roughly 365MB/s); at the
  challenge's ~1GB scale that alone is in the 2.5-3s range — close enough
  to the stage's 3-second budget that the one-time read, not the index
  b-tree walk, is the part most likely to matter for margin on the real
  (non-public) 1GB `companies.db`.

## Run

```sh
python3 examples/codecrafters/build-your-own-sqlite/tests/gen_fixtures.py
./examples/codecrafters/build-your-own-sqlite/your_sqlite.sh \
    examples/codecrafters/build-your-own-sqlite/tests/fixtures/sample.db .dbinfo
./examples/codecrafters/build-your-own-sqlite/your_sqlite.sh \
    examples/codecrafters/build-your-own-sqlite/tests/fixtures/sample.db \
    "SELECT name, color FROM apples WHERE color = 'Yellow'"
./examples/codecrafters/build-your-own-sqlite/tests/run_stages.sh
./examples/codecrafters/build-your-own-sqlite/tests/diff_sqlite.py
```
