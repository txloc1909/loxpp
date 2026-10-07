# Lox++ backend benchmarks

A self-timing benchmark suite that runs the **same `.lox` source** on both
Lox++ backends and reports the difference:

| backend | path | how a program runs |
|---|---|---|
| `native` | `build/loxpp` | bytecode compiled in-process, run by the C++ `VM` |
| `jvm`    | `tools/loxpp_jvm.sh` | `--target jvm` → Jasmin `.j` → `.class` → HotSpot |

Both consume one source file, so any speed difference is the backend, not
the program.

## Layout

```
benchmarks/
  core/            algorithm source, no timing harness (18 AWFY + CLBG programs)
  generate.py      core/<name>.lox + a standard harness footer -> programs/
  programs/        the runnable, self-timing benchmarks
  programs/prof/   one-batch, no-warm-up variants for the profiler build
  latency/         GC pause benchmarks, run by `run.py --latency`
  run.py           run programs/ on each backend, emit the comparison table
  gc_compare.py    native and JVM GC reports in one format (see "Where the GC
                   time goes")
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
frequency drift over the run stays small. `warm` batches let the JVM
JIT reach steady state before the first measured batch.

`clock()` is **process CPU time** on the native backend and
**wall-clock** on the JVM backend (`System.nanoTime`, see
`runtime/jvm/src/lox/LoxRuntime.java`). For a single-threaded steady-state loop
on an unloaded machine the two agree closely; `run.py` also records external
wall-clock per process as an independent cross-check, and the JVM's background
JIT threads only affect the warm-up batches, not the measured ones.

## Running

```bash
# inside the dev-managed container, with build/, build-profile/, and
# runtime/jvm/lox-rt.jar all built:
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
      -DLOXPP_JVM_BACKEND=OFF
