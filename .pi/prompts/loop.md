---
description: Autonomous fix-until-green loop over the Lox++ build/test gate
argument-hint: "[goal]"
---
Work in a tight fix-until-green loop for: $@

1. Make one focused change.
2. Run the gate:
   cmake --preset debug && cmake --build build
   ctest --test-dir build --output-on-failure -j$(nproc)
3. If it fails, read the failure output, fix the smallest thing that unblocks
   it, and repeat from step 1.
4. Stop only when the gate is fully green. Then format and lint before
   committing (AGENTS.md step 6):
   find src test -name '*.cpp' -o -name '*.h' | xargs clang-format -i
   find src -name '*.cpp' | xargs clang-tidy -p build
5. Commit when the code compiles and push after each commit, per AGENTS.md.
   Keep each commit atomic: one concern per commit; split if the subject would
   say "and".

Before claiming success, re-run the full build — ctest does not rebuild — and
report the actual command output you just produced. A remembered "it was
green" is not evidence.
