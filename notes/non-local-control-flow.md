# Non-local control flow: design record

## Why

`notes/expressiveness-roadmap.md` item 3 names non-local control flow as the
next genuine capability gap (item 1 reflection and item 2 OS/world-access
basics are already done/partially done). Today nothing in Lox++ can unwind:
runtime faults (index OOB, calling `nil`, type errors) are unconditionally
fatal, a deep call chain can't escape without threading a `Result` through
every return, and there is no cleanup-on-unwind — which is also why
`ObjFile` in `src/container_objects.h` carries a TODO warning that its
destructor must never be relied on to close file handles, since GC timing is
non-deterministic.

This is a design record, not a backlog list — see
`notes/missions/2026-09-non-local-control-flow/` (or its eventual successor)
and the tracking issue for the actionable node breakdown, per `AGENTS.md`'s
"file it, don't list it."

## Current architecture (verified in-repo)

**Native VM** (`src/vm.h`/`vm.cpp`) — a flat `Value stack[2048]` plus a fixed
`CallFrame m_frames[256]` array; each frame's `slots` is just a pointer into
the shared stack (`vm.h:26-30`). `run()` returns `InterpretResult`; every
fault site calls `runtimeError()` (`vm.cpp:1248-1272`), which prints a frame
trace and then unconditionally calls `resetStack()` (zeroes `stackTop` and
`m_frameCount` — full teardown, no partial unwind). No C++
exceptions/setjmp are used in the interpreter loop itself. Native stdlib
functions report faults via a thread-local sentinel (`StdlibContext`,
`src/stdlib/stdlib_context.h`) checked after each native call. GC root
marking (`markRoots`, `vm.cpp:1274-1294`) already walks the full flat stack
and frame array — the same traversal a handler-stack unwind needs. No
existing opcode pushes/pops an exception handler.

**JVM/CLR backends** (`runtime/jvm/src/lox/LoxRuntime.java`,
`runtime/clr/src/LoxRuntime.cs`) — every Lox++ runtime fault, on both
backends, already raises a uniform `LoxError` (`extends RuntimeException` /
`: Exception`), not a raw host exception. Nothing catches it today — it
propagates to the process boundary (`runtime/clr/host/LoxHost.cs`
re-throws it explicitly so `tools/check_clr_probes.sh` can grep stderr for
the literal type name). `src/backend/jvm_emitter.cpp` and `clr_emitter.cpp`
have no `try`/`catch` region scaffolding today; the only existing
exception-producing emission is `MATCH_ERROR`, which does a hard `athrow`/
`throw`, never caught. `notes/jvm-emission-contract.md`'s scope-exit rule
(every exit path gets its own copy of local cleanup, e.g. `CLOSE_UPVALUE`
duplicated at each `break`/`continue`/`return`/match-arm site) is the
pattern any new exit path (`throw`-unwind) must extend.

**Spec** (`spec/04-semantics.md`) uses numbered-steps prose keyed to grammar,
with an existing two-tier Error Taxonomy (static vs runtime error, the
latter defined as "must halt execution and report the error" —
`try`/`catch` changes this for runtime errors specifically). `break`/
`continue` (`04-semantics.md:423-449`) are the closest existing non-local-exit
precedent and the "Binding Identity" section already enumerates which
constructs end scopes on exit — a throw-unwind path needs to join that list.
No runtime error/exception value exists among the 10 runtime types in
`spec/03-types.md` today. `notes/dynamic-functional-programming.md`'s
Result-by-convention (`enum Result { Ok(v) Err(msg) }` + exhaustive `match`)
already solves error **values**; this design is scoped to error **control
flow** only, and deliberately reuses `match` rather than inventing parallel
typed-catch machinery.

