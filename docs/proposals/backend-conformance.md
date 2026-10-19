# Observable conformance for the secondary backends

**Status:** proposal. Waits for the owner's approval and for the owner's
change to `VISION.md`. It answers #722 and widens that issue to the JVM and
QBE backends.

## Why

This proposal serves "The backends" in `VISION.md`: "The native VM is the
product. The other backends exist to serve it, and each one owes conformance
only for its role."

Today each secondary backend owes more than its role needs. CI compares the
JVM, QBE, and the bootstrap interpreter with native on error text, on the
token, line, and recovery of each compile error, and on which faults are
fatal. These are properties of how native reports a wrong program. They are
not properties of what a correct program does.

The cost is measurable. From 2026-09-19 to 2026-10-19, 53 of the 207 commits
on `main` name the bootstrap in their subject. Most of them make an error
path of the bootstrap match native. Each one cost a review cycle and a CI
gate. Few of them advanced the language. The gates also make each change to
a native message a change to every other backend.

The roles do not need this:

- The bootstrap is a capability test: "a practical language can implement
  itself". A practical implementation runs correct programs correctly and
  rejects incorrect ones. Its diagnostics do not have to be the diagnostics
  of native.
- The JVM is a semantic oracle for "the meaning of Lox++ bytecode". The
  meaning of a correct program is that meaning. The text that native prints
  for a fault is a choice of native.
- QBE "owes what the current experiment needs". AOT and JIT experiments need
  correct output, not native's error text.

## The rule

The spec defines two conformance levels. `VISION.md` assigns a level to each
backend.

- **Full conformance.** The implementation follows every rule of the spec,
  including message texts, compile-error position and recovery, limits, and
  the fatal or catchable disposition of each fault. Native has full
  conformance.
- **Observable conformance.** The implementation matches native on the
  observable behavior of every valid run, and on a small set of properties of
  invalid runs. It may differ from native in the closed list below. The JVM,
  QBE, and the bootstrap interpreter have observable conformance.

`spec/README.md` already says that "the observable behavior" must match. This
proposal says exactly what that behavior is.

### Valid run

A **run** is one program with one set of inputs: the source, the program
arguments, the environment, stdin, and the files and peers it reads.

A run is **valid** when native compiles the program and executes it with no
fatal fault. A valid run can end normally, through `exit(n)`, or with an
uncaught catchable fault or an uncaught `throw`.

The definition uses runs, not programs, because one program can fault on
one input and not on another.

### What must match native

| Property | Applies to |
|---|---|
| stdout, byte for byte | every valid run |
| exit status | every valid run |
| effects on files, sockets, and child processes | every valid run |
| the deferred calls that run, and their order | every valid run, including the unwind of an uncaught catchable fault |
| `e.kind` and `type(e)` of a caught fault | every valid run |
| whether compilation rejects the program (exit 65, nothing runs) | every program, in both directions |
| a fault that reaches the top level ends the program with exit 70 and text on stderr | every run, on that backend |

Map iteration order stays unspecified (`spec/03-types.md`). Nothing in this
proposal changes it.

### What may differ (closed list)

An implementation with observable conformance may differ from native only in
these ways:

1. **Diagnostics.** The text of every error message, at compile time or at
   run time, caught or not. This includes `e.message` and `str(e)` of an
   `Error`, all of stderr, stack traces, the token and line of a compile
   error, the recovery after it, and the number of compile errors it reports.
2. **Fault disposition.** A fault that the spec makes fatal may be
   catchable, and pending deferred calls may run during its unwind. A run
   that reaches such a fault is not valid, so nothing about it binds except
   the last row of the table above.
3. **Limits.** Call depth, compile-time size limits, memory use, and the
   timing of garbage collection. A run that native completes can raise
   `StackOverflowError` or a limit error on another backend.
4. **Scheduling.** Fewer host threads than native. A blocking call may block
   more than its own task, up to every task of every worker. Workers may not
   run at the same time. A program whose progress needs two workers to run
   at the same time can behave differently.
