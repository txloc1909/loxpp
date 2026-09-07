# Lox++ Backend Benchmark Report — native VM vs JVM vs CLR

**Date:** 2026-08-26
**Repo:** `main` @ `1decf3d`; benchmark infra on `feat/backend-benchmarks`
**Machine:** 12th Gen Intel Core i5-1235U (2 P-cores + 8 E-cores), Linux 7.1.9
**Container:** `loxpp-dev-env-managed` (Ubuntu 24.04)
**Toolchains:** clang 18.1.3 `-O3 -DNDEBUG` · OpenJDK 21.0.11 (G1GC, tiered) ·
.NET 8.0.130 (RyuJIT) · Jasmin · `ilasm` 8.0

---

## 1. Purpose

Three back ends run the **same `.lox` source**:

| back end | how a program runs |
|---|---|
| `native` | `build/loxpp` — bytecode compiled in process, run by the C++ `VM`, a clox-style stack machine |
| `jvm` | `loxpp --target jvm` → Jasmin → `.class` → HotSpot C1/C2 JIT |
| `clr` | `loxpp --target clr` → `ilasm` → `.dll` → .NET RyuJIT |

The input is identical, so any speed difference is the execution engine. This
report measures that difference, shows where the native VM spends its time,
and lists what to change.

**Headline result.** Steady-state, warm:

* The **JVM back end is 2.2×–17× faster than the native VM on every one of the
  17 benchmarks** — geomean **6.1×** (0.16× the native time).
* The **CLR back end is roughly tied with the native VM** — geomean 0.91× —
  but with a wide spread: 3× faster on string/array batch work, and **1.2×–1.9×
  slower than the native interpreter** on recursion and polymorphic dispatch
  (`fib`, `richards`, `towers`, `nbody`).

The native VM's problem is not a single hot spot. It is an unoptimised
interpreter: it never stops interpreting, it has no inline caching, and its
garbage collector is stop-the-world non-generational. §4 breaks this down;
§5 is the action list.

`richards` was silently dead during this run (§2 caveats) and is now mostly
fixed; both geomeans above and the `richards` figures wherever they appear
should be treated as provisional until re-measured under the fix.

---

## 2. Method

* **Suite:** 17 programs from *Are We Fast Yet?* (AWFY) and the Computer
  Language Benchmarks Game (CLBG). Source: `benchmarks/core/`.
* **Harness** (`benchmarks/generate.py`): each program calls `benchmark()` in
  batches — **25 un-timed warm-up batches**, then **12 timed batches**, each
  printing `HARNESS <i> <microseconds> <checksum>`. Batch size is tuned so one
  native batch is 80–250 ms.
* **Runner** (`benchmarks/run.py`): every (program, back end) pair runs in 4
  processes pinned to CPU 0 (`taskset -c 0`). The runner drops the first third
  of the 12 timed batches, then takes the **median** of the rest from the
  fastest process. `spread` = `(max−min)/median` over those batches.
* **Checksums** from all three back ends were compared — every row matched.
* `clock()` is **process CPU time** on native and CLR, **wall-clock**
  (`System.nanoTime`) on the JVM. For a single-thread steady-state loop on an
  idle machine the two agree; the runner also records external wall-clock.

### Caveats

* The i5-1235U scales frequency aggressively; absolute numbers drift ~±10 %
  run to run. With CPU pinning, native `spread` is 1–2 % except on
  allocation-heavy programs (`storage` 29 %, `binary_trees` 19 % — GC pauses).
  Ratios are stable to ~±15 %.
* 25 warm-up batches fully warm HotSpot and RyuJIT: measured batch 0 already
  equals steady state (`benchmarks/results/run_full.json`). The one-time cost
  the steady-state numbers exclude is in §3.3.
* `nbody` (12 000 steps), `mandelbrot` (size 120), `fib` (n = 28),
  `binary_trees` (depth 10) use reduced sizes so a batch fits the time window.
* `fannkuch` is not in this report's data — at the time of this run it did
  not terminate on the native VM. That turned out to be a carry-propagation
  bug in the benchmark's Lox++ port, not a VM limitation, and is now fixed
  (`benchmarks/core/fannkuch.lox`); it is in `generate.py`'s `CONFIG` as of
  a later commit than the one this report is dated against, so its numbers
  are not part of §3's tables.
