# Referee rulings, August to September 2026

Mined on 2026-10-16 from every binding referee ruling that a multi-agent
mission produced between 2026-08-14 and 2026-09-22. The sources are the
`[Researcher]` comments on the pull requests and issues named below. Each
entry records what the referee verified, what it ruled, and the lesson that
outlives the node. The rules distilled from these rulings are in `AGENTS.md`
and in `docs/process/multi-agent-playbook.md`. This record holds the evidence
behind them.

## The lessons, and the rulings that taught them

| Lesson | Rulings |
|---|---|
| Three rounds of findings in one file are one wrong premise, not three bugs. Find the mechanism, then close the class. | #101, #103, #113, #115, #142, #148, #231, #237, #312, #314, #346, #352 |
| Restate a mechanism the VM or the compiler already has. An inference over bytecode patterns breaks on the next shape. | #101 (amendment 3), #103, #231, #236, #237 |
| One choke point closes a class. A patch per call site invites the next round. | #115, #142, #148, #231, #312 |
| A completeness claim is a check, or it is nothing. Prose drifts. | #115, #135, #140, #142, #298, #339, #345, #346 |
| Count against a ground truth: a spec table, a grep, a small scanner. Not against the cases a reviewer happened to reach. | #245, #339, #345, #346 |
| A check that cannot fail certifies nothing. Prove the failure before the fix. | #101 (rounds 10 and 11), #142, #153, #364 |
| The trigger's file name is a pointer, not the diagnosis. Verify the facts yourself. | #131, #140, #148 (second ruling), #314, #346 |
| Bound the next round: name the exact actions, and the only findings that may still block. | every ruling from #113 on |
| Two mechanisms for one fact drift apart: two thresholds, two roots, two close rules, two fatal paths. | #101 (amendment 2), #312, #357 |
| A disposition comes from a local value at the call site, never from interpreter state. | #364 |
| When a check must stay able to fail, change the assertion, not the program. | #364 |
| Measure through the boundary a real program uses. | #126 |
| Push after every commit. A review of an unpushed tree is a lost round. | #140 |
| A ruling can be wrong. Amend it in the open and say what was wrong. | #101 (three amendments) |
| Build the prototype before ruling. A design argued on paper lost two rounds; a design measured on the corpus held. | #101, #103, #312, #314, #357, #364 |

## JVM backend mission (#96 to #116), August 2026

### PR #101, capture analysis (node N3): seven rulings in one PR

The reviewer found a new defect in the capture pass in rounds 1, 2, 3, 6, 7,
10 and 11. The referee ruled seven times.

- **Decision 1.** Replace the linear span tracker with a CFG dataflow. One
  linear state cannot describe two mutually exclusive paths. The referee
  prototyped the design and swept the corpus before ruling.
- **Amendment 1 (R17).** The first decision assumed every captured local is
  open on every path. The compiler emits a close for every statically
  captured local, even on a path where the capture never ran. A static
  overlay was added.
- **Amendment 2 (R20).** The merge throw of decision 1 rejected a legal
  program. One slot open from two captures at a merge is one cell: unify
  them, as `captureUpvalue` does.
- **Amendment 3 (R22).** The overlay premise was wrong too. The referee read
  the compiler in full, derived the emission contract, and deleted the
  overlay. The slot a `CLOSE_UPVALUE` reclaims is the stack height before it
  minus one, which is what the VM executes. The contract derivation also
  found a compiler bug the whole suite had missed: a match arm body with no
  final expression stole a pattern binding as its value.
- **Unblock decision.** Reproduced that compiler bug, verified the fix end
  to end, found one more offender in the tests, and ordered a separate
  prerequisite PR.
- **Round 10 (R25 to R27).** Dead code voids the premise "every reachable
  close resolves to a range". Close resolution became total: four outcomes,
  no throw on legal input. The reviewer's own fix was rejected with two
  programs that still broke under it.
- **Round 11 (R28, R29).** Option (b): the test file's design was right. The
  trigger had counted two unrelated classes, a check without teeth and a
  `vector<bool>` build hazard. The node converged with a one-line fix and a
  convergence rule.