cmake --build build-profile --target loxpp
tools/build_lox_rt.sh
```

## GC pause latency

`latency/gc_latency.lox` measures the pause a collection adds to a program.
It keeps a graph of 50000 nodes live, runs 10000 small units of work (each
allocates 100 short-lived objects and replaces one live node, which then
becomes garbage), and times each unit with `clock()`. A unit that includes a
collection takes much longer than the others. The program sorts the unit times and prints one line:

```
LATENCY <units> <live-nodes> <p50-us> <p99-us> <max-us> <checksum>
```

```bash
python3 benchmarks/run.py --latency [--backends native jvm qbe] [--procs 3]
```

`run.py --latency` runs every program in `latency/` and prints p50, p99, and
max per backend. It reports the median of each statistic over the launches.
All launches of one backend must print the same checksum.
The `clock` column has the same meaning as in the throughput table: process
CPU time on native and QBE, wall-clock on JVM. Checksums must agree between
backends.

## Excluded from `core/`

`generate.py`'s `EXCLUDED` dict lists every `core/*.lox` file that has the
`class X { benchmark() }` shape but is not in `CONFIG`, and why. `cd`,
`deltablue`, `earley`, `havlak` (AWFY macro) and `for_in`, `instantiation`,
`string_interning`, `zoo` (Wren) have the right shape but are not yet tuned
into `CONFIG` — they are kept in `core/` for a later pass. `generate.py`
fails loudly if a `core/*.lox` file with the harness shape is in neither
`CONFIG` nor `EXCLUDED`.

## Where the GC time goes

Two tools answer two different questions. Use both.

### GC trace: phases and pauses

`LOXPP_GC_TRACE=<file>` makes the native VM write one line per collection, with
a timestamp at each phase boundary, on any build. It adds no clock read and no
counter to the mutator, so it does not skew the run. Unset, it costs one
branch per collection and nothing on the allocation path.

```bash
cmake --preset release && cmake --build build --target loxpp
python3 benchmarks/generate.py
LOXPP_GC_TRACE=/tmp/storage.trace build/loxpp benchmarks/programs/storage.lox
python3 tools/gc_report.py /tmp/storage.trace      # --json for scripts
```

The report gives GC overhead (% of wall time), p50/p99/max pause, the share of
each phase, minimum mutator utilisation (MMU) for 1, 10, 100 and 1000 ms
windows, allocation rate, and mark and sweep rates. The trace format and the
metric definitions are in the header of `tools/gc_report.py`.

Use the `release` preset. A `debug` build prints an instruction trace to stdout
and runs much slower, so the shares are wrong.

The trace cannot see the cost of `new`, `delete`, `malloc`, and `free`. They
are inside the phases, but the trace does not name them. Use `perf` for that.

### The same report for the JVM backend

One command prints the native report and the JVM report, in the same format,
for the GC-bound programs (`storage`, `binary_trees`, `towers`, `json`):

```bash
python3 benchmarks/gc_compare.py                 # add --latency for latency/*.lox
python3 benchmarks/gc_compare.py --only storage gc_latency --json /tmp/gc.json
python3 benchmarks/gc_compare.py --jvm-opts=-XX:+UseG1GC
```

The native run uses `LOXPP_GC_TRACE`. The JVM run uses
`LOXPP_JVM_OPTS="-Xlog:gc*:file=...:uptimenanos"` (`tools/jvm_run.sh` adds
`LOXPP_JVM_OPTS` to the `java` command line), and `tools/jvm_gc_log.py`
converts the log. Both then go through `gc_report.summarise_gcs`, so overhead,
percentiles, MMU, and allocation rate come from one piece of code. To read a
log you made yourself: `python3 tools/jvm_gc_log.py gc.log`. Use the `release`
build, as above. Each process is pinned to one cpu, as in `run.py`.

The two reports do not measure the same thing. Read the JVM report with these
differences in mind:

- **Pauses are stop-the-world pauses.** Each `Pause ...` line in the JVM log is
  one collection (G1 young, remark, cleanup, and full; Serial and Parallel
  young and full). A pause ends at the log timestamp and began its duration
  earlier. G1 concurrent cycles run on other threads while the program runs,
  so they are not in gc time, pauses, overhead, or MMU. A `concurrent` line
  gives their count and total duration. The JVM can also stop threads for
  other reasons (safepoints for the JIT, for example); those are not GC pauses
  and are not here.
- **No phase data.** The log has no mark, trace, or sweep timestamps and no
  object counts, so `phase share`, `mark rate`, and `sweep rate` are `n/a`.
- **The allocation rate is approximate.** The log rounds heap sizes to whole
  MB or KB. The rate also includes what the JIT and class loader allocate.
- **Wall time differs.** JVM wall is JVM uptime at the last log line. It
  includes JVM start and JIT warm-up but not the Jasmin assembly before the
  JVM starts. Native wall starts when the memory manager is created.
- **The collector depends on the cpu set.** With one cpu the JVM picks the
  Serial collector. The pinning in `run.py` and `gc_compare.py` gives one cpu,
  so use `--jvm-opts=-XX:+UseG1GC` or `-XX:+UseParallelGC` to compare with
  another collector. Only Serial, Parallel, and G1 are supported: ZGC and
  Shenandoah log pauses without heap sizes, and the parser rejects their logs
  with an error. The collector name is in the report heading.
- **A collection is not the same unit.** The native VM collects the whole heap
  at each pause. A JVM young pause collects only the young generation, so
  pause counts and sizes do not compare one to one. Compare overhead, MMU,
  and the tail of the pause distribution.

`tools/check_jvm_gc_log.py` (ctest `JvmGcLog`) tests the parser on the logs in
`tools/testdata/`.

### perf: costs the trace cannot see

Build a release binary that keeps frame pointers, so `perf record -g` can walk
the stack without DWARF:

```bash
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer -g"
cmake --build build-perf --target loxpp

# Counters for the whole run: cycles, instructions, cache and branch misses.
perf stat -e cycles,instructions,cache-misses,branch-misses \
    build-perf/loxpp benchmarks/programs/storage.lox

# Where the cycles go, with call stacks.
perf record -g -o /tmp/perf.data build-perf/loxpp benchmarks/programs/storage.lox
perf report -i /tmp/perf.data --no-children --stdio | head -60
# Callers of one symbol, for example the allocator:
perf report -i /tmp/perf.data --no-children --stdio -S _int_free -g caller
```

Look for `malloc`, `free`, `operator new`, `operator delete`,
`MemoryManager::sweep`, and `std::vector::erase` in the `--no-children` list.
Their total, set against the trace's sweep and trace phase shares, shows how
much of a pause is allocator work and how much is the collector's own loops.
`perf` can need `sysctl kernel.perf_event_paranoid=1` (or root) on the host. In
a container, add `--cap-add=SYS_ADMIN` or `--privileged`.
