# CLR backend: mission completion record

Written for a contributor who never saw the mission that built this backend.
It states what the four mission conditions (`--target clr`, all four measured
here) prove, what the delivered gate is weaker than, and what a later change
should re-check before it trusts this file's numbers.

## The four mission conditions

1. **The example program tests pass on the CLR backend.**
   `python3 tools/check_examples.py tools/loxpp_clr.sh examples/ --exclude
   tools/clr_excluded_examples.txt` — 54 passed, 0 failed, 6 skipped (4 map-
   order permutations plus two examples with no `CHECK` directives). The
   stronger, full-stdout form (`tools/diff_runtimes.py`, no `CHECK`-directive
   blind spot) also holds over the same corpus: 56 matched, 4 permutation-
   excused, 0 diverged. The `--exclude` flag is added here, and the literal
   command without it fails one `CHECK` on a map-order permutation
   (`anagram_groups.lox`, 56 passed, 1 failed, 3 skipped) — the same
   permutation the exclusion list and the guards below already prove is not
   a defect.
2. **`bootstrap/loxpp_interpreter.lox` runs on the CLR backend and gives the
   same output as the native VM.** `tools/loxpp_clr_bootstrap.sh` (the CLR
   twin of `tools/loxpp_jvm_bootstrap.sh`) reproduces
   `bootstrap/lox_wrapper.sh`'s output byte for byte on `examples/
   fibonacci.lox`, guarded against a silent empty-vs-empty match on either
   side — the guard checks only for emptiness, not the exit status, so a
   run that prints part of its output and then fails still reaches the
   `diff` that follows it. `LANGUAGE` unset (the default arm, selecting
   `bootstrap/lox_interpreter.lox` instead) also reproduces
   `bootstrap/lox_wrapper.sh`'s output byte for byte on a small plain-Lox
   program; no CI step pins this arm, the same gap `tools/
   loxpp_jvm_bootstrap.sh` already carries.
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
   `check_jvm_probes.sh` (45/45), and `check_clr_probes.sh` (100/100, corpus
   sweep and permutation guard both included) all stay green.

## The CLR exclusion list

`tools/clr_excluded_examples.txt` holds four entries, all proven map-order
permutations (native's `ObjMap` iterates by bucket order; `runtime/clr/src/
LoxMap.cs` iterates by insertion order instead, a deliberate, documented
choice — see its own header comment): `symbol_table.lox`,
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

## Measured run time

The JVM path (`tools/loxpp_jvm_bootstrap.sh`) took about 50 seconds for the
full bootstrap example run. The CLR path, in the same container, on the
same corpus, measured fresh at this file's own head:

- `check_examples.py` through `tools/loxpp_clr_bootstrap.sh`, 60 examples:
  about 9 s.
- `diff_runtimes.py` through the same wrapper, 59 files (every example
  except `bench_jump_table.lox`): about 12 s. `.github/workflows/ci.yml`'s
  `--timeout 60` for this step comes from this figure, not a guess: 60 s
  is comfortably above the roughly 12 s the full run takes, and short
  enough that a real hang still fails the job instead of stalling it.
- `tools/check_clr_probes.sh`, 100 probes plus the permutation guard and
  the corpus sweep: about 15-17 s, measured across several runs in the
  same container.

## The map-order choice, and its consequence

`LoxMap`'s insertion-order iteration (above) is the same choice the JVM
runtime made with `LinkedHashMap`. The consequence is the same on both
managed backends: a program that reads a map's `keys()`/`values()`/
`entries()` in the order they come back, without sorting, prints in
insertion order on JVM and CLR alike, and in bucket order on native. Four
programs in this repository do this; all four are excluded, proven
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
  side; this is measured cost, not a CLR-side defect. `check_examples.py`
  also never runs this example through the interpreter, but for a different
  mechanism: it skips any file with no `CHECK` directives before it runs
  the file at all, so the run time never enters into that gate's own
  decision. Both gates leave the same hole, each for its own reason. Unlike
  a map-order permutation, a timeout is not something
  `tools/clr_excluded_examples.txt` can excuse (`tools/diff_runtimes.py`
  reports DIVERGE on any timeout, exclusion list or not), so the CI step's
  own file list leaves this one example out directly. This
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
- **CI runs `ctest` on two of the four presets the "No regression" condition
  above names.** `.github/workflows/ci.yml`'s `build-and-test` matrix runs
  `ctest` on
  `debug` and `release` only. Its `variant-value` job builds `--target
  loxpp` under `release-variant` and runs `check_examples.py` against it,
  with no GTest suite and no `ctest` step; `debug-variant` is not built in
  CI at all. This is a pre-existing CI design choice (`ci.yml`'s own
  comment on that job), not something this node changed, so the
  std::variant `Value` representation's unit-test coverage in CI comes
  only from the example corpus, not from `ctest`.
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
  (`tools/check_clr_probes.sh`, and the CI step that runs the same probe
  through `tools/diff_runtimes.py`) pins the fix: native and the CLR
  backend give the same 20,000-deep output on every run, so a later
  regression back to running the emitted assembly directly — bypassing
  `LoxHost` — fails this probe with the `SIGABRT` above, not a silent
  pass. `check_clr_probes.sh` proves this directly rather than by
  argument: it runs this probe twice, once with `LOX_CLR_STACK_BYTES`
  pinned to the shipped default (must match native) and once pinned to
  the measured 8 MiB overflow point (must NOT match native — a match
  there means the probe can no longer catch the regression it exists for,
  and the gate fails naming it disarmed).
  This depends on the CLR side never getting a bigger main-thread stack
  than the host's own ambient default: `check_clr_probes.sh`'s
  `run_native` helper and `diff_runtimes.py`'s `_raise_native_stack_limit`
  each raise the stack floor for native's own child process only, in a
  subshell or a `preexec_fn`, never for the script's own shell or for any
  `dotnet` child — an earlier version of both gates raised the floor for
  the whole script instead, which gave a `LoxHost`-bypassing `dotnet`
  process the same enlarged stack and let this exact regression pass
  silently. Both helpers also raise the floor ONLY when the current soft
  limit is finite and below it — an earlier version of both compared
  RLIM_INFINITY (unlimited, -1) as a plain number and unconditionally
  assigned the floor over it, which lowered an already-unlimited soft
  limit instead of leaving it alone. This probe's depth also has to stay
  under native's OWN process-stack ceiling, because `check_clr_probes.sh`
  runs native first and fails the probe by name if that run alone fails,
  and `stringifyObj`
  shares the same unguarded-recursion shape. Measured on the default 8 MiB
  process stack, native holds through 25,000 levels and segfaults by
  27,000 — the probe's 20,000 carries about a 1.3x margin under that
  ceiling, not a wide one, and the ceiling itself moves with whatever
  process stack limit the script runs under; a host whose HARD limit is
  already capped below the floor still gets its soft limit raised, but only
  up to that hard ceiling, not to the full floor — such a host's probe
  fails on its native side only if the ceiling itself is smaller than the
  depth needed, a reason unrelated to the CLR backend, not because the raise
  did nothing. `LoxHost`'s larger stack does
  change which programs succeed, exactly for this unbounded-recursion
  case: it fails on an 8 MiB thread and prints the correct 20,000-deep
  output on the shipped 256 MiB one. What stays the same at any thread
  size is Lox-level call recursion: a program that overflows `FramesMax`
  still gets the identical `Stack overflow.` `LoxError` either way,
  because that ceiling is a fixed count of Lox call frames, not a
  function of the underlying CLR thread stack. The
  underlying recursion in `Stringify` is unbounded either way; native's own
  `stringifyObj` (`src/object.cpp`) has the identical shape and the
  identical lack of a guard, confirmed to segfault a clean release build on
  the same repro (issue #152) — this is a pre-existing native limitation,
  not something either backend introduced or is charged with fixing.