**Lesson.** Every layer that survived restates one VM or compiler mechanism.
Every layer that failed was an inference over bytecode. A referee that is
wrong three times in one PR, and says so, still ends the loop faster than one
more patch.

### PR #103, abstract-stack reconstruction (node N2)

A POP that reclaims a local and a POP that discards a temporary compile to
byte-identical chunks, so no source-recovery rule can tell them apart. Ruled
for a canonical rule: a persistence test at every POP, where a later push on
top of the exposed cell proves a local. A tail theorem makes the brief's group
structure a derived guarantee. Prototyped: 32 bad runs to 0 over 603 chunks.
**Lesson.** When two sources compile to one chunk, pick a deterministic
canonical reading and test it. Do not try to recover the source.

### PR #113, classes and methods (node N8)

Each round found one more consumer of a stale tracker. The referee wrote three
three-line programs and found that `main` already printed silently wrong
output for every match-result consumer. Ruled the design wrong: one depth-0
mechanism, with a cross-check that throws at emit time when two estimates
disagree. **Lesson.** Silent wrong output on `main` forbids deferral. A loud
throw where two estimates disagree is a strict improvement over a silent
guess.

### PR #115, match and enum dispatch (node N10)

Nine shapes of one class: a consumer whose bottom operand is a folded match
result, each with its own depth branch. Ruled for one table, exhaustive over
`Op` with no default, and one normalization point before dispatch, with the
private rules deleted in the same commit. An analysis gap outside the node was
deferred by ruling, with a named owner. **Lesson.** An enumeration that
nothing forces to be complete cannot carry a completeness claim. A table the
compiler checks can.

## CLR backend mission (#125 to #153), August 2026

### PR #126, CLR runtime library (node C-RT)

Three rounds on the OS boundary. Ruled (a): a BCL call was used as the equal
of a C call with no proof at the edges. Upheld the file-type findings
(`File.Exists` is true for `/dev/null` and for a dangling symlink), and
ordered a P/Invoke of libc plus a permanent differential sweep against the
native oracle. Overturned the `env()` finding: the reviewer had measured an
in-process `SetEnvironmentVariable`, which .NET documents as a delete.
**Lesson.** Measure through the boundary a generated program will use, and
close a surface with a sweep, not with one more case.

### PR #128, straight-line CIL emit (node C-N4)

Ruled (b): three findings in one test file were three classes, provenance
text, missing run checks, and a stale PR body, not one design. Introduced the
termination rule that every later CLR ruling reused: a new blocking finding is
legal only as a gate failure, a reachable spec violation, or a defect in the
lines the final commit changed. **Lesson.** Name the finding classes before
ruling. Three unrelated classes are not stagnation.

### PR #131, functions and calls (node C-N6)

Ruled (b). Round 3's only blocker was a stale PR body, anchored on the gate
script only because the body cited it. Upheld two gate repairs: the
error-probe loop had to require `Lox.LoxError` in stderr, because a missing
runtime satisfied it; and the gate had to run the six newly runnable examples.
**Lesson.** A loop that cannot tell a host failure from a language error is
not a gate.

### PR #135, closures and upvalues (node C-N7)

Ruled (a) on a hand-written completeness claim: "the group holds every
runnable example" was asserted in a comment and disproved by measurement three
rounds running. Ordered a corpus sweep that fails when an unlisted example
matches native, and deleted the prose it replaced. **Lesson.** A sentence of
the form "these are all of X" is enforced by a check or removed.

### PR #140, aggregates and iterators (node C-N9)

Ruled (b): the real series was two review passes, because five fix commits sat
unpushed while round 2 read the old head. Wrote the prose rule that bound every
later CLR node: a scope claim in a source, test, or tools file is a mechanism
statement, a check, or absent; never an enumeration, a mission-relative term,
or a measurement pinned to a moving reference. **Lesson.** Push after each
commit, and keep counts in the commit and the PR, which are stamped to one
head.

### PR #142, match and enum dispatch (node C-N10)

Ruled (a), scoped: 36 sites formatted a slot index into CIL text with no
check, so a consumer the spill-sizing pass missed emitted a module that exited
0, assembled, and died at JIT time naming no instruction. Ordered one choke
point for every slot index. **Lesson.** The node's own new mechanism had no
check that could fail. Central enforcement, not one more test per term.

