# Benchmark Sources

Third-party benchmark implementations are baked into container images at the
commits listed below. No submodules — the Docker build fetches them.

---

## Are We Fast Yet (AWFY)

- **Repo**: https://github.com/smarr/are-we-fast-yet
- **Pinned commit**: `74306fec151070fd07157cefeacf19e7e0bcdc89`
- **License**: MIT (see `LICENSE` in that repo)
- **Languages used**: Python (`benchmarks/Python/`), Lua (`benchmarks/Lua/`),
  JavaScript (`benchmarks/JavaScript/`)
- **Benchmarks**: Bounce, CD, DeltaBlue, Havlak, Json, List, Mandelbrot, NBody,
  Permute, Queens, Richards, Sieve, Storage, Towers
- **Note**: filenames are lowercase in the current AWFY master (`bounce.py`, not
  `Bounce.py`). `fib` and `earley` have no Python/Lua/JS equivalents in AWFY.

## Wren

- **Repo**: https://github.com/wren-lang/wren
- **Pinned commit**: `99d2f0b8fc2686134b32b18166e037639f7e9f2c`
- **License**: MIT (see `LICENSE` in that repo)
- **Benchmark files**: `test/benchmark/*.wren`
- **Status**: The standalone `wren` CLI was removed from the upstream repo; the
  Wren suite currently runs Lox++ only. The container builds `wren_test` but it
  is not used for benchmarking. See Dockerfile.wren for details.

## Crafting Interpreters (clox)

- **Repo**: https://github.com/munificent/craftinginterpreters
- **Pinned ref**: `master` branch (commit-only SHAs are not served by GitHub archives)
- **License**: MIT (see `LICENSE` in that repo)
- **Build target**: `make clox` with `CFLAGS="-O3 -march=native"`
- **Note**: This is the book's canonical bytecode interpreter. No lists/maps/modulo;
  only the benchmarks that use no extended features can run on it (fib, mandelbrot,
  towers). queens uses `%` (modulo) and fails on clox.

---

## Updating a pinned commit

1. Confirm the archive is accessible:
   ```bash
   curl -fsSL "https://github.com/smarr/are-we-fast-yet/archive/<SHA>.tar.gz" -o /dev/null -w "%{http_code}"
   ```
   GitHub only serves archives for commits reachable from a branch or tag.
2. Edit the `ARG *_COMMIT=...` line in the relevant `Dockerfile.*`.
3. Update the commit hash in this file.
4. Rebuild the image from the repo root (build context must be the repo root):
   ```bash
   podman build -f benchmarks/containers/Dockerfile.python -t bench-python .
   ```
5. Smoke-test:
   ```bash
   podman run --rm bench-python python3 /benchmarks/run_bench.py /benchmarks/awfy/Python/bounce.py
   ```
