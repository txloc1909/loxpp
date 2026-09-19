# Mission brief — catchable `StackOverflowError` across all four consumers

Tracking issue: **#271**.

The binding rules for this mission — node order, dependencies, the #267
ruling, and the corrected Wave 3 design — are posted on #271 itself, in the
`[Orchestrator] MISSION BRIEF` comment. Read that comment before any node
issue (#267, #268, #238, #248).

This file exists only because `.claude/workflows/backend-dag.js` requires
`args.briefPath` to name a committed file. It is not a second copy of the
brief; it is a pointer to it, so the rules stay in one place. It gets
deleted once the mission closes.
