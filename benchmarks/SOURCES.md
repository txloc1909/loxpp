# Benchmark sources

The programs in `core/` are Lox++ ports of two well-known cross-language
suites. They were first written for the `feat/benchmarks` branch (PR #86) and
moved here unchanged except for stripping each file's ad-hoc timing footer —
`generate.py` now appends a single shared harness instead.

## Are We Fast Yet? (AWFY)

* Repo: https://github.com/smarr/are-we-fast-yet
* License: MIT (Copyright (c) 2015–2016 Stefan Marr)
* Ported programs: `bounce`, `list`, `mandelbrot`, `nbody`, `permute`,
  `queens`, `sieve`, `storage`, `towers`, `json`, `richards`, plus `fib`
  (AWFY micro), and the unused macro benchmarks `cd`, `deltablue`, `earley`,
  `havlak`.
* Each `core/*.lox` keeps the upstream copyright line.

## Computer Language Benchmarks Game (CLBG)

* Site: https://benchmarksgame-team.pages.debian.net/benchmarksgame/
* In the generated suite: `fasta`, `k_nucleotide`, `reverse_complement`,
  `spectral_norm`, `binary_trees`.
* Ported but excluded from the suite: `fannkuch` — `fannkuch(7)` does not
  terminate on the native VM (`benchmarks/generate.py`'s `EXCLUDED`,
  `notes/benchmark_report_2026-06-08.md`).
* These are re-implementations of the published algorithms, not copies of a
  specific licensed entry.

## Wren benchmark suite

* Repo: https://github.com/wren-lang/wren (`test/benchmark/`)
* Ported but not currently in the suite: `for_in`, `instantiation`,
  `string_interning`, `zoo` — each has the `class X { benchmark() }` shape
  the harness expects, but none is yet tuned into `generate.py`'s `CONFIG`
  (see that file's `EXCLUDED`).

## Not ported

The reference C VM (`clox`) and the other-language AWFY entries
(Python / Lua / Node) that `benchmark_report_2026-06-08.md` used are **not**
part of this suite: its question is native VM vs JVM vs CLR back end, all
three fed the same Lox++ source.
