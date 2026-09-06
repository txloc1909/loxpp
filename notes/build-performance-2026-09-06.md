# Debug build performance — measurement and improvement plan

Date: 2026-09-06. Trigger: the Debug build feels slow and resource-heavy,
both locally (several agents share one 12-core / 15 GB host) and in CI.

## 1. Measurement

### CI (GitHub-hosted `ubuntu-latest`, 4 vCPU, no ccache on `main`)

From run 34045786577 (`main`, commit 225259f) and four earlier runs.

| Job step | Duration |
|---|---|
| `Build & Test (debug)` → **Build** (`cmake --build build`) | **6–10 min** (10m19s this run) |
| `Build & Test (debug)` → Test (`ctest -j`) | **13 s** |
| `Build & Test (debug)` → Run example checks | 73 s |
| `Build & Test (release)` → Build | **11m33s** |
| `Managed toolchains` → Build loxpp (interpreter only, release) | **19 s** |
| Build dev image (cached layers) | 27–55 s |

Ninja step timing inside the cold debug Build step (same run):

| Steps | What | Wall time |
|---|---|---|
| 1–40 | the `loxpp` binary: all 28 `src/*` TUs + link | **17 s** |
| 41–1347 | the 43 test executables | **~10 min** |

**97% of the cold build is the test suite. The interpreter builds in 17 s.**

### Local (this host, 12 cores / 15 GB, shared by several agents)

- Clean `cmake --preset debug` configure: 1.6 s.
- Clean `ninja` build (1347 steps), measured 2026-09-06 while 2–3 other
  agents were also building (load average 15–31 throughout):
  - **wall: 11m56s**
  - **user CPU: 91m11s**, sys: 4m16s
- 91 CPU-minutes for one debug build is the "heavy resource" complaint.
  On an idle 12-core host that is ~8 min of ideal wall; the extra ~4 min
  here is contention. Run it on 3 agents at once and ~270 CPU-minutes
  fight over 12 cores, and ASan roughly doubles peak RAM per `clang`, so
  12-way parallel compile + link approaches the 15 GB limit.
- The absolute number is inflated by contention, but the *shape* matches
  CI exactly: the test-suite compilation is essentially the entire cost.

### ccache branch (`ci/shared-ccache`, unmerged)

Run 33991921610: with the ccache launcher wired in and the cache warm, the
**entire 1347-step Build step finished in ~55 s** (vs 6–10 min cold), and
the whole `Build & Test (debug)` job in ~3.5 min including the image build.

## 2. Root cause

`test/CMakeLists.txt` builds **41 of 43 test executables** by listing the
same `${FULL_SRCS}` set — 28 `src/*.cpp` files — directly in every
`add_executable`. Nothing is shared:

- 41 targets x 28 shared TUs = **~1150 compilations of the same sources**,
  with identical flags for all but 11 of them.
- The heavy files are recompiled ~40 times each:
  `compiler.cpp` (2342 lines), `jvm_emitter.cpp` (2219),
  `clr_emitter.cpp` (2215), `vm.cpp` (1312), `abstract_stack.cpp` (960),
  `capture_analysis.cpp` (703).
- 10 `_gc` targets recompile all 28 sources only to flip
  `LOXPP_STRESS_GC` (a one-line `m_nextGC` initial value +
  one inlined branch in `MemoryManager::create`). `test_profiler` does the
  same for `LOXPP_PROFILE`.

Secondary factors, small next to the duplication:

- Debug adds `-fsanitize=address,undefined` + full `-g` to every TU.
  Release has neither and is *equally* slow, so instrumentation is not the
  driver — duplication is.
- No compiler launcher / ccache on `main`. The `Dockerfile` set
  `CC="ccache clang"` but every preset sets `CMAKE_CXX_COMPILER` directly,
  which overrides it — so ccache never actually ran. (Fixed on
  `ci/shared-ccache`.)
- Default GNU `ld` for 43 ASan executables; `lld` is installed but unused.
- `release` CI job rebuilds and (re-)runs the full GTest suite that
  `debug` already covers with sanitizers.

## 3. Improvements (ranked by payoff / effort)

### A. Deduplicate the shared sources into an object library — biggest win

