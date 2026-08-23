# CLR backend: mission completion record

Written for a contributor who never saw the mission that built this backend.
It states what the four mission conditions (`--target clr`, all four measured
here) prove, what the delivered gate is weaker than, and what a later change
should re-check before it trusts this file's numbers.

## The four mission conditions

1. **The example program tests pass on the CLR backend.**
   `python3 tools/check_examples.py tools/loxpp_clr.sh examples/ --exclude
   tools/clr_excluded_examples.txt` — 54 passed, 0 failed, 6 skipped (5 map-
   order permutations plus one example with no `CHECK` directives). The
   stronger, full-stdout form (`tools/diff_runtimes.py`, no `CHECK`-directive
   blind spot) also holds over the same corpus: 56 matched, 4 permutation-
   excused, 0 diverged.
2. **`bootstrap/loxpp_interpreter.lox` runs on the CLR backend and gives the
   same output as the native VM.** `tools/loxpp_clr_bootstrap.sh` (the CLR
   twin of `tools/loxpp_jvm_bootstrap.sh`) reproduces
   `bootstrap/lox_wrapper.sh`'s output byte for byte on `examples/
   fibonacci.lox`, guarded against a silent empty-vs-empty match on either
   side.
3. **The example program tests pass through the CLR-hosted bootstrap
   interpreter.** `python3 tools/check_examples.py
   tools/loxpp_clr_bootstrap.sh examples/ --exclude
   tools/clr_excluded_examples.txt` — 54 passed, 0 failed, 6 skipped. The
   stronger, full-stdout form holds too, over every example except one (see
   "A known limit of the bootstrap gate" below): 55 matched, 4 permutation-
   excused, 0 diverged.
4. **No regression.** `ctest` is green on all four presets (`debug`,
   `release`, `debug-variant`, `release-variant` — both `Value`
   representations); `check_examples.py` against the native binary,
   `check_jvm_probes.sh` (45/45), and `check_clr_probes.sh` (99/99, corpus
   sweep and permutation guard both included) all stay green.

## The CLR exclusion list

`tools/clr_excluded_examples.txt` holds five entries, all proven map-order
permutations (native's `ObjMap` iterates by bucket order; `runtime/clr/src/
LoxMap.cs` iterates by insertion order instead, a deliberate, documented
choice — see its own header comment): `symbol_table.lox`, `huffman.lox`,
`anagram_groups.lox`, `word_freq.lox`, `phonebook.lox`. Each was proven with
its own permutation proof when it was added — same lines, different order —
before the CLR differential gate accepted it.
`tools/check_clr_probes.sh`'s corpus sweep and
`tools/diff_runtimes.py --only-excluded` both re-check every entry on every
run, so a list entry that stops being a permutation fails the gate by name.

## Measured emission limits

`notes/clr-emission-limits.md` holds the measured ceilings (largest emitted
method, longest string literal, branch range) against the full corpus,
`bootstrap/loxpp_interpreter.lox` included — it was already the largest
program in that file's own table before it was compiled and run here, and
running it changed none of those numbers.

## The map-order choice, and its consequence

`LoxMap`'s insertion-order iteration (above) is the same choice the JVM
runtime made with `LinkedHashMap`. The consequence is the same on both
managed backends: a program that reads a map's `keys()`/`values()`/
`entries()` in the order they come back, without sorting, prints in
insertion order on JVM and CLR alike, and in bucket order on native. Five
programs in this repository do this; all five are excluded, proven
permutations, not defects. A new example that reads map order directly
needs the same proof before it can join the exclusion list, or it needs to
sort its own output instead (the design most of the corpus already uses).

## Known limits of the delivered gate

- **The full-stdout bootstrap comparison excludes one example by name, not
  by the permutation-exclusion mechanism.** `examples/bench_jump_table.lox`
  is a 5,000,000-iteration microbenchmark with no `CHECK` directives.
  Interpreting it twice over — native once as the oracle, the CLR backend
  once as the comparison, each already running a tree-walking interpreter
  written in itself — does not finish inside a CI-sized timeout on either
  side; this is measured cost, not a CLR-side defect, and it is the same
  reason `check_examples.py`'s `CHECK`-directive gate never ran this example
  through the interpreter either. Unlike a map-order permutation, a timeout
  is not something `tools/clr_excluded_examples.txt` can excuse (`tools/
  diff_runtimes.py` reports DIVERGE on any timeout, exclusion list or not),
  so the CI step's own file list leaves this one example out directly. This
  is narrower than the direct-path gate above, which does cover it (`tools/
  check_clr_probes.sh`'s own `examples/bench_jump_table.lox` probe runs it
  directly, with no interpreter in between, and passes).
- **Differential scope stays stdout only, per the mission brief.** An
  uncaught Lox++ runtime error terminates the two backends differently:
  native's own top-level driver catches it and exits 70; nothing in the
  generated CLR code or its run harness catches it, so CoreCLR's own
  unhandled-exception path reports it and the process ends via `SIGABRT`
  (exit 134). This has been true since the first CLR emission node and is
  unrelated to recursion depth — it happens identically for a zero-
  recursion runtime error (`notes/translation-probes/24_call_before_
  closure.lox`, an existing, passing `check_clr_probes.sh` probe). Every
  differential check in this repository compares stdout only, never stderr
  or the exit code, so this difference is accepted, not hidden.
- **`runtime/clr/host/LoxHost.cs`: why every CLR run goes through it, and
  what it does not change.** CoreCLR gives no `-Xss`-equivalent flag; a
  thread's stack size is fixed at creation, and the process's main thread
  already exists by the time managed code runs. `LoxClosure`'s own frame
  ceiling (`FramesMax = 256`, already present when this gate was built,
  matching native's `FRAMES_MAX`) bounds Lox-level call recursion
  identically on both backends, and measurement found no case in the
  required corpus, the probes, or the bootstrap interpreter that comes
  close to exhausting the default 8 MiB thread stack through that path
  alone.
  The real, reproducible risk is a different, pre-existing one:
  `LoxOps.Stringify` (`runtime/clr/src/LoxOps.cs`) recurses once per level
  of list/map nesting, with no depth guard at all — unbounded by
  `FramesMax`, because it is plain C# recursion over a value's own shape,
  not a Lox-level call. Measured directly, running the emitted program's
  own entry point straight through `dotnet` with no host in between:
  printing a list nested 20,000 deep overflows the default 8 MiB thread
  stack partway through formatting the resulting `System.
  StackOverflowException`'s own message (CoreCLR's own trace shows the
  recursion reaching about 15,868 levels before that), which CoreCLR can
  then only report as a bare `SIGABRT`, with no `Lox.LoxError` line and no
  indication of what failed. The same program, run through `LoxHost` on
  its 256 MiB thread instead, completes and prints the correct output.
  Every CLR run in this repository now goes through `LoxHost` this way
  (`LOX_CLR_STACK_BYTES` overrides the stack size) — 256 MiB was chosen to
  clear this measured failure point with a wide margin; no attempt was made
  to find exactly where the new ceiling falls.
  `notes/translation-probes/clr-only/39_deep_nested_stringify.lox`
  (`tools/check_clr_probes.sh`) pins the fix: native and the CLR backend
  give the same 20,000-deep output on every run, so a later regression back
  to running the emitted assembly directly fails this probe with the
  `SIGABRT` above, not a silent pass. `LoxHost` does
  not change which programs succeed and which raise a Lox-level error — a
  program that overflows `FramesMax` still gets the same `Stack overflow.`
  `LoxError` either way — it only gives CoreCLR room to finish reporting a
  genuine stack exhaustion instead of aborting while trying to. The
  underlying recursion in `Stringify` is unbounded either way; native's own
  `stringifyObj` (`src/object.cpp`) has the identical shape and the
  identical lack of a guard, confirmed to segfault a clean release build on
  the same repro (issue #152) — this is a pre-existing native limitation,
  not something either backend introduced or is charged with fixing.