5. **Speed.**

The list is closed. A difference that is not on it is a defect of that
backend.

### Why the valid-run set is the right set

`AGENTS.md`, "Plans that limit their scope", asks a scope limit to state its
set, the property the set must satisfy, and why the two are equal.

- **The set:** the valid runs, plus compile rejection and the top-level exit
  status of every run.
- **The property:** every behavior that a correct program can depend on.
- **Why they are equal:** a correct program does not reach a fatal fault,
  because native halts it there. It does not read its own diagnostics:
  the standard library has no call that prints to stderr, and an `Error`
  value gives its kind for decisions. `e.message` is text for a person, like stderr. A
  program that branches on `e.message` already depends on text that the spec
  may change. Limits and scheduling are already implementation-defined or
  already excepted for the bootstrap. The dynamic call graph does not change
  this: validity is a property of the whole run on native, not of one
  function.

One program can see the diagnostics of another: a Lox++ program can `run`
another Lox++ program and read its `stderr`. That parent depends on the
diagnostics of one implementation. It is valid, and its output is correct
only on the implementation it was written for. No program in the repository
does this today.

## Effect on each backend

| Backend | Keeps | Loses |
|---|---|---|
| JVM | The oracle for the meaning of every valid run, including caught faults, defer order, coroutines, workers, and the self-hosted interpreter running on it. | The oracle for error text and for the Fatal Runtime Errors table. Native's own checks cover those. |
| QBE | Byte parity on valid runs of the probes, the examples, and the self-hosted interpreter. | Exact fatal fast-path behavior. A fatal probe checks only the exit status. |
| Bootstrap | Valid-run parity, compile rejection, workers on one thread (now a general scheduling rule), lower call depth (now a general limits rule). | Every diagnostic gate and every fatal-path gate. |

The JVM change is the real cost of this proposal. Today the JVM catches a
native fault-table defect, because two implementations disagree. After this
change, only native's own spec checks guard the fault table. I accept this
because the fault table is a spec table with probes, and native must pass
those probes at full conformance.

## Consumers to update

### Binding documents

| Consumer | Change |
|---|---|
| `VISION.md`, "The backends" (owner only, no code in the PR) | JVM: "Observable conformance with native where it serves as an oracle. It does not need features that exist only to give native code low-level access, such as FFI or a JIT." QBE: "Observable conformance for what the current experiment needs. A language change does not have to wait for QBE." Bootstrap: "Observable conformance." One line under the table: "The spec defines the conformance levels. Only native owes full conformance." |
| `spec/README.md` | A new section, "Conformance", that defines the two levels, the valid run, the table of what must match, and the closed list. |
| `spec/04-semantics.md`, "Implementation-Gated Exception: the Bootstrap Interpreter" | Delete. Its two exceptions become items 3 and 4 of the closed list. The call-depth paragraph under "Calls" points to the new section. |
| `spec/04-semantics.md`, the Runtime Errors and Fatal Runtime Errors tables, and the other `Message` columns | No text change. Each table states that its `Message` column and its `Catchable` column bind at full conformance. |
| `AGENTS.md`, "Plans for a language change" | Rule 1: a secondary backend owes observable conformance. Rule 3: "its output matches the native VM within observable conformance". Rule 5: the compile phase compares rejection (exit 65), not text. |

### Non-binding documents

| Consumer | Change |
|---|---|
| `docs/reference/bootstrap-accepted-fatal-limits.md` | Delete. Item 2 of the closed list covers its eight kinds and every other kind. |
| `docs/reference/bootstrap-stack-depth.md` | Point to item 3 of the closed list. |
| `docs/decisions/qbe-backend.md`, S7 | "Byte-identical to native" applies to valid runs. |
| `docs/decisions/concurrency-model-decision.md`, the backend table | The bootstrap row cites observable conformance. |
| `docs/proposals/blocking-call-suspension.md`, F4 and the consumer table | Item 4 of the closed list covers the bootstrap. That mission needs no new exception. |

### Gates

