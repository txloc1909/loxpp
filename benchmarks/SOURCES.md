# Benchmark Sources

Third-party benchmark implementations are baked into container images at the
commits listed below. No submodules — the Docker build fetches them.

---

## Are We Fast Yet (AWFY)

- **Repo**: https://github.com/smarr/are-we-fast-yet
- **Pinned commit**: `5e9fa8e657a4b9c4b3dfb4e9a2ac31b5ad3b0ef3`
- **License**: MIT (see `LICENSE` in that repo)
- **Languages used**: Python (`benchmarks/Python/`), Lua (`benchmarks/Lua/`),
  JavaScript (`benchmarks/JavaScript/`)
- **Benchmarks**: Bounce, CD, DeltaBlue, Earley, Fib, Havlak, Json, List,
  Mandelbrot, NBody, Permute, Queens, Richards, Sieve, Storage, Towers

## Wren

- **Repo**: https://github.com/wren-lang/wren
- **Pinned commit**: `4a8e084b8a46e2f7b98c5b9ee1b44e91a8eef9c7`
- **License**: MIT (see `LICENSE` in that repo)
- **Benchmark files**: `test/benchmark/*.wren`
- **Benchmarks**: zoo, string_interning, for_in, instantiation (and AWFY overlap)

## Crafting Interpreters (clox)

- **Repo**: https://github.com/munificent/craftinginterpreters
- **Pinned commit**: `4a840f70f69c6ddd17cfef4f6964f8e1bcd8c3d4`
- **License**: MIT (see `LICENSE` in that repo)
- **Build target**: `make clox` with `CFLAGS="-O3 -march=native"`
- **Note**: This is the book's canonical bytecode interpreter. No lists/maps;
  only the 4 benchmarks that need no extended features can run on it.

---

## Cross-language microbenchmarks (apples-to-apples)

`benchmarks/crosslang.py` is the only **cross-comparable** harness: every
language runs the *identical* algorithm at the *identical* size and self-times
the compute region (startup excluded). Programs live in `benchmarks/clox/`
(shared by lox++ and clox) and `benchmarks/{python,lua,js,wren}/`. Images:
`loxpp-dev-env`, `bench-clox` (Dockerfile.clox), `bench-lua` (Dockerfile.lua),
`bench-wren` (Dockerfile.wren), and the official `python:3.12-slim` /
`node:22-alpine`.

The `wren` CLI is built from **wren-lang/wren-cli** (pinned commit
`18553636618a4d33f10af9b5ab92da6431784a8c`, `make config=release_64bit`); it
bundles the wren VM and libuv as submodules. The `wren-lang/wren` repo builds
only a test runner, not a CLI.

## AWFY suite (runner.py) — python/lua/js

`runner.py` runs the AWFY suite for python/lua/js via the SOM harness
(`harness.<ext> <Name> <iters> <inner>`) from `smarr/are-we-fast-yet`
@`74306fec151070fd07157cefeacf19e7e0bcdc89`, baked into `bench-{python,lua,js}`.
The lox++ ports under `benchmarks/lox/` are faithful translations of AWFY's
`benchmark()`; `manifest.toml` carries a per-benchmark `inner` so the harness
does the identical work (`inner` is the problem size for the size-parameterized
benchmarks — mandelbrot 750, nbody 250000, cd 2, deltablue 100 — and `1` for the
rest, whose `benchmark()` already encapsulates one unit of work).

**Apples-to-apple across lox++/python/lua/js:** bounce, cd, deltablue, list,
mandelbrot, nbody, permute, queens, richards, sieve, storage, towers.

Lox++-only: `fib`/`earley` (no AWFY python/lua/js port); `havlak` (its recursive
`doDFS` exceeds lox++'s 256-frame call stack at meaningful sizes, and its port
uses a non-AWFY signature — needs an iterative rewrite to run/compare).

---

## Updating a pinned commit

1. Edit the `ARG *_COMMIT=...` line in the relevant `Dockerfile.*`.
2. Update the commit hash in this file.
3. Rebuild the image: `podman build -f benchmarks/containers/Dockerfile.python -t bench-python benchmarks/`.
4. Smoke-test with `podman run --rm bench-python python3 /benchmarks/awfy/Fib.py`.
