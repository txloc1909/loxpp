/**
 * Tests for grading a finished subagent run. The closed-by-user rule is the
 * one that decides whether a person ending an interactive window reports
 * success, so each end condition gets its own case.
 *
 * Run with `node --test .pi/extensions/subagent/result.test.ts`.
 */

import assert from "node:assert/strict";
import { test } from "node:test";
import { isFailed, resultOutput, type SingleResult } from "./result.ts";

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

test("a print-mode window closed by hand is a failure", () => {
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
