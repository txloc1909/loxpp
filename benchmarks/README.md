# Lox++ backend benchmarks

A self-timing benchmark suite that runs the **same `.lox` source** on all three
Lox++ backends and reports the difference:

| backend | path | how a program runs |
|---|---|---|
| `native` | `build/loxpp` | bytecode compiled in-process, run by the C++ `VM` |
| `jvm`    | `tools/loxpp_jvm.sh` | `--target jvm` → Jasmin `.j` → `.class` → HotSpot |
| `clr`    | `tools/loxpp_clr.sh` | `--target clr` → `ilasm` → `.dll` → .NET 8 (RyuJIT) |

All three consume one source file, so any speed difference is the backend, not
the program.

## Layout

```
benchmarks/
  core/            algorithm source, no timing harness (18 AWFY + CLBG programs)
  generate.py      core/<name>.lox + a standard harness footer -> programs/
  programs/        the runnable, self-timing benchmarks
  programs/prof/   one-batch, no-warm-up variants for the profiler build
  run.py           run programs/ on each backend, emit the comparison table
  profile.py       run programs/prof/ on the LOXPP_PROFILE build, collect
                   opcode / function / GC stats
  results/         *.json and *.txt output (git-ignored except a committed baseline)
```

## The harness

`generate.py` appends this to every core program:

```
_batch(b, reps)   calls b.benchmark() `reps` times, returns the last checksum
                  warm-up: run _batch `warm` times, print nothing
                  measure: run _batch `meas` times, print one line each:
                      HARNESS <batch-index> <microseconds> <checksum>
```

`reps` is tuned per program (see `CONFIG` in `generate.py`) so one batch is
80–200 ms on the native backend — long enough to time, short enough that CPU
frequency drift over the run stays small. `warm` batches let the JVM and CLR
JITs reach steady state before the first measured batch.

`clock()` is **process CPU time** on the native and CLR backends and
**wall-clock** on the JVM backend (`System.nanoTime`, see
`runtime/jvm/src/lox/LoxRuntime.java`). For a single-threaded steady-state loop
on an unloaded machine the two agree closely; `run.py` also records external
wall-clock per process as an independent cross-check, and the JVM's background
JIT threads only affect the warm-up batches, not the measured ones.

## Running

```bash
# inside the dev-managed container, with build/, build-profile/,
# runtime/jvm/lox-rt.jar and runtime/clr/LoxRuntime.dll all built:
python3 benchmarks/generate.py
python3 benchmarks/run.py --procs 5 --json benchmarks/results/run.json
python3 benchmarks/profile.py --json benchmarks/results/profile.json
```

`run.py` pins every process to CPU 0 (`taskset -c 0`) and, for each
(program, backend) pair, launches it `--procs` times, keeps the fastest launch,
and reports the median of its measured batches plus a spread figure
`(max-min)/median` as a noise indicator.

## Prerequisites (dev-managed container)

```bash
cmake --preset release && cmake --build build --target loxpp
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Release -DLOXPP_PROFILE=ON \
      -DLOXPP_JVM_BACKEND=OFF -DLOXPP_CLR_BACKEND=OFF
cmake --build build-profile --target loxpp
tools/build_lox_rt.sh
tools/build_lox_rt_clr.sh
```

## Excluded from `core/`

`generate.py`'s `EXCLUDED` dict lists every `core/*.lox` file that has the
`class X { benchmark() }` shape but is not in `CONFIG`, and why. `fannkuch`
does not terminate on the native backend (a pre-existing issue). `cd`,
`deltablue`, `earley`, `havlak` (AWFY macro) and `for_in`, `instantiation`,
`string_interning`, `zoo` (Wren) have the right shape but are not yet tuned
into `CONFIG` — they are kept in `core/` for a later pass. `generate.py`
fails loudly if a
`core/*.lox` file with the harness shape is in neither `CONFIG` nor
`EXCLUDED`.
