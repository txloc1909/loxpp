# Design documents

This directory holds every design document of Lox++ that is not the
specification or a rule. The path of a document tells its authority. A reader
does not open a file to learn whether it binds.

| Tier | Binds? | Living? | Who changes it |
|---|---|---|---|
| `decisions/` | Yes, until a later decision supersedes it | Yes | A pull request the owner approves |
| `proposals/` | No. A proposal binds nothing | Yes | Anyone |
| `reference/` | No. It describes the implementation as it is | Yes. Keep it current | Anyone |
| `process/` | The procedure for multi-agent work | Yes | Anyone |
| `records/` | No | Frozen. Nobody edits a record after it merges | Nobody |

The binding documents outside this directory, in priority order:

1. `VISION.md` says what the language is for. Only the owner changes it.
2. `spec/` says what the language does.
3. `AGENTS.md` says how work is done.

A decision must serve a goal in `VISION.md`. When a decision and the vision
conflict, the decision changes, or the owner changes the vision.

## Where a new document goes

- A design you want approved: `proposals/`. Put `**Status:** proposal` under
  the title.
- An approved design: `decisions/`. The pull request that gets the approval
  moves the file from `proposals/` and sets `**Status:** accepted`, with the
  date and the issue or pull request that decided it.
- A fact about the code that other code relies on, or a survey of the
  literature: `reference/`.
- A measurement, a retrospective, or a plan that completed: `records/`. Name
  it with its date when the date matters.
- A decision that a later decision replaces: move it to `records/` and add
  `Superseded by <path>` to its status line.
- A backlog item, a follow-up, or a "do this later": a GitHub issue, never a
  document. See `AGENTS.md`, "Backlog and task tracking".

The repository has no `notes` directory. A document goes in one of the tiers
above.

## Status line

The first paragraph after the title of a decision or a proposal is one of:

```
**Status:** accepted. <date, and the issue or pull request that decided it>
**Status:** proposal. <what approval it waits for>
```

`tools/check_docs_placement.py` fails when a status line disagrees with its
directory, when a file is outside a tier, or when a path reference to a
markdown file points at nothing. CI runs it in the lint job. Nothing it checks
is hand-kept: `ls docs/` is the index.

## References between documents

Refer to a document in the same tier by its file name. Refer to a document in
another tier, or outside `docs/`, by its path from the repository root, for
example `docs/proposals/jit-pipeline.md`.
