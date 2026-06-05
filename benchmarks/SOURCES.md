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
- **Pinned commit**: `d9a658f4fc40c90c0e09ba7fc5bb0e17c96f7b1f`
- **License**: MIT (see `LICENSE` in that repo)
- **Build target**: `make clox` with `CFLAGS="-O3 -march=native"`
- **Note**: This is the book's canonical bytecode interpreter. No lists/maps;
  only the 4 benchmarks that need no extended features can run on it.

---

## Updating a pinned commit

1. Edit the `ARG *_COMMIT=...` line in the relevant `Dockerfile.*`.
2. Update the commit hash in this file.
3. Rebuild the image: `podman build -f benchmarks/containers/Dockerfile.python -t bench-python benchmarks/`.
4. Smoke-test with `podman run --rm bench-python python3 /benchmarks/awfy/Fib.py`.
