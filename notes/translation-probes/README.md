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

| Probe | Isolates |
|---|---|
| `01_assign_local` | locals live on the operand stack; scope-exit `POP`; `SET_LOCAL` peeks |
| `02_if_else` | `JUMP_IF_FALSE` peeks; branch `POP` discards the condition |
| `03_and_or` | short-circuit value-carrying merge (needs `dup`) |
| `04_while` / `05_for` | loop CFG from byte offsets; `for` desugaring has two back-edges |
| `06_shared_upvalue` | two closures capture one local → one shared cell |
| `08_call` | calling convention: stack args vs `Object[]`, slot 0 = callee |
| `09_class` | `this` = slot 0; `init` returns `this`; `SET_PROPERTY`/`DEFINE_METHOD` leave a value |
| `10_super` | `super` is compiled as an upvalue capture of the superclass |
| `11_for_in` | iterator + loop var are stack locals; `ITER_*` consume a loaded copy |
| `12_list_map_index` | aggregate construction impedance; `SET_INDEX` leaves a value |
| `13_enum_match` | `GET_TAG` (double) → `JUMP_TABLE` (int switch); default = `MATCH_ERROR` |
| `14_enum_payload` | enum payload read via `GET_INDEX`; `CALL` on an enum ctor |
| `15_nested_arith` | operand-stack high-water mark (CIL `.maxstack`) |
| `16_slice_in` | `SLICE`/`IN` arg order and arity |
| `V1_fresh_cell` | body-local captured in a loop → **fresh cell/iter** → prints `0 1 2` |
| `V2_shared` | mutable shared upvalue → prints `2` |
| `V3_loopvar` | loop var captured directly → **one shared cell** → prints `3 3 3` |