### PR #148, differential corpus gate (node C-N11), two rulings

First ruling: the trigger files were fine, and the repeated class lived in the
runtime, outside the diff. The bound built-in method model cached a method per
instance and compared by name, where native wraps a fresh bound value per get
and compares by identity. Ordered the deletion of the name arms and the caches.
Second ruling: (b), after the redesign landed in full. The round-4 findings
were in lines the fix had added, and closed in one commit. **Lesson.** A
comment that describes the native VM before a commit changed it is a model
error, not a wording error.

### PR #153, self-hosted interpreter gate (node C-N12)

Ruled (a) on the relation between the native stack raise, the ambient limit,
and a probe's ability to fail. Found a fact no round had named: the probe was
still disarmed with the round-3 fix in place, under an unlimited ambient
limit. Ordered a paired MUST-PASS and MUST-FAIL row that the gate arms itself
on every run, and a raise that never lowers. **Lesson.** A probe whose ability
to fail depends on ambient state the gate does not pin is not armed.

## Editor-tooling mission, September 2026

### PR #188, tooling parser (node N5)

Three findings of one class, unbounded depth. The referee reproduced a stack
overflow in the destructor of a 45,000-term left-leaning tree, showed that a
parse-time cap already fixed the capped shapes at a million units, and showed
that an iterative teardown would protect neither the resolver's recursive walk
nor an LSP worker thread's small stack. Ruled for one counter that bounds
nesting and chain length together. **Lesson.** One bound at the source
protects every consumer. A fix at one consumer protects one.

## Non-local control flow mission (#223), September 2026

### PR #231, THROW and PUSH_HANDLER against the CFG (node X1)

Round 1 patched one edge-adding site. Round 2 found three more, and the
referee found a fifth in five minutes. Ruled: teach the one existing `addEdge`
function about handler entries, delete the point patch, and add the assertion
that fires if any site bypasses the funnel. **Lesson.** Completeness is a
property of the code when every site goes through one function, and a property
of the review when it does not.

### PR #237, JVM try/catch regions (node X4)

Shapes (A) and (B) and issue #240 were one bug. Handler seeding was a single
lookahead pass where it had to be a fixpoint, and the catch-bound value was
never recognized as a declared local. The referee replaced the round-2
backward-scan hack instead of stacking a fix on it. **Lesson.** A catch
entry's state is declared, not discovered. Extend the declared contract
instead of searching for it.

### PR #236, CLR try/catch regions (node X5)

The catch region's end had been found by scanning for the next CFG label,
which a branch inside the catch body also placed. Replaced with the compiler's
own declared `POP_HANDLER` offset. **Lesson.** Derive a boundary from declared
data, never from incidental text.

### PR #245, bootstrap interpreter (node X7)

A small parser that balances brackets counted 76 `setError` calls and exactly
the 17 two-argument sites round 3 had named, with no 18th. The referee found
that the wrapper masked native crashes as exit 0, which is why three rounds of
stale sites stayed invisible, and fixed it at the wrapper. **Lesson.** Audit
against a countable ground truth. A gate that masks a crash hides every
defect behind it.

## Correctness backlog (#261), September 2026

### PR #298, bootstrap fault kinds (node F6)

Ruled (a), narrow: the detection logic was right; the hand-written counts and
the claims about native's message text were wrong round after round. Ordered:
no count in a comment the script does not check; a baseline that carries
kind, message, and a judged-or-recorded status; no comment that states what
another implementation prints. **Lesson.** A number the script checks is
code. A number it does not check is prose, and prose rots.

### PR #302, CLR file flush at exit (node F9)

Ruled (b). The loop's driver was that every code push moved the head and the
body's CI evidence was then one head behind. It stopped when the code stopped
moving. The script's unmeasured outcomes were filed as one issue. **Lesson.**
Separate a stale-evidence loop from a design loop. The first ends with a body
edit.

## Catchable stack overflow (#271), September 2026

### PR #312, native overflow sites (node N1)