* **`richards`'s row (§3.1, §3.2) is stale.** At the time of this run its
  scheduler was silently dead — a task-readiness bug made it do almost no
  work (`queuePacketCount=0` of an intended `23246`) — so its 236.2/81.8/
  457.1 ms and the 0.35/1.93 ratios measure a near-no-op, not the real
  richards workload. A later commit fixes most of this (`queuePacketCount`
  now reaches ~97% of target, deterministic and cross-backend consistent,
  though the benchmark's own self-check still does not fully pass): richards
  now runs its actual workload and a single native call takes ~2s, not the
  236.2 ms per batch this report measured — nearly a 10× change this
  report's numbers do not reflect. Re-running §3 for richards under the fix is a follow-up; §4's
  qualitative claim that richards is OO/dispatch-heavy and belongs in the
  2×–3× regime likely still holds (the bug did not change what richards
  does, only whether it ran to completion), but the specific ratios and
  the geomean that includes them should not be trusted until re-measured.

---

## 3. Results

### 3.1 Steady-state median per batch (ms, lower = faster)

`jvm/nat` and `clr/nat` below 1.00 mean that back end is faster than the
native VM.

| benchmark | native | jvm | clr | jvm/nat | clr/nat | spread n/j/c |
|---|--:|--:|--:|--:|--:|---|
| binary_trees | 129.0 | 20.4 | 90.1 | **0.16** | 0.70 | 19/8/14 % |
| bounce | 93.2 | 29.6 | 112.9 | **0.32** | 1.21 | 1/6/0 % |
| fasta | 68.0 | 12.9 | 73.1 | **0.19** | 1.07 | 1/14/1 % |
| fib | 91.1 | 13.6 | 132.8 | **0.15** | 1.46 | 1/7/1 % |
| json | 98.8 | 27.4 | 83.0 | **0.28** | 0.84 | 0/27/4 % |
| k_nucleotide | 141.2 | 11.4 | 52.5 | **0.08** | 0.37 | 1/72/2 % |
| list | 81.6 | 16.6 | 55.2 | **0.20** | 0.68 | 1/13/2 % |
| mandelbrot | 96.5 | 5.8 | 74.9 | **0.06** | 0.78 | 0/22/1 % |
| nbody | 153.3 | 71.1 | 186.4 | **0.46** | 1.22 | 1/3/11 % |
| permute | 118.0 | 26.4 | 116.5 | **0.22** | 0.99 | 1/6/3 % |
| queens | 125.7 | 24.5 | 124.6 | **0.20** | 0.99 | 1/7/1 % |
| reverse_complement | 113.8 | 7.2 | 37.6 | **0.06** | 0.33 | 1/15/1 % |
| richards | 236.2 | 81.8 | 457.1 | **0.35** | 1.93 | 1/3/1 % |
| sieve | 97.9 | 8.2 | 74.6 | **0.08** | 0.76 | 2/19/2 % |
| spectral_norm | 117.1 | 14.6 | 132.3 | **0.12** | 1.13 | 0/10/1 % |
| storage | 123.8 | 13.5 | 143.1 | **0.11** | 1.16 | 29/39/6 % |
| towers | 132.4 | 32.6 | 162.9 | **0.25** | 1.23 | 1/7/2 % |
| **geomean** | | | | **0.165** | **0.91** | |

### 3.2 Reading the ratios

Sorting by `jvm/nat` reveals two regimes:

**Where the JVM wins biggest — 8×–17× (`jvm/nat` ≤ 0.16)**
`mandelbrot` 0.06, `reverse_complement` 0.06, `sieve` 0.08, `k_nucleotide`
0.08, `storage` 0.11, `spectral_norm` 0.12, `fib` 0.15, `binary_trees` 0.16.
The hot path is a tight loop over numbers or bytes (`mandelbrot`,
`spectral_norm`, `sieve`), a byte scan (`reverse_complement`,
`k_nucleotide`), or allocate-and-discard (`storage`, `binary_trees`). HotSpot
C2 compiles it to machine code, escape analysis removes the `Double` boxing,
and a generational nursery makes the allocation nearly free. The native VM
interprets every iteration and stop-the-world-collects. **This gap is pure
interpreter + GC overhead** — the part a non-JIT VM can shrink but not erase.

**Where the JVM wins least — 2×–3× (`jvm/nat` ≥ 0.25)**
`nbody` 0.46, `bounce` 0.32, `richards` 0.35, `towers` 0.25, `json` 0.28.
The hot path is `this.field`, `obj.method(...)`, and object allocation. Every
engine — native, JVM, CLR — resolves fields and methods through a hash table
keyed by name, and the JIT has little left to compile away. This is the
**structural floor** a non-JIT VM shares with a JIT one, and it is where the
native VM's own improvements pay the most (§5 items 3–5, 9).

CLR sits in the middle: RyuJIT is single-tier with weaker escape analysis, so
it keeps more boxing. It is **slower than the native interpreter** on `fib`,
`richards`, `towers`, `nbody`, `bounce`, `fasta`, `spectral_norm`, `storage` —
its per-call `object[]` argument array and boxed return value cost more than
the native VM's direct stack when the JIT cannot inline the call.

### 3.3 One-time cost per invocation

Wall-clock for a one-line script (`print "hi";`), median of 4 runs, pinned:

| back end | fixed cost | what it is |
|---|--:|---|
| native | **4 ms** | scan + compile to bytecode + run |
| clr | **109 ms** | emit `.il` + `ilasm` + CLR start + JIT |
| jvm | **305 ms** | emit `.j` + Jasmin + JVM start + class-load + C1 |

Plus JIT warm-up: the managed back ends need ~1–3 s of steady work before they
reach the §3.1 numbers. **For short-lived CLI scripts the native VM wins by
two orders of magnitude** and is the right default; the managed back ends are
for long-running compute.

---

## 4. Analysis — where the native VM's time goes

### 4.1 Opcode-dispatch profile

`benchmarks/profile.py` runs each program on a `-DLOXPP_PROFILE=ON` build and
records exact dispatch counts (`benchmarks/results/profile.json`). Aggregate
share across all 17 programs (≈ 200 M dispatches):

| opcode | share | | opcode | share |
|---|--:|---|---|--:|
| `GET_LOCAL` | 30.6 % | | `LESS` | 2.3 % |
| `POP` | 13.7 % | | `LOOP` | 2.2 % |
| `CONSTANT` | 8.1 % | | `RETURN` | 2.2 % |
| `JUMP_IF_FALSE` | 6.3 % | | `GET_INDEX` | 1.9 % |
| `GET_PROPERTY` | 5.1 % | | `EQUAL` | 1.8 % |
| `ADD` | 5.0 % | | `GET_GLOBAL` | 1.7 % |
| `SET_LOCAL` | 3.1 % | | `SUBTRACT` | 1.7 % |
| `MULTIPLY` | 2.8 % | | `SET_PROPERTY` | 1.2 % |
| `INVOKE` | 2.4 % | | others | ~7 % |

Three groups matter:

1. **Stack shuffling — `GET_LOCAL` + `SET_LOCAL` + `POP` + `CONSTANT` ≈ 55 % of
   all dispatches.** These do almost no work; their cost is *dispatch +
   operand fetch*. A JIT turns locals into registers and these vanish. This is
   the case for register-caching `ip`, direct-threaded dispatch, and
   superinstructions (§5 items 1, 6, 7).
2. **Hashed dictionary lookups — `GET_PROPERTY` + `SET_PROPERTY` + `INVOKE` +
   `GET_GLOBAL` ≈ 10 %.** Every one is a hash probe by interned-string
   pointer, with no caching. `nbody` is 14 % `GET_PROPERTY`, `bounce` 12 %,
   `permute` 9 %. This is the case for inline caches and slot-based fields
   (§5 items 5, 9, 10).
3. **Untyped arithmetic — `ADD`+`MULTIPLY`+`LESS`+`SUBTRACT`+`EQUAL`+
   `GREATER`+`DIVIDE`+`MODULO` ≈ 16 %.** Each runs the `BINARY_OP` macro:
   two `is<Number>` tag checks, two `pop()`, one `push()`, no fast path
   (§5 item 8).

### 4.2 Garbage collection

One profiled batch, mark-sweep collector (`benchmarks/results/profile.json`):

| program | GC runs | GC ms | longest pause | ≈ share of the native batch |
|---|--:|--:|--:|--:|
| storage | 12 | 65 | 14.2 ms | ~50 % |
| binary_trees | 57 | 50 | 2.7 ms | ~40 % |
| towers | **541** | 26 | 17.8 ms | ~20 % |
| json | 47 | 2.5 | 1.1 ms | ~3 % |
| sieve | 18 | 0.4 | 0.1 ms | <1 % |
| bounce, list, nbody, fib, mandelbrot, … | 0 | 0 | — | 0 % |

The collector is stop-the-world, non-generational, non-incremental mark-sweep;
`m_nextGC` starts at 1 MiB and doubles (`src/memory_manager.*`). `towers`
allocating a `TowersDisk` per move triggers **541 collections in one batch** —
the live set stays small, so the threshold never grows and the VM re-scans the
whole heap each time. On `storage` and `binary_trees` GC is roughly
40–50 % of native runtime. The JVM and CLR generational collectors make this
disappear (`storage` jvm/nat = 0.11, `binary_trees` 0.16).

### 4.3 The native VM is an unoptimised clox-style interpreter

All of this is on `main`, in `src/vm.cpp` / `src/table.*` / `src/compiler.cpp`,
and is visible in the profile:

**Instruction fetch and dispatch**
* `run()` uses a plain `switch (toOpcode(instruction))` — no computed-goto /
  direct threading.
* `readByte()` is `*m_frames[m_frameCount - 1].ip++`; `readShort()` and
  `readConstant()` too. They re-index the frame array on **every operand
  byte** instead of using the `frame` pointer `run()` already holds or a
  register-cached `ip`. `readConstant()` also re-chases
  `frame->closure->function->chunk`.
* `push()` branches on stack overflow every push; the loop head tests
  `m_stackOverflow` every iteration.

**Variable / property / method access — no inline caching anywhere**
* `GET_GLOBAL` / `SET_GLOBAL`: a hash probe per access. `fib`'s own name is
  looked up on every one of its ~1.03 M calls per batch — `GET_GLOBAL` is
  exactly 8.3 % of `fib`'s opcodes, one per `CALL`.
* `GET_PROPERTY`: `isFile()` then `isMap()` checks, then a hash probe on
  `instance->fields`. `this.x` is a dictionary lookup.
* `INVOKE` on an instance: a `fields` probe **and** a `methods` probe per
  call. `INVOKE` on a list: a chain of `std::string` comparisons
  (`name->chars == "append"`, then `"pop"` …) per call.
* `Op::CALL` on a class: `m_mm.findString("init")` — a string-table probe on
  **every** instantiation.

**Arithmetic**
* `BINARY_OP`: two tag checks + two `pop()` + one `push()`, no typed fast
  path, no specialisation when both operands are compile-time-known numeric.
* `GET_INDEX` / `SET_INDEX`: `n != std::floor(n)` — a floating-point
  round-trip — to check integer-ness on every subscript.

**Compiler**
* Single-pass, no constant folding, no peephole, no superinstructions (only
  `JUMP_TABLE` for dense `match`). `1 + 2` → `CONSTANT; CONSTANT; ADD`.
* Locals and globals have no short forms or slot indices.

### 4.4 Why the JVM wins and the CLR doesn't

Both back ends box every number (`java.lang.Double` / boxed `object`) and put
locals in `Object`/`object` slots; `a - b` is a static call to a `LoxOps`
helper. HotSpot's two-tier JIT recompiles hot methods with C2, whose escape
analysis deletes the boxing inside loops → near-native machine code, hence the
8×–17× on arithmetic benchmarks. RyuJIT compiles once, keeps the boxing, and
adds a per-call `object[]` args array — so the CLR only wins where the batch
is dominated by a few large loops over arrays/strings and loses where calls
dominate. Method dispatch on both goes through
`LoxOps.invoke(receiver, String, Object[])` — array alloc + two hash probes,
megamorphic, not inlined — which is why even the JVM only gets 2×–3× on
`bounce` / `richards` / `nbody`.

### 4.5 Bottleneck, in one sentence

> The native VM never stops interpreting. On arithmetic-in-a-loop code a warm
> JIT beats it 8×–17× and the entire gap is interpretation overhead —
> dispatch, operand fetch, untyped `BINARY_OP`, no register allocation. On
> object-and-dispatch code the gap narrows to 2×–3× because every engine
> resolves `this.field` and `obj.method()` through a hash table, and there the
> native VM's own extra cost is its non-generational GC.

---

## 5. Actionable items for the native VM

Ordered by (expected gain ÷ effort). "Gain" is an estimate from the dispatch
profile and the clox / Wren-VM / CPython-3.11 literature; **measure each with
`benchmarks/run.py --backends native` before/after** (the harness exists for
this).

| # | item | change | effort | est. gain | hits |
|--:|---|---|:--:|---|---|
| 1 | **Register-cache `ip`** | keep `ip` (+ constant-pool base) in a `run()` local; write back to `frame->ip` only at `CALL`/`RETURN`/error; `readByte/Short/Constant` read the local | S | 5–15 % broad | all |
| 2 | **`-march=native -flto`**, try PGO | CMake release preset, native binary only | XS | 5–15 % on float code | mandelbrot, nbody, spectral_norm |
| 3 | **Cache `"init"`** + `ObjClass::initializer` | drop the `findString("init")` probe on every `Foo()` | XS | 3–8 % on alloc-heavy | bounce, storage, list, towers |
| 4 | **Interned-pointer list/map method dispatch** | replace the `name->chars == "append"` `std::string` chain in `INVOKE`/`GET_PROPERTY` with an interned-`ObjString*` compare or a method-id switch | S | 5–15 % on list-heavy | bounce, sieve, storage, queens |
| 5 | **Monomorphic inline caches** for `GET_GLOBAL`, `GET_PROPERTY`, `SET_PROPERTY`, `INVOKE` | 1-entry per-site cache `(last ObjString*/ObjClass*, slot)`, hash-probe fallback on miss | M | 1.3–1.8× on OO code | nbody, bounce, richards, json, permute |
| 6 | **Direct-threaded dispatch** | computed-goto (`&&label`) table behind a compiler macro, `switch` kept as fallback | M | 10–25 % on interpreter-bound loops | fib, mandelbrot, sieve, queens |
| 7 | **Superinstructions / peephole** | fuse `GET_LOCAL GET_LOCAL <binop>`, `GET_LOCAL CONSTANT <binop>`, `<cmp> JUMP_IF_FALSE`, `GET_LOCAL RETURN` | M | 10–20 % on numeric loops | mandelbrot, nbody, spectral_norm, fib |
| 8 | **Typed arithmetic / index fast paths** | check both `Number` tags once; drop re-validation in `ADD`/`SUB`/`LESS`; replace `n != std::floor(n)` subscript check with a direct `int` cast + range test | S–M | 5–15 % | mandelbrot, nbody, sieve, queens |
| 9 | **Slot-based instance fields (shapes / hidden classes)** | each class assigns its fields fixed indices; instances hold a flat `Value[]`; `this.x` → shape-check + array load; compounds with #5 | L | 1.5–2× on OO code | nbody, bounce, richards, json |
| 10 | **Compile-time global slots** | resolve global names to array indices; `GET_GLOBAL` → array load | M | 5–10 % on call-heavy | fib, every function call |
| 11 | **Generational / incremental GC** (or bump-pointer nursery) + a heap-size floor | shorten and spread collection pauses; stop `towers`-style thrash | L | removes 24–58 % of runtime on the GC-bound benchmarks | storage, binary_trees, towers, json |
| 12 | **Drop per-`push` overflow branch** | check stack depth at `CALL` only | XS | 1–3 % | all |
| 13 | **profiler.h opcode-name table** lags the `Op` enum (`BUILD_MAP`, `SLICE`, `GET_ITER`, `JUMP_TABLE`, … report as `UNKNOWN`) | extend the `opcodeName` switch | XS | profiler accuracy only (≤ 0.1 % of dispatches) | — |

Effort: XS < ½ day · S ≈ ½–1 day · M ≈ 2–5 days · L ≈ 1–2 weeks.

### Reading the list by which back end you are behind

Items 1–11 are one ranked list, but the two back ends expose different parts
of the same bottleneck. This groups them by which back end a benchmark loses
to, ordered by effort within each group.

**Behind CLR — `binary_trees`, `json`, `k_nucleotide`, `list`, `mandelbrot`,
`reverse_complement`, `sieve` (`clr/nat` 0.33–0.84, §3.1).** Each hot path is
one large loop over an array, a string, or a number (§3.2). RyuJIT does not
remove boxing the way HotSpot does — it wins these by compiling a simple loop
straight to machine code while the native VM still dispatches one opcode at a
time.

* XS — #2 `-march=native -flto` (+PGO); #12 drop the push-overflow branch
* S — #1 register-cache `ip`; #4 interned-pointer list dispatch
* S–M — #8 typed arithmetic / index fast paths
* M — #6 direct-threaded dispatch; #7 superinstructions/peephole
* L — #11 generational GC (`binary_trees` only — 57 collections per batch,
  ~40 % of its runtime, §4.2)

