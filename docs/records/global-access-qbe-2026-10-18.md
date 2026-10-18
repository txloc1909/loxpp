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
| Machine | Intel Core 7 240H, 10 cores, 16 threads; Linux 7.2.9 (Arch host, Ubuntu container) |
| Power | AC on, governor `powersave`, energy preference `balance_performance` |
| Pinning | every run on core 2, a P-core, with `taskset` |
| Protocol | each program compiled once in the image; 5 warm-up runs discarded, 10 measured, median; `benchmarks/coroutine_cost_qbe.sh` for QBE, the same loop for native |

The "before" column is the same machine and the same protocol on the
unmodified build. The 2026-10-17 record's own numbers (148 and 38.5) come from
a different machine state and appear only in parentheses.

## The two benchmarks from #592

`coroutine_call_baseline.lox` keeps its state at top level, so its loop makes
five global accesses per iteration: four reads and one write.
`coroutine_mode_floor.lox` runs the same loop with local slots.

| Program | QBE before, ns | QBE after, ns | Native, ns |
|---|---|---|---|
| `coroutine_call_baseline` (globals) | 125.6 (148) | **30.7** | 71.1 |
| `coroutine_mode_floor` (locals) | 33.7 (38.5) | **16.4** | 53.4 |

The local-state floor does not pay for the change: its hot functions name no
global, so their prologue does not fetch the slot base.

Per global access, from the difference between a program's global loop and
its local floor over five accesses per iteration:

| | QBE | Native |
|---|---|---|
| Before | 18.4 ns | 3.5 ns |
| After | 2.9 ns | 3.5 ns |

The compiled access is now below the interpreter's, which is the deliverable.
End to end, QBE is 2.3 times faster than native on the global loop; before, it
was 1.8 times slower.

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
