# Build your own grep (Codecrafters) in Lox++

A complete implementation of the public
[Build your own grep](https://github.com/codecrafters-io/build-your-own-grep)
challenge (all 33 stages: base regex, backreferences, file search,
quantifiers, printing, `-o`, highlighting) in pure Lox++. No membership
is needed: every test here is transcribed from the public
`stage_descriptions/*.md` files, with system GNU `grep -E` as a second
oracle (modulo the documented dialect mapping below).

## Layout

| File | Role |
|---|---|
| `grep.lox` | Regex engine + CLI, pure Lox++ (no VM changes) |
| `your_grep.sh` | Challenge entrypoint: resolves `loxpp`, owns `-r` expansion, `--color=auto`, ESC bytes |
| `tests/run_stages.sh` | 63 transcribed stage cases (exact stdout + exit code) |
| `tests/diff_grep.sh` | 292-case differential vs GNU `grep -E` |

## Why the wrapper does more than exec

Two things are outside Lox++'s reach, so the shell owns them:

- **Recursive search (`-r`).** The stdlib has `exists`/`is_dir`/`stat`
  but no directory listing, so `your_grep.sh` expands directory
  operands via `find ... | sort` before `grep.lox` ever runs. Prefixes
  stay relative (`dir/sub/file.txt`), as the stage docs require.
- **Highlighting.** ESC (byte 27) is not writable as a Lox++ string
  literal (only `\" \\ \n \t \r \0` exist), and there is no `isatty`,
  so the wrapper resolves `--color=auto` with `[ -t 1 ]` and passes
  real ESC sequences via `LOXPP_COLOR_OPEN`/`LOXPP_COLOR_CLOSE`.
  Without them, `--color=always` degrades to plain output.

Binary resolution: `build/loxpp` beside the checkout first, then
`loxpp` on `PATH`.

## Regex dialect (Codecrafters, not POSIX)

`\d` is `[0-9]` and `\w` is `[A-Za-z0-9_]` — GNU `grep -E` instead
reads `\d` as literal `d`, so `diff_grep.sh` translates those two
classes for the GNU side only. Everything else (`+ ? * {n,m} . [] [^]
^ $ ( ) | \1..`) agrees with GNU byte-for-byte on the battery.

## Run

```sh
./examples/codecrafters/build-your-own-grep/your_grep.sh -E '\d'
./examples/codecrafters/build-your-own-grep/tests/run_stages.sh
./examples/codecrafters/build-your-own-grep/tests/diff_grep.sh
```