Items 1, 2, 6, 7, and 8 alone should flip most of this group to a native win:
the gap here is dispatch overhead on a simple loop, not a structural cost.

**Behind JVM — all 17 benchmarks** (`jvm/nat` is below 1.00 everywhere,
geomean 0.165). §3.2 splits this into two regimes that call for different
items.

*8×–17× regime* — `mandelbrot`, `reverse_complement`, `sieve`,
`k_nucleotide`, `storage`, `spectral_norm`, `fib`, `binary_trees`: the same
loop-bound bottleneck as the CLR group above, only wider, because C2's escape
analysis and generational nursery go further than RyuJIT's. Same order — #2,
#12, then #1, #3, then #8, then #6, #7 — plus #11 for `storage` and
`binary_trees`, where GC is 40–50 % of native runtime (§4.2). A non-JIT
interpreter has a real ceiling in this regime (see "What not to do", below).

*2×–3× regime* — `nbody`, `bounce`, `richards`, `towers`, `json`, `queens`,
`permute`, `list`, `fasta`: the **structural floor**. Every engine resolves
`this.field` / `obj.method()` through a hash probe by name, so the JVM's own
JIT has little to compile away here either (§4.4) — this is where the native
VM's own fix has the most relative room:

* XS — #3 cache `"init"`
* S — #4 interned-pointer dispatch
* M — #5 monomorphic inline caches (est. 1.3–1.8×); #10 compile-time global
  slots