**Cross-backend precedent** — item 1's `callMethod` re-entrancy cap
(roadmap lines 76-92) is the closest existing asymmetry case: native's
`run()` loop can't call back into itself, so a capability trivial on
JVM/CLR was capped on all three backends to avoid observable divergence
under `tools/diff_runtimes.py`. Try/catch does **not** hit *that specific*
problem — unwinding to a handler is a bounded, non-reentrant operation the
native `run()` loop can do internally (pop frames, don't recurse), so no
capability cap is needed. That said, it is not "all three backends trivially
implement this independently" either — see the correction below.

## Prior art surveyed

- **Lua**: `pcall`/`error` via `setjmp`/`longjmp` (or host exceptions) over
  a linked list of handler records; Lua 5.4's `<close>` attribute runs
  cleanup on scope exit including error unwind — direct precedent for
  `defer`-as-resource-cleanup over `finally`-as-block-attachment.
- **CPython**: modern (3.11+) zero-cost exception tables map bytecode
  ranges to handler offsets; `finally` is compiled by **duplicating**
  cleanup code at each static exit — this is exactly the pattern Lox++'s
  compiler already uses for scope-exit `POP`/`CLOSE_UPVALUE` (per
  `jvm-emission-contract.md`), so `defer` can reuse it for the
  compile-time-enumerable exits (return, fall-through) and only needs new
  VM logic for the dynamic case (a throw unwinding through the frame).
- **Java/JVM, CLR**: table-driven exception regions
  (`start_pc,end_pc,handler_pc,catch_type` / `.try{}catch{}`) are a native,
  first-class bytecode/IL feature.
- **Go**: `panic`/`recover`/`defer` — an untyped, per-call-frame LIFO defer
  list that always runs on any exit (normal or panicking). This is the
  direct model for the `defer`-only design below and for why Go itself
  omits a separate `finally`.
- **Crafting Interpreters** (clox — the direct architectural ancestor of
  Lox++'s native VM: flat stack + `CallFrame` array): the book's own
  `runtimeError`/`resetStack` shape is exactly what Lox++ inherited. The
  natural extension for a stack-VM of this shape is a handler-record stack
  holding `{frameCount, stackTop, catchIp}`, generalizing the existing
  `resetStack()` (an unconditional unwind-to-zero) into an
  unwind-to-checkpoint.

## Correction: this is not free on the shared-bytecode pipeline

The native VM, JVM backend, and CLR backend are **not** three independent
compilers. `src/main.cpp` calls the same `compile(source, &mm)` in all three
paths (`main.cpp:195,256`) to produce one `ObjFunction`/`Chunk` tree; the
native VM interprets that chunk directly (`vm.cpp`), while
`jvm::emitProgram`/`clr::emitProgram` **translate that same chunk** into JVM
class files / CLR IL through a shared pipeline
(`src/backend/chunk_decoder.cpp` → `cfg.cpp` → `capture_analysis.cpp` →
`abstract_stack.cpp` → emission). `notes/bytecode-translation-problems.md`
documents, at length and empirically (probe programs, not hypotheses), how
much hard-won correctness work went into that pipeline — and specifically
into `abstract_stack.cpp`'s `validateMergeConsistency`
(`src/backend/abstract_stack.cpp:723-753`), which checks that **every CFG
predecessor of a block agrees on operand depth**, because the JVM/CLR
verifiers require it. Predecessors come only from `cfg.buildCfg`'s decoding
of `JUMP`/`JUMP_IF_FALSE`/`LOOP`/`JUMP_TABLE` (`src/backend/cfg.h:18-30`) —
today's opcode set where every branch target's incoming depth is statically
derivable from the function's own bytecode.

New opcodes for `try`/`throw` (`PUSH_HANDLER <catchOffset>`, `THROW`) do not
fit that model if implemented naively: a catch handler's real predecessors
are every fault/throw site reachable inside the protected region — including
ones several call frames down, in a callee this function's own CFG pass
cannot see at all. If `PUSH_HANDLER`'s catch-offset operand is decoded the
way a plain `JUMP` offset is, `cfg.buildCfg` will fold it into the generic
predecessor list and `validateMergeConsistency` will try to reconcile depths
that were never meant to agree by the existing rule, either rejecting
legitimate programs or silently doing the wrong thing — exactly the class of
bug `bytecode-translation-problems.md`'s "still open, unowned, REACHABLE"
residues (the `and`/`or`-fold merge disagreements) already show this
subsystem is prone to.

**This does not touch, revert, or waste the existing P1-P8 work.** The fused
local/operand-stack model, capture analysis, calling-convention translation,
and every existing opcode's lowering are unaffected — none of that is being
redesigned. What's needed is new, clearly-scoped, structurally *separate*
machinery:

- `THROW` must be classified as a **terminal instruction** in `cfg.cpp`
  (like `RETURN` — no fall-through/branch successor edge), so it never
  contributes a phantom predecessor anywhere.
- `catchIp`'s entry depth is a **declared contract** stated by
  `PUSH_HANDLER` (checkpoint depth + 1 for the thrown value), not something
  discovered via predecessor agreement — it must be explicitly excluded from
  `validateMergeConsistency`'s generic loop, not swept into it.
- On JVM/CLR, the protected region must lower to a **real host exception
  region** (JVM exception table / CLR `.try{}catch{}` IL) rather than a
  hand-replayed "truncate the operand stack and jump" — real exception
  dispatch is precisely the feature that lets a handler receive control from
  an unknown-depth, unknown-origin throw without the static uniform-depth
  proof ordinary branches require. This sidesteps the merge-consistency
  problem entirely for JVM/CLR, rather than fighting it.

Given how many rounds of genuinely subtle, silently-wrong-output bugs the
*existing*, much simpler opcode set already produced in this exact
subsystem (see the N10 GAP entry's history in
`notes/bytecode-translation-problems.md`), this part of the work should
start with its own **probe-driven research node** — new programs under
`test/translation-probes/` exercising: throw from inside a nested
expression/loop within a protected region, throw crossing a function
boundary (callee throws, caller catches), nested `try`/`catch`, and `defer`
interacting with early return — checked against actual disassembly/output
the same way the original P1-P8 probes were, **before** `cfg.cpp`,
`abstract_stack.cpp`, or either emitter is touched.

## Proposed design

**Surface syntax** (three constructs, no `finally` keyword):

```
try { ... } catch (e) { ... }
throw expr;
defer expr;   // expr must be a call; evaluated (callee+args) immediately,
              // invocation deferred to function exit, LIFO
```

- `catch (e)` binds **any** thrown value untyped — no typed catch clauses.
  Use `match` inside the handler to discriminate, reusing existing
  enum-exhaustiveness machinery instead of new pattern-matching-on-catch.
- `defer` is function-scoped (like Go), not try-scoped: it runs on every
  exit from the enclosing function — normal return, fall-through, or a
  throw unwinding past that frame — in LIFO order. This is the sole
  cleanup mechanism (no separate `finally`), and is what fixes the
  `ObjFile` leak TODO: `defer f.close();` right after open.
- A caught runtime fault (index OOB, nil call, type error, ...) is
  delivered as an instance of a new builtin `Error` class with a `message`
  field (and a `kind` constant per fault category). Uncaught, it prints and
  exits exactly as today (same trace format, exit code 70) — no observable
  change for existing programs that don't use `try`.

**Native VM**: new opcodes for entering/leaving a protected region and for
raising (`PUSH_HANDLER`/`POP_HANDLER`, `THROW`, reusing `RAISE_ERROR` sites
to go through the throw path when a handler is active instead of
unconditionally halting). A handler stack of `{frameCount, stackTop,
catchIp}` records; `THROW` unwinds LIFO to the nearest handler by closing
upvalues and draining each discarded frame's pending `defer` list (a new
small per-`CallFrame` vector), truncating the stack to the handler's
checkpoint, pushing the thrown value, and jumping to `catchIp` — a bounded
generalization of the existing `resetStack()`/`closeUpvalues` machinery, not
a new capability class **for the interpreter loop itself**. `defer`'s
compile-time-enumerable exits (return, fall-through) reuse the compiler's
existing per-exit-site duplication pattern; the unwind-through-a-frame case
is handled by the VM draining that frame's defer list during `THROW`.
Because this same bytecode is also the JVM/CLR emitters' *input*,
`THROW`/`PUSH_HANDLER` must additionally be given first-class treatment in
`chunk_decoder.cpp`/`cfg.cpp` from the start — `THROW` decoded and
classified as terminal (no successor edge), and `PUSH_HANDLER`'s
catch-offset operand decoded as a block leader with a *declared* entry
depth rather than a discovered branch target. This is native-VM-adjacent
work but lives in the shared, target-independent passes, not in `vm.cpp`.

**JVM/CLR**: `try`/`catch` in Lox++ compiles to a real host
`try{}catch(LoxError e){}` region — not a translation of the native VM's
truncate-and-jump behavior, a structurally different lowering built on
`PUSH_HANDLER`/`THROW`'s block boundaries from the CFG pass above.
`throw expr;` wraps the value in `LoxError`'s existing type (add a `value`
field carrying the boxed Lox++ value); every existing fault site in
`LoxOps`/`LoxRuntime` already raises `LoxError` uniformly, so runtime faults
become catchable automatically once a catch region exists — no retrofitting
needed there. `defer` compiles via a host `finally` block wrapping the rest
of the function body (LIFO order for multiple defers falls out of nesting
one `finally` per `defer` statement) — structurally cheaper here than the
native VM's manual defer-list, since `finally` is host-native, but the
`try`/`catch` region construction itself is genuinely new emitter machinery
on both backends, not a trivial reuse of existing per-opcode translation.

**Spec**: grammar entries in `spec/02-syntax.md` for `tryStmt`, `throwStmt`,
`deferStmt` (modeled on the existing `breakStmt`/`return` entries); prose in
`spec/04-semantics.md` extending the Binding Identity unwind list and the
Runtime Errors table (mark each fault "catchable via `try`"); a new builtin
`Error` type documented in `spec/03-types.md`.

## Decisions reached

- `defer` only, no separate `finally` keyword — halves the grammar/VM/
  backend surface for no capability loss; `defer` subsumes `finally`'s use
  case.
- Caught runtime faults are delivered as a builtin `Error` class instance
  (`message` + `kind`), not a plain map — mirrors JVM/CLR's existing
  `LoxError.message` for cross-backend parity with minimal new concepts.
- `catch (e)` is a single untyped clause; typed dispatch is left to `match`
  inside the handler, reusing the enum-exhaustiveness machinery rather than
  inventing typed catch clauses.
- The shared-bytecode translation risk (above) gates implementation: a
  probe-driven research node must run first, before native VM or emitter
  code is written.

See the tracking issue (linked from this file once opened) for the node
breakdown.