Replace the 41x `${FULL_SRCS}` copies with a small number of
`add_library(... OBJECT ...)` (or `STATIC`) targets that carry the common
include dirs, `LOXPP_NAN_TAGGING`, and warning flags:

- `loxpp_testobj` — the 28 sources, default flags. ~30 plain targets link it.
- `loxpp_testobj_gc` — same sources + `LOXPP_STRESS_GC`. 10 `_gc` targets.
- `loxpp_testobj_profile` — same sources + `LOXPP_PROFILE`. `test_profiler`.

Shared-source compilations: **~1150 → 84** (28 sources x 3 variants). Each
test target then compiles only its own 1–2 `.cpp` files and links.

Prototype measured — see §5: **1347 → 245 build steps, 91 → 17 CPU-minutes,
1102/1102 tests pass.** `test/CMakeLists.txt` only, no CI edit,
`gtest_discover_tests` untouched.

### B. Merge `ci/shared-ccache` (already written, verified on CI; an agent session is actively finishing this)

- `CMAKE_{C,CXX}_COMPILER_LAUNCHER=ccache` when `ccache` is found.
- `actions/cache` for `.ccache` keyed per preset in the debug/release jobs.
- Shared `-v loxpp-ccache:/ccache` volume for the agent containers, so N
  agents on this host pay for one compile of each unchanged TU, not N.

Turns every incremental rebuild (worktree switch, rebase, CI re-run,
docs-only change) into **~1 min**. Complementary to A — A shrinks the
*cold* build that ccache cannot help (e.g. after a `value.h` change).

### C. Link with `lld`

Add `-fuse-ld=lld` for the debug/release presets (`lld` is already in the
image). Cuts the 43-executable link phase; matters most once A removes the
compile bottleneck.

### D. Collapse the `_gc` / `_profile` compile variants

Make `LOXPP_STRESS_GC` a runtime switch (env var or `MemoryManager` ctor
flag) instead of a compile-time macro. Removes `loxpp_testobj_gc`
entirely (**84 → 28** shared compiles) and the 10 duplicate link steps.
Needs care: some `test_*_gc` cases assert an abort under stress, so they
would set the flag at start-up. Medium effort, follow-up to A.

### E. Trim CI duplication

- Drop the GTest build+run from the `release` job (debug+ASan already runs
  it); keep release building only `loxpp` + the example/differential
  checks. Saves ~10 min of runner time per push (parallel, so wall-clock
  neutral, but halves the C++ compute and the queue pressure).
- Consider `runs-on` with more cores for the debug job if A/B do not bring
  it under ~3 min.

### F. Local ergonomics

- Document a `--target loxpp` fast path in `AGENTS.md` for changes that do
  not need the suite (17 s vs minutes).
- `cmake --build build -j` is already implied by Ninja; add
  `CMAKE_JOB_POOLS` / `-l` load limiting guidance so parallel agents do not
  oversubscribe the shared host (load 30 on 12 cores was observed).

## 5. Prototype result (improvement A) — measured

`perf/objlib-proto`, `test/CMakeLists.txt` rewritten to three `OBJECT`
libraries (`loxpp_testobj`, `_gc`, `_profile`); every test target links one
of them instead of recompiling `FULL_SRCS`. Nothing else changed.

| | `main` baseline | prototype |
|---|---|---|
| Ninja build steps | 1347 | **245** (−82%) |
| Clean build, user CPU time | **91m11s** | **16m40s** (−82%) |
| Clean build, wall (this shared host) | 11m56s | 1m43s* |
| `ctest` result | (n/a) | **1102/1102 pass**, 9.0 s |

\* wall times are not a clean comparison — host contention differed between
the two runs — but **user CPU time is apples-to-apples and tracks the step
count exactly: one debug build goes from 91 to 17 CPU-minutes.**

Remaining cost in the prototype, in order: the 43 ASan executable links
(default `ld`), `test_harness.cpp` still compiled ~40x (small; could join
the object libs), the 43 unique test `.cpp` files. This is what motivates
improvements C and D.

## 6. Suggested order

