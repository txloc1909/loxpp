# Bytecode translation: the problems a lookup table hides

The JVM and CLR backend plans (`notes/jvm-backend-plan.md`,
`notes/clr-backend-plan.md`) present translation as an **opcode → instruction
lookup table**. That framing is misleading. A table implies each Lox++ opcode
can be lowered independently, in isolation, by textual substitution. It cannot.
The Lox++ VM and the JVM/CLR share a surface similarity — all three are
stack machines — but they differ on the one thing that matters for a correct
lowering: **what "the stack" is**. Getting from one to the other is a small
compiler, not a table.

This document derives the real problems empirically. Each is grounded in a probe
program under `notes/translation-probes/` whose *actual* disassembly (and, for
the `Vn_*` probes, actual runtime output) is quoted below. Nothing here is
hypothesised; every claim was produced by a debug-print build of the current
`main` and can be regenerated (see the probes' README).

The payoff is at the end: the plans' tables contain **provable correctness
bugs** and **missing opcodes**, and the shape of the real backend — the passes
you must write before you emit a single instruction — follows directly from the
problems below.

---

## The root cause: Lox++ fuses the local array into the operand stack

clox (and Lox++) has one stack. A function's frame is a window into it:

```
slots[0]         = the callee itself (a closure) — or, in a method, the receiver `this`
slots[1..arity]  = arguments
slots[arity+1..] = locals, in declaration order
slots[top..]     = operand-stack temporaries for expression evaluation
```

`GET_LOCAL n` / `SET_LOCAL n` index `slots[n]` **by absolute frame offset**.
A local variable *is* a stack slot. There is no separate storage. Consequences
that the table framing hides:

- **A variable declaration emits no instruction.** `var a = 1;` compiles to just
  `CONSTANT 1` — the value is left on the stack and the compiler privately notes
  "slot 1 is now `a`". The binding is invisible in the bytecode.
- **Reclaiming a local at scope exit is a `POP`** — the same opcode used to
  discard an expression-statement result.
- **The boundary between "locals" and "temporaries" is not in the chunk.** It is
  compiler scope state that was discarded during compilation.

The JVM and CLR do the opposite: **operand stack and local-variable array are
disjoint**. `aload`/`astore` (JVM) and `ldloc`/`stloc` (CIL) touch the local
array; arithmetic and calls touch the operand stack; the two never alias. And
their verifiers *require* the operand stack to be empty-or-consistent at every
control-flow merge.

So the backend's foundational job is to **un-fuse**: reconstruct which stack
positions are named locals (→ local array) and which are transient (→ operand
stack), a fact the compiler deleted. Every problem below is a facet of this.

---

## P1 — Abstract-stack reconstruction (the foundational pass) · correctness

You cannot translate a single opcode without knowing the abstract stack at its
offset. `01_assign_local`:

```
{ var a = 1; a = 2; print a; }

0:  CONSTANT 0 ('1')   ; var a = 1  → value stays on stack, slot 1 := a  (no store op!)
3:  CONSTANT 1 ('2')
6:  SET_LOCAL 1        ; slot[1] = peek(0)   — does NOT pop
8:  POP                ; discard the value of the assignment expression
9:  GET_LOCAL 1
11: PRINT              ; pops and prints
12: POP                ; <-- reclaim local `a` at block exit
13: NIL
14: RETURN
```

Offset 12 is the tell. `PRINT` already consumed its operand, so on the JVM the
operand stack is **empty** there — yet Lox++ emits `POP`. That `POP` reclaims the
local `a`. The plan's `POP → pop` would underflow the operand stack and the
verifier would reject the class. Meanwhile the `POP` at offset 8 is a genuine
temporary discard and *must* become `pop`.

**Two `POP`s, byte-identical, opposite translations.** The only way to tell them
apart is to know, at each offset, whether the top of the abstract stack is a
named-local slot or a temporary. That requires a pass that symbolically executes
the chunk, tracking stack height and slot classification. Sub-facts it must also
recover:

- **The invisible `var`** (P4): offset 0 leaves `1` where slot 1 lives. On the
  JVM `a` is a local, so the backend must *insert* an `astore_1` that Lox++ never
  emitted — at exactly the point the value becomes the binding.
- **Max operand-stack depth** for CIL's mandatory `.maxstack` (P?, `15_nested_arith`)
  — the same walk yields the high-water mark. (Jasmin computes it for the JVM;
  ilasm does not.)

This pass is not optional or an optimisation. Without it the output does not
verify. It is essentially a *decompilation* step: undo clox's slot/stack fusion.

---

## P2 — The "peek, don't pop" family · correctness

Assignment is an expression in Lox++, so every assigning opcode **leaves its
value on the stack** and relies on a following `POP` to discard it when used as a
statement. The VM confirms it (`vm.cpp`, `SET_LOCAL`): *"assignment is an
expression; leave value on stack"* — `frame->slots[slot] = peek(0)`. The whole
family behaves this way:

| Opcode | Stack in → out | Naïve table entry | Why it is wrong |
|---|---|---|---|
| `SET_LOCAL n` | `[v] → [v]` | `astore n` | `astore` pops; loses `v` |
| `SET_GLOBAL` | `[v] → [v]` | `putstatic` | `putstatic` pops |
| `SET_UPVALUE n` | `[v] → [v]` | `…aastore` | store pops |
| `SET_PROPERTY` | `[obj,v] → [v]` | `callvirt setProperty` → void | consumes both, leaves nothing |
| `SET_INDEX` | `[c,i,v] → [v]` | `call SetIndex` | must leave `v` |
| `DEFINE_METHOD` | `[cls,fn] → [cls]` | `callvirt defineMethod` → void | must leave `cls` |

`09_class`, `init`:

```
2: GET_LOCAL 1        ; x
4: SET_PROPERTY 0 ('x'); this.x = x  → leaves the value on the stack
7: POP                ; discard it
```

The correct lowering keeps the result: `SET_LOCAL n → dup; astore n`;
`SET_PROPERTY → dup_x1; …setProperty`; and so on. **Or** you fuse the common
`SET_* ; POP` statement idiom into the plain store — but fusing requires
recognising the pair, which requires the P1 abstract stack again.

The same defect infects control flow. `JUMP_IF_FALSE` **peeks** the condition
(`vm.cpp`: `if (isFalsy(peek(0)))`) — it does not pop. The plan's
`isFalsy(obj) → ifne` consumes the condition, so the value Lox++ expects to still
be there is gone. `03_and_or`, `print 1 and 2`:

```
0:  CONSTANT 0 ('1')       ; [1]
3:  JUMP_IF_FALSE -> 10    ; peek; if falsy, arrive at 10 with [1] still present
6:  POP                    ; truthy path: discard [1]
7:  CONSTANT 1 ('2')       ; [2]
10: PRINT                  ; merge: top is [1] (short-circuit) or [2]
```

At the merge (offset 10) the stack has depth 1 on **both** paths — the falsy path
kept `1`. A consuming `isFalsy → ifne` empties the falsy path (depth 0) →
depth 0 vs 1 at the merge → the verifier rejects it. The correct lowering is
`JUMP_IF_FALSE off → dup; isFalsy; ifne label_off`: dup so the peeked value
survives, and then the paired `POP` (offset 6) becomes a real `pop`. This is
uniform, but it is *not* the table entry, and it is invisible until you draw the
stack at the merge.

---

## P3 — Control-flow reconstruction & verifier legality · correctness / feasibility

Lox++ jumps are relative byte offsets; targets can land mid-sequence. The
backend must scan the chunk, decode every (variable-length) instruction to find
jump targets, and place labels — a mini basic-block recovery. `05_for` shows the
`for` desugaring is not a structured loop:

```
for (var i = 0; i < 3; i = i + 1) print i;

3:  GET_LOCAL 1        ; condition
9:  JUMP_IF_FALSE -> 34
13: JUMP -> 28         ; skip the increment on the first pass
16: …increment…        ; i = i + 1
25: LOOP -> 3          ; back-edge #1: to condition
28: …body…             ; print i
31: LOOP -> 16         ; back-edge #2: to increment
34: POP; 35: POP       ; exit; second POP reclaims `i`
```

Two back-edges and a forward skip. Any assumption of `while`/`for` structure is
wrong; you must emit label-and-goto and let the CFG be what it is.

The verifier then imposes a constraint Lox++ never checks: **at every merge the
operand stack must have identical depth and merge-compatible types.** Depth is
handled by P1/P2. Types are handled by a deliberate design choice, worth stating
as a rule: **all Lox++ values lower to `Object`/`object`.** If numbers were
lowered to raw `double` on the stack (2 slots, primitive type), the merge in
`03_and_or` — `double` from one path, whatever from the other — would be
un-mergeable and every arithmetic-carrying branch would fail verification.
Uniform boxing is not a performance compromise to apologise for; it is what makes
the translation *verifiable at all*.

---

## P4 — Closures: capture analysis, cell lifetime, and the `CLOSE_UPVALUE` bug · correctness · HIGHEST VALUE

This is where the plans are not merely imprecise but **provably wrong**.

An upvalue captures a *variable*, by reference, so that a mutation is seen by
every closure over it. On the JVM/CLR you cannot reference a local slot, so a
captured local must live in a heap **ref-cell** (`Object[1]`). Both plans do
this, and both add: *"`CLOSE_UPVALUE` as no-op … valid because all captured
locals are ref-cells from the start"* — i.e. allocate every captured local's cell
once, at function entry.

That is wrong, and two probes prove it at runtime.

First, capture is genuinely shared. `06_shared_upvalue` / `V2_shared`: `get` and
`inc` both list `local 1` in their `CLOSURE` metadata → they must receive the
**same** cell. `V2_shared` runs `inc(); inc(); return get();` and prints:

```
2
```

So a single cell, mutated through one closure, is observed through another. Fine
so far — one cell.

Now the killer. When a captured local is declared *inside a loop*, each iteration
must get a **fresh** cell, or all the closures alias and see the last value.
Whether that happens is decided entirely by **where `CLOSE_UPVALUE` sits**, which
in turn depends on the variable's scope. Compare:

`V3_loopvar` — captures the loop variable `i` directly. `CLOSE_UPVALUE` is emitted
**once, after the loop** (as in `07`'s layout), so all closures share one open
upvalue:

```
for (var i = 0; i < 3; i = i + 1) { fun f() { return i; } fns[i] = f; }
→ prints:
3
3
3
```

`V1_fresh_cell` — captures a body-local `snapshot` declared inside the loop. Its
`CLOSE_UPVALUE` sits **inside the loop body, every iteration** (offset 48 in its
`make` chunk, before the `LOOP`), closing the cell so the next iteration
allocates a new one:

```
for (…) { var snapshot = i; fun f() { return snapshot; } fns[i] = f; }
→ prints:
0
1
2
```

Same loop shape, opposite results — decided by the position of `CLOSE_UPVALUE`.
Now apply the plans' recipe (one cell per captured local, allocated at function
entry, `CLOSE_UPVALUE` = no-op) to `V1`: `snapshot` gets a single hoisted cell,
every `f` captures it, and the program prints `2 / 2 / 2`. **The documented
strategy silently corrupts `V1`.** It happens to get `V3` right, which is exactly
what makes the bug dangerous — it passes the obvious test.

The correct model, forced by the evidence:

- **Capture analysis is a prerequisite pass.** Before emitting a function, walk
  all nested `CLOSURE` operands; the `isLocal=1` entries name which of *this*
  function's slots are captured. Those slots — and only those — become ref-cells.
- **A cell is (re)allocated where the variable is declared**, i.e. at the point
  its initialiser value lands on the stack (the invisible `var` of P4/P1). Inside
  a loop body that means once per iteration.
- **`CLOSE_UPVALUE` is not a no-op.** It marks the end of a captured slot's live
  range. It is the signal that the *next* declaration of that slot must bind a
  fresh cell. You can implement it as "stop reusing this cell," but you cannot
  delete the concept and hoist the allocation.

`CLOSURE` decoding is itself non-trivial (P4b): variable-length trailing
`(isLocal, index)` pairs, `isLocal=1` → grab the parent's cell for local `index`,
`isLocal=0` → forward the parent's upvalue `index`. Two cases, and the closure's
own upvalue array must be built from a mix of both.

And `super` rides entirely on this machinery — it is **not** a separate feature.
`10_super`, method `B.greet`:

```
2: GET_UPVALUE 0       ; the superclass A, captured as an upvalue
4: SUPER_INVOKE 0 ('greet') 0
```

The compiler synthesises a hidden local holding the superclass and captures it;
`41: CLOSE_UPVALUE` closes it. So `GET_SUPER`/`SUPER_INVOKE` are correct **only
if** upvalue capture and cell lifetime are correct. Fix P4 and super falls out;
get P4 wrong and super breaks with it.

---

## P5 — Calling-convention impedance · correctness

`08_call`, `add(a, b)`:

```
== add ==
0: GET_LOCAL 1   ; a  (slot 1 — slot 0 is the callee closure)
2: GET_LOCAL 2   ; b
== script ==
6:  GET_GLOBAL 0 ('add')  ; callee
9:  CONSTANT ('1')        ; arg
12: CONSTANT ('2')        ; arg
15: CALL 2                ; [callee, a, b] → result
```

Several mismatches at once:

- **Slot 0 is the callee/receiver, not the first argument.** In a method
  (`09_class`) slot 0 is `this` (`init` does `GET_LOCAL 0` for the receiver and
  even `GET_LOCAL 0; RETURN` to return it — `init` implicitly returns `this`). The
  JVM `call(Object[] args)` receives args as an array with **no** slot-0 entry, so
  the backend must build a prologue that maps: Lox slot 0 → the closure/receiver
  (`this`/`aload_0`), Lox slots `1..arity` → `args[0..arity-1]` unpacked into
  local slots. Every `GET_LOCAL n` inside the body then offsets through that map;
  it is not `aload n`.
- **Stack-passed args vs array-passed args.** Lox pushes args individually then
  `CALL n`. `LoxCallable.call([Object])` wants one array. The args are already
  loose on the operand stack, so the backend must repack them into an `Object[]`
  (allocate array, `astore` each in reverse, or spill to temps). This "N loose
  values → one aggregate" reshaping recurs (P6).
- **`RETURN` is overloaded.** In a real function it is `areturn <Object>`. At
  top level `script` ends `NIL; RETURN`, but the JVM host is `static void
  main(String[])` — you cannot `areturn` from `void`. The lowering of `RETURN`
  depends on whether the enclosing chunk is the script or a function; the script's
  trailing `NIL; RETURN` becomes `return`.

---

## P6 — Runtime polymorphism behind monomorphic opcodes · correctness

Single opcodes fan out to several runtime behaviours the table cannot capture
with one row:

- **`CALL` dispatches on the callee's kind.** `08` calls a closure; `09`'s
  `CALL 1` calls a **class** (`C(5)` allocates an instance and runs `init`);
  `14`'s `CALL 1` calls an **enum constructor** (`Ok(5)`); `print`-style natives
  are another kind. `checkcast LoxCallable; invokevirtual call` only works if all
  of {closure, class, bound method, enum ctor, native} implement one interface and
  each `call` does the right allocation/dispatch. That is a runtime-library design
  constraint, not a codegen detail — and it must be stated as normative semantics.
- **`CONSTANT` dispatches on the constant's runtime type.** The plans list Number
  and String. But `13_enum_match` shows `CONSTANT 0 ('<ctor Color::Red>')` — the
  pool also holds **enum-constructor objects**, which must be *materialised* as
  runtime objects, not `ldc`'d. The backend must switch on the pooled value's
  type; the table's two rows are incomplete.
- **`GET_INDEX` is polymorphic over List / String / Map / enum payload.**
  `14_enum_payload` extracts a variant field with `GET_LOCAL 2; CONSTANT 0;
  GET_INDEX` — i.e. `subject[0]` reads payload slot 0. `LoxOps.getIndex` must
  therefore accept `LoxEnum`, which neither plan mentions.

---

## P7 — Aggregate-construction impedance · structural

`BUILD_LIST n`, `BUILD_MAP n`, and the `CALL` arg list all share one shape: Lox
pushes N loose values, then one opcode consumes them. `12_list_map_index`:

```
0: CONSTANT('1'); 3: CONSTANT('2'); 6: CONSTANT('3')
9: BUILD_LIST 3     ; [1,2,3] → LoxList
```

The plans say `new LoxList` + `n × add`. But when you execute `new LoxList` the
three elements are already on the operand stack *below* where the fresh list
reference lands, so `dup; <elem>; add` doesn't reach them — the elements are in
the wrong place relative to the container. You must spill the N values to temps
(or build bottom-up with `dup_x1` gymnastics). This is the same repack as the
`CALL` arg array (P5). It is mechanical but it is real reshaping, invisible in a
one-to-one table.

---

## P8 — Opcode gaps and required fusions · correctness / gap

- **`JUMP_TABLE` is absent from both plans' tables entirely.** `13_enum_match`:

  ```
  38: GET_TAG           ; pop enum, push tag as a Number (double)
  39: JUMP_TABLE min=0 count=4
              | tag 0 -> 51
              | tag 1 -> 60 …
  50: MATCH_ERROR       ; fall-through / out-of-range
  ```

  (R7 fix, PR #115 round 1: these offsets shift by 2 from an earlier version
  of this bullet — commit 82df1fa added a `CALL 0` earlier in the same chunk,
  to construct `Green()` instead of naming the bare constructor; see the
  "Nullary enum variants" bullet below.)

  It lowers to `tableswitch` (JVM) / `switch` (CIL): `min` is the switch base,
  the count×2 forward offsets become case labels (offsets are relative to the
  table end — convert to absolute, then to labels), and fall-through targets the
  `MATCH_ERROR` default. But note the seam: **`GET_TAG` pushes a boxed `double`
  and `JUMP_TABLE` wants an `int`.** A context-free lowering would box the tag and
  then have to unbox+`d2i` before the switch. The clean answer is to **fuse
  `GET_TAG ; JUMP_TABLE`** into `getTag()D; d2i; tableswitch`, keeping the tag
  primitive and never boxing it. Fusion again needs the P1 stack view.

- **Iterator protocol.** `11_for_in`:

  ```
  11: GET_ITER          ; list → iterator, left on the stack (becomes a local)
  12: NIL               ; the loop var `x`, pre-initialised to nil (invisible var again)
  13: GET_LOCAL 1       ; load the iterator (a normal local slot!)
  15: ITER_HAS_NEXT     ; consume the loaded copy → bool
  …
  22: ITER_NEXT         ; consume loaded copy → element, advances the shared iterator
  32: POP; 33: POP; 34: POP  ; reclaim bool?, `x`, iterator
  ```

  The plans originally posited a "dedicated iter-local slot allocated by the
  backend" (since corrected). The
  compiler *already* makes the iterator an ordinary local; `ITER_HAS_NEXT`/
  `ITER_NEXT` operate on a copy loaded by the preceding `GET_LOCAL`. So the
  backend should treat it as a local like any other (P1), not invent a side
  channel — and the three exit `POP`s are scope reclamation (drop them, per P1),
  not operand-stack pops.

- **`SLICE` / `IN` arity and operand order.** `16_slice_in`: `SLICE` pops
  `[seq, start, end]`; `IN` pops `[elem, seq]` (rhs then lhs — `chunk.h`) and the
  static helper's parameter order must match the on-stack order. Small, but a
  transposed pair silently inverts `x in xs`.

---

## Where the plans are wrong or incomplete — checklist

Tie-back to the problems above. The "Plan claim" column records the *original*
first-pass table entry. The plans now carry a Status note routing here as the
authoritative correction list; rows marked ✔ have additionally been fixed directly
in the plan tables, and the rest remain first-pass sketch (implement per the Fix
column).

| Plan claim | Verdict | Fix |
|---|---|---|
| `SET_LOCAL n → astore n` | **wrong** (P2) | `dup; astore n`, or fuse `SET_LOCAL;POP` |
| `SET_PROPERTY → callvirt setProperty` (void) | **wrong** (P2) | preserve value (`dup_x1`) or fuse with `POP` |
| `DEFINE_METHOD → callvirt` (void) | **wrong** (P2) | must leave the class value |
| `JUMP_IF_FALSE → isFalsy; ifne` | **wrong** (P2/P3) | `dup; isFalsy; ifne` — value is peeked, needed at merge |
| `POP → pop` | **wrong in general** (P1) | pop *only* temporaries; drop scope-reclaim pops |
| `GET_LOCAL n → aload n` | **incomplete** (P1/P5) | needs slot-0/arg remapping + captured-slot cells |
| `CLOSE_UPVALUE → no-op`, cells at fn entry | **wrong** (P4, proven by `V1`) ✔ | fresh cell at declaration; `CLOSE_UPVALUE` ends live range |
| `CONSTANT`: Number / String only | **incomplete** (P6) | also enum-ctor objects (materialise) |
| `GET_INDEX` over List/String/Map | **incomplete** (P6) | also enum payload |
| `JUMP_TABLE` | **missing** (P8) ✔ | `tableswitch`/`switch`; fuse with `GET_TAG`, default→`MATCH_ERROR` |
| "iterator in a dedicated backend slot" | **misleading** (P8) ✔ | it is already an ordinary local |
| `RETURN → areturn` | **incomplete** (P5) | script-level `RETURN` → `return` (void `main`) |
| `.maxstack` unaddressed (CLR) | **gap** (P1) | compute the operand high-water mark in the P1 walk |

None of these is exotic. They all trace to one fact — Lox++ fused the local array
into the operand stack — which the table framing papers over.

---

## The backend this actually implies

Not "walk the chunk, emit per the table." Rather, a short pipeline of passes that
run *before* emission, per `ObjFunction`:

1. **Label / CFG recovery.** Decode all instructions (respecting variable-length
   `CLOSURE`, `JUMP_TABLE`, invoke operands); collect every jump/loop/table target;
   place labels. (P3, P8)
2. **Capture analysis.** Walk nested `CLOSURE` operands; mark which of this
   function's slots are captured (become ref-cells) and record `CLOSE_UPVALUE`
   points as live-range ends. (P4)
3. **Abstract-stack reconstruction.** Symbolically execute the chunk tracking
   stack height, slot classification (named-local vs temporary), and the
   operand-stack high-water mark. This is the pass that makes `POP`, `SET_*`,
   `JUMP_IF_FALSE`, the invisible `var`, and `.maxstack` translatable. (P1, P2)
4. **Emission**, now context-aware: each opcode is lowered using (2) and (3), with
   a handful of peephole fusions (`SET_*;POP`, `GET_TAG;JUMP_TABLE`) that are
   *correct* only because the stack model backs them.

The runtime library, in turn, must satisfy semantics the opcodes only imply:
one `Callable` interface spanning closures/classes/enum-ctors/bound-methods/
natives (P6); `getIndex` over enum payloads (P6); `init` returning the receiver
(P5); `IN`/`SLICE` operand order (P8); everything boxed to `Object` (P3).

## Semantics the probes pinned down (grounding notes for the runtime library)

- **`init` always yields the receiver.** A bare `return;` in `init` still
  produces the instance (`class K { init(){ this.a=1; return; } } K().a` → `1`),
  and `return <expr>;` in an initialiser is a *compile* error ("Can't return a
  value from an initializer"). So the lowered `init` method unconditionally
  returns `this`; there is no value-return case to handle.
- **Nullary enum variants do not auto-construct, and construct by identity.**
  `13_enum_match` originally errored (`GET_TAG: expected an enum value`)
  because bare `Green` is the *constructor object*, not an instance — `print
  Green` yields `<ctor C::Green>`. You must call it: `Green()`. Commit
  82df1fa fixed the probe to do that (R7, PR #115 round 1); it no longer
  errors. And two separate constructions are **not** equal — `Green() ==
  Green()` is `false`. So `LoxEnum` equality is reference identity, not
  structural; the runtime and `GET_TAG`/`CONSTANT`-of-ctor lowering must
  respect that. `spec/` does not state this — it is worth adding.
- **No list concatenation.** `a + b` on two lists raises "Operands must be
  numbers" (hit while writing `V1`). `ADD` is numbers-and-strings only; worth
  stating in the `ADD` semantics so no backend invents list `+`.
- **`RETURN` can return a named local, not only a temporary — N4 must load it
  explicitly.** A `match` expression whose arm is a block of statements with
  no trailing bare expression (every statement's value fully discarded)
  leaves the match's synthetic result sitting in the hidden local slot the
  compiler allocated for it, instead of on top as a temporary; `return`ing
  that value does not follow it with the usual reclaim-`POP` a temporary
  would need. `examples/or_pattern_demo.lox`, function `must_stop`: `RETURN`
  at offset 58 sees height 3, localCount 3 — operand depth 0, not the 1 a
  temporary return value would leave. The value that must be returned is
  local slot 2, written earlier by `SET_LOCAL 2` at offset 30. Measured over
  `examples/*.lox` and `bootstrap/loxpp_interpreter.lox` (N2's abstract-stack
  pass, node N2): **33** such `RETURN` sites, zero among the translation
  probes (P1's `01_assign_local`-style checkpoints never exercise this
  shape). N4 must detect "the position at `RETURN` is a local, not a
  temporary" and emit an explicit load before the return, the same
  recognition N2 already performs for every other opcode.
- **GAP, open, owner N10 — corrected (PR #113 round 4 referee decision).**
  Two earlier versions of this bullet were wrong. The first said the "local,
  not a temp" family was fixed at `RETURN` only; commit 84a8d5b also fixed
  `SET_LOCAL` in the same PR, before this bullet was corrected. The second
  said `SET_GLOBAL` "still emits a plain pop-from-JVM-stack sequence with no
  such check" and "underflows the JVM operand stack at emit time" — both
  false. `emitSetGlobal`, `emitSetUpvalue`, and `emitJumpIfFalse` all held the
  `operandDepth() == 0` check already; the defect was not a missing check, it
  was that the check read the wrong tracker (`lastInvisibleVarSlot`, this
  pass's own forward walk over the most RECENTLY DECLARED invisible var, not
  the topmost LIVE one) and so **emitted a class that loads, silently, the
  wrong value — no underflow, no verifier error, no exception, just wrong
  output**. `main` gave this wrong output for a `match` result reaching
  `SET_GLOBAL`, `SET_UPVALUE`, `JUMP_IF_FALSE`, or `GET_ITER` (N9's own
  reader of the same tracker) — proof: three plain, unnested `match`
  programs, T1 (`SET_GLOBAL`), T2 (`JUMP_IF_FALSE`), T3 (`SET_UPVALUE`, see
  `test/test_jvm_emit.cpp`) each disagree with `build/loxpp` on `main`
  (commit `32991ff`), with exit code 0 and no diagnostic of any kind.
  - **Fixed, this PR.** `loadNamedLocalAtZeroDepth` (jvm_emitter.cpp) is now
    the one mechanism every zero-depth consumer shares —
    `emitReturn`/`emitSetLocal` (already fixed) plus `emitSetGlobal`,
    `emitSetUpvalue`, `emitJumpIfFalse`, `emitInherit`, and `emitGetIter`.
    Away from a CFG label, `localCount - 1` (N2's reconstructed count) names
    the slot outright — exact by construction, no tracker consulted. At a
    label, `localCount - 1` and `lastInvisibleVarSlot` must agree, or
    emission throws at emit time rather than guess. T1/T2/T3 and R9's three
    nested-match reproductions all now produce output identical to
    `build/loxpp` on this branch — the cross-check did not fire on any of
    these repros. **Correction (PR #115 round 4, R22): the cross-check DOES
    fire on a later-found, reachable repro family — see the third residue in
    the N10 GAP entry, below, for the four known programs that trigger it.**
  - **Fixed, N10 (PR #115 round 1).** `emitPrint`, `emitDefineGlobal`,
    `emitNot`, a one-element `emitBuildList`, and a folded-collection
    `emitGetIndex` all now route through the same `loadNamedLocalAtZeroDepth`
    (or, for `GET_INDEX`, a spill-then-load reorder built on it — see its own
    note). `examples/match_http_status.lox` and
    `examples/match_state_machine.lox` (both `print match ...;`) now match
    `build/loxpp`.
  - **Fixed, N10 (PR #115 round 2, R11/R12).** A folded match result on the
    LEFT of a two-operand op — `(match ...) + 1`, `* 10`, `< 5`, `== 2` — and
    the one-operand `-(match ...)` were refused before this round, though
    `build/loxpp` answers every one of them correctly (round 1's own
    rebuttal for `ADD` only tested the match on the RIGHT, where `build/loxpp`
    itself fails; it does not cover this, the LEFT-operand shape). Every
    two-operand rule parses its left side first (compiler.cpp), so the
    match's result is always the folded operand when this shape occurs, and
    the RHS is always the one genuine value still on the JVM stack —
    `emitAdd`, `emitSubtract`, `emitMultiply`, `emitDivide`, `emitModulo`,
    `emitEqual`, `emitGreater`, and `emitLess` (`reorderFoldedLeftOperand`)
    spill the genuine RHS, load the named LHS, then restore the RHS, the same
    reorder `emitGetIndex` already used for its own depth-1 shape; `emitNegate`
    is `emitNot`'s own one-operand twin. All nine repro programs (PR #115
    round 2, R11/R12 threads) now matched `build/loxpp` exactly, each with its
    own unit test proving it failed when its guard was reverted.
    `reorderFoldedLeftOperand` and its eight call sites no longer exist —
    superseded by the round-3 redesign below, which generalizes the same
    reorder to any operand count.
  - **Redesigned, N10 (PR #115 round 3, researcher referee decision on
    R15/R16).** Three site-by-site rounds (R3/R7 round 1, R11/R12/R13 round
    2) each closed the consumers a review round happened to name, and a
    fourth round (R15) named nine more: `BUILD_LIST` with width above 1 and
    its FIRST element folded, `BUILD_MAP` with a folded key, `CALL`/`INVOKE`
    with a folded callee/receiver, `GET_PROPERTY`, `SET_PROPERTY`,
    `SET_INDEX`, `SLICE`, and `IN`. Every one of the nine is legal and
    correct on `build/loxpp`. The referee's diagnosis: the design itself —
    one private `if (operandDepth() == ...)` branch per consumer — could
    never state a completeness claim, because nothing forced the enumeration
    to cover every opcode; R16 is the proof, disproving the THIRD version of
    this very GAP entry within one round.
    The fix is structural, not one more enumeration. `jvm_emitter.cpp`'s
    `nativePops(Op, DecodedInstruction)` is an exhaustive `switch` over `Op`,
    no `default` (clang's `-Wswitch` warns on a missing enumerator; this
    project builds with neither `-Werror` nor `-Wall`, so a missing row
    still compiles and throws only at run time) stating how many
    operand-stack cells `src/vm.cpp` pops for that instruction.
    `normalizeFoldedOperands`, one step in the dispatch loop every
    instruction passes through alike, compares that count against N2's own
    `operandDepth()` and repairs exactly the shortfall: it spills whatever
    genuine operands are already on the real stack, loads the missing bottom
    operand through `loadNamedLocalAtZeroDepth` (the same cross-checked
    mechanism the peek/locals-model family already trusted), then reloads
    the genuine operands on top, in order. A folded operand can only ever be
    the bottom-most of an instruction's own operands — `compileMatchBody`
    folds a `match` expression's result into its own named local before any
    later sibling operand is even parsed — so this covers every
    `nativePops`-covered opcode's own single-folded-operand shape at once,
    including `GET_TAG`'s own (a match nested directly inside another
    match's subject) and `RETURN`'s own, neither of which any review round
    had named yet. The nine R15 programs, a `RETURN`-of-a-folded-match
    program, and a nested-match-subject program all now match `build/loxpp`
    exactly; one unit test per shape proves it fails when its own
    `nativePops` row is reverted to `std::nullopt`.
  - **Still open, unowned, not silent — a deficit of two or more.** A
    `match` result sandwiched below TWO OR MORE live sibling operands of its
    own consumer (`CALL`'s own sandwiched-argument shape, `ADD`'s own
    mirror-image and both-sides-folded shapes — `1 + match ...`,
    `(match ...) + (match ...)`) still gets no answer, and `normalizeFoldedOperands`
    now throws a named, loud error for it instead of underflowing silently
    at emit time. This is NOT owed a fix: `compileMatchBody`'s own
    `resultSlot`/`subjectSlot` allocation (compiler.cpp) uses `m_localCount`
    alone, blind to a sibling operand already live on the real VM stack, so
    the two collide at the same slot — a pre-existing defect in
    `build/loxpp` itself, proven by seven one-line programs across PR #115's
    three rounds, all raising `Operands must be numbers.` /
    `Can only call functions, classes and enums.` on `build/loxpp` (exit 70).
    `1 + match 1 { case 1 => 2 case _ => 3 };` is legal per spec/02-syntax.md
    (`match` is `primary`). Fixing this is a compiler change, out of any
    backend node's charter (brief.md section 8); no later emission node owns
    it either, since N10 is the last one. A second, theoretical residue is
    unchanged by this round: the case where `loadNamedLocalAtZeroDepth`'s two
    slot estimates agree and are BOTH wrong is not ruled out by its
    cross-check — only real, per-edge merge verification closes it, which no
    node has attempted. No known program reaches either residue; the
    mission's definition of done is not blocked.
  - **Still open, unowned, REACHABLE — deferred by referee ruling, PR #115
    (originally R13, PR #115 round 2; disposition ruled at round 3).**
    `and`/`or` (`compiler.cpp`'s `and_`/`or_`) compile `JUMP_IF_FALSE` then
    either `POP` (true path, falls through into the right operand) or `JUMP`
    straight to the shared exit (false path, the left operand's own value
    stays live). When either operand is a folded `match` result, the two
    paths reach that shared exit at genuinely different operand depths.
    `abstract_stack.cpp`'s `validateMergeConsistency` — N2's own
    target-independent merge check, in a file this PR does not touch —
    catches the disagreement and throws at ANALYSIS time, before
    `jvm_emitter.cpp` ever runs:
    ```
    abstract_stack: merge disagreement in function '0' at offset 43:
    incoming operand depths disagree (0 vs 1) — the JVM/CLR verifier would
    reject this merge
    ```
    for `var t = true; print (match 1 {case 1 => t case _ => false}) and
    "yes";`, which `build/loxpp` runs correctly (prints `yes`). More one-line
    repros in the R13 thread put the folded operand on either side, with
    both `and` and `or`. Unlike the deficit-of-two-or-more residue above,
    this is a REAL loss of parity for a legal, correctly-answered native
    program, not a pre-existing compiler bug — but fixing it means changing
    which expressions N2 folds into an invisible local, or relaxing
    `validateMergeConsistency`'s own equivalence rule, in
    `abstract_stack.cpp`, an already-reviewed, target-independent file no
    diff in this PR touches; that is a change to N2's own design, which
    brief.md section 8 reserves for a research referee, not an in-PR edit
    smuggled through N10.
    **Disposition (round-3 referee ruling).** The round-2 implementer named
    N11 the owner; the referee ruled that call out of procedure (brief.md
    section 7 gives an unanticipated-problem ruling to the researcher, not to
    the implementer that hit it) and resolved it on substance instead: this
    gap is **not assigned an owning node**. It does not block N10 — the
    implementer, the reviewer, and the referee each independently verified no
    required gate reaches it: a pattern search over all of `examples/*.lox`,
    the nine N10 checkpoint files, every numbered translation probe
    (`01`-`28`, 27 files — `07` was never assigned), and
    `bootstrap/loxpp_interpreter.lox` (2583 lines) found no program that
    places a `match` expression directly on either side of `and`/`or`. N11
    does not fix this gap and does not own the fix; N11's own differential
    runs against the growing corpus will detect it loudly if any program ever
    reaches it, and that event is itself a researcher-unblock condition
    (brief.md section 7), not something N11 resolves alone. This gap stays
    unassigned until a program actually reaches it or a researcher rules
    on N2's fold model if the corpus ever reaches this shape.
  - **Still open, unowned, REACHABLE, not silent — a folded operand plus a
    sibling operand ending in `and`/`or` (R22, PR #115 round 4).** Distinct
    from the residue above: there, `and`/`or` sits ABOVE the fold, and the
    analysis-time merge check catches it. Here, a folded `match` result is
    one operand of an ordinary consumer (`ADD`, `SET_PROPERTY`, `BUILD_LIST`,
    …), and a LATER sibling operand of that SAME consumer ends in `and`/`or`.
    The consumer's own offset is then the `and`/`or` join label.
    `normalizeFoldedOperands` computes `deficit == 1` correctly and calls
    `loadNamedLocalAtZeroDepth`, but that function's own cross-check, at a
    label, compares `localCount - 1` against `lastInvisibleVarSlot` and the
    two disagree at THIS join, so it refuses rather than guess. Four repros,
    each `enum E { A B }` then one line, all correct on `build/loxpp`:
    ```lox
    print [match A() { case A => 1 case B => 2 }, (true and 5)];
    print [match A() { case A => 1 case B => 2 }, (true or 5)];
    print (match A() { case A => 1 case B => 2 }) + (true and 5);
    class K { init(v) { this.v = v; } }
    var k = K(3);
    (match A() { case A => k case B => k }).v = (true and 8);
    ```
    Each throws `jvm_emitter: offset <n> is a CFG merge; localCount - 1 (1)
    disagrees with the forward-walk tracker (2)` instead of matching
    `build/loxpp`. Adding a THIRD, later sibling operand moves the join off
    the consumer's own offset, and the program then works (`[match A() {
    case A => 1 case B => 2 }, (false or 5), 9]` matches `build/loxpp`
    exactly). No required gate reaches this shape — `check_jvm_probes.sh`
    (44 probes), `check_examples.py` (56 examples), and
    `bootstrap/loxpp_interpreter.lox` all stay green. The failure is loud,
    so no output is silently wrong. The real fix is the same one the residue
    above needs: real, per-edge merge verification in place of the
    cross-check's two estimates, a recorded residue from PR #113, still
    outside N10's charter. Not assigned to any node, for the same reason as
    the residue above.
