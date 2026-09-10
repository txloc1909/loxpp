# isocline — vendored line editor

isocline is a lightweight, single-file C library that provides interactive line
editing for the REPL. It replaces GNU Readline and carries no GPL code. The
`loxpp` interpreter uses it for history and keyword completion without any
external terminal library dependency.

## Version

| Field | Value |
|---|---|
| Release tag | `v1.1.0` |
| Annotated tag commit | `b4f1796627e75fc765cc26ec8091c683ef4d7417` |
| Commit | `d55a58139badbe83d61c5d89954fa5bddcabe6d7` |
| Upstream project | https://github.com/daanx/isocline |
| Downloaded from | https://github.com/daanx/isocline/archive/refs/tags/v1.1.0.tar.gz |
| Archive SHA-256 | `1e5f0efa2b719c3e1d292f501e5329e141a039deefc801099f8bbb9a50255531` |
| Fetch date | 2026-09-10 |

## Files vendored

Copied verbatim from the archive:

- `src/` — all 20 implementation files and 11 internal headers: `attr.{c,h}
  bbcode.{c,h} bbcode_colors.c common.{c,h} completers.c completions.{c,h}
  editline.c editline_completion.c editline_help.c editline_history.c env.h
  highlight.{c,h} history.{c,h} isocline.c stringbuf.{c,h} term.{c,h}
  term_color.c tty.{c,h} tty_esc.c undo.{c,h} wcwidth.c`
- `include/isocline.h` — the sole public header
- `readme.md` — upstream documentation
- `LICENSE` — upstream MIT license

## Directories dropped

These upstream directories are not needed for the REPL and are excluded:

- `test/` — test suite
- `doc/` — additional documentation
- `ide/` — IDE integrations
- `haskell/` — Haskell bindings
- `util/` — utilities
- `.github/` — GitHub workflows

Build files dropped: `CMakeLists.txt`, `isocline.cabal`, `isocline.pc.in`,
`package.yaml`, `stack.yaml`, `.gitignore`.

## Build and use

The single translation unit is `src/isocline.c`. Compile with `-Iinclude`:

```bash
clang -std=c99 -c -Iinclude src/isocline.c -o isocline.o
```

No extra defines are needed. The library uses only libc (`<termios.h>`, `<linux/kd.h>`).

## Rules for this copy

- The entire `src/` and `include/` trees are copied **verbatim** from the
  upstream release archive. Do not hand-edit any file. To update, download
  the new release tag, verify the archive SHA-256, extract, and replace these
  directories, record the new tag and SHA-256 here, and commit.
- `third_party/` is exempt from `clang-format`, `clang-tidy`, and the
  `-Werror` warning sweep. The local `third_party/.clang-format` and
  `third_party/.clang-tidy` files turn both tools off for this tree, and the
  CI lint and static-analysis jobs already scope their file lists to `src/`
  and `test/`.

## Per-file notice: `src/wcwidth.c`

One file, `src/wcwidth.c`, is Markus Kuhn's `wcwidth` implementation, vendored
by isocline upstream. It carries a separate copyright notice and permission:

```
Markus Kuhn -- 2007-05-26

Permission to use, copy, modify, and distribute this software for any
purpose and without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.
```

This notice is MIT-compatible and the implementation is public domain. The
file carries no "Daan Leijen" attribution — this is expected and correct.

## License

MIT License. Full text in `LICENSE`, copied verbatim from upstream:

```
MIT License

Copyright (c) 2021 Daan Leijen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
