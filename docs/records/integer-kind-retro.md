# Int kind mission (mission #824) — retrospective

**Status:** frozen at mission close, 2026-10-25. The decision is
`docs/decisions/integer-kind.md`. Every statement here comes from a command, a
pull request comment, or a CI run named in this file; this record is history
and it does not bind later work.

## Purpose

The mission added a signed 64-bit `Int` beside `Number`, with the bit
operators, two conversions, and the `integer` module. It serves the *memory*
control under "No high-level facility removes low-level control" in
`VISION.md`: a program can hold a 64-bit pattern as one numeric value with
every bit exact, and apply a bit operation to it in one step.

The mission started from the accepted decision of #823 and the tracking issue
#824, and ran nine nodes in one DAG, one branch and one pull request per node:

| Wave | Node | Issue | Pull request |
|---|---|---|---|
| 1 | N1: spec, probes, check scripts, expected-failure wiring | #825 | #835 |
| 2 | N2: native value layer and literals | #826 | #842 |
| 2 | N7: editors and LSP | #831 | #843 |
| 3 | N3: native operators | #827 | #844 |
| 4 | N4: native conversions, the module, and indexing | #828 | #848 |
| 5 | N5: the JVM backend | #829 | #854 |
| 6 (swapped) | N8: the QBE backend | #832 | #863 |
| 7 (swapped) | N6: the bootstrap interpreter | #830 | #874 |
| 8 | N9: the differential gate on both phases, and close | #833 | #890 |

The owner swapped waves 6 and 7 on 2026-10-24, so N8 landed before N6: the QBE
job compiles `bootstrap/loxpp_interpreter.lox`, and N6 puts Int operations in
that file. Without N8 first, each of the seven Int opcodes reached
`default: unsupported(ins.op)` in the QBE emitter, which is a compile failure
of the interpreter itself and not a probe failure, so no expected-failure file
could have covered it. The decision needed no change: only the order that
satisfies "QBE owes every Int construct the bootstrap uses" changed.

## Review rounds

Twelve review rounds ran before N9, in one referee-free mission except for N1:

| Pull request | Rounds | Findings |
|---|---|---|
| #835 (N1) | 4, plus 2 referee rulings | 14 findings, 2 of them design-level |
| #842 (N2) | 1 | none |
| #843 (N7) | 1 | 1 blocking finding (the Neovim fallback syntax) and 1 correction to it |
| #844 (N3) | 1 | 1 blocking finding (an opcode report that stopped at 64) |
| #848 (N4) | 1, plus 1 re-check after a CI fix | none in the code; the re-check followed a main move |
| #854 (N5) | 1 | none |
| #863 (N8) | 1 | none |
| #874 (N6) | 2 | 3 findings, all in the PR body and none in the code |

Seven of the eight merged pull requests needed one round or fewer for the code
itself. The one node that consumed three quarters of the review effort was the
spec node, and both of its design faults were in the check scripts, not in the
language text.

