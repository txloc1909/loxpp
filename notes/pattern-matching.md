# Pattern Matching — Remaining Work

## `JUMP_TABLE` optimization (Phase 5)

Replace the sequential tag-comparison chain emitted for `enum` match arms with a single jump
table dispatch when the match covers a dense range of tags. No semantic change — purely a
code-generation optimization.

New opcode: `JUMP_TABLE` — pops a tag integer, indexes into an inline jump offset table, and
branches to the matching arm. Falls back to the existing sequential chain when the tag range is
sparse.