| Gate | Change |
|---|---|
| `tools/diff_runtimes.py` | A new mode, `--observable`. The compile phase compares the exit status (65) and empty stdout, not stderr. The run phase is the same as today: stdout and exit status. Each secondary-backend step uses the mode. |
| Fatal-fault probes in `test/translation-probes/` | Move to `test/translation-probes/fatal/`. Only native's spec-probe step and native's differential steps read that folder. The JVM, QBE, and bootstrap walks skip it. |
| `tools/check_spec_probes.py` | A new option, `--observable`, which ignores `// STDERR:` lines. CHECK and STATUS lines still bind. |
| `tools/check_fault_table.py` | For a catchable row: compare caught or not, `e.kind`, and `type(e)`. Drop `e.message` and `str(e)`. Fatal rows run on native only. |
| `tools/check_fatal_table.py`, `tools/fatal_table_xfail.txt` | Native only. |
| `tools/check_qbe_s6_errors.sh` | Its two fatal fast-path probes check the exit status only. Every other probe stays. |
| Bootstrap steps: fatal fast path, re-entrant overflow, stack-overflow catchability, resolver error parity, uncaught-throw report, wrong-argument-count fatal path, deferred-callable fatal fault | Remove the CI step, the ctest entry, and the tool. |
| `tools/check_bootstrap_error_kinds.py`, `tools/run_bootstrap_error_kind_causes.py` | Keep the catchable half: two causes must not share one `kind`. Drop the fatal half. |
| `tools/check_bootstrap_headroom.sh` | Keep the example-corpus half. Drop the ceiling ratios of the recursion probes. |
| Valid-run gates on every backend: examples, coroutine probes, print lines of two workers, file durability, OS stdlib, sockets and subprocess, NUL and CR byte probes, operator overload, match-arm-break, the self-hosted interpreter on the JVM and on QBE | Keep. |

Every new mode needs a proof that it can fail (`AGENTS.md`, "Engineering
rules"). For `--observable`: a program that native rejects and the other
runner accepts must still report DIVERGE, and so must a program that the
other runner rejects and native accepts.

## Order of work

1. The owner changes `VISION.md`. The PR has no code.
2. One PR changes `spec/README.md`, `spec/04-semantics.md`, `AGENTS.md`, the
   non-binding documents, and the gates. A spec change without its gates
   would leave CI enforcing the old rule.
3. Close each issue that the new rule puts out of scope, with a link to
   #722. The pull request for this proposal lists them.

This is one PR after the vision change, not a mission. It changes no
backend code.

## Alternatives

- **Only the bootstrap.** This was the first scope of #722. The JVM and QBE
  have the same class of gate, and #703 and #730 are the same class of
  issue. One rule for every secondary backend is simpler than one rule per
  backend.
- **Drop compile rejection as well.** This would also put #639, #678, #680,
  and #699 out of scope. I keep rejection because a compile phase is part of
  an implementation of the language, and because a backend that accepts what
  native rejects runs programs that native never runs. The gate is cheap once
  it compares only the exit status. The JVM and QBE pass it by construction:
  they compile with native's front end (`loxpp --target`).
- **Keep `e.message` binding.** A valid program can print it, so this is the
  stricter reading of "observable". It keeps every catchable message a
  parity item on every backend. Six examples print `.message` today, and
  they pass. If one diverges later, change the example. Every Lox++ program
  is in this repository.
- **Fatal faults stay fatal everywhere.** This keeps the Fatal Runtime Errors
  table binding on every backend. It is the most expensive item: #666, #685,
  #707, #713, #720, and #730 are all this class, and
  `docs/reference/bootstrap-accepted-fatal-limits.md` already records eight
  kinds that the owner accepted as different.

## Open questions

1. Does the owner accept the loss of the JVM as an oracle for error text and
   for the fatal table?
2. Does compile rejection stay binding (this proposal), or does it go too?
3. Must a language change still update the bootstrap in the same PR? This
   proposal says yes, for valid-run behavior only: without that, the
   capability claim decays without notice.
