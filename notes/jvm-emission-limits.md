# JVM backend: measured emission limits

Measured against the full example/bootstrap corpus during the JVM backend
build (merged to `main` 2026-08-16). Recorded here so a later contributor
does not re-measure, or build unneeded headroom, from scratch.

| Measurement | Value | JVM limit |
|---|---|---|
| Largest emitted method (`LoxMain.main`) | 9,050 bytes | 65,535 bytes |
| Largest `LoxFn$<n>.invoke` body | 2,095 bytes | 65,535 bytes |
| Longest string literal | 83 bytes | 65,535 bytes |
| Jump range | ±32,767 bytes | a jump cannot span more than its own method |

## Conclusion

None of the three came close to their limit on any corpus program. The
project does **not** build:

- a method splitter (`LoxMain.main`/`LoxFn$<n>.invoke` bodies stay far under
  the 64 KB method-size cap),
- `goto_w` support (every jump stays inside the `±32,767`-byte `goto`/`if_*`
  range, and a jump never needs to cross a method boundary — see
  `src/backend/jvm_emitter.h`'s local-variable-layout note for why one
  chunk always lowers to one method),
- a string-literal splitter (`ldc`'s 65,535-byte operand limit is not close
  on any corpus program).

If a future program approaches one of these numbers, re-measure before
reaching for one of the three — the corpus that grounds this table may not
be representative of the new program's shape.
