# The spec binds behavior, not diagnostics

**Status:** proposal. The owner set its direction and answered its design
questions on PR #731 (2026-10-19). Waits for the owner's approval and for the
owner's change to `VISION.md` (PR #732).

It answers #722 and widens it. #722 asked how much the bootstrap interpreter
must match native. This proposal changes what the spec asks of every
implementation, native included. Then no backend needs its own exception.

## Why

`VISION.md`, "The backends": "The native VM is the product. The other
backends exist to serve it, and each one owes conformance only for its role."
The owner plans more backends to experiment with implementations of the
language. Each one must be able to conform to the spec without copying the
internals of native.

The spec does not allow that today. It is coupled to native in four places:

1. **Messages.** The spec gives the message text of most faults, and
   `Error.message` "holds the same text the implementation reports". CI
   compares the text of every compile error and of many run-time faults
   across implementations.
2. **The fatal set.** The Fatal Runtime Errors table defines itself as the
   faults that "the native implementation" does not deliver as catchable. It
   says: "this is a record of current native behavior, not a design
   decision". Every other implementation must copy that record.
3. **Exit codes and reports.** CI requires exit 65 for a compile error and
   exit 70 for a run-time fault, and compares the first line of stderr.
4. **Named exceptions.** The spec names the bootstrap interpreter in an
   "Implementation-Gated Exception" for workers and for call depth.

The cost is measurable. From 2026-09-19 to 2026-10-19, 53 of the 207 commits
on `main` name the bootstrap in their subject. Most of them make an error
path of the bootstrap match native. Few of them advanced the language. Each
new backend would pay the same cost again.

## The rule

The spec binds what a program can observe and depend on. It does not bind
how an implementation reports an error. One conformance rule applies to
every implementation. Native has no special level. It is the product, and
its own choices (its messages, its exit codes, its limits) are documented in
`docs/reference/`, not in the spec.

### 1. Static errors

The spec states which programs have a static error. A conforming
implementation:

- rejects such a program: it runs none of it, writes nothing to stdout, and
  exits with a non-zero status;
- writes a report on stderr;
- accepts every program that has no static error.

The report text, the token and line it names, the recovery after the first
error, and the number of errors it reports are implementation-defined.

### 2. Faults

The spec states each fault: the cause that raises it. Faults are in two
sets.

- **Catchable faults.** The spec lists them, each with its `kind`. Every
  implementation delivers such a fault to a `catch` block as an `Error`
  value with that `kind`. The `message` field is a String whose text is
  implementation-defined.
- **Halting faults.** The spec lists their causes, with no `kind` and no
  message. A halting fault halts the program. An implementation may make a
  halting fault catchable instead, with an implementation-defined `kind`. A
  program that catches a halting fault is not portable.

The catchable set is a floor: an implementation must make each fault on it
catchable, and may make more faults catchable. A fault must still occur in
every case the spec names. An implementation that gives a value where the
spec raises a fault does not conform.

### 3. Halts

A program **halts** when a fault is not caught: a halting fault, a catchable
fault that no `try` statement catches, or an uncaught `throw`. When a program
halts:

- the exit status is non-zero;
- every effect before the fault binds: the output to stdout, the files, the
  sockets, and the child processes, in the order the spec defines;
- the implementation writes a report on stderr. Its text is
  implementation-defined. This keeps "fail loudly" in `VISION.md`.
- whether pending deferred calls run before the program ends is
  implementation-defined, and so is what they write.

`exit(code)` is not a halt. It ends the program with exactly that status. A
program that ends normally exits 0.

### 4. Limits

Each limit is implementation-defined, for every implementation:

- call depth (self tail calls still run in constant depth; that is a
  semantic rule, not a limit);
- compile-time sizes: the arguments of a call, the parameters of a
  function, the elements of a list literal, the pairs of a map literal, and
  every other size the spec gives today;
- the nesting depth of a canonical string;
- memory use and the timing of garbage collection.

Going past a limit is a static error or a fault. The spec does not say
which, and gives no message.

### 5. Scheduling

Whether workers run at the same time is implementation-defined. A blocking
call may block more than its own task, up to every task of every worker.
Every other rule of workers and tasks holds: separate globals, messages as
the only shared values, the faults, and the order rules. A program whose
progress needs two workers to run at the same time is not portable.

### What does not change

Everything else in the spec binds as it does today: the grammar, every value
and its canonical string, evaluation order, the semantics of every
construct, the stdlib, `defer` on a normal return and on a caught fault, and
the order of output of a program that does not halt. Map iteration order
stays unspecified.

## A portable program

A program is portable when its output is the same on every conforming
implementation. It is portable when it:

- does not catch a halting fault;
- does not print `e.message`, or `str(e)` of an `Error`, and does not decide
  on their text (it decides on `e.kind`);
- stays within the limits of each implementation;
- does not need two workers to run at the same time to make progress.

The differential gates compare the implementations on these programs, and
on the binding parts of the runs that halt.

## Fit with VISION.md

| Item in `VISION.md` | Effect |
|---|---|
| "Fail loudly" | Kept. Every static error and every halt exits non-zero with a report. A fault that the spec names must occur; it cannot become a value. |
| Control: *failure* ("a fault is a value that the program can catch or let end the program") | Kept. The catchable set is a floor that every implementation provides. |
| Control: *evaluation order* | Kept, also before a halt: the effects before the fault bind. |
| Control: *resource release* | Kept for a running program. Only a halt, where the process ends, leaves the pending deferred calls to the implementation. |
| Control: *suspension*, *memory*, *cost* | Not changed. |
| "The backends" | Every backend conforms to the same spec. Each backend's role decides how much of it that backend implements. No backend has its own exception in the spec. |

## Consumers to update

### Binding documents

| Consumer | Change |
|---|---|
| `VISION.md`, "The backends" (owner only, PR #732) | Every backend implements the spec, as much as its role needs. A line under the table says what the spec binds and what it leaves to each implementation. |
| `spec/README.md`, "Error Taxonomy" | Static error and runtime error use sections 1 and 3 above. |
| `spec/README.md`, new section "Conformance" | Sections 1 to 5 above, and the closed list of implementation-defined behavior. |
| `spec/03-types.md`, "Error" | `message` is a String with implementation-defined text. |
| `spec/03-types.md`, `spec/02-syntax.md`, `spec/04-semantics.md`, size limits (255 arguments, parameters, list and map literal sizes, the canonical-string depth of 200) | Implementation-defined, with a reference to "Conformance". |
| `spec/04-semantics.md`, "Runtime Errors" | The table keeps Cause, Example, and `Error.kind`. The text about `message` changes as in `spec/03-types.md`. |
| `spec/04-semantics.md`, "Fatal Runtime Errors" | Becomes "Halting Faults": Cause and Example only. It drops the Message column, the text about native, and the rule that a halting fault skips every pending deferred call. It states that an implementation may make a halting fault catchable. |
| `spec/04-semantics.md`, the notes after the fault tables (the canonical-string depth guard, the stdlib error texts, native's report for a non-`Error` throw, native's fast paths) | Delete, or move to `docs/reference/` as a description of native. |
| `spec/04-semantics.md`, "`throw` Statement" step 5, and "Deadlock" | The report and the deferred calls of a halt follow section 3. |
| `spec/04-semantics.md`, "Worker and Task Faults" | Drop the Message column. |
| `spec/04-semantics.md`, "Implementation-Gated Exception: the Bootstrap Interpreter" | Delete. Sections 4 and 5 cover it for every implementation. |
| `spec/04-semantics.md`, "Calls" | The call-depth paragraph refers to "Conformance". |
| `spec/05-stdlib.md` | The text of a stdlib error is implementation-defined. |
| `AGENTS.md`, "Plans for a language change" | Rule 1: each backend owes the spec, as much as its role needs. A language change updates the bootstrap in the same PR, only as much as CI requires. Rule 3: "its output matches the native VM on the binding behavior of the spec". Rule 5: the compile phase compares rejection, not text. |

### Non-binding documents

| Consumer | Change |
|---|---|
| `docs/reference/` | A new page, `native-diagnostics.md`: native's exit codes (65, 70), its report format, its fatal set, its limits, and its fast paths. The text that leaves the spec goes here. |
| `docs/reference/bootstrap-accepted-fatal-limits.md` | Delete. Section 2 allows its eight kinds on every implementation. |
| `docs/reference/bootstrap-stack-depth.md` | Refer to section 4. |
| `docs/decisions/qbe-backend.md`, S7 | "Byte-identical to native" applies to portable programs. |
| `docs/decisions/concurrency-model-decision.md`, the backend table | The bootstrap row refers to section 5. |
| `docs/proposals/blocking-call-suspension.md`, F4 and the consumer table | Section 5 covers the bootstrap. That mission needs no exception. |

### Programs

Six examples print `e.message`: `config_validator.lox`, `csv_cleaner.lox`,
`catch_index_error.lox`, `try_catch_error_instance_properties_catchable.lox`,
`try_catch_error_vs_ordinary_instance_catchability.lox`, and
`test_error_kind_message.lox`. Their output is not portable. The follow-up PR
makes them print `e.kind`, or moves a check of native's text to a native-only
test.

### Gates

| Gate | Change |
|---|---|
| `tools/diff_runtimes.py` | Three cases. If native exits 0 or through `exit(n)`, the other run must give the same stdout and status. If native halts, the other run must exit non-zero, and one stdout must be a prefix of the other (the deferred calls of a halt are implementation-defined). In the compile phase, both runs must exit non-zero with empty stdout. Never compare stderr. |
| `tools/check_spec_probes.py` | A `STATUS` line of 65 or 70 becomes `STATUS: halt` (any non-zero). `// STDERR:` lines leave the spec probes. A CHECK line after a halt is a prefix check. |
| `test/translation-probes/` | Each probe that pins a message or a deferred call during a halt moves to a native-only folder, or changes. |
| `tools/check_fault_table.py` | A catchable row: caught, `e.kind`, `type(e)`. A halting row: uncaught, the run exits non-zero; inside `try`, the run halts or is caught (both conform). Drop `e.message` and `str(e)`. |
| `tools/check_fatal_table.py`, `tools/fatal_table_xfail.txt` | Delete. `check_fault_table.py` covers the halting rows. |
| `tools/check_qbe_s6_errors.sh` | Its two fatal fast-path probes check a non-zero exit only. |
| Bootstrap steps: fatal fast path, re-entrant overflow, stack-overflow catchability, resolver error parity, uncaught-throw report, wrong-argument-count fatal path, deferred-callable fatal fault | Remove the CI step, the ctest entry, and the tool. |
| `tools/check_bootstrap_error_kinds.py`, `tools/run_bootstrap_error_kind_causes.py` | Keep the catchable half: two causes must not share one `kind`. Drop the fatal half. |
| `tools/check_bootstrap_headroom.sh` | Keep the example-corpus half. Drop the ceiling ratios. |
| Native's own tests of its messages | Keep. They test native's choices, not conformance. |
| Gates on portable programs: examples, coroutine probes, print lines of two workers, file durability, OS stdlib, sockets and subprocess, NUL and CR byte probes, operator overload, match-arm-break, the self-hosted interpreter on the JVM and on QBE | Keep. |

Every changed gate needs a proof that it can fail (`AGENTS.md`, "Engineering
rules"). For `diff_runtimes.py`: a program that native rejects and the other
runner accepts must report DIVERGE; so must a program that native runs to
exit 0 and the other runner halts on, and a halting run whose stdout is not
a prefix of the other.

## Order of work

1. The owner changes `VISION.md` (PR #732). The PR has no code.
2. One PR changes the spec, `AGENTS.md`, `docs/`, the six examples, and the
   gates. A spec change without its gates would leave CI enforcing the old
   rule. No implementation code changes: native, the JVM, QBE, and the
   bootstrap already conform to the relaxed rule, except for the open issues
   that the comment on PR #731 lists as still relevant.
3. Close each issue that the new rule puts out of scope, with a link to
   #722.

## Owner answers

The owner set the direction and answered these questions on PR #731
(2026-10-19).

1. **Scope.** Relax the spec for every implementation, not only the
   obligations of the secondary backends. Error messages bind neither
   between the spec and an implementation nor between two implementations.
   An implementation conforms on a halt by exiting non-zero.
2. **Faults that are fatal today.** The catchable set is a floor. Any other
   fault halts, and an implementation may make it catchable.
3. **Output on a halt.** The output before the fault binds. Whether the
   pending deferred calls run is implementation-defined.
4. **Limits and scheduling.** Implementation-defined for every
   implementation. The spec no longer names the bootstrap.
5. **The JVM as an oracle.** The owner accepts that the JVM stops being an
   oracle for error text and for the fatal set.
6. **Compile rejection.** It stays binding, in both directions.
7. **A language change and the bootstrap.** A language change still updates
   the bootstrap in the same PR, only as much as CI requires.

## Alternatives

- **Relax only the secondary backends.** The first version of this proposal
  did this, with two conformance levels: full for native, observable for the
  rest. It kept the spec coupled to native's diagnostics and fatal set, and
  each new backend would have needed the same exception. The owner rejected
  it.
- **Halting faults stay halting everywhere.** The spec would keep the fatal
  set as a design rule. Every implementation would have to keep the two sets
  apart, and #666, #685, and #730 would stay in scope.
- **Every fault catchable.** This removes the halting set. It is a language
  change, and native and every backend must change their fatal paths.
- **Keep the deferred calls of a halt binding.** This keeps today's rule (an
  uncaught catchable fault runs them, a halting fault skips them). It makes
  each implementation copy native's unwind on the error path.
