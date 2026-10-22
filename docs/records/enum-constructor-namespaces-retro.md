# Enum constructor namespaces: mission retrospective

## Purpose

Mission #771 implemented `docs/decisions/enum-constructor-namespaces.md`: a
program writes an enum constructor as `E.A`, in expressions and in patterns.
It ran as eight nodes (N1 to N8) from 2026-10-20 to 2026-10-22. The decision's
"Shipped" section lists the nodes and their PRs. This note records what the
plan missed and what the reviews found, so that the next language change
plans for it. Each claim cites an issue, a PR, or a file path.

## What the plan missed

**1. A consumer had an additive step and no removal step.** The plan listed
the LSP as a consumer (#771, "Consumers") and gave it work in N2 (#774): the
missing-arm code action, hover and go-to-definition on `E.A`. The sequencing
had a removal step for native (N6) and for the bootstrap (N7), but none for
the LSP. When N6 (#796) removed bare constructors from native, the tooling
resolver kept its old model: it still declared each constructor as a global,
so a bare name in a pattern resolved to a constructor that native no longer
had. The gap showed up in the N6 work, not in CI, because no gate compares
the tooling resolver with the compiler. It became N8 (#795, PR #800).

*Cause.* Rule 1 of "Plans for a language change" (`AGENTS.md`) asks for every
consumer, and the plan had every consumer. It did not ask, for each
consumer, for every phase of a change that runs in phases. An "add the new
form, rewrite, remove the old form" sequence owes each consumer that accepts
the old form a removal step.

*Rule.* `AGENTS.md`, rule 1, now says this.

**2. A rule moved earlier than planned.** The plan put the field-count rule
(pattern rule 4) in the removal nodes. Once N2 and N3 parsed `E.A(q)`, probe
139 diverged between native and the bootstrap, because each consumer kept a
different prefix rule. N2 (#783) and N3 (#790) took the rule. A rule whose
probe uses only the new form belongs to the additive step, because the
probe runs as soon as the new form parses.

**3. The plan stated a string form from memory.** The decision first said
that `str` of a constructor is `<A>` "as today". Every consumer printed
`<ctor E::A>`. The owner kept today's form in N2 (#774). A plan that says
"as today" must show the output of today's build.

## What the reviews found

The reviews ran programs on every node, and most findings came from that.
A separate reviewer agent did each review, in its own worktree and
container.

| Node | PR | Rounds | Findings |
|---|---|---|---|
| N2 | #783 | 2 | 5 (R1 to R5) |
| N3 | #790 | 2 | 1 |
| N5 | #791 | 2 | 2 |
| N6 | #796 | 4 | 2, then two owner changes (the expected-failure lists and a merge from `main`) |
| N7 | #799 | 1 | 0 |
| N8 | #800 | 2 | 1 |
| N4 | #811 | 2 | 2 |

- **A compile-error case passed on a halt** (N3, #790). The compile-error
  phase of `tools/diff_runtimes.py` counts a run with no stdout and a
  non-zero exit as a rejection. A consumer that faults at run time therefore
  passes a case that it does not reject. The cases now print a line before
  the bad statement. The decision records this as a bug class.
- **Two gates disagreed** (N3, #790). The bootstrap passed probe 130 before
  native. `tools/check_spec_probes.py` wanted the bootstrap's
  expected-failure line removed, and `tools/diff_runtimes.py` reported a
  divergence from native. The new `--oracle-xfail` option reports such a
  probe as AHEAD.
- **A rule order hid a binding** (N3, review R1). The bootstrap checked
  constructor names before enum names, so `case E` became a constructor test
  when another enum had a constructor `E`.
- **Programs outside `.lox` files** (N5, review R1). Three gate scripts
  embedded programs with bare constructors. A search of `.lox` files alone
  did not find them; the PR now shows a search of the whole repository.
- **A check with no test** (N8 and N4 reviews). Each node claimed a
  mutation proof for each rule. The reviewers found rules that no test
  guarded: the class-only head rule of the LSP resolver, the two grammar
  removals, and two highlight rules. Each got a test that fails without the
  rule.

## Process notes

- **A mutation must leave a valid program.** In N7, three mutations of the
  bootstrap used `endsWith`, which Lox++ does not have, so the mutated
  bootstrap failed for that reason. Each mutation now runs a probe that
  must still pass (a sanity run) before its target probes count.
- **A staged change goes into the next commit.** In N6, `git rm` and
  `git mv` had staged probe changes, and the first of two commits took them
  by mistake. The commits were redone before review.
- **A stacked branch rebases badly after a squash merge.** N7 was stacked on
  N6. After the squash merge of N6, a rebase replayed N6's commits; a
  cherry-pick of N7's own commits onto `main` worked.
- **A merge from `main` can move a shared table.** The REPL fix (#785) moved
  the compiler's enum tables into `ProgramDeclarations` while N6 was in
  review. The merge kept the new structure and the `E.A` key, and the review
  checked the REPL path again.
