# Build your own SQLite (Codecrafters) in Lox++

A Lox++ implementation of the public
[Build your own SQLite](https://github.com/codecrafters-io/build-your-own-sqlite)
challenge's base stages (1-8: `.dbinfo`, `.tables`, `SELECT COUNT(*)`,
single/multi-column `SELECT`, `WHERE` equality, and multi-page table
b-tree traversal), in pure Lox++, plus a thin wrapper for the one thing
Lox++ cannot do on its own.

## Layout

| File | Role |
|---|---|
| `sqlite.lox` | Varint/record/b-tree parsing + a small SQL subset, pure Lox++ |
| `bytetable.bin` | Committed 256-byte file (`0x00`..`0xFF` in order) — see below |
| `your_sqlite.sh` | Challenge entrypoint: resolves `loxpp`, points `LOXPP_BYTE_TABLE` at `bytetable.bin` |
| `tests/gen_fixtures.py` | Regenerates the two test databases via Python's stdlib `sqlite3` |
| `tests/run_stages.sh` | Cases transcribed from the public `stage_descriptions/base-*.md` files |
| `tests/diff_sqlite.py` | Differential battery vs. Python's `sqlite3` as a second, independent oracle |

## Why `bytetable.bin` exists

Reading a SQLite file needs byte -> integer conversion for all 256 byte
values (varints, big-endian page/cell fields). Lox++ has no bitwise
operators and no `ord`/`chr`, and only six escapes exist in a string
literal (`\" \\ \n \t \r \0`), so most byte values are not writable as
Lox++ source at all.

`bytetable.bin` holds bytes `0x00..0xFF` in file order. `sqlite.lox` opens
it at startup and, for each index `i`, reads the one-byte slice at offset
`i` to build `ORD` (byte-string -> Number) and `CHR` (Number -> byte-string)
lookup tables — all 256 values, entirely in Lox++. The file is fully
deterministic, so it is committed rather than generated per run; the
wrapper only points `LOXPP_BYTE_TABLE` at it. This is the same trick
`build-your-own-grep/your_grep.sh` uses to hand `sqlite.lox`'s sibling
example the ESC byte.

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
- **Stage 9 (index-accelerated `WHERE`) is not implemented.** It needs a
  real b-tree descent for a sub-3-second point lookup on a ~1GB file, a
  materially different (and materially riskier) piece of work from a full
  scan; tracked separately rather than folded into this example.

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
