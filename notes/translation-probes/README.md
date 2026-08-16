# Translation probes

Small Lox++ programs, each chosen so that its *bytecode* isolates one
translation problem for the JVM/CLR backends. They are the empirical ground for
`notes/bytecode-translation-problems.md` — read that document alongside these.

The `Vn_*` probes are **verification** programs: their runtime *output* (not
just their bytecode) settles a semantic question the disassembly alone leaves
open — chiefly whether `CLOSE_UPVALUE` can be treated as a no-op (it cannot).

## Regenerating the bytecode

The stock build hides disassembly. Build a debug-print binary (clang required —
GCC rejects the parser's designated initializers):

```bash
clang++ -std=c++20 -O0 -Wno-c99-designator -Isrc \
  -DLOXPP_DEBUG_PRINT_CODE -DLOXPP_NAN_TAGGING \
  src/*.cpp src/stdlib/*.cpp -o /tmp/loxpp-dis
```

(or `cmake -DLOXPP_DEBUG_PRINT_CODE=ON` for the same effect). Then:

```bash
/tmp/loxpp-dis notes/translation-probes/01_assign_local.lox
```

Each probe prints the disassembly of every compiled chunk, followed by the
program's runtime output. Slot 0 of every chunk is the callee/receiver, so the
first *user* local is slot 1.

## Index

The `Problem(s)` column maps each probe to the problem IDs in
`../bytecode-translation-problems.md`; see the verdict table below for whether
each has an off-the-shelf solution.

| Probe | Isolates | Problem(s) |
|---|---|---|
| `01_assign_local` | locals live on the operand stack; scope-exit `POP`; `SET_LOCAL` peeks | P1, P2 |
| `02_if_else` | `JUMP_IF_FALSE` peeks; branch `POP` discards the condition | P2, P3 |
| `03_and_or` | short-circuit value-carrying merge (needs `dup`) | P2, P3 |
| `04_while` / `05_for` | loop CFG from byte offsets; `for` desugaring has two back-edges | P3 |
| `06_shared_upvalue` | two closures capture one local → one shared cell | P4 |
| `08_call` | calling convention: stack args vs `Object[]`, slot 0 = callee | P5 |
| `09_class` | `this` = slot 0; `init` returns `this`; `SET_PROPERTY`/`DEFINE_METHOD` leave a value | P2, P5, P6 |
| `10_super` | `super` is compiled as an upvalue capture of the superclass | P4 |
| `11_for_in` | iterator + loop var are stack locals; `ITER_*` consume a loaded copy | P1, P8 |
| `12_list_map_index` | aggregate construction impedance; `SET_INDEX` leaves a value | P2, P7 |
| `13_enum_match` | `GET_TAG` (double) → `JUMP_TABLE` (int switch); default = `MATCH_ERROR` | P8 |
| `14_enum_payload` | enum payload read via `GET_INDEX`; `CALL` on an enum ctor | P6, P8 |
| `15_nested_arith` | operand-stack high-water mark (CIL `.maxstack`) | P1 |
| `16_slice_in` | `SLICE`/`IN` arg order and arity | P8 |
| `17_super_value` | `GET_SUPER` reads a super method as a value, not a call | P4 |
| `18_peek_of_named_local` | a `SET_LOCAL` peek reads a value N2 already folded into a named local, not a stack temp | P1, P2 |
| `19_peek_of_named_local_global` | same fact through `SET_GLOBAL`, inside a block scope | P1, P2 |
| `20_float_imprecise_constant` | a number constant a 32-bit float cannot hold exactly | P2 |
| `21_exponent_constant` | a number constant whose exponent text has no decimal point (jasmin rejects it) | P2 |
| `22_and_or_assignment_statement` | an `and`/`or` right side ends in an assignment; the merge `POP` is also a CFG block leader | P2, P3 |
| `23_and_or_local_initializer` | `JUMP_IF_FALSE` on a condition N2 already folded into a named local, not a stack temp | P2, P3 |
| `24_call_before_closure` | a global function called before its own `fun` declaration has run: late-bound globals must fail, not silently succeed | P5 |
| `25_seq_map_string_coverage` | `IS_SEQ` via a discarded `match` sequence pattern (list, string, map subject), `SLICE` on a List, `in` on a String and a Map, iteration over a String and a Map, `BUILD_MAP` with 3 pairs | P7, P8 |
| `V1_fresh_cell` | body-local captured in a loop → **fresh cell/iter** → prints `0 1 2` | P4 |
| `V2_shared` | mutable shared upvalue → prints `2` | P4 |
| `V3_loopvar` | loop var captured directly → **one shared cell** → prints `3 3 3` | P4 |
| `V4_mutate_through_upvalue` | `set` writes a shared cell, `get` reads it back → prints `7 9` | P4 |
| `V5_self_recursive_closure` | a local `fun` captures its own slot (direct recursion) → prints `120` | P4 |
| `V6_self_recursive_closure_in_loop` | self-recursive local `fun`, fresh cell per loop trip → prints `12` | P4 |

## Are these problems solved in the literature?

Each problem was mapped against compiler literature and real dynamic-language
JVM/CLR backends. **None is research-open** — every one decomposes into named,
textbook techniques with abundant precedent. The difficulty concentrates in one
place: no mainstream dynamic-language backend lowers from a clox-style *fused*
stack bytecode (they all keep locals and temporaries distinct in their own IR),
which is exactly why **P1/P3/P4 are `SOLVED-WITH-ADAPTATION`** while
**P5/P7/P8 are plain `SOLVED`** that merely consume P1's output.

- **P4 is the sharpest hazard.** The technique is standard, but the current
  backend plans are *provably wrong* (`CLOSE_UPVALUE`-as-no-op with hoisted
  cells) in a **test-evading** way — `V3` passes (`3 3 3`) while `V1` is silently
  miscompiled (`2 2 2` instead of `0 1 2`).
- **P1 is the foundational systemic risk.** Everything downstream consumes its
  slot-vs-temporary classification; get it wrong and the output fails to
  *verify*, not merely misbehave.
- **P6 is not really a codegen problem** — it is a runtime-library contract to
  specify (one `Callable` interface, uniform boxing, dispatch helpers).

| Problem | Verdict | Technique — precedent |
|---|---|---|
| **P1** Abstract-stack reconstruction | `SOLVED-WITH-ADAPTATION` | Abstract interpretation of stack effects (the verifier's own data-flow) + stack→register de-stacking — *no tool ingests clox's fused local/stack bytecode*, so the local/temporary partition must be rebuilt first (Soot Baf→Jimple; ASM `COMPUTE_MAXS`) |
| **P2** Peek-not-pop stack effects | `SOLVED` | Stack-effect normalization: `dup`/`dup_x1` for assignment-as-expression, or fuse the `SET_*;POP` idiom into a plain store — *exactly what `javac` emits for value-producing stores* |
| **P3** CFG recovery + verifier legality | `SOLVED-WITH-ADAPTATION` | Leaders-algorithm CFG recovery + uniform `Object` at merges to satisfy the type-merge rule — must then synthesize JVM `StackMapTable` / CIL `.maxstack` (ASM `COMPUTE_FRAMES`; Jython/Clojure/Kotlin all do this) |
| **P4** Closures: capture + cell lifetime | `SOLVED-WITH-ADAPTATION` | Closure conversion + **assignment conversion** (box captured mutable vars into heap cells) — `CLOSE_UPVALUE` = *fresh cell per iteration*, not a no-op (cf. C# 5 loop-var change; Scala/Kotlin `Ref`; ORBIT/Dybvig) |
| **P5** Calling-convention impedance | `SOLVED` | ABI/prologue lowering + argument marshalling into a generic `call(receiver, Object[])`, with `RETURN` selecting `areturn`/void — *precisely Rhino `Callable.call`, Jython `__call__`, JRuby `DynamicMethod.call`* (consumes P1) |
| **P6** Runtime polymorphism behind opcodes | `RUNTIME-DESIGN` | Uniform boxed values + one dispatch interface/helper set — a runtime contract, not a codegen algorithm (cf. Clojure `IFn`/`RT`, Jython `PyObject`, DLR `CallSite`, Nashorn `invokedynamic`) |
| **P7** Aggregate construction from N temps | `SOLVED` | Spill the N stack temps then array-init (`dup`/`aastore` / `newarr`/`stelem`) — a mechanical reshape (AST compilers sidestep it entirely; cf. Clojure `VectorExpr`, Groovy) |
| **P8** Opcode fusion + switch/iterator lowering | `SOLVED` | `GET_TAG`→`tableswitch`/CIL `switch` with box/unbox peephole fusion; for-in → `hasNext()`/`next()` desugar — *cf. `javac`/Kotlin enum-switch (`$SwitchMap` + `tableswitch`)* (consumes P1/P3) |

### Key references

- **Verification / stack-state merge:** JVMS §4.10 (type-checking verifier, merge
  rule), §4.7.4 (`StackMapTable`); ECMA-335 (CLI) III §1.7.5 & §1.8.1.3, II §25.4.3
  (`.maxstack`); Leroy, *Java Bytecode Verification* (2003).
- **CFG / de-stacking:** Dragon 2e §8.4 (leaders algorithm), §8.7 (peephole);
  Vallée-Rai et al., *Soot* — Baf→Jimple (CASCON 1999); ASM `COMPUTE_MAXS`/`COMPUTE_FRAMES`.
- **Closures:** Appel, *Modern Compiler Implementation* Ch. 15 (closure conversion);
  ORBIT (SIGPLAN '86) & Dybvig (1987) (assignment conversion / boxing);
  Nystrom, *Crafting Interpreters* Ch. 25 (`closeUpvalues` at scope exit).
- **Calling convention / switches:** Appel Ch. 6 (activation records); JVMS §3.10
  (switches), §6.5 (`invoke*`, `dup` family, `tableswitch`); JLS §14.14.2 (for-each
  desugaring).
- **Precedents (dynamic language → JVM/CLR):** Jython, Rhino/Nashorn, JRuby,
  Clojure, Groovy, Kotlin, Scala; IronPython/IronRuby (DLR); GraalVM/Truffle.

> Verdicts are a literature-mapping pass over P1–P8; the full problem statements,
> evidence, and the implied backend pipeline are in
> `../bytecode-translation-problems.md`.
