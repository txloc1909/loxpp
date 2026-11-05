# Invariant probes for the concurrent machine: retrospective

## Purpose

Mission #1098 wrote program-level probes for three invariants of the
concurrent machine, so that the runtimes can be refactored (#1068) while the
tests stay green. The decision is `docs/decisions/concurrency-invariant-probes.md`.
The mission ran on 2026-11-05. It had five nodes: #1096 (the decision), #1104
(I2 and the check script), #1108 (I3), #1126 (I1) and the close.

## What went well

- **The check script made the probes part of the spec.** The rows of
  `tools/check_invariant_probes.py` come from the task-owned state table and the
  suspend point table of `spec/04-semantics.md`. A new row in either table fails
  the script until a probe exists. I broke each input of the script in a scratch
  copy, and it failed each time.
- **One mutation patch covered three backends.** QBE links the native runtime,
  and the bootstrap sends Socket and receive calls to the host. A temporary patch
  with an environment variable (`LOXMUT`) to choose the mutation let one build
  run all mutations of a node. The JVM needed its own patch and a new jar.
- **A subagent answered the questions about the code in one report.** It showed
  that QBE does support `yield` and coroutines (my plan assumed it did not), and
  that two rows of the task-owned state table cannot leak at a park.

## What went wrong

- **The plan used a seam that a program cannot reach.** The plan said to use the
  scheduler clock seam. It is C++ only. I found this by reading the code, before I
  wrote a probe, and the owner chose probes without wall-clock time where
  possible. For the probes that make a worker see several waits end at one point,
  no `.lox` program can avoid a computation of fixed length. Probes 358 and 366
  already did this.
- **CI was red on the first push of N1.** The resolver corpus test
  (`ToolingResolverCorpus.NoCrashAndKnownWarnings`) warns on an unused local, and
  my generated probes had two. The test is not in the probe gates. I now build
  `test_tooling_resolver` and run that one test before I push a new probe. N2 and
  N3 passed CI on the first push.
- **A mutation of shared machinery fails every probe.** The mutations of the
  mailbox and of the call frames failed all nine I1 probes, because each probe
  uses both. That meets the rule that a probe fails on a mutation, but the failure
  does not point at the row. Issue #1128 holds the work to narrow it.
- **A mutation that is not observable.** On the JVM, the per-worker counters of a
  parked task and the current coroutine can be shared without a change in any
  probe. The JVM runs each task on its own thread, so the language state is
  naturally per task. Issue #1129 holds the question.
- **GitHub gave a 502 on a merge.** The merge was in progress, and a second
  request answered "Merge already in progress". A loop that retries and reads the
  state of the pull request finished it.

## Rules

This retrospective produces no new rule for `AGENTS.md`. The mutation method and
the resolver test are practice, not a rule that the repository can check.

## Not done

The write side of a call that waits again (issue #933) has no probe (#1127).
