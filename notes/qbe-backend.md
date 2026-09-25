# QBE backend: design proposal

**Status: proposal, not approved.** No issues or Project board exist yet. When
the plan is approved, each stage in "Staged plan" below becomes one GitHub
issue, per `AGENTS.md`'s "file it, don't list it". This note then stays as the
design record.

## Why

The CLR backend costs about as much to maintain as the JVM backend: a bigger
emitter (`clr_emitter.cpp`), a bigger runtime (`runtime/clr`), 10 CI
steps named for the CLR, and a list of CLR-only faults (#238, #316, #415–417, #426).
It is also dominated on both axes users care about
(`benchmark_report_2026-08-26.md`): the native VM starts about 25× faster,
and the JVM runs compute about 6× faster (geomean). Its best remaining
argument is that it is a third implementation for differential testing.

A QBE backend (`loxpp --target qbe prog.lox -o prog`) takes over that third
slot and adds something neither the native VM nor the managed backends
offer: a standalone native executable. The plan is to delete the CLR backend
when the QBE backend passes its parity gate, so the differential suite never
drops to two implementations.

QBE was chosen over LLVM because this design leaves little for LLVM's
optimizer to work on. The code is dynamically typed, most operations are
runtime calls, and every value lives in stack memory the GC must see. The
main speed-up comes from removing the interpreter's dispatch loop, and QBE
gets that too. QBE is small (under 10k lines of C, MIT licence), compiles
fast, and takes plain-text IL. That fits the existing emit-text, assemble,
link pattern (Jasmin, ilasm). If benchmarks later show the runtime's fast
paths need inlining across the runtime boundary, that is the point to
reconsider LLVM.

## What the third implementation can and cannot catch

The QBE backend reuses the native runtime (see "Runtime reuse"). A bug in
runtime semantics (`%g` number printing, map methods, `GET_PROPERTY` edge
cases) therefore shows up identically on native and QBE. A native-vs-QBE
diff cannot see it; only the JVM backend checks runtime semantics
independently.

What a native-vs-QBE diff does check independently: dispatch, control flow,
stack heights from the shared analysis layer, the call mechanism, and
exception/defer unwinding.

When native and JVM disagree, `spec/` decides which side is wrong, not a
majority vote across backends (`AGENTS.md`: spec > implementation). A third
backend helps find the bug faster; it does not decide which side is right.

## The central design choice: keep clox's fused stack

The JVM and CLR backends must un-fuse the clox stack into a local-variable
array and an operand stack (`bytecode-translation-problems.md`, P1), because
both VMs keep the two apart and verify the operand stack. QBE has no operand
stack and no verifier. So the QBE backend keeps clox's model:

> The stack cell at height *h* is the memory word `base + 8h` on the VM's
> own value stack.

Consequences:

- A call passes a pointer to the callee's window of the stack, exactly as
  `VM::call` does (`slots = stackTop - argc - 1`). Slot 0 stays the
  callee or receiver.
- `markRoots()` already scans `stack..stackTop`, so GC rooting needs no new
  mechanism.
- Upvalues point into the stack, as in the VM, so `captureUpvalue` and
  `closeUpvalues` work unchanged.

### Prototype evidence

A toy emitter in Python used this layout: NaN-boxed number fast paths
inline, a C++ helper on the slow path, a status check after every call, and
global lookup through a hash map. It hand-lowered `fib`'s bytecode and linked
against a minimal C++ stand-in runtime, using a 2021 QBE snapshot. Median of
3 runs of `fib(32)`, `release` preset:

| | time |
|---|---|
| native VM (`build/loxpp`) | 0.62 s |
| QBE prototype | 0.17 s (about 3.6×) |

Both print `2.17831e+06`. This is an optimistic bound: the prototype does
not push a real `CallFrame`, keep defer lists, record line offsets, or
handle exceptions. The JVM backend runs `fib` at 0.15× native (about 6.7×),
so expect QBE to beat native clearly but not to catch the JVM on call-heavy
code.

The emitted shape, for `n < 2` at heights 2→3→4→3 (constants are the
NaN-box masks from `src/value.h`):

```
  %t1 =l loadl %s1                        # GET_LOCAL 1
  storel %t1, %s2
  storel 4611686018427387904, %s3         # CONSTANT 2.0
  %t2 =l loadl %s2                        # LESS: both numbers?
  %t3 =l loadl %s3
  %t4 =l and %t2, 9222246136947933184
  %t6 =w cnel %t4, 9222246136947933184
  %t5 =l and %t3, 9222246136947933184
  %t7 =w cnel %t5, 9222246136947933184
  %t8 =w and %t6, %t7
  jnz %t8, @fast9, @slow9
@slow9
  %t10 =w call $op_less(l %rt, l %s4)     # shared helper, same as VM::run
  jnz %t10, @throw_route, @next
@fast9
  %t11 =d cast %t2
  %t12 =d cast %t3
  %t14 =w cltd %t11, %t12
  ...
```

## Reuse of the analysis passes

| Pass | QBE uses it? | Why |
|---|---|---|
| `chunk_decoder` | yes, as is | Decoding and the function tree. Its stable ids (`"0.2.1"`) also key startup (see "Startup"). |
| `cfg` | yes, as is | One QBE block label per leader. `HandlerEntry` gives the catch blocks. |
| `abstract_stack`: `height` and `reachable` | yes | Height is the only stack fact QBE needs: it fixes each opcode's slot addresses. Unreachable code (the trailing `NIL;RETURN`) is not emitted. |
| `abstract_stack`: `HandlerEntryContract` | yes | Catch-block entry height = checkpoint + 1 |
| `handler_depth` | yes | Tells each fallible op which catch block to branch to (see Q2) |
| `abstract_stack`: `localCount`, `PopKind`, invisible vars | not at first | They exist to separate locals from temporaries. QBE does not separate them. |
| `zero_depth_local`, `native_pops` | no | They repair a separated operand stack, which QBE does not have |
| `capture_analysis` | not at first | The VM's own upvalue mechanism works unchanged on the shared stack |

`capture_analysis` and the local/temporary split come back only for the
optional register-promotion stage (S8).

QBE is the first backend where a wrong height breaks output directly: a wrong
height means a wrong slot, which means a wrong result. So the backend tests
the height computation the JVM and CLR backends also rely on.

## Runtime reuse

### Layer 0: reuse as is

These do not depend on the VM:

- `value.h`, NaN-tagged layout only (see Q6)
- the object model and `memory_manager`, whose GC finds roots through a
  callback (`setMarkRootsCallback`)
- `table`, `container_objects`, `math`
- `src/stdlib/`: `NativeFn = Value(*)(int, Value*)` already reports errors
  through a flag (`StdlibContext::nativeError`), not through the VM

### Layer 1: move the opcode bodies out of `VM::run`

Opcode semantics today live inline in `vm.cpp`'s switch, with macros such as
`RAISE_ERROR` and `CATCHABLE_OR_RETURN`. Each moves into a helper that has
the same stack effect as the opcode:

```cpp
// Called by both VM::run and compiled code.
OpStatus op_get_index(Runtime& rt, Value* top);  // reads top[-2], top[-1]; writes top[-2]
```

`VM::run`'s case becomes one call plus `stackTop -= 1`. Number fast paths
stay inline in the interpreter, so it does not slow down; only slow paths
call helpers.

The same change applies to `bindMethod`, `captureUpvalue`/`closeUpvalues`,
call dispatch (closure, class, bound method, native, enum constructor),
`GET_PROPERTY`'s special cases for error, file and map, `INHERIT`, and the
iterators.

It also applies to the throw policy. `handleThrow`/`raiseThrowableError`
decide whether a fault is catchable and whether a handler has room for a
stack overflow. That decision becomes one shared function. The VM and QBE
then differ only in how control reaches the handler.

### Layer 2: split `VM` into `Runtime` and the interpreter loop

`Runtime` owns the value stack, frames, open upvalues, globals, defer lists,
handler state, `markRoots`, `runtimeError` with stack traces, and the stdlib
context. Compiled code pushes a small `CallFrame` for each call: the closure
plus a slot for the current bytecode offset. The existing stack-trace code
then works unchanged.

### Layer 3: a C interface (QBE only)

QBE only speaks the C calling convention, so compiled code calls
`extern "C"` wrappers such as `int rt_op_get_index(Runtime*, Value*)`. Every
wrapper is `noexcept` and turns any C++ exception into a fatal status. QBE
writes no unwinding tables (its output has no `.cfi` directives), so a C++
exception that crosses a QBE frame calls `std::terminate`.

### Startup

Compiled code needs the constant table, line tables, strings, classes and
enum-constructor objects at run time. The cheapest source for them is the
existing compiler: embed the program's source in the executable, compile it
at startup (a few milliseconds), and attach each compiled function's code to
its rebuilt `ObjFunction` by `decodeFunctionTree` id. `ObjFunction` gains
one field, a code pointer.

To fail fast if the two ever disagree, the emitter also writes each
function's id, arity and a hash of its chunk bytes, and startup checks them.
A real constant serializer can replace this later if needed.

### Pipeline

```
loxpp --target qbe prog.lox -o prog
  compile → decode → cfg / abstract_stack / handler_depth → emit prog.ssa
  → qbe → prog.s → cc prog.s libloxrt.a -lstdc++ -lm → prog
```

## Translation problems compared with JVM/CLR

P1–P8 refer to `bytecode-translation-problems.md`.

| Problem | JVM/CLR | QBE |
|---|---|---|
| P1 fused locals and temporaries | The core pass | Gone: heights only, no local/temporary split |
| P2 peek-not-pop ops (`SET_*`, `JUMP_IF_FALSE`) | `dup` sequences, fusions | Gone: values stay in memory, and a peek leaves the height unchanged |
| P3 verifier merge rules, uniform boxing | Everything is `Object` | Gone: no verifier. Labels still come from `cfg`. |
| P4 closure cells, `CLOSE_UPVALUE` (V1/V3) | Capture analysis plus runtime `isinst` checks | Gone: `CLOSE_UPVALUE` → `rt_close_upvalues(base + 8(h-1))`, the VM's own code. V1/V3 must pass with no capture analysis (stage Q4). |
| P5 calling convention (slot 0, argument arrays) | Unpack prologue, `Object[]` | Gone: pass the stack window pointer |
| P6 runtime polymorphism behind one opcode | New Java/C# runtime | Same problem, solved by the shared helpers |
| P7 aggregate construction | Spill to temporaries | Gone: `rt_build_list(base + 8(h-n), n)` |
| P8 `JUMP_TABLE` | `tableswitch` / `switch` | Harder: QBE block terminators are only `jmp`, `jnz` and `ret`, with no switch and no indirect jump. Lower to a compare chain or a binary search. |

### New problems specific to QBE

**Q1: missed GC roots.** The GC runs only inside allocating runtime calls.
An object held only in a QBE temporary across such a call is invisible to
the GC. Baseline rule: every Lox value lives in its stack slot, and
`rt->top = base + 8h` is written before every runtime call. Run the whole
corpus with `LOXPP_STRESS_GC=1`. This rule is also what makes register
promotion (S8) hard.

**Q2: exceptions without unwinding.** QBE has no exception support and emits
no unwinding tables. Every fallible call returns `OK`, `THROW` or `FATAL`.
On `THROW`, the code branches to the innermost catch block for that offset
(from `handler_depth`), sets the height to checkpoint + 1, and stores the
thrown value. With no handler in the function, it runs the frame's defers
and returns `THROW`. The shared throw policy (Layer 1) keeps catchability
identical to native.

Rejected alternative: `setjmp`/`longjmp` driven by the VM's dynamic handler
stack. It reuses more of `handleThrow`, but brings returns-twice register
hazards in QBE code and skips C++ destructors in runtime frames.

**Q3: catchable stack overflow and C stack depth.** Each Lox call uses a real
C frame. On entry, check a frame counter against `kFramesMax` (1024) plus
`kStackOverflowFrameReserve` (16), and the slot limit against `kStackMax`.
Overflow then happens at native's depth by construction. That is the fault
class behind #415–417 and #426 on the CLR. 1040 small frames fit easily in
an 8 MB C stack, but re-entry through defers and class constructors
(`rt_call` → compiled code → `rt_call`) needs a measured test.

**Q4: line numbers in error traces.** Native traces map `ip` to a line.
Compiled code stores the bytecode offset into the frame's offset slot before
each fallible op: one store of a constant. The chunk's line table exists
because startup rebuilds it (see "Startup").

**Q5: inline fast paths must match native exactly.** The inline `ADD`,
`LESS` and `EQUAL` paths duplicate what the VM and `valuesEqual` do. Inline
only the plain double-double cases (`cast`, `add`, `cltd`); send everything
else, including `MODULO`'s floor-division rule, to the shared helpers.
Numbers are never boxed, so a hardware NaN keeps bit 50 clear, the same as
native.

**Q6: value layout depends on a build option.** `LOXPP_NAN_TAGGING` can be
OFF (the `*-variant` presets use a 16-byte `std::variant`). `--target qbe`
requires NaN tagging; `libloxrt` enforces it with a `static_assert`.

**Q7: toolchain on the user's machine.** Output needs `qbe`, an assembler and
a linker, plus `libloxrt.a` built for musl for the static release. Ship
`libloxrt.a` in the release tarball. Later, QBE can be built into `loxpp`
itself (MIT licence, needs a `THIRD_PARTY.md` entry).

**Q8: very large functions.** The top-level script and the bootstrap
interpreter become very large QBE functions. Measure QBE's time and memory
on `bootstrap/loxpp_interpreter.lox` early.

## Staged plan

Each stage ends with a check that can be run, in the style of
`backend-implementation-dag.md`.

| Stage | Deliverable | Checkpoint |
|---|---|---|
| **S0** Toolchain | Pinned QBE release in a `dev-qbe` image stage; `tools/check_qbe_toolchain.sh` | A hand-written `.ssa` links against a C++ static library and runs in CI |
| **S1** Runtime split (native only) | Layers 1–2: `Runtime`, op helpers, shared throw policy | All ctest and examples unchanged; benchmarks show no slowdown beyond noise. Useful on its own: it shrinks `vm.cpp`. |
| **S2** `libloxrt.a` and startup | C interface, recompile-and-attach startup, `rt_call`, `CallFrame` push | A hand-written `.ssa` prints a string and calls a stdlib function |
| **S3** Straight-line code and jumps | Constants, arithmetic with inline fast paths, `PRINT`, locals, globals, `JUMP`/`JUMP_IF_FALSE`/`LOOP` | Probes 01–05 and 15 byte-identical to native |
| **S4** Calls and closures | `CALL`/`RETURN`/`CLOSURE`/upvalues/`CLOSE_UPVALUE`, frame limits | Probes 06 and 08, V1/V2/V3 with no capture analysis, `fib` |
| **S5** The rest of the language | Classes, `INVOKE`/super, lists and maps, iterators, `SLICE`/`IN`, enums, `JUMP_TABLE` compare chains | Probes 09–14 and 16 |
| **S6** Errors | Status protocol, try/catch, `THROW`, `defer`, fatal errors and exit codes, stack overflow | `check_fault_table.py`; stack-overflow probes; whole corpus under `LOXPP_STRESS_GC=1` |
| **S7** Parity gate | `diff_runtimes.py` runner, `check_qbe_probes.sh`, the bootstrap interpreter compiled with QBE | Full corpus byte-identical to native. The CLR backend is deleted after this stage. |
| **S8** Speed (optional) | Register promotion for slots that are never captured and do not escape, safe points, `GET_TAG;JUMP_TABLE` fusion | A new benchmark report |

## Open questions

1. Is the JVM enough of an independent check on runtime semantics, given
   that QBE shares the native runtime?
2. Startup: embed the source and recompile (proposed), or write a constant
   serializer?
3. Should S1 start now, independent of the decision to build QBE? It is a
   native-only refactor and removes most of the risk from later stages.
