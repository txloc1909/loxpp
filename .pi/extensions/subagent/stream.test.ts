import assert from "node:assert/strict";
import { test } from "node:test";
import { addUsage, emptyState, EventStream, finalOutput, formatUsage, lastToolCall } from "./stream.ts";

const assistant = (text: string, usage: Record<string, unknown> = {}, extra: Record<string, unknown> = {}): string =>
  JSON.stringify({
    type: "message_end",
    message: {
      role: "assistant",
      content: [{ type: "text", text }],
      usage,
      ...extra,
    },
  }) + "\n";

test("a line split across chunks is decoded once complete", () => {
  const stream = new EventStream();
  const line = assistant("hello");
  const split = 20;
  assert.equal(stream.push(line.slice(0, split)), false);
  assert.equal(stream.state.messages.length, 0);
  assert.equal(stream.push(line.slice(split)), true);
  assert.equal(stream.state.messages.length, 1);
  assert.equal(finalOutput(stream.state), "hello");
});

test("end() flushes a final line without a newline", () => {
  const stream = new EventStream();
  stream.push(assistant("done").trimEnd());
  assert.equal(stream.state.messages.length, 0);
  assert.equal(stream.end(), true);
  assert.equal(finalOutput(stream.state), "done");
});

test("assistant usage accumulates into session totals", () => {
  const stream = new EventStream();
  stream.push(assistant("one", { input: 100, output: 10, cacheRead: 5, cacheWrite: 2, cost: { total: 0.01 }, totalTokens: 110 }, { model: "p/m" }));
  stream.push(assistant("two", { input: 200, output: 20, cacheRead: 0, cacheWrite: 0, cost: { total: 0.02 }, totalTokens: 130 }, { model: "p/m" }));
  assert.deepEqual(stream.state.usage, {
    input: 300,
    output: 30,
    cacheRead: 5,
    cacheWrite: 2,
    cost: 0.03,
    contextTokens: 130,
    turns: 2,
  });
  assert.equal(stream.state.model, "p/m");
});

test("a tool result is kept but is not a turn", () => {
  const stream = new EventStream();
  stream.push(
    JSON.stringify({ type: "tool_result_end", message: { role: "toolResult", content: [{ type: "text", text: "ok" }] } }) + "\n",
  );
  assert.equal(stream.state.messages.length, 1);
  assert.equal(stream.state.usage.turns, 0);
  assert.equal(stream.state.usage.input, 0);
});

test("malformed and unrelated lines are ignored", () => {
  const stream = new EventStream();
  assert.equal(stream.push("not json\n"), false);
  assert.equal(stream.push("\n"), false);
  assert.equal(stream.push('{"type":"tool_result_end"}\n'), false);
  assert.equal(stream.push('{"type":"turn_start"}\n'), false);
  assert.equal(stream.state.messages.length, 0);
});

test("the run boundaries say whether the child waits at its prompt", () => {
  const stream = new EventStream();
  assert.equal(stream.state.idle, false, "a starting child is not idle");
  assert.equal(stream.push('{"type":"agent_start"}\n'), false, "no change while it already runs");
  assert.equal(stream.push('{"type":"agent_end"}\n'), true);
  assert.equal(stream.state.idle, true);
  // A person typing into an interactive child starts another run.
  assert.equal(stream.push('{"type":"agent_start"}\n'), true);
  assert.equal(stream.state.idle, false);
  assert.equal(stream.push(assistant("done")), true);
  assert.equal(finalOutput(stream.state), "done");
});

test("stop and error reasons surface from the last assistant message", () => {
  const stream = new EventStream();
  stream.push(assistant("boom", {}, { stopReason: "error", errorMessage: "rate limited" }));
  assert.equal(stream.state.stopReason, "error");
  assert.equal(stream.state.errorMessage, "rate limited");
});

test("finalOutput is the last text of the last assistant message", () => {
  const state = emptyState();
  state.messages.push({ role: "assistant", content: [{ type: "text", text: "first" }] });
  state.messages.push({ role: "toolResult", content: [{ type: "text", text: "ignored" }] });
  state.messages.push({ role: "assistant", content: [{ type: "toolCall", name: "bash" }, { type: "text", text: "last" }] });
  assert.equal(finalOutput(state), "last");
  assert.equal(finalOutput(emptyState()), "");
});

test("lastToolCall finds the newest tool call", () => {
  const state = emptyState();
  state.messages.push({ role: "assistant", content: [{ type: "toolCall", name: "read", arguments: { path: "a" } }] });
  state.messages.push({ role: "assistant", content: [{ type: "text", text: "thinking" }, { type: "toolCall", name: "bash", arguments: { command: "ls" } }] });
  assert.deepEqual(lastToolCall(state), { name: "bash", args: { command: "ls" } });
  assert.equal(lastToolCall(emptyState()), null);
});

test("formatUsage names the model only when there is one", () => {
  const usage = { input: 1200, output: 800, cacheRead: 0, cacheWrite: 0, cost: 0, contextTokens: 2000, turns: 3 };
  assert.equal(formatUsage(usage, "p/m"), "3 turns ↑1.2k ↓800 ctx:2.0k p/m");
  assert.equal(formatUsage(usage), "3 turns ↑1.2k ↓800 ctx:2.0k");
});

test("addUsage sums counters and keeps the largest context size", () => {
  const a = { input: 1, output: 2, cacheRead: 3, cacheWrite: 4, cost: 0.5, contextTokens: 100, turns: 1 };
  const b = { input: 10, output: 20, cacheRead: 30, cacheWrite: 40, cost: 1, contextTokens: 50, turns: 2 };
  assert.deepEqual(addUsage(a, b), {
    input: 11,
    output: 22,
    cacheRead: 33,
    cacheWrite: 44,
    cost: 1.5,
    contextTokens: 100,
    turns: 3,
  });
});
