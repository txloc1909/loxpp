# Global access on the QBE backend, 2026-10-18

The measurement for #592: a global access in compiled code must cost no more
than the interpreter's. The change gives every global name an index into a
per-Runtime slot array, so a read is an inline slot load instead of a call
that re-interns the name and then probes the globals table.

## Setup

| Item | Value |
|---|---|
| Commit | `b528dbd`, release preset, `loxpp` and `loxrt` targets |
| Toolchain | `loxpp-dev-qbe`: Ubuntu clang 18.1.3, QBE 1.3; `tools/check_qbe_toolchain.sh` passed |
| Machine | 12th Gen Intel(R) Core(TM) i5-1235U, 10 cores, 12 threads; Linux 7.2.9 (Arch host, Ubuntu container) |
| Power | AC on, governor `powersave`, energy preference `balance_performance` |
| Pinning | every run on core 2, a P-core, with `taskset` |
| Protocol | each program compiled once in the image; 5 warm-up runs discarded, 10 measured, median; `benchmarks/coroutine_cost_qbe.sh` for QBE, the same loop for native |

The "before" column is the same machine and the same protocol on the
unmodified build. The 2026-10-17 record's own numbers (148 and 38.5) come from
a different machine state and appear only in parentheses.

## The two benchmarks from #592

`coroutine_call_baseline.lox` keeps its state at top level, so its loop makes
six global accesses per iteration: five reads and one write. The reads are
`i` and `REPS` in the condition, `step` for the call, `i` for the argument,
and `REPS` inside `step`. The write is the assignment to `i`.

`coroutine_mode_floor.lox` moves the counter and the limit to local slots, but
it is not global-free: `run` reads the global `step` once per iteration. So
the floor makes one global access per iteration, and the difference between
the two programs covers five accesses. `step` itself names no global and does
not fetch the slot base; `run` does.

| Program | QBE before, ns | QBE after, ns | Native, ns |
|---|---|---|---|
| `coroutine_call_baseline` (6 accesses) | 125.6 (148) | **30.7** | 71.1 |
| `coroutine_mode_floor` (1 access) | 33.7 (38.5) | **16.4** | 53.4 |

Both programs improve, because the floor's one read uses the same new path.
The floor falls from 33.7 to 16.4 ns: that is the cost of one global read
becoming cheaper.

## A direct one-read comparison

The difference between the two issue benchmarks above is an end-to-end figure.
It also includes a call with two arguments instead of one, and the different
shape of a top-level loop against a function loop. To isolate one global read,
this pair runs the same loop and changes only the source of `g`:
`benchmarks/global_access_read.lox` reads a global; the identical loop in
`benchmarks/global_access_local.lox` reads a local slot.

| Program | QBE, ns per iteration | Native, ns per iteration |
|---|---|---|
| one read of a global `G` | 3.06 | 35.93 |
| the same loop with a local `g` | 2.37 | 32.38 |
| difference: one global read | **0.69** | **3.55** |

Repeated runs on this host give 0.3 to 0.7 ns for the compiled read and 3.5
to 5.5 ns for the interpreted read. The `powersave` governor changes the clock
between runs. In every run the compiled read is below the interpreted read.
That is the per-access result; the two benchmarks above are the end-to-end
gate. On them QBE is 2.3 times faster than native; before the change it was
1.8 times slower.

The same change also improves the top-level coroutine benchmark, because its
loop is global too: `coroutine_yield.lox` goes from 179 ns (2026-10-17) to
80.4 ns per round trip, against native's 120.4 ns.

## Profile before and after

`perf record -e cycles:u`, user-space samples, `REPS` raised to 5,000,000.
Share of the loop's cycles:

| Symbol | Before | After |
|---|---|---|
| `Table::findString` (intern probe) | 19.0 % | — |
| `__memcmp_avx2` (intern key compare) | 10.8 % | — |
| `MemoryManager::makeString` | 10.7 % | — |
| `rt_new_string` + `strlen` | 7.9 % | — |
| `rt_set_frame_offset` | 16.1 % | 9.5 % |
| globals table read (`Table::get`, `opGetGlobal`) | 10.1 % | — |
| `CoreHashMap::set` (the write) | 4.2 % | 15.7 % |
| `constantName` (name from the constant pool) | — | 7.8 % |
| compiled code (`lox_fn_0`, `lox_fn_0_0`) | 7.9 % | 23.4 % |

Re-interning was about half of the loop. It is gone. The remaining global
cost is the write, which still takes the runtime path to keep `m_globals`
authoritative; the issue's target does not need it changed.

## What this does not measure

- The whole benchmark corpus. The two programs in #592 are the gate.
- A faster bound write. The profile now puts the globals-table write and the
  name fetch at the top. A write cannot throw when the name is already bound,
  so it could store the slot inline and call a small non-throwing map sync.
  The deliverable is met without it, and the plan named it as a fallback only.