1. **B** — ✅ **merged as #175** (`ci: shared ccache`, 2026-09-06).
2. **A** — ✅ **merged as #178** (`refactor: compile shared test sources
   once via object libraries`, 2026-09-06). Adversarially reviewed;
   CI `Build & Test (debug)` dropped from ~10 min to ~2 min.
3. **C** (`lld`) — ✅ **merged as #180** (`ci: link debug and release builds
   with lld`, 2026-09-07). `-fuse-ld=lld` added to `CMAKE_EXE_LINKER_FLAGS`
   of the `debug` and `release` configure presets. Serial link phase (44
   executables): debug 9.3 s → 3.9 s, release 3.1 s → 2.0 s. 1102/1102 tests
   pass on both presets, same inventory.
4. **E** (CI release-job trim) — CI-only. ✅ **merged as #179** (`ci: drop the
   duplicate GTest build and run from the release job`, 2026-09-07). The
   `build-and-test` release leg now builds the `loxpp` target only and skips
   `ctest`; the debug + ASan leg stays the full GTest gate.
5. **D** (runtime STRESS_GC) — ✅ **merged as #181** (`refactor: make
   LOXPP_STRESS_GC a runtime switch, drop the _gc compile variant`,
   2026-09-07). `MemoryManager` reads the `LOXPP_STRESS_GC` environment
   variable once in its constructor into a `bool`; `create()` and `rawAlloc()`
   gate on that flag. The `loxpp_test_core_gc` object library is gone; the ten
   `_gc` test targets link `loxpp_test_core` and get `LOXPP_STRESS_GC=1`
   through a per-test ctest `ENVIRONMENT` property. `LOXPP_PROFILE` /
   `loxpp_test_core_profile` left in place — see section 10.

## 7. Post-merge measurement (A + B together)

CI `Build & Test (debug)` job, PR #178, run 34049545182:

| | `main` before (#174) | after #175 + #178 |
|---|---|---|
| Build & Test (debug) job | ~12 min | **2m18s** |
| Build & Test (release) job | ~13 min | **56 s** |
| ninja build graph | 1347 steps | 209 steps |
| `ctest` | 1102/1102 | 1102/1102 (identical inventory) |

D remains available if the build needs to get faster still.

## 8. Post-merge measurement (E — CI release-job trim)

The `build-and-test` release leg now compiles the `loxpp` target only and
runs no `ctest`. The debug + ASan leg is unchanged and stays the full GTest
gate. Change is one YAML file, `.github/workflows/ci.yml`.

| release-leg Build step | before E | after E |
|---|---|---|
| ninja build graph | ~209 steps (full GTest suite) | **31 steps** (`loxpp` only) |
| `ctest` run in the release leg | 1102 tests | **0** (debug + ASan leg still runs all 1102) |

Local reproduction on the shared 12-core host, `loxpp-dev-env` container,
release preset, ccache disabled: the trimmed leg's compile + link is **14 s**
(matches section 1's ~17 s for the interpreter). It keeps its example,
bootstrap, and craftinginterpreters checks.

Because the release and debug legs run in parallel, the wall-clock of the
whole `build-and-test` job does not change (the debug leg is the long pole).
The saving is compute and queue pressure: ~178 fewer ninja steps
(test-source compilations and the GTest executable links) and 1102 fewer
test executions on every push. After A (#178) the release job was already
~56 s, so the
absolute runner-time cut is smaller than the plan's original "~10 min"
estimate, which predated A.

## 9. Post-merge measurement (C — `lld` link phase)

Branch `ci/link-with-lld`, PR #180. `-fuse-ld=lld` added to
`CMAKE_EXE_LINKER_FLAGS` of the `debug` and `release` configure presets.
Nothing else changed. Host: shared 12-core dev container, ccache warm so
only the link edges run.

Link phase = delete all 44 executables (`loxpp` + 43 GTest binaries), then
`cmake --build build`. Serial (`-j1`) is the apples-to-apples number;
parallel (`-j 12`) is what a full machine sees. Each value is the mean of 3
runs; the runs were within 0.05 s of each other.

| Link phase, 44 executables | GNU `ld` (before) | `lld` (after) | change |
|---|---|---|---|
| debug (`-fsanitize=address,undefined`), serial `-j1` | 9.34 s | 3.92 s | **−58 %** (2.4x) |
| debug, parallel `-j12` | 2.76 s | 0.96 s | −65 % |
| release, serial `-j1` | 3.07 s | 1.97 s | −36 % |
| release, parallel `-j12` | 1.04 s | 0.36 s | −65 % |

Proof `lld` ran, not just that the flag is present:

```
$ readelf -p .comment build/test/test_vm_runtime
String dump of section '.comment':
  [     1]  GCC: (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
  [    2e]  Ubuntu clang version 18.1.3 (1ubuntu1)
  [    55]  Linker: Ubuntu LLD 18.1.3
```

The GNU `ld` build has no `Linker:` line in `.comment`. `ctest`:
1102/1102 pass on both presets with `lld`, same inventory as `main`.

## 10. Post-merge measurement (D — runtime `LOXPP_STRESS_GC`)

Branch `refactor/runtime-stress-gc`, PR #181, rebased on merged C (#180).
`LOXPP_STRESS_GC` is a runtime `bool` in `MemoryManager`, read once from the
environment in the constructor. The `loxpp_test_core_gc` object library is
deleted; the ten `_gc` test targets link `loxpp_test_core` and run under a
per-test ctest `ENVIRONMENT` property. `LOXPP_PROFILE` and
`loxpp_test_core_profile` are unchanged and out of scope — the notes' full
"84 → 28" figure needs that second collapse, which is a bigger change (the
profiler adds data members to `MemoryManager` and `VM`).

Host: shared 12-core dev container. Clean debug build, **ccache disabled**
(the apples-to-apples number — with ccache warm the duplicate `_gc`
compilations are already near-free, because ccache dedups by preprocessed
content and only `memory_manager.cpp` actually changes with the flag).

| Clean debug build, ccache off | `main` (C merged) | branch D | change |
|---|---|---|---|
| ninja build steps (`cmake --build build`) | 209 | **179** | −30 (−14 %) |
| `ninja -t targets all` | 501 | 468 | −33 |
| user CPU time | 284.1 s | **265.5 s** | **−18.6 s (−6.5 %)** |
| real time (this shared host) | 28.1 s | 26.6 s | −1.5 s |

Shared-source compilations drop one of the (then) three variants: the 29
`src/*` core sources plus `test_harness.cpp` are compiled twice, not three
times. `ctest`: 1102/1102 pass, identical inventory to `main` (`diff` of
`ctest -N` is empty).

Proof the `_gc` targets still run collect-on-every-allocation
(`AGENTS.md`: "prove that a new check can fail"): reintroduce bug #31
(drop the `pushTempRoot(native)` in `StdlibRegistrar::defineGlobal`).

- Mechanism active: `ctest -R "test_gc_regression|test_stress_gc"` → **9/9
  fail**, ASan heap-use-after-free.
- Same binary run directly with no `LOXPP_STRESS_GC` in the environment (the
  non-`_gc` twin's path) → **3/3 pass**, bug not exercised.
- Ctor's env read forced to `return false`, `ENVIRONMENT` still set → **9/9
  pass** vacuously. This is the failure mode the node must not ship.

Then both temporary edits reverted.

Proof the real `loxpp` binary honours the switch without a rebuild
(debug build, `LOXPP_DEBUG_LOG_GC` on): `build/loxpp examples/huffman.lox`
runs 0 collections; `LOXPP_STRESS_GC=1 build/loxpp examples/huffman.lox`
runs 942 collections with identical program output.

### CI coverage note (review round 1)

Before D, the debug preset compiled `loxpp` with `-DLOXPP_STRESS_GC` on by
default (`option(... ${_debug_default})`, ON for `CMAKE_BUILD_TYPE=Debug`).
So the `Build & Test (debug)` job's `Run example checks` step ran the whole
example corpus under collect-on-every-allocation, which covers the corpus
for rooting bugs that no `_gc` unit test hits. D makes the flag a runtime
switch, so that step now sets `LOXPP_STRESS_GC=1` explicitly for the debug
leg only (`.github/workflows/ci.yml`, matrix-preset conditional on the
`docker run`). The release leg never had GC stress and keeps its normal GC
schedule. Verified in the container: the debug example corpus passes with
`LOXPP_STRESS_GC=1` set.
