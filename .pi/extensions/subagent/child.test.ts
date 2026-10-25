import assert from "node:assert/strict";
import { test } from "node:test";
import type { AgentConfig } from "./agents.ts";
import { childArgs, childEnv, PI_OVERRIDE_ENV, piInvocation } from "./child.ts";

const agent = (overrides: Partial<AgentConfig> = {}): AgentConfig => ({
  name: "scout",
  description: "recon",
  systemPrompt: "you are scout",
  source: "user",
  filePath: "/tmp/scout.md",
  ...overrides,
});

test("the override hook wins over script and PATH resolution", () => {
  const invocation = piInvocation({ [PI_OVERRIDE_ENV]: "/tmp/fake-pi" }, ["node", "/nonexistent/cli.js"]);
  assert.deepEqual(invocation, { command: "/tmp/fake-pi", args: [] });
});

test("a real script path runs under the current runtime", () => {
  const invocation = piInvocation({}, ["node", new URL("./child.ts", import.meta.url).pathname]);
  assert.deepEqual(invocation, { command: process.execPath, args: [new URL("./child.ts", import.meta.url).pathname] });
});

test("a plain node runtime means pi comes from PATH", () => {
  const invocation = piInvocation({}, ["/usr/bin/node", "/nonexistent/cli.js"]);
  assert.deepEqual(invocation, { command: "pi", args: [] });
});

test("childEnv forwards PATH and PI_* only", () => {
  const env = childEnv({ PATH: "/bin", PI_OFFLINE: "1", PI_CODING_AGENT_DIR: "/cfg", HOME: "/home/x", AWS_SECRET: "s" });
  assert.deepEqual(env, { PATH: "/bin", PI_OFFLINE: "1", PI_CODING_AGENT_DIR: "/cfg" });
});

test("childEnv drops empty values", () => {
  assert.deepEqual(childEnv({ PATH: "", PI_OFFLINE: "" }), {});
});

test("childArgs inherits the model and thinking level", () => {
  assert.deepEqual(childArgs(agent(), "find auth", "/tmp/p.md", { model: "p/m", thinking: "high" }), [
    "--mode",
    "json",
    "-p",
    "--no-session",
    "--model",
    "p/m",
    "--thinking",
    "high",
    "--append-system-prompt",
    "/tmp/p.md",
    "Task: find auth",
  ]);
});

test("childArgs keeps a pinned model and drops the inherited thinking level", () => {
  const args = childArgs(agent({ model: "p/fast", tools: ["read", "grep"] }), "look", undefined, { model: "p/m", thinking: "high" });
  assert.deepEqual(args, ["--mode", "json", "-p", "--no-session", "--model", "p/fast", "--tools", "read,grep", "Task: look"]);
});

test("childArgs omits a system prompt flag when there is no prompt", () => {
  const args = childArgs(agent({ systemPrompt: "  " }), "x", undefined, {});
  assert.equal(args.includes("--append-system-prompt"), false);
  assert.equal(args.at(-1), "Task: x");
});

test("an interactive child keeps its terminal and loads the event mirror", () => {
  const args = childArgs(agent({ model: "p/fast" }), "find auth", "/tmp/p.md", { model: "p/m", thinking: "high" }, {
    interactive: true,
    mirrorPath: "/ext/mirror.ts",
  });
  assert.deepEqual(args, [
    "--name",
    "subagent: scout",
    "--extension",
    "/ext/mirror.ts",
    "--model",
    "p/fast",
    "--append-system-prompt",
    "/tmp/p.md",
    "Task: find auth",
  ]);
  // Print mode would end the child as soon as the task is done, and the parent
  // would read its events from stdout, where an interactive child draws the TUI.
  for (const flag of ["-p", "--mode", "--no-session"]) assert.equal(args.includes(flag), false, flag);
});

test("an interactive child without a mirror path still runs, with no event file", () => {
  const args = childArgs(agent(), "x", undefined, {}, { interactive: true });
  assert.equal(args.includes("--extension"), false);
  assert.deepEqual(args.slice(0, 2), ["--name", "subagent: scout"]);
  assert.equal(args.at(-1), "Task: x");
});

test("a print-mode child is unchanged", () => {
  const args = childArgs(agent(), "x", undefined, {}, { interactive: false, mirrorPath: "/ext/mirror.ts" });
  assert.deepEqual(args, ["--mode", "json", "-p", "--no-session", "Task: x"]);
});
