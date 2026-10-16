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
directory, when a file is outside a tier, when a file has no row in the index
below, or when a path reference to a markdown file points at nothing. CI runs
it in the lint job.

## References between documents

Refer to a document in the same tier by its file name. Refer to a document in
another tier, or outside `docs/`, by its path from the repository root, for
example `docs/proposals/jit-pipeline.md`.

## Index

### `decisions/`

| File | Decides |
|---|---|
| `decisions/coroutines.md` | Asymmetric, stackful coroutines with `yield` and `resume`; the native VM, backend, and profiler design |
| `decisions/design-tensions-2026-10-08.md` | The eight design tensions of 2026-10-08 and their resolutions |
| `decisions/distribution.md` | One static binary per target, two packages, Go-style distribution, install and upgrade, the container image |
| `decisions/dynamic-functional-programming.md` | Dynamic FP, not typed FP; combinators as module functions with a pipe operator; the TCO design |
| `decisions/editor-tooling.md` | The `loxpp --check` contract, the LSP capability set, the tree-sitter grammar, the editor plugins |
| `decisions/expressiveness-roadmap.md` | The capability litmus test, the seven genuine gaps in order, the performance direction |
| `decisions/import-system.md` | The approved frame for imports: compile-time composition, explicit exports, two kinds of stdlib |
| `decisions/non-local-control-flow.md` | `try`, `catch`, `throw`, `defer`: the handler stack and frame unwinding design |
| `decisions/qbe-backend.md` | The QBE AOT backend: the fused stack, runtime reuse, the staged plan |

### `proposals/`

| File | Proposes |
|---|---|
| `proposals/actor-model-design.md` | The open problems if the actor model is adopted: message copy, scheduler, FFI thread safety |
| `proposals/concurrency-model-decision.md` | A scoring of CSP, actors, and OS threads against the constraints; no model is chosen |
| `proposals/concurrency-model-next-steps.md` | The sequence to the concurrency go/no-go, item 6 |
| `proposals/jit-pipeline.md` | A v0 JIT that reuses the QBE emitter as an opt-in tier |

### `reference/`

| File | Describes |
|---|---|
| `reference/bootstrap-stack-depth.md` | Why no reliable deep-recursion guard is possible in the bootstrap interpreter |
| `reference/bytecode-translation-problems.md` | The problems P1 to P8 a bytecode backend must solve, with probe evidence |
| `reference/concurrency_in_bytecode_vms.md` | The design space and literature for concurrency in bytecode VMs |
| `reference/jvm-emission-contract.md` | The facts about the compiler's bytecode that every backend relies on |
| `reference/profiler-concurrency-notes.md` | What the profiler is coupled to, and what each concurrency model would change |

### `process/`

| File | Describes |
|---|---|
| `process/multi-agent-playbook.md` | Roles, node specifications, escalation, and the referee format for a multi-agent mission |

### `records/`

| File | Records |
|---|---|
| `records/backend-implementation-dag.md` | The build plan for the JVM and CLR backends, both complete |
| `records/benchmark_report_2026-08-26.md` | Native VM against JVM and CLR, with the native hot spots |
| `records/benchmark_report_jump_table.md` | The `JUMP_TABLE` opcode measurement |
| `records/benchmark_report_qbe_2026-10-01.md` | Native against JVM and QBE, with the S8 speed work |
| `records/build-performance-2026-09-06.md` | The debug build measurement and its improvement plan |
| `records/coroutines-retro.md` | Why the two-phase QBE plan for coroutines was unsound |
| `records/non-local-control-flow-retro.md` | The four waves of bugs after mission #223, and the process rules drawn from them |
