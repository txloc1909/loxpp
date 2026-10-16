# Block-level kernels: data parallelism for multi-core, GPU, and NPU

**Status:** proposal. It waits for the owner to decide if data parallelism is
in scope (see "Fit with the vision"), and for the item 6 concurrency decision
to state how kernel threads relate to its model.

## The question

What does the Lox++ frontend need for block-level programming that targets
parallel processors (multi-core CPUs, GPUs, NPUs), while the program keeps
its normal control structure on the CPU?

"Block-level" means the model of Triton, Pallas, and cuTile. The programmer
writes code that works on a *tile*: a block of typed data with a fixed shape.
The compiler, not the programmer, maps the tile to threads, warps, SIMD lanes,
or NPU cores.

## Summary

- A program has two levels. **Host code** is normal Lox++ on the CPU, with
  all of its control flow. **Kernel code** is a small, checked subset that
  runs as a grid of independent program instances, one tile each.
- A kernel does not touch the GC heap. Thus the native VM can run kernel
  instances on many OS threads without a thread-safe `MemoryManager`.
- The language stays dynamic. A launch reads the dtypes and `const` values of
  its arguments and specializes the kernel for them. No variable has a
  declared type.
- The first deliverable is slow and complete: a reference kernel interpreter
  on every backend. Fast CPU and GPU code come later and are checked against
  it.

## Fit with the vision

`VISION.md` is the test. This section names each goal that the design touches.
The owner decides on the points marked **open**.

| Goal | Effect | How the design answers |
|---|---|---|
| Language gap test | No primitive runs code on two cores at once. Data parallelism cannot be composed from closures, lists, maps, and `match`. It is part of roadmap item 7, "True parallelism". | It is a real gap. |
| Simple | It adds a `kernel` declaration, `const` parameters, a `Buffer` type, and a tile op library. | The host surface is one keyword and library functions. The subset is smaller than Lox++, not larger. **Open:** is this surface small enough? |
| Dynamically typed | Kernels need dtypes and static shapes. | Types come from values (a `Buffer` carries its dtype) at launch, not from declarations. The kernel is static after specialization. The program has no static type system. |
| Fail loudly | A GPU does not check bounds or races at no cost. | A masked access is checked by construction. An access without a mask sets a fault flag for the launch, and the host throws when it reads the result. Races: see "Semantics". **Open:** is a stated race rule acceptable? |
| Evaluation order | The order of program instances is not defined. | The spec states that instances are independent and their order cannot be observed by a program that obeys the race rule. Inside one instance, order stays defined. **Open:** this is a new kind of rule for the spec. |
| Resource release | Device memory is an external resource. | A device `Buffer` is released with `defer` or an explicit `free`. No GC finalizer frees device memory. |
| Memory | Device memory can be large and hidden. | All host-to-device copies are explicit. There is no implicit migration. Device use is visible through the same report as the heap. |
| Cost | A launch, a copy, and a tile op each have a cost. | Each one is named at its site. A condition on a tile is a mask, not a hidden branch, so divergence is visible in the code. Loops do not unroll unless the program asks. |
| Suspension | A GPU launch is asynchronous. | A launch returns an event. The program waits only where it names the wait. How a coroutine waits on an event depends on item 6. |
| Distribution | GPU and NPU code generation need a native toolchain. | The reference interpreter and the CPU path are in the simple package. GPU and NPU paths are in the full package only, as an opt-in tier like the JIT (`docs/proposals/jit-pipeline.md`). |

## The model

```lox
kernel add(x, y, out, n, const BLOCK) {
  var offs = program_id(0) * BLOCK + arange(BLOCK);
  var m = offs < n;
  store(out, offs, load(x, offs, m) + load(y, offs, m), m);
}

var x = buffer.from(f32, xs);
var y = buffer.from(f32, ys);
var out = buffer.empty(f32, len(xs));
defer out.free();
launch(add, [cdiv(len(xs), 1024)], x, y, out, len(xs), 1024).wait();
```

Control flow works at three levels:

