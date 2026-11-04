# Invariant probes for the concurrent machine

**Status:** accepted. The owner approved it on 2026-11-05 in the pull request
that moved it here (#1096).
**Issue:** #1066. Follow-up of mission #1058
(`docs/decisions/concurrency-semantics-grounding.md`).

## Goal in VISION.md

- *Fail loudly*: a broken invariant must stop a test, not pass.
- *Evaluation order*: the order of tasks is defined (D1). A probe pins it.
- The backends conform to the spec. A shared probe checks each backend
  against the same expected output.

## Why

Mission #1058 closed the forks of the concurrency semantics. It named three
bug classes that can return. The runtimes will be refactored later (#1068).
Unit tests of internals do not survive that refactor. Program-level probes
do. So each invariant gets probes that run on every backend.

## The invariants

| Id | Invariant | Spec anchor | Issues |
|---|---|---|---|
| I1 | A task never sees state that belongs to another task | The Task Machine, the table of task-owned state | #984, #1009, #729 |
| I2 | No task waits while the condition of its wait is already true | Sides rules 2 and 6; `receive` tests the mailbox before it waits | #1054, #1047, #948 |
| I3 | The ready order follows the first wait of each call | The Task Machine, moves and run queue | #950, #996 |

### Probes (filled in by the nodes)

Each probe is a spec probe in `test/translation-probes/`, numbered from 374.

| Id | Case | Probe |
|---|---|---|
| I1 | one probe for each field of the task-owned state table | N3 |
| I2 | one probe for each call kind: `read`, `read_bytes`, `read_some`, `readline`, `readlines`, `accept`, `write`, `receive`, `receive_where` | N1 |
| I3 | a call that wakes and waits again keeps its place; one case for a socket call and one for a receive | N2 |

## Rules

1. **No wall-clock time where it can be avoided.** The scheduler clock seam
   (#999, #1044) is C++ only and native only. A `.lox` probe cannot reach it,
   and a new Lox surface breaks the D4 freeze. A probe uses sends, yields,
   buffered bytes and closes to make the order. It uses `clock()` only where a
   limit must pass, as probes 358 and 369 do. The seam stays for the C++ unit
   tests of native.
2. **Each probe fails on a mutation of its property.** For each probe and each
   backend, the pull request records the mutation and the result. Where a
   backend cannot be mutated, the pull request says so and names where the
   defect becomes reachable. The bootstrap keeps
   `tools/check_bootstrap_park_state.py`.
3. **A check script connects the probes.** `tools/check_invariant_probes.py`
   reads the table above. It requires that each row names an existing probe
   with a `STATUS` line and a row in `test/translation-probes/README.md`, and
   that each field of the task-owned state table has an I1 probe. CI runs it.
4. **Each probe names the spec rule it pins.** The node states that no other
   section of `spec/` decides the same case differently.
5. **No spec change.** A probe that finds a spec gap stops the node. The gap
   becomes an issue.
6. **No probe duplicates probes 347 to 373.** The node reads each of them
   first.

## Consumers

| Consumer | Owes |
|---|---|
| Native VM | Passes every new probe. Mutation evidence |
| JVM backend | The same. It is the semantic oracle |
| QBE backend | The same. QBE has no `yield`: a coroutine probe uses `NOT-BINDING: qbe` and a probe in `qbe-only/` |
| Bootstrap interpreter | The same. A probe that maps to a record field adds a mutation to the park-state check |
| Resolver, LSP, tree-sitter grammar, editor syntaxes | Nothing. No syntax changes. The tree-sitter step only parses the probes |
| `tools/check_invariant_probes.py` | New |
| `ci.yml` | Runs the new script next to `tools/check_task_machine.py` |

A probe lands with all consumers passing. If a consumer cannot pass yet, its
xfail file lists the probe.

## Nodes

The tracking issue holds the DAG: N0 this decision, N1 I2 probes and the check
script, N2 I3 probes, N3 I1 probes, N4 close. N1 to N3 run in parallel.

## Open question

- None for I3. The owner ruled on 2026-11-05 that I3 must pin #950 and #996
  without wall-clock time. If N2 finds a case that needs a limit to pass, N2
  uses `clock()` as rule 1 allows and states why.
- Does QBE need a replacement probe for the coroutine field of I1? N3 decides.
