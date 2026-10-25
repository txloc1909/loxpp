/**
 * Tests for the run-kind decision. Each row of the table in README.md is one
 * case here, so a change to the default or to the batch rule fails by name.
 */

import assert from "node:assert/strict";
import { test } from "node:test";
import { MULTI_TASK_INTERACTIVE_PROBLEM, planRun } from "./plan.ts";

const task = { agent: "scout", task: "find it" };

test("one task with no flag is watched", () => {
  assert.deepEqual(planRun({ agent: "scout", task: "find it" }), { kind: "watched", explicit: false });
});

test("one task with interactive: true is watched, explicitly", () => {
  assert.deepEqual(planRun({ agent: "scout", task: "find it", interactive: true }), { kind: "watched", explicit: true });
});

test("interactive: false is unattended", () => {
  assert.deepEqual(planRun({ agent: "scout", task: "find it", interactive: false }), { kind: "unattended", explicit: true });
});

test("a parallel batch is unattended by default", () => {
  assert.deepEqual(planRun({ tasks: [task, task] }), { kind: "unattended", explicit: false });
});

test("a chain is unattended by default", () => {
  assert.deepEqual(planRun({ chain: [task] }), { kind: "unattended", explicit: false });
});

test("an explicit interactive batch is refused, not quietly ignored", () => {
  for (const params of [
    { tasks: [task], interactive: true },
    { chain: [task], interactive: true },
  ]) {
    const plan = planRun(params);
    assert.equal(plan.kind, "invalid");
    if (plan.kind === "invalid") assert.equal(plan.problem, MULTI_TASK_INTERACTIVE_PROBLEM);
  }
});

test("a batch with interactive: false says so", () => {
  assert.deepEqual(planRun({ tasks: [task], interactive: false }), { kind: "unattended", explicit: true });
});

test("an empty batch is one task, not a batch", () => {
  assert.deepEqual(planRun({ agent: "scout", task: "t", tasks: [] }), { kind: "watched", explicit: false });
});
