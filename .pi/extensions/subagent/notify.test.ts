/**
 * Tests for what the parent session is told about a watched job.
 *
 * The delivery options matter as much as the text: a result that does not wake
 * the parent leaves the answer unread, and one delivered as a steer cuts into a
 * running turn.
 */

import assert from "node:assert/strict";
import { test } from "node:test";
import type { JobView } from "./jobs.ts";
import { capText, deliverResult, launchText, RESULT_CUSTOM_TYPE, resultText, statusText } from "./notify.ts";

const usage = { input: 10, output: 5, cacheRead: 0, cacheWrite: 0, cost: 0, contextTokens: 0, turns: 2 };
const timing = { input: 10, output: 5, cacheRead: 0, cacheWrite: 0, cost: 0, contextTokens: 0, turns: 2 };

const view = (overrides: Partial<JobView> = {}): JobView => ({
  id: 3,
  agent: "scout",
  task: "find the parser\nand the resolver",
  cwd: "/repo",
  startedAt: 0,
  status: "completed",
  windowId: "42",
  output: "the parser is in src/parser.cpp",
  usage,
  turns: 2,
  ...overrides,
});

test("a launch says the run is in a window and the result comes later", () => {
  const text = launchText(view({ status: "running", output: undefined }));
  assert.match(text, /job #3/);
  assert.match(text, /scout/);
  assert.match(text, /window 42/);
  assert.match(text, /find the parser and the resolver/, "the task is one line");
  assert.match(text, /subagent-result/);
  assert.match(text, /Do not guess/);
});

test("a completed run carries its answer and usage", () => {
  const text = resultText(view());
  assert.match(text, /job #3 \(scout\) completed/);
  assert.match(text, /src\/parser\.cpp/);
  assert.match(text, /Usage:/);
});

test("a run cut off by its window says so and warns about the answer", () => {
  const text = resultText(view({ status: "stopped" }));
  assert.match(text, /was stopped/);
  assert.match(text, /window closed while a turn was still running/);
  assert.match(text, /Only the messages the child finished/);
});

test("a failed run carries the reason", () => {
  const text = resultText(view({ status: "failed", errorMessage: "rate limited" }));
  assert.match(text, /failed/);
  assert.match(text, /Reason: rate limited/);
});

test("an empty answer is stated, not left blank", () => {
  assert.match(resultText(view({ output: "" })), /\(no output\)/);
});

test("the result is delivered as a custom message that wakes the parent", () => {
  const sent: unknown[] = [];
  deliverResult((message, options) => sent.push({ message, options }), view());
  assert.equal(sent.length, 1);
  const { message, options } = sent[0] as {
    message: { customType: string; content: string; display: boolean; details: unknown };
    options: { triggerTurn: boolean; deliverAs: string };
  };
  assert.equal(message.customType, RESULT_CUSTOM_TYPE);
  assert.equal(message.display, true);
  assert.match(message.content, /completed/);
  assert.deepEqual(message.details, view());
  assert.deepEqual(options, { triggerTurn: true, deliverAs: "followUp" });
});

test("capText keeps a large answer bounded and says what it dropped", () => {
  const big = "x".repeat(60 * 1024);
  const capped = capText(big);
  assert.ok(capped.length < big.length);
  assert.match(capped, /Output truncated: \d+ bytes omitted/);
  assert.equal(capText("small"), "small");
});

test("statusText names a running job, then adds a count", () => {
  assert.equal(statusText([]), undefined);
  assert.equal(statusText([view({ status: "running", tool: "bash" })]), "subagent scout #3 · window 42 · bash");
  const two = statusText([view({ status: "running" }), view({ id: 4, agent: "worker", windowId: "43", status: "running" })]);
  assert.equal(two, "subagent scout #3 · window 42 +1");
});