Ruled (a): three rounds found two design faults one symptom at a time. Class
A, two thresholds at the value-stack site with different rounding: delete the
soft one. Class B, a value in flight not rooted while the unwind runs Lox++
code: `handleThrow` owns the thrown value for its whole duration, one root for
both callers. Class B was latent on `main` for a thrown temporary.
**Lesson.** A redesign that deletes a second threshold or a second root closes
the class. A patch that adds one opens the next.

### PR #314, JVM frame ceiling (node N2)

The trigger fired on the probe README. The mechanism was the life of the
unwind guard. The referee wrote a program no round had named, a deferred throw
that replaces an unrelated fault, which an R8-only fix would not close. Ruled:
the exceptional-path handler is the only code that knows which fault a
deferred throw replaces, so it passes that fault in. Prototyped, all probes
green. **Lesson.** Read the trigger's file as "what do these rounds share",
not as the diagnosis.

## Fault taxonomy (#288) and its bootstrap tail (#356), September 2026

### PR #339, the native fault ground truth (node N1)

Ruled (b) with one binding rule. Every finding was a claim about reachability
or message text written from reading the code, not from running a program.
Every fatal row became one runnable program and one literal message. An
unreachable site goes in a paragraph that names the guard. The checkpoint is a
script the reviewer runs. **Lesson.** A row in a fault table is evidence only
if a script can run its Example and compare its Message.

### PR #345, JVM fault sites (node #333)

Ruled (a) against the audit, not the code: `grep -c "throw"` counts sites
that already throw and cannot find a site that does not. The correct ground
truths were the spec tables and the seven `instanceof LoxInstance` sites, one
of which was the open door. The CI job became the checkpoint, after three
rounds each found a gate the checkpoint had not run. **Lesson.** A count of
the right thing is countable. A count of the wrong thing is exact and
useless.

### PR #346, bootstrap fault sites (node #335)

Ruled (a): the manifest had a human-only judgment column, so each round a
person re-read 44 sites with a slightly different lens. Ordered a differential
check against native, with a coverage rule and an alias rule the script
enforces. The referee found three more aliased sites itself. **Lesson.** When
a node's deliverable is "N sites, 0 wrong", the tool that prints the count must
be the one that can fail on a wrong site.

### PR #352, the differential fault table (node #336)

Ruled (b) with one closing commit: the script accepted evidence weaker than a
row's claim, a compile error as fatal, a halt before the after-marker as
caught. Ordered a mandatory anchor per row, validated at start-up.
**Lesson.** Say in the script what evidence a row must carry. Otherwise each
round closes one entry point.

### PR #357, bootstrap defer of a non-callable (node #351)

The referee measured that the bootstrap interpreter had no fatal mechanism at
all, found that the PR edited a loop that never runs for a valid program, and
that its new kind existed in no file. Ruled for a four-line `fatalError` that
halts the host process, rejected a `fatalFlag` design it had also built,
placed the mechanism in the first node that needs it, and corrected the
mission brief. **Lesson.** A helper lands in the first node that calls it. A
flag that four sites must honor breaks at the fifth.

### PR #364, split fused bootstrap fault sites (node #348)

The referee found a regression the checkpoint could not see: a disposition
read from interpreter state left behind by an unrelated property read made a
catchable call fatal. Ruled: delete both fields, pass a parameter. Rejected a
weakened probe, a `try` removed so the check passed with the fix absent, with
the rule "change the assertion, not the program". Ruled that a missing state
line is accepted only for a clean halt whose exit code equals native's.
**Lesson.** A disposition comes from a local value. A check that passes
without the fix is not a check.

## Defer semantics (#414), September 2026

### Issue #326, one deferred call too few

Ruled neither of the issue's two options. The count difference was real, but
it was the `return EXPR` ordering bug of #311, already fixed, and the unwind
was correct all along. Proved with one variable that recorded when each
deferred call ran. **Lesson.** Before choosing between two fixes, check
whether `main` already contains a third.

### Issue #426, CLR overflow with a defer

Root cause found while examining #326: the CLR runtime copied native's unwind
guard but not its frame reserve, so the deferred call at the ceiling hit the
ceiling again. Native and the JVM both reserve frames. **Lesson.** When you
copy a guard, copy the room the guard depends on.