1. **Host.** Full Lox++: `if`, loops, `match`, `try`, coroutines.
2. **Kernel, scalar values.** `if` and `for` on a scalar stay real branches
   and loops in the tile IR. The structure is not flattened.
3. **Kernel, tile values.** A condition on a tile is not a branch. The program
   writes it as a mask (`where`, masked `load` and `store`).

The scalar `for` has two forms: a normal loop with a dynamic bound, and an
explicit `static_range` that unrolls. Nothing unrolls silently.

## Frontend work

| Area | Work |
|---|---|
| Lexer and parser | The `kernel` keyword and `const` parameters. A launch is a library call, not syntax. |
| Subset checker | A resolver pass that rejects the following inside a kernel: allocation, strings, maps, lists, instances, closures that capture heap values, dynamic dispatch, `yield`, `throw`, `try`, `defer`, and calls to anything other than a kernel or a tile op. A kernel call inlines. Recursion is an error. This is a new shared analysis layer: every consumer uses the same rules. |
| Host types | A `Buffer`: typed, contiguous, owned by one device. The roadmap FFI item (6) needs raw typed memory too. One design must serve both. |
| Dtypes | `f16`, `bf16`, `f32`, `f64`, `i8`, `i32`, `i64`, `bool`. A host scalar (a double) converts to the kernel dtype at launch, with a fault if the conversion loses data. |
| Tile IR | A new IR, separate from the stack bytecode. Ops: `program_id`, `arange`, `load`, `store`, broadcast, element-wise arithmetic and comparison, `where`, `reduce`, `dot`, `atomic_*`, and scalar `if` and `for`. |
| Specialization | A cache keyed by kernel identity, argument dtypes, `const` values, and target. The key rule in `docs/proposals/jit-pipeline.md` ("id alone is not enough") applies. |
| Lowering | CPU: a loop nest with vector ops, and instances spread across cores. GPU: emit Triton IR, an MLIR tile dialect, or SPIR-V or PTX, and compile it in a subprocess. NPU: an MLIR or vendor dialect. |

## Semantics the spec must define

1. **Independence.** Program instances run in an order the program cannot
   observe. Two instances that write the same address, or one that writes and
   one that reads it, are a race unless both use `atomic_*`. The spec must
   choose one rule:
   - (a) A race is a fault. The reference interpreter detects it. Fast
     backends detect it only in a checked mode.
   - (b) A race gives one of the written values, with no promise which.

   Rule (a) fits "fail loudly" but a fast backend cannot always meet it.
   Rule (b) is honest about the hardware but adds unspecified behavior.
   **Recommendation:** (a), with the checked mode on in the reference
   interpreter and in tests.
2. **Float reductions.** `reduce` and `dot` are not associative in floating
   point. The spec must either fix one reduction tree (deterministic and
   slower), or give each op an error bound in ULPs. The differential gate
   compares bytes today, so option two also changes the gate.
   **Recommendation:** fix the tree for the reference semantics. Let a target
   opt out only with an explicit `fast_reduce` op at the call site, so the
   cost and the loss are named where they happen.
3. **Faults.** An access without a mask outside the buffer sets a per-launch
   fault flag. `wait()` throws an `Error` with a new kind (for example
   `KernelFaultError`) and the first instance id that faulted.
4. **Launch.** `launch` returns an event. `wait()` blocks. Arguments are
   checked on the host before any instance runs, so an argument error is a
   normal host fault.
5. **Copies.** `buffer.from`, `buf.to(device)`, and `buf.to_list()` are the
   only transfers.

## Consumers

| Consumer | What the change owes it (roles from `VISION.md`) |
|---|---|
| Native VM | The full spec. A reference kernel interpreter that runs instances one at a time in a fixed order: the oracle for all fast code. Then multi-core CPU code. Then GPU and NPU in the full package. |
| JVM | The reference semantics only. GPU and NPU access is low-level native access, the same class as FFI and the JIT, which the JVM does not owe. |
| QBE | Not owed. QBE has no vector types. A scalar loop-nest lowering is a possible experiment. The language change does not wait for it. |
| Bootstrap interpreter | Parses kernels, runs the subset checker, and runs the reference semantics. It must match native. |
| Resolver | The subset checker. |
| LSP | Diagnostics from the subset checker, and hover for dtypes after specialization if the launch site is known. |
| Tree-sitter grammar, editor syntaxes | The `kernel` keyword and `const` parameters. |
| Differential gate | Compile time: subset-checker errors match. Run time: reference output matches across native, JVM, and bootstrap. |

