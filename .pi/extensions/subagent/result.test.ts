/**
 * Tests for grading a finished subagent run. The closed-by-user rule is the
 * one that decides whether a person ending an interactive window reports
 * success, so each end condition gets its own case.
 *
 * Run with `node --test .pi/extensions/subagent/result.test.ts`.
 */

import assert from "node:assert/strict";
import { test } from "node:test";
import { isFailed, resultOutput, runResult, runStatus, type SingleResult } from "./result.ts";
import { emptyState } from "./stream.ts";

const answer = (text: string): SingleResult["messages"] => [
  { role: "assistant", content: [{ type: "text", text }] },
];

const result = (overrides: Partial<SingleResult> = {}): SingleResult => ({
  agent: "scout",
  agentSource: "user",
  task: "t",
  exitCode: 0,
  messages: [],
  stderr: "",
  usage: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, cost: 0, contextTokens: 0, turns: 0 },
  interactive: true,
  ...overrides,
});

test("a person closing an interactive window is a normal end", () => {
  const closed = result({ closedByUser: true, exitCode: 128, stopReason: "stop", messages: answer("done") });
  assert.equal(isFailed(closed), false);
  assert.equal(resultOutput(closed), "done");
});

test("closing the window does not turn an errored turn green", () => {
  const errored = result({
    closedByUser: true,
    exitCode: 128,
    stopReason: "error",
    errorMessage: "rate limited",
    messages: answer("partial answer"),
  });
  assert.equal(isFailed(errored), true, "an error outranks the closed-by-user rule");
  assert.equal(resultOutput(errored), "rate limited", "the stored error reaches the parent");
});

test("an interrupted turn stays a failure when the person then closes the window", () => {
  const aborted = result({ closedByUser: true, exitCode: 128, stopReason: "aborted", messages: answer("half") });
  assert.equal(isFailed(aborted), true);
});

test("a closed window in an unattended run is a failure", () => {
  const closed = result({ interactive: false, closedByUser: true, exitCode: 128, messages: answer("partial") });
  assert.equal(isFailed(closed), true);
});

test("a child that exits non-zero is a failure", () => {
  const crashed = result({ exitCode: 1, stopReason: "stop", messages: answer("partial") });
  assert.equal(isFailed(crashed), true);
  assert.equal(resultOutput(crashed), "partial", "the last answer is the evidence when there is no error message");
});

test("a failure with no message falls back to stderr, then to the text", () => {
  assert.equal(resultOutput(result({ exitCode: 1, stderr: "boom", messages: answer("x") })), "boom");
  assert.equal(resultOutput(result({ exitCode: 1 })), "(no output)");
});

test("a run that closed with no output says so", () => {
  const closed = result({ closedByUser: true, exitCode: 128 });
  assert.equal(isFailed(closed), false);
  assert.equal(resultOutput(closed), "(no output)");
});

test("runStatus separates a completed run from one cut off mid-turn", () => {
  assert.equal(runStatus(result({ stopReason: "stop" })), "completed");
  assert.equal(runStatus(result({ closedByUser: true, exitCode: 128, idle: true, stopReason: "stop" })), "completed");
  assert.equal(
    runStatus(result({ closedByUser: true, exitCode: 128, idle: false, messages: answer("earlier") })),
    "stopped",
    "a window that went away during a turn is not a completed run",
  );
  assert.equal(
    runStatus(result({ closedByUser: true, exitCode: 128 })),
    "completed",
    "without proof that a turn was running, do not call a run stopped",
  );
  assert.equal(runStatus(result({ exitCode: 1 })), "failed");
  assert.equal(runStatus(result({ stopReason: "error", exitCode: 0 })), "failed");
});

test("runResult carries the child's state into the result", () => {
  const state = emptyState();
  state.idle = true;
  state.model = "anthropic/x";
  state.stopReason = "stop";
  const built = runResult({
    agent: "scout",
    agentSource: "user",
    task: "t",
    exitCode: 0,
    state,
    stderr: "",
    windowId: "42",
    interactive: true,
  });
  assert.equal(built.idle, true);
  assert.equal(built.model, "anthropic/x");
  assert.equal(built.stopReason, "stop");
  assert.equal(built.windowId, "42");
  assert.equal(built.interactive, true);
});
