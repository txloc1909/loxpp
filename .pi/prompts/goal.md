---
description: Autonomous hand-off: plan, implement, verify, and finish a goal
argument-hint: "<goal>"
---
Goal: $@

Work autonomously to the goal. No approval gate — plan, implement, and finish.

1. Plan: read the binding context first (VISION.md, spec/, docs/decisions/,
   docs/README.md). If this is a language change, list every consumer before
   any work per AGENTS.md. Keep the plan as your working notes, not a waiting
   step.
2. Add a test or probe for the goal's behavior and connect it to ctest
   (AGENTS.md engineering rules). A goal without a check is unproven.
3. Implement, then run the gate until it passes:
   cmake --preset debug && cmake --build build
   ctest --test-dir build --output-on-failure -j$(nproc)
4. Commit when the code compiles and push after each commit (AGENTS.md).
   Conventional commits; split if the subject would say "and".
5. When the gate is green and the work is complete, summarize what changed and
   stop.

Do not stop to ask for approval.
