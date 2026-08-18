# The bytecode emission contract

Facts about `src/compiler.cpp` and `src/vm.cpp` that any bytecode backend
(JVM, CLR, or otherwise) can rely on. `src/backend/jvm_emitter.cpp`,
`src/backend/abstract_stack.cpp`, and `src/backend/capture_analysis.cpp` each
depend on one or more of these; they are recorded once, here, rather than
re-derived per file.

## The scope-exit rule

`Compiler::endScope()` (`src/compiler.cpp`) retires every local a block
declares, on **every** exit path, before control leaves the block:

```cpp
void Compiler::endScope() {
    m_scopeDepth--;
    while (m_localCount > 0 &&
           m_locals[m_localCount - 1].depth > m_scopeDepth) {
        if (m_locals[m_localCount - 1].isCaptured) {
            emitByte(Op::CLOSE_UPVALUE);
        } else {
            emitByte(Op::POP);
        }
        m_localCount--;
    }
}
```

A captured local retires with `CLOSE_UPVALUE`; an uncaptured one retires with
a plain `POP`. This holds for the normal fall-through exit and for every
early exit (`break`, `continue`, a `match` arm) that crosses the scope
boundary — each gets its own copy of this cleanup, emitted at the exit site.

## `CLOSE_UPVALUE` closes a height-derived slot, not a tracked one

`VM::run()` (`src/vm.cpp`) handles `CLOSE_UPVALUE` as:

```cpp
case Op::CLOSE_UPVALUE: {
    closeUpvalues(stackTop - 1);
    pop();
    break;
}
```

The closed slot **is** the frame's stack height, immediately before the
instruction runs, minus one — a fact derivable purely from control flow
(see `src/backend/capture_analysis.h`'s `computeFrameHeights`), with no need
to track which local a `CLOSE_UPVALUE` "means" through dataflow.

## No bytecode analysis can recover source form

`{ var a = 1; }` and `1;` compile to **byte-identical** chunks, despite
opposite source truth (one declares a local that outlives the statement in
scope terms even though it is popped at scope exit; the other is a bare
expression statement). An analysis that tries to tell these apart by pattern
matching the bytecode is chasing a distinction the compiler already erased.
The correct approach is a canonical rule derived from execution behavior —
see `src/backend/abstract_stack.cpp`'s persistence test (R8) for the concrete
one this project uses to classify a `POP`'s target.

## The target-independent reuse point for a future backend

`src/backend/chunk_decoder.{h,cpp}`, `cfg.{h,cpp}`, `abstract_stack.{h,cpp}`,
and `capture_analysis.{h,cpp}` hold no JVM knowledge in any API declaration —
verified by grepping the four headers for `JVM`/`jvm`/`Jasmin`/`jasmin`
outside a comment; every hit is in prose explaining a consumer, never in a
function, struct, or field name. A future backend (CLR or otherwise) reuses
all four unchanged and starts at the emission layer. Confirm this property
holds before that work starts — it is not enforced by a test, and a later
change could break it silently.

## Why this belongs here, not only in the mission history

Three separate design passes in `src/backend/capture_analysis.h` and
`src/backend/abstract_stack.cpp` derived versions of these facts independently
before converging on the statements above. Any future backend (a CLR target,
for instance) needs them on day one, not after repeating that derivation.
