# CLR backend: measured emission limits

Measured against the full `examples/` corpus, `notes/translation-probes/`
(`clr-only/` included), and `bootstrap/loxpp_interpreter.lox`, during the CLR
backend build (merged to `main` 2026-08-24). The CLR sibling of
`notes/jvm-emission-limits.md`, recorded here so a later contributor does
not re-measure, or build unneeded headroom, from scratch.

| Measurement | Value | CLR ceiling |
|---|---|---|
| Largest emitted `Main` body (`scanner.lox`) | 1,378 instructions, `.maxstack 46` | `.maxstack` is a 16-bit field (max 65,535); a fat method header's `CodeSize` is 32-bit |
| Largest emitted closure/function body (`parser.lox`, one `Invoke` method) | 2,250 instructions, `.maxstack 7` | same as above |
| Largest emitted `Main` body in the repository (`bootstrap/loxpp_interpreter.lox`) | 3,540 instructions, `.maxstack 44`, 50 locals, 425 methods total in the module | same as above |
| Longest emitted string literal (`huffman.lox`) | 95 UTF-16 code units (190 `ldstr bytearray` bytes) | the CLR `#US` (user string) metadata heap indexes with a compressed integer; a single entry is not capped anywhere near this size |
| Branch range | not applicable — see below | not applicable |

## Method: how these were measured

Every runnable program in `examples/`, `notes/translation-probes/`
(`clr-only/` included), and `bootstrap/loxpp_interpreter.lox` was compiled
with `--target clr --out-dir <scratch>`, and each emitted `.il` file's
`.method` blocks were scanned for their
mnemonic-line count, their `.maxstack` directive, and their `ldstr bytearray`
operands (`src/backend/clr_emitter.cpp` emits every string constant as a
UTF-16LE byte array, not a quoted literal — see `ilasmDoubleLiteral`'s own
note in `src/backend/clr_emitter.h` for the matching ECMA-335 rule for
doubles). Instruction count is a text-based proxy for the actual assembled
`CodeSize`; the `dev-managed` image has no IL disassembler (`ildasm`,
`monodis`, `dotnet-ildasm`, and `ikdasm` are all absent) to measure the true
post-assembly byte count directly, and no IL verifier either — the same gap
applies to verifying a method's IL as to disassembling it. The line count is
a LOWER bound on `CodeSize`, not an upper one: each assembled opcode/operand
pair takes one to five bytes, so `CodeSize` is at least the line count and
some small multiple of it, never less. Even a generous 10x multiplier on
every row in the table above leaves it five orders of magnitude under the
32-bit field it is compared against, so the direction of the bound does not
change the conclusion.

## Branch range: not a number, a mechanism

The JVM backend computes literal byte offsets for `goto`/`if_*` and would
need `goto_w` if a jump ever had to span more than ±32,767 bytes. The CLR
backend has no equivalent quantity to measure: every `JUMP`/`LOOP`/
`JUMP_IF_FALSE` lowers to `br`/`brtrue`/`brfalse` followed by a symbolic
label name (`src/backend/clr_emitter.h`'s own note on `JUMP`/`LOOP`
lowering), never a hand-computed offset. The emitter writes only the long
form of each branch instruction (`br`, `brtrue`, `brfalse`, `switch`), and
never the short (`.s`) form. `ilasm` keeps whatever form the text names: on
a method whose `br` target is its own next instruction — the shortest
branch there is — `ilasm -exe` and `ilasm -exe -optimize` both assembled the
5-byte long-form encoding, not the 2-byte `br.s` form ECMA-335 allows for
it. `ilasm` does not widen a short form either: a hand-written `br.s` whose
target later moves outside its signed 8-bit range fails assembly instead of
being silently corrected. So there is no branch-range ceiling for the same
reason there is no splitter to build: the long form's operand is a 32-bit
signed offset, wide enough for any file this backend can emit, and the
emitter never chooses between the two forms in the first place.

## Conclusion

None of the three measured quantities came close to a real CLR ceiling on
any corpus program, including `bootstrap/loxpp_interpreter.lox` — the
largest single program in the repository, and 2.6x the next-largest `Main`
body's mnemonic-line count. This is the same conclusion
`notes/jvm-emission-limits.md` reached for the JVM backend. The project does
**not** build a method splitter or a string-literal splitter for the CLR
target, and branch-range splitting is not merely unneeded but inapplicable,
since the emitter never computes a branch encoding in the first place.

If a future program approaches one of the real numbers in the table
above, re-measure before reaching for a splitter — the corpus that grounds
this table may not be representative of the new program's shape.