N1 also needed two approvals that are not review rounds: main moved twice
during it (#834's Number print format and its probe renumbering, then #840),
and the reviewer re-approved each merge head. That is a cost of a long node on
a moving `main`, and it is the reason the probe numbers shifted from the ones
its issue names: probes 243 to 246 became 244 to 247.

## The two referee rulings on N1

Both fired on the stagnation trigger, not on a dispute: the implementer
accepted every finding in every round.

**Ruling of 2026-10-23T19:26.** The trigger fired on `spec/01-lexical.md`
after three rounds and four blocking findings (R1, R2, R6, R9) on one section.
The referee verified the facts in its own worktree and ruled that the four
findings were one defect: the section stated each lexical rule more than once,
in four different forms (the grammar, the prose, the exception by spelling,
the range by value), so each round found the next pair of statements that gave
two answers for one program. The ruling mandatorily redesigned the section:
three normative rules (the longest match over the grammar, the range by value,
the unary-minus exception by value and on the grammar), one examples table
with a `Source`, `Tokens` and `Result` column, and a check that reads that
table. It wrote the class into the decision as constraint 5, "One normative
statement per lexical question".

**Ruling of 2026-10-23T20:08.** The trigger fired a second time, and the
harness named `tools/check_int_fault_rows.py`. The referee read the file name
as a pointer, not as the diagnosis, and found the same shape in three findings
(R7, R11, R13): **the check took its evidence from a wider scope than its claim
named.** The claim "each function has a probe" was satisfied by a `COVERS` tag
for a whole row; "the fault line calls the function" was satisfied by any line
of the probe; "the section states the exception" was satisfied by the phrase
anywhere in the file. It ruled the fix by deleting the prose checks, not by
tightening them, and wrote constraint 6 into the decision: a check reads one
object, never prose, and every site that adds a problem has one mutation on
record that makes it fire.

The second ruling is the more valuable one. It produced the mission's only
audit against a countable ground truth: 75 `problems.append` sites across the
six Int check scripts, each with one mutation that makes it fire, and after the
two deletions 72 sites, 6 `cannot read` sites and 1 `raise`. The same
mechanism, "the evidence is wider than the claim", is worth looking for in
every check script a later mission adds.

The playbook's warning that a trigger can fire on the wrong file and still be
right was exact here: the file name drifted from `spec/01-lexical.md` to
`check_int_fault_rows.py` to `check_int_literal_rejections.py` while the
mechanism stayed one.

## The permission denials

The referee's second ruling required two deletions: a prose check in
`tools/check_int_literal_rejections.py` and two prose checks in
`tools/check_operator_protocol_table.py`. The agent harness denied both
deletions. The implementer did not work around the denial, stopped, and
reported the block on the pull request with a table of what was done and what
was open.

A researcher reproduced the denials in its own worktree: the same edits passed
in its session, and the calls next to them were then denied. It concluded that
no hook and no permission rule in the repository or in the user settings named
the action, so the auto-mode classifier judged each call from the conversation
around it, and it showed that the same deletion can pass in one session and
fail in another. The researcher supplied the exact diff of the two deletions,
so the owner could apply them, and the deletions are on `main` today
(`grep -n exception_paragraph tools/check_int_literal_rejections.py` finds
nothing).

Two lessons. A permission denial is not evidence that the change is wrong, and
it is not a reason to find another way to make the same edit: the first
implementer stopped and posted, which is the correct response. And a rule that
a check must be able to fail is worth a denial: the deletions removed checks
that could not fail on the object they named.

## The bug classes

The decision names six classes before any node started. Each one fired at
least once during the mission.

| Constraint | What it predicted | What happened |
|---|---|---|
| 1. Undefined behavior under the test gate | Wrapping in `uint64_t`, the smallest Int over `-1i`, a range check before every double-to-int cast | The debug gate (`-fsanitize=address,undefined`) stayed green on every node; the probes for the smallest Int over `-1i` and for `int()` at both ends of the range are the ones that would have fired |
| 2. A new object kind touches every switch over `ObjType` | GC mark and free, `isMessage`, `stringify`, equality, hashing, the type name, the QBE C API | N2's review checked the site counts by `grep`, and N2 added a boxed `Int` at each of them as probes |
| 3. The constant pool keys by bit pattern, not by `==` | `1` and `1i` are two constants | Probe 191 holds both in one program; the JVM's `Long.equals(Double)` is `false` for the same reason |
| 4. Four forms change meaning under the longest match | `1in`, `1if`, `1.5in`, `1.5if` | Probes 240 to 242 and 244 pin them, and the tree-sitter step keeps them named; no program in the repository wrote one |
| 5. One normative statement per lexical question | The class of the first referee ruling | The redesign and the examples table; every scanner implements the rules, and the check reads the table |
| 6. A check reads one object, never prose | The class of the second referee ruling | The prose checks deleted, the 72 sites audited with one mutation each |

Constraint 5 and constraint 6 are the two that a review cannot find by reading
a diff of language code. Both came from a referee, not from a reviewer.

## Work that changed the mission's edges

- **#834 (the Number print format) closed mid-mission, by #849.** It was not a
  node of this mission: N1 found the gap in the spec before the mission ran.
  It landed on 2026-10-24, changed the Number print form to the shortest
  decimal that round-trips, and added the Int and Number print parity probes
  that the decision's "Printing" section cites. Its merge renumbered four Int
  probes (243 to 246 became 244 to 247) under N1's feet.
- **#883 and #884 are the mission's last cleanup.** At close, no
  `tools/spec-probe-xfail/*.txt` file held a probe line, so the six files and
  the C++ loader that reads them (`specProbeXfail` in `test/corpus.h`) had no
  purpose. The cleanup ran as an open pull request (#884) in parallel with
  N9, by the owner's decision, so N9 does not touch those files and says so in
  its decision text. A mission that ends with every expected-failure file
  empty also owns the removal of the mechanism, and that removal was the one
  item of the mission that could not be in one node's branch.
- **#856 stays open.** `BootstrapStackOverflow` times out under the `debug`
  preset on a 12-core host, on `main` too. It makes the local `loxpp_gate`
  `native` scope red for every node, so the native debug matrix was graded in
  CI for the whole mission, and no node could use the local native gate as its
  checkpoint. It is a harness fault, not a language fault, and it outlived the
  mission.

## Process faults

1. **A hazard comment carried numbers from memory, not from a command.** The
   N1 hazard comment on #833 said "the ten static-error probes (166 to 175)".
   The ground truth is 22 probes that carry `// STATIC-ERROR`, found by
   `grep -l STATIC-ERROR test/translation-probes/*int*.lox`; 166 is valid and
   176 is a static error. The same comment named probe 197 as the flag set and
   198 as the bitboard, which was stale after the renumbering. The N9 plan
   corrected both from the source before any work started, and N9 ran the
   compile phase against the command, not the range.
2. **The stagnation trigger named the wrong file twice in N1.** The referee
   ruled correctly both times, so no ruling was lost, but the trigger's file
   label cost a round of confusion each time. The playbook already says to
   read the label as a pointer. This mission is the evidence.
3. **The DAG order and the merge order disagreed for one pair.** The DAG ran
   N7 after N2 and before N3, but N3's pull request merged 14 minutes before
   N7's on 2026-10-24. Nothing broke, because the two touch disjoint files, but
   a later reader of the tracking issue would see an order that the merge log
   does not show.
4. **The closing node's fail-proof list named a mutation that cannot work.**
   The plan said "break the bitboard mask and watch the JVM and QBE diffs
   diverge". A differential gate compares one program on two consumers, so a
   change to the program moves both sides and the diff stays `MATCH`; N9
   measured exactly that. The run phase can only be made to fail by breaking a
   consumer. N9 ran the working mutations instead: the JVM emitter's `>>>`
   lowering, the QBE emitter's `>>>` lowering, the bootstrap interpreter's
   fractional-suffix rejection, and the shared front end's decimal range
   check. Each one turned the named gate red.
5. **A partially rebuilt tree produced a false red.** A QBE probe run reported
   14 failures because `build/loxpp` came from one image and the QBE runtime
   objects from another; a full rebuild in one image reported 184 pass and 0
   fail. The playbook's "Rebuild fully before trusting a test result" is the
   rule; this is one more data point for it.
6. **A long node on a moving `main` costs approvals, not just conflicts.** N1
   needed three approvals because main moved twice. The cost is visible in the
   review-round count.

## Lessons

- **Grade the compile phase by a command, not by a range.** A static-error
  class is the set of probes that carry the marker, and a range in prose loses
  members when a `main` merge renumbers the corpus. N9's compile-phase ground
  truth is one `grep -l`, and the count it gave, 22, is the number the
  decision and the pull request both hold.
- **A gate matrix is a table of CI step names.** N9 found every consumer ×
  phase cell already wired, so it added no check script: the compile phase is
  graded per program by `tools/diff_runtimes.py`, and constraint 6 forbids a
  check that reads prose. The work was to run each cell and record the count,
  which is the part a table cannot do for itself.
- **Prove a differential gate with a mutation in a consumer.** A mutation in
  the program moves both sides of the comparison. A mutation in a consumer
  moves one side, and it is the only mutation that can turn the gate red.
- **State a lexical rule once and make every other sentence an example.** The
  first referee ruling is general: two statements of one rule give two
  answers, and each review round finds the next pair. A table of examples with
  a check is the countable form of the same rule.
- **A check's evidence must be no wider than its claim.** The second ruling is
  general too, and it applies to every mission's check scripts, not only to
  this one.
- **Stop at a permission denial.** The block is not a finding against the
  change, and working around it hides the block from the owner. N1's block was
  resolved because the implementer stopped and posted.
- **A one-site consumer defect belongs to the closing node; a redesign does
  not.** N9's bootstrap mutation was one site and it was fixed by reverting
  the mutation; a compile-phase gap that needs a redesign would have stopped
  the run.
- **The closing node cannot own another open pull request's files.** #884 held
  the six expected-failure files and the loader, so N9 stated the cleanup in
  its decision text and touched nothing. A parallel cleanup needs its own
  owner from the start.
