# CLR backend: measured emission limits

Measured against the full `examples/` and `notes/translation-probes/` corpus
during the CLR backend build (merged to `main` 2026-08-24). The CLR sibling
of `notes/jvm-emission-limits.md`, recorded here so a later contributor does
not re-measure, or build unneeded headroom, from scratch.

| Measurement | Value | CLR ceiling |
|---|---|---|
| Largest emitted `Main` body (`scanner.lox`) | 1,064 instructions, `.maxstack 46` | `.maxstack` is a 16-bit field (max 65,535); a fat method header's `CodeSize` is 32-bit |
| Largest emitted closure/function body (`parser.lox`, one `Invoke` method) | 1,837 instructions | same as above |
| Longest emitted string literal (`huffman.lox`) | 95 UTF-16 code units (190 `ldstr bytearray` bytes) | the CLR `#US` (user string) metadata heap indexes with a compressed integer; a single entry is not capped anywhere near this size |
| Branch range | not applicable — see below | not applicable |

## Method: how these were measured

Every runnable program in `examples/` and `notes/translation-probes/`
(`clr-only/` included) was compiled with `--target clr --out-dir <scratch>`,
and each emitted `.il` file's `.method` blocks were scanned for their
mnemonic-line count, their `.maxstack` directive, and their `ldstr bytearray`
operands (`src/backend/clr_emitter.cpp` emits every string constant as a
UTF-16LE byte array, not a quoted literal — see `ilasmDoubleLiteral`'s own
note in `src/backend/clr_emitter.h` for the matching ECMA-335 rule for
doubles). Instruction count is a text-based proxy for the actual assembled
`CodeSize`; the `dev-managed` image has no IL disassembler (`ildasm`,
`monodis`, `dotnet-ildasm`, and `ikdasm` are all absent) to measure the true
post-assembly byte count directly, and no IL verifier either — the same gap
applies to verifying a method's IL as to disassembling it. The proxy runs
safely high — one line of IL text is at least as costly as, and
usually costlier than, the one to five bytes its assembled opcode/operand
pair takes — so a number this far under a 16- or 32-bit field leaves no
realistic doubt.

## Branch range: not a number, a mechanism

The JVM backend computes literal byte offsets for `goto`/`if_*` and would
need `goto_w` if a jump ever had to span more than ±32,767 bytes. The CLR
backend has no equivalent quantity to measure: every `JUMP`/`LOOP`/
`JUMP_IF_FALSE` lowers to `br`/`brtrue`/`brfalse` followed by a symbolic
label name (`src/backend/clr_emitter.h`'s own note on `JUMP`/`LOOP`
lowering), never a hand-computed offset or an explicit choice between the
short (`br.s`) and long (`br`) opcode forms. `ilasm` resolves every label and
picks the shortest legal encoding itself. A method whose control flow needs
the long form is exactly as easy to emit as one that does not — the emitter
does not know or care which form ilasm chose — so there is no possible
splitter to build here, independent of any corpus measurement.

## Conclusion

None of the three measured quantities came close to a real CLR ceiling on
any corpus program, the same conclusion `notes/jvm-emission-limits.md`
reached for the JVM backend. The project does **not** build a method
splitter or a string-literal splitter for the CLR target, and branch-range
splitting is not merely unneeded but inapplicable, since the emitter never
computes a branch encoding in the first place.

If a future program approaches one of the two real numbers in the table
above, re-measure before reaching for a splitter — the corpus that grounds
this table may not be representative of the new program's shape.
