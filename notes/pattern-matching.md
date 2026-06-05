# Pattern Matching — Remaining Work

## `@`-bindings (Phase 4)

Bind a whole matched value while also inspecting its structure:

```lox
case node @ Node(l, r) => process(node, l, r)
```

`not` is restricted to non-binding sub-patterns; `@`-bindings require no other changes beyond
what Phase 4.1 (sequence patterns) introduced.

---

## `JUMP_TABLE` optimization (Phase 5)

Replace the sequential tag-comparison chain emitted for `enum` match arms with a single jump
table dispatch when the match covers a dense range of tags. No semantic change — purely a
code-generation optimization.

New opcode: `JUMP_TABLE` — pops a tag integer, indexes into an inline jump offset table, and
branches to the matching arm. Falls back to the existing sequential chain when the tag range is
sparse.