* L — #9 slot-based instance fields / shapes (est. 1.5–2×, compounds with #5)

Items 5 and 9 matter most here — the "Structural OO pass" below — since they
are the only ones that touch the hash-probe cost the JVM's JIT cannot remove
either.

### Suggested sequence

1. **Cheap-wins pass:** items 1, 2, 3, 12, 13 — independent, near-free,
   re-baseline after.
2. **Localised:** items 4 and 8.
3. **Dispatch pass:** items 6 + 7 together (a threaded interpreter is the
   right place to add fused handlers).
4. **Structural OO pass:** items 5 + 9 designed as one piece — this is what
   moves the native VM off the "clox baseline" and closes most of the
   remaining `bounce`/`richards`/`nbody` gap.
5. **GC:** item 11, last — it only becomes the dominant cost once 1–9 land.

### Dependencies on the expressiveness roadmap

Some items touch the same code that a planned language feature will rewrite.
The feature list and its order are in `expressiveness-roadmap.md` (roadmap
items R1–R7) and `concurrency-model-next-steps.md`. Where a §5 item and a
roadmap item change the same handler, do them in a deliberate order — the
table says which item waits for, or must leave room for, which.

| §5 item | roadmap item it depends on | why |
|---|---|---|
| **1** register-cache `ip` **(DONE, #170)** | **R5** coroutines/generators; **R3** non-local control flow | Item 1 shipped with the seam these features need: `FrameSync` (flush the cached `ip` into `frame->ip` on construct, reload `frame`/`ip`/`chunk` from the top of `m_frames` via `FrameSync::loadTop` on destruct) brackets every nested call, and the `RAISE_ERROR` macro flushes before `runtimeError()`. So every non-executing `CallFrame` already carries a correct `ip` at any depth — R3's unwind pops frames then calls `loadTop`; R5's `yield` flushes, saves the frame/stack slice, and reloads on resume. Both must call that same flush+reload at their new suspend/throw sites; nothing about #170 blocks them, and it removed the "retrofit the cache later" risk. |
| **4**, **8** list/map dispatch + typed index fast paths | **R4** extensible protocols / operator overloading | R4 makes `[]`, `for-in`, `==`, `len`, `()`, and map-key hashing dispatch to user methods (`__index__`/`__iter__`/`__eq__`/`__hash__`/`__call__`) — the same `INVOKE`/`GET_PROPERTY`/`GET_INDEX`/`SET_INDEX` handlers items 4 and 8 rewrite, including the `n != std::floor(n)` check item 8 drops. Shape the fast path as "built-in type → fast path, else protocol dispatch" so the two compose. R4 also subsumes the map-key hash question in `language-extension.md`. |
| **5**, **9** inline caches + slot-based fields / shapes | **R1** reflection (**already merged, #167** — `src/stdlib/reflect_api.cpp`, all three back ends) | Reflection shipped first, so items 5 and 9 must accommodate it, not the reverse. `fields`/`getField`/`hasField`/`setField`/`callMethod` all read `ObjInstance::fields` (a `Table`) directly. Item 9 deletes that member for a shape pointer + flat `Value[]`, so all five natives must be rewritten to go through the shape **in the same PR**. `setField(inst, name, v)` sets a field under a runtime-computed name — same layout change as `SET_PROPERTY` on an unknown field — so item 9's shapes must allow transitions on field addition (they already must, for `this.x = v` from any method) and item 5's `GET_PROPERTY`/`SET_PROPERTY` cache key must carry a shape/layout identity, not a bare `ObjClass*`. Item 5's `INVOKE`/method caches are safe: `ObjClass::methods` is immutable after class definition and Lox++ has no runtime method addition. `fields()`/`methods()` order is already spec-unspecified (`spec/05-stdlib.md`: "Sort the result if a deterministic order is needed"; probe `40_reflection.lox` keeps them single-element), so item 9 changing native's enumeration to slot order is legal and does not break the differential suite. `callMethod` is natives-only today (no re-entrant VM call path, roadmap R1 note); when that path lands, user-method `callMethod` should reuse the same shape + cache path as `INVOKE`. |
| **6** direct-threaded dispatch | **R5** coroutines; **R7** true parallelism | The threaded loop is where suspend/`yield` points and GC safepoints belong (loop back-edges, call sites — see `concurrency_in_bytecode_vms.md` §5). Same state-flush seam as item 1. |
| **10** compile-time global slots | **R1** reflection (`eval` sub-feature, deferred) | `eval` is deferred, so no conflict now, but item 10 raises the future cost of runtime-defined globals (compile-time slot assignment cannot see them). Separately, the JVM/CLR back ends keep a name→value global map on purpose, to match the native VM's global-lookup cost for benchmark parity (`backend-implementation-dag.md`, "Globals → A2"); item 10 breaks that parity premise — methodology only, the differential suite compares stdout. |
| **11** generational / incremental GC | **R7** true parallelism + `concurrency-model-next-steps.md` | `concurrency-model-next-steps.md` item 7: do not start the GC redesign before the concurrency-model go/no-go. The model choice sets the GC shape — per-actor heaps (BEAM-style) vs. one shared concurrent collector. An incremental collector with write barriers is groundwork a concurrent GC needs anyway, but the nursery / promotion design must be chosen knowing which. Item 11 must not be built in isolation from that decision. Also flags the profiler coupling in `profiler-concurrency-notes.md` (`m_profilerScopes[]` follows whatever owns `m_frames[]`). |
| **12** drop per-`push` overflow branch | **R5** coroutines | Minor: growable / segmented coroutine stacks change stack-overflow semantics and the single `CALL`-site depth check. |

**No roadmap dependency:** items 2 (`-march=native -flto`), 3 (cache `"init"`),
7 (superinstructions / peephole), 13 (profiler opcode-name table). Item 7's
fused handlers and the nested-pattern match compilation in
`dynamic-functional-programming.md` evolve match bytecode on separate tracks
that do not collide.

### What not to do

Do not write a native-code JIT for the native VM — it is a separate project,
and items 1–11 recover most of the realistic gain for a fraction of the cost.
The managed back ends already cover JIT-throughput workloads; §3.2 says point
compute-bound users at `--target jvm` today, and keep the native VM as the
default for scripts and interactive use, where its 4 ms start-up is the
feature.

---

## 6. Reproducing

```bash
# dev-managed container, worktree mounted at /workspace
cmake --preset release && cmake --build build --target loxpp
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Release -DLOXPP_PROFILE=ON \
      -DLOXPP_JVM_BACKEND=OFF -DLOXPP_CLR_BACKEND=OFF
cmake --build build-profile --target loxpp
tools/build_lox_rt.sh && tools/build_lox_rt_clr.sh

python3 benchmarks/generate.py
python3 benchmarks/run.py     --procs 4 --json benchmarks/results/run.json
python3 benchmarks/profile.py            --json benchmarks/results/profile.json
python3 benchmarks/report_tables.py benchmarks/results/run.json \
                                    benchmarks/results/profile.json
```

Harness details: `benchmarks/README.md`.
Raw data for this report: `benchmarks/results/baseline_2026-08-26.json` and
`benchmarks/results/profile.json`.