The spec names two countable sets: the tile ops and the dtypes. Each gets a
table in `spec/` and a script that checks every consumer covers it.

## Relation to item 6, the concurrency decision

A kernel and a task are different things. A task (actor, CSP process, or
thread) owns or shares heap values. A kernel instance owns none. So:

- Kernel worker threads do not need a concurrent GC, per-task heaps, or a new
  message-copy facility. The constraint "no new thread touches the heap"
  (`docs/proposals/jit-pipeline.md`) holds.
- But a pool of worker threads is scheduler work, and
  `docs/proposals/concurrency-model-next-steps.md` says not to start scheduler
  work before item 6. This proposal does not start the multi-core phase until
  item 6 decides. Item 6 should record the heap-free property, so that its
  model does not exclude a kernel thread pool.
- The event that `launch` returns must fit the model item 6 picks.

## Alternative: tracing in a library

The spec has operator hooks (`__add__`, `__lt__`, `__mul__`, and others). A
`Tile` class could record operations when a normal function runs on symbolic
tiles, as JAX and Pallas do. This needs no frontend change.

| | Tracing library | `kernel` subset |
|---|---|---|
| Frontend change | None | Keyword, parameters, checker |
| Scalar control flow | A host `if` on a tile fails at trace time. Loops unroll. | Kept as structured `if` and `for` |
| Cost | Silent unrolling hides cost | Unrolling is explicit |
| Errors | At launch | At compile time |
| Bootstrap parity | Free | Must implement the checker |

**Recommendation:** build the tracing library first as a prototype. It finds
the op set and the dtype rules at low cost and passes the language gap test as
a library. Adopt the `kernel` subset when the op set is stable, because only
the subset keeps the scalar control structure and keeps cost visible.

## Phases

Each phase is complete for the programs it accepts. No phase exists only to
remove a fallback of an earlier phase.

| Phase | Content | Complete for | Waits for |
|---|---|---|---|
| 0 | Tracing prototype as a library | Programs that use its op set. It is not part of `spec/`. | Nothing |
| 1 | `Buffer` and dtypes on the host | Every program: it adds a value type | A shared design with FFI (roadmap item 6) |
| 2 | `kernel` subset, checker, and reference interpreter on native, JVM, and bootstrap | Every kernel program. Slow, but correct, and the oracle | Phase 1, owner approval of the semantics |
| 3 | Native multi-core CPU code, checked against phase 2 | Every kernel program | Item 6 decision |
| 4 | GPU tier in the full package | Every kernel program on a supported device. Other devices fall back to phase 3, which gives the same result | Phase 3 |
| 5 | NPU tier | As phase 4 | Phase 4, a target device |

The phase 4 fallback is equal to its claim because phases 3 and 4 implement
one specification and the gate checks both against phase 2. A program cannot
tell which tier ran it, except for time and `fast_reduce`.

## Open questions

1. Is data parallelism in scope for Lox++ at all, given "Simple"? (Owner.)
2. Race rule (a) or (b)? (Owner.)
3. Is a tile a host value, or does it exist only inside kernels? Only inside
   kernels is simpler and is the recommendation.
4. Is `dot` a core op? GPU tensor cores and NPU matrix units need it as one.
   The recommendation is yes.
5. Shapes: static only, or symbolic dimensions bound at launch?
6. Does `Buffer` come from this proposal or from the FFI design? One of them
   must own it.
7. Which GPU target first: Triton (ROCm and CUDA through one IR), or SPIR-V
   through Vulkan (no vendor stack)?

When the owner approves the direction, each phase becomes one GitHub issue,
and phases 2 to 5 go on a GitHub Project board.
