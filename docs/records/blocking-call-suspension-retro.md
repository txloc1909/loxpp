# Blocking-call suspension mission (mission #711) — retrospective

**Status:** frozen at mission close, 2026-10-27. The decision is
`docs/decisions/blocking-call-suspension.md`. Every statement here comes from a command, a pull request comment, or a CI run named in this file, except where a sentence says it has no such evidence. The sections "Rules this retrospective proposes" and "Lessons" are the reading of the author. This record is history. It does not bind later work.

## Purpose

The mission made nine calls suspend only their own task: `sleep`, `connect`,
`Server.accept`, and the Socket calls `read`, `read_bytes`, `readline`,
`readlines`, `write` and `writeline`. A worker with a parked task now runs its
other tasks. It serves the goal "access to files, the network, and processes"
in `VISION.md` and the server goal of #519.

The mission started from the accepted decision (#908) and the tracking issue
#711. It ran ten nodes in one DAG, one branch and one pull request per node.
Two nodes (N8 and N9, the `input()` group and the process group) left the DAG
by owner decision F1. All nine pull requests of the code nodes merged on
2026-10-26, between 04:28 and 19:45 UTC.

| Node | Issue | Pull request | Merged (UTC) | Commit on `main` |
|---|---|---|---|---|
| N1: spec, tables, probes, lists | #909 | #920 | 04:28 | `715a28e` |
| N2: per-worker poller, idle accounting | #910 | #921 | 06:40 | `2f40eca` |
| N3: park and resume, `sleep` | #911 | #922 | 09:39 | `5d422f8` |
| N4: raw descriptors for Socket and Server | #912 | #926 | 12:26 | `3d981c8` |
| N10: JVM park and resume | #916 | #927 | 14:14 | `e516027` |
| N5: read side | #913 | #928 | 15:13 | `6bb66ac` |
| N6: write side | #914 | #933 | 17:22 | `dbb6ea9` |
| N7: `connect` | #915 | #929 | 18:59 | `657b5e1` |
| N11: bootstrap and QBE parity, delete the lists | #917 | #936 | 19:45 | `e07095c` |
| N12: close | #918 | this change | | |

The prerequisites #726 (PR #906) and #723 (PR #728) merged before the
mission. The merge order is not the DAG order: N10 (wave 5) merged before N5
and N6 (waves 3 and 4), and N6 merged before N7.

## Review rounds

A round is one reviewer comment with a verdict.

| Pull request | Rounds | Blocking findings | Where the blocking findings were |
|---|---|---|---|
| #920 (N1) | 3 | 3 | A gate script without the list flags (1). The Conformance text (2: one in round 1, one in round 2) |
| #921 (N2) | 2 | 1 | The deadlock accounting call had no test that could fail |
| #922 (N3) | 2 | 1 | `endTasks` left the wait of the main task of a session |
| #926 (N4) | 2 | 1 | The input buffer kept the capacity of a long line |
| #927 (N10) | 1 | 0 | None. Five non-blocking findings |
| #928 (N5) | 2 | 1 | A saved scan offset of `readline` that pointed into a changing buffer |
| #933 (N6) | 1 | 0 | None. Eight notes |
| #929 (N7) | 4 | 2 | The next-address fallback had no test (1). A racy descriptor count in a new test (1) |
| #936 (N11) | 1 | 0 | None. Three non-blocking findings |

Six of nine pull requests needed a change after the first review. Nine blocking findings in total. Seven of the nine came with a program, a test run, or a mutation that the reviewer ran. The other two were the Conformance text of #920. The reviewers wrote down what they ran, and
most reviews list the gates they ran themselves on the exact tip.

## What went well

1. **The spec came first and the gate showed the gap.** #920 landed the spec,
   the tables, the probes and three expected-failure lists before any
   consumer. The count of listed probes then fell with each node, and each
   count is in a review: 15 at #920, 12 after #922, 2 after #928, and 0 for
   native after #933. The JVM list was gone after #927. The last node deleted
   the empty files. The mission rule "the gate turns green with the last node"
   held.
2. **Reviewers ran mutations, and the tables are in the threads.** The review
   of #921 lists eight mutations, each with the test that failed. The review of #933 lists twelve, and it names the six that `test_socket_park` did not catch. A check that
   nobody has seen fail is unproven (`AGENTS.md`, "Engineering rules"); this
   mission applied the rule in every node review.
3. **The cost control held.** The reviewers of #921 and #922 counted system
   calls. A program that never parks made no `epoll_create1`, `eventfd2`,
   `epoll_ctl` or `epoll_wait` call (an `LD_PRELOAD` shim in #921, `gdb`
   catchpoints in #922). Later reviews measured `fib(32)`, spawn, and
   ping-pong against `main`. One real cost remains and is tracked in #923.
4. **A reviewer found a bug in a state that no probe could show.** The
   scenario of #928 (two readers on one socket) needs two tasks and a late
   write. It is now a unit test, `ASecondReaderScansTheBufferAsItIsNow`. The
   implementer put the old code back and watched it fail.
5. **The JVM backend used the same probes.** #927 passed 215 probes with no
   expected-failure file. The reviewer ran five contexts where a task cannot suspend (`__str__`, a map-key `__hash__`, `callMethod`, a deferred call and a nested coroutine) with four calls each (20 cases) and found the JVM output
   equal to native in all of them.
6. **The reviewers found defects in `main` that the mission only exposed.**
   The reviewer of #926 showed that `SigpipeIgnored` leaks "SIGPIPE ignored"
   to child processes on `main` after concurrent writes (`SigIgn:
   0000000000001000` in a spawned shell). The socket path now uses
   `MSG_NOSIGNAL`. The Process path keeps the old guard (see "Follow-ups").

## What went badly

### The Conformance wording took three rounds

The first text of `spec/README.md`, Conformance, in #920 contradicted the
suspension rule of `spec/04-semantics.md` (blocking finding 2, round 1). The
second text said a portable program "does not need a task to run while another
task of its worker waits". "Need" means progress. Probes 260, 279 and 282 do not need it for progress, yet on the bootstrap 260 and 279 print a different order and 282 ends normally instead of halting.
The text was also circular: the exemption hung on the behavior it excused
(blocking finding, round 2). Any implementation, native included, could have
cited it. The third text hangs the exemption on a property of the
implementation: it "does not run workers at the same time". It also defines a
portable program by what its output depends on, not by what it needs.

Here the trigger of the exemption was the effect it excused.

### The bootstrap gate failed in CI

The first tip of #922 failed the CI step "Check spec probes - bootstrap
interpreter". The reviewer reproduced it: 191 pass and 6 fail (probes 272 to
277), against 197 pass on `main`. The cause was not in the native code. The
bootstrap runs its interpreted tasks as host tasks, and its `sleep` called the
host `sleep`, which now parks. The implementer's reply says "It was not undefined behavior" and "The native probes pass in a clean build". The fix was a bootstrap change: it sleeps inside a
`__str__` call, where the host cannot suspend. The implementer reported the failure from the red CI step.

### State outlived a suspension and pointed at changed data

- **#928, `ParkedIo.scanned`.** A parked `readline` kept the offset where the
  next scan for a newline starts. Another task could take bytes from the
  front of the buffer while the first task waited. The offset then pointed
  past bytes that nobody had scanned. The reviewer's program returned
  `r2: [yy\nzz]`, a line with a newline in it, and delayed the line `yy`
  until more data came. The fix is to scan from the front at each resume.
  The cost is quadratic time for a very long line that arrives in small pieces
  (#952).
- **#922, `endTasks`.** `endTasks` cleared the table of parked tasks, but the
  main task of a session is not marked dead at the end of a line. A main task
  that was parked kept its wait in the poller, and nothing could end it. A
  later `task.receive()` reported its deadlock 2.9 s late. The fix releases the
  wait of the kept task. The reviewer audited every writer of the parked set.
- **#933 and #928, `rearmPark`.** A re-armed wait gets a new token, and the
  poller reports ready waits by token. Tasks may then run in an order other
  than "the order in which their calls began to wait". Nobody built a failing
  program. It is open at close as #950.

### Memory was higher than the decision allowed

The first tip of #926 kept the buffer capacity of a 100 MB line for the life
of the socket (103 MB resident while blocked, against 6 MB on `main`) and
peaked at 291 MB against 196 MB. The decision says the buffer holds the result
of the call plus at most one chunk. The memory manager does not count a
`std::string` member of `ObjSocket`, so the GC could not see it. The fix moves
the buffer into the line. The peak of `readlines` is still 291 MB (#947).

### Tests asserted more than they could know

- **#929.** The first tip passed 22 tests. Two mutations of the next-address
  fallback passed all of them, because on loopback a refused port gives
  `EINPROGRESS` and a poll that ends at once, so only one of three paths ran.
  The fix was a seam, `ConnectTestHooks`, that replaces the address list, the
  `connect(2)` call and the poll.
- **#929, round 3.** The new descriptor-count test counted descriptors while a
  helper thread of the same process opened and closed descriptors. It failed
  on "Build & Test (debug)" with `openDescriptorCount()` 10 against 11 (the
  reviewer reproduced it by adding a sleep). It did not fail in 30 single runs
  and 64 parallel runs.
- **`main` broke.** The merge of #933 (`dbb6ea9`) turned the debug job of
  `main` red. In CI run 40367947452, `test_socket_park` and
  `test_socket_park_gc`, test `TheEndOfTheProgramClosesTheDescriptorOfAParkedWrite`,
  failed at 4.52 s and 4.79 s against a 4000 ms bound. The test builds a
  128 MB string by doubling, and the reviewer says that alone can take 4 s or more in the debug build on a loaded machine.
  The reviewer of #929 had reproduced it on `main` in 4 of 4 `ctest -j12`
  runs. Two CPU-tick tests (`AParkedSleepDoesNotSpin`,
  `ABlockingSleepDoesNotSpin`) failed in 2 of 4 runs. They are #934, which #929 fixed. A third, older test, `SendCostDoesNotDependOnTheMessageSize`, still fails under 8 busy loops (#935). The debug job of `main` was green again at `657b5e1` and `e07095c` (`gh run list`), after #929 replaced the 4000 ms bound with a check of the property.
- **The tests of #933** did not catch six of twelve mutations: the EOF check of `close_write` (#951), `liveWriter` (#956), the EINTR retry, `writers.clear()`, and two that the reviewer judged equivalent.
  **The tests of #927** did not catch two mutations of `LoxPoller` (#948).

### Gate sites outside the one file the node changed

The first tip of #920 failed the "QBE toolchain" job. The script
`tools/check_qbe_s8_promotion.sh` ran the probe directory without the list
flags. The implementer then listed 17 places that run the probe directory under a consumer. Three were wrong: that script and two lines of `.pi/extensions/lib/gate-scopes.ts` (the "pi mode guards" job failed first).
At the end, #936 removed the `--xfail` flag from the native step, and the
reviewer found two gaps that follow from it: a stale native list is read by
`test/corpus.h` and not checked, and the probe README does not say that a new
list needs the flag too (#954).

### Process faults

1. **An exemption wider than needed.** Twelve of the 18 `NOT-BINDING` probes still pass
   on the bootstrap (#936 review, #953). A regression there goes unseen.
2. **Parallel nodes on the same files.** #929 and #933 both changed
   `IoOp`, `ParkedIo`, `step`, `runIo` and the fault table. The reviewer of
   #929 predicted the conflict in round 1 (a `git merge-tree` result). The rebase had six conflict hunks. The implementer resolved them once as a 3-way patch, and the reviewer checked all six hunks again on the rebased tip. No defect came out of it.
3. **Commits that were not green one by one.** #927 commit `e19dfb5` deleted
   `jvm.txt` while `ci.yml` still passed it, until `75792f1`. The QBE list
   lines of #933 went in the last commit, not in the feature commit. The first
   commit of the second round of #922 holds the code of three review items
   (the implementer said so and did not rewrite pushed history). Squash merges
   hide this. The rule is in `AGENTS.md`, "Conventions".
4. **Hung processes in containers.** The implementer of #928 stopped two hung containers by exact name (`keen_jennings` and
   `loxpp-feat-read-side-park-1479374`), and then gave each container a
   unique `--name` and ran every test under `timeout`. The review of #928 also names a test risk of the same kind: a "free port" that a stranger takes before `listen` leaves the VM in `accept` until the ctest limit.
   `AGENTS.md` already says to stop only a process that you started.
5. **Git does not work in a container with a linked worktree** (#871, open).
   The worktree `.git` file names a host path. The title of the issue says the pre-commit hook is off and the backend gate cannot pass there. The container build log of this node shows it again: `cmake` printed "Failed to set core.hooksPath" and `fatal:
   not a git repository` in the container build log.
6. **Shared scratch directories.** The task of this node reports that parallel
   agents collided in a shared scratch directory. No pull request comment
   records it, so this record cannot show the evidence. This node used a
   private directory.
7. **Approvals while a job was pending.** The approvals of #922, #927 and #936
   were posted while the "QBE toolchain" job still ran. Each reviewer said so
   in the comment, and the reviewer of #936 ran the QBE gates himself and told
   the merger to check the job. The threads report no later failure of that job. The playbook has no rule for it.
8. **Checks that a label could fool.** The reviewer of #920 found
   that a `COVERS` label was a label: moving `COVERS: suspend-call connect` to
   probe 83 passed the check. The check now needs the call to sit in the named
   function (`in <function>`). The reviewer of #929 found the same kind of
   gap again in `check_net_faults.py` (a commented-out fault, #930). The review of #926 found that `fgets(..., stdin)` passed; the script now fails on it. The approving review of #928 found that a split assignment (`out.faultKind` on one line and `= "X"` on the next) still passes. That case is open as #959.

## The bug classes

The decision named seven constraints (C1 to C7) before any node started. No review reported a defect in one of them: the reviewers ran the probe for C1 (#928) and the checks for C6 and C7 (#922) and found them correct. The classes below are the ones the reviews
found. Rule 4 of "Plans for a language change" says to write a named class into
the decision at once. The mission did not do this. The decision file changed
only in #908 and #920. They were written into "Shipped" at close.

| Class | Found in | Layer | Recorded |
|---|---|---|---|
| State of a parked call points into shared mutable state | #928 | The park record (`ParkedIo`) | Decision, "Shipped", bug classes |
| A teardown path misses one owner of a wait | #922 | The writers of the parked set | Decision, "Shipped", bug classes |
| A buffer keeps memory after a call | #926 | The per-socket input buffer | Decision, "Shipped", bug classes |
| A re-armed wait gets a new place in the order | #933 | The poller registry | Decision, "Shipped"; open at close as #950 |
| A gate site runs the probe directory without the lists | #920, #936 | The gate scripts | Decision, "Shipped"; open at close as #954 |
| A test measures more than it names | #929, #933 | The test suite | Decision, "Shipped"; #935 open at close |
| A mutation that no test catches | #927, #933 | The unit tests of the poller and the park code | Decision, "Shipped"; #948, #951, #956 open at close |

## Rules this retrospective proposes

`AGENTS.md` states current rules only. The owner decides whether a rule goes
in. This change proposes the rules below in its pull request and does not edit
`AGENTS.md`. Each has evidence in this file.

1. **A test does not assert an upper bound on the time of work that the test
   does not control. A test does not count a resource that the whole process
   shares while another thread runs.** A bound that catches a hang is not
   covered (#925 asks for one). Compare sets of descriptors, not counts. Wait
   for a quiet window, not a fixed window. Evidence: #929 round 3, the red
   debug job of `main` at `dbb6ea9` (two tests), #934, #935. At least four
   different tests broke on a loaded runner.
2. **A record that outlives a suspension holds only values it owns. It reads
   shared state again at each resume.** Evidence: #928 (a confirmed defect).
   #950 is the same class and is not confirmed. The rule rests on one defect
   and one suspicion, so it is the weakest of the three.
3. **Each agent runs tests under `timeout`.** Evidence: the hung containers of
   #928 and the "free port" risk in the same review. The task loop of
   `AGENTS.md` already names the container, so only `timeout` is new.

I do not propose a rule for approvals while a job is pending (no defect came
from it) or for the shared scratch directory (no pull request evidence).

## Follow-ups

Every follow-up is one issue. The decision lists them in "Shipped". New issues are marked "new".

| Issue | Subject |
|---|---|
| #937 (new) | Socket timeouts, with the two-minute `connect` to a dead host |
| #938 (new) | `Socket.read_some` |
| #939 (new) | Sendable sockets |
| #940 (new) | UDP sockets |
| #941 (new) | Unix-domain sockets |
| #631 | Suspending FFI calls (the FFI design holds it) |
| #942 (new) | Fair mode |
| #943 (new) | A catchable fault for a Map that changes size during `for`-in |
| #944 (new) | `input()` suspends only its task |
| #945 (new) | Process calls and `run()` suspend only their task |
| #946 (new) | A resolver thread for `connect` |
| #947 (new) | Peak memory of `readlines` on one very long line |
| #948 (new) | JVM `LoxPoller`: two mutations that no check catches |
| #949 (new) | JVM: a Socket never closed keeps its selector key |
| #950 (new) | A re-armed wait gets a new token |
| #951 (new) | The EOF check of `close_write` cannot fail |
| #952 (new) | A resumed `readline` scans from the front |
| #953 (new) | 12 of 18 bootstrap-exempt probes also pass |
| #954 (new) | A new expected-failure file is not gated |
| #955 (new) | `seesQuietWindow` is copied |
| #956 (new) | `liveWriter` has no test that can fail |
| #957 (new) | Probes 268 and 271 cannot fail on the property they name |
| #959 (new) | `check_net_faults.py` misses a fault kind set by an assignment split over two lines |
| #960 (new) | No test fails when `inbox.polling` is not reset after the wait |
| #961 (new) | `ObjSocket` allocates a `std::deque` for each Socket |
| #919, #923, #924, #925, #930, #931, #932, #935 | Filed during the mission |

The 2.5% slower ping-pong of the #928 review is a comment on #923. No second
issue exists for it.

## Lessons

- **Write an exemption on a property, not on the effect it excuses.** Three
  rounds of review on one paragraph are the cost of a trigger that depended
  on the behavior it excused.
- **List the bootstrap as a consumer of every native call it reaches
  through the host.** The bootstrap's tasks are host tasks. A change to what
  `sleep` does is a change to the bootstrap.
- **After a suspension, assume the shared data changed.** Keep a position as
  a value that the call owns or compute it again. Audit every field of the
  park record and every writer of the parked set, not only the one found.
- **A seam beats a clever test.** `ConnectTestHooks` made two fallback paths
  and two poll errors testable, and the tests do not depend on the routes of
  the host.
- **A test of a fault path needs a way to cause the fault.** `liveWriter` and
  the wake paths of `LoxPoller` stay unproven for lack of one.
- **Run the debug job with load before you merge a timing test.** `dbb6ea9`
  turned `main` red because a bound of 4000 ms did not hold in the debug job.
- **Say in the plan which nodes touch the same file.** #929 and #933 had six conflict hunks, and the reviewer named the conflict in round 1 of #929.
