/**
 * End-to-end tests for the tool itself: the real `index.ts`, a fake
 * `ExtensionAPI`, a fake kitty and a stand-in child.
 *
 * These carry the two rules the redesign exists for:
 *
 * 1. A watched call returns while its run is still going, and the run's end
 *    arrives later as a message that wakes the session. Replacing the launch
 *    with a wait on the run leaves the first case waiting for the child's own
 *    exit, so it fails by timeout.
 * 2. An unattended call opens no window: the fake kitty records every call, and
 *    this file asserts it was never called.
 *
 * `index.ts` imports the pi packages, so it loads only where they are
 * installed. Without them every case here skips.
 */

import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import { pathToFileURL } from "node:url";

const HERE = import.meta.dirname;

/**
 * Where the pi packages are, and where `typebox` is. npm puts a global
 * scoped package's dependencies inside that package, so `typebox` is not
 * necessarily beside `@earendil-works`.
 */
interface PiModules {
  /** Every directory that holds an `@earendil-works` scope, nearest first. */
  scopes: string[];
  typebox: string;
}

function piModules(): PiModules | null {
  const roots: string[] = [];
  if (process.env.PI_PACKAGES_DIR) roots.push(process.env.PI_PACKAGES_DIR);
  try {
    roots.push(execFileSync("npm", ["root", "-g"], { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }).trim());
  } catch {
    // npm is not installed; the fixed candidates still count.
  }
  roots.push(path.join(path.dirname(process.execPath), "..", "lib", "node_modules"));
  roots.push("/usr/lib/node_modules", "/usr/local/lib/node_modules");

  for (const root of roots) {
    for (const dir of [path.join(root, "pi", "node_modules"), root]) {
      const scope = path.join(dir, "@earendil-works");
      if (!fs.existsSync(path.join(scope, "pi-coding-agent"))) continue;
      // A global install nests the sibling pi packages inside pi-coding-agent,
      // so the sibling scope is a second source for the same names.
      const scopes = [scope, path.join(scope, "pi-coding-agent", "node_modules", "@earendil-works")].filter((entry) =>
        fs.existsSync(entry),
      );
      const typebox = [
        path.join(dir, "typebox"),
        path.join(scope, "pi-coding-agent", "node_modules", "typebox"),
        path.join(dir, "..", "typebox"),
      ].find((candidate) => fs.existsSync(candidate));
      if (typebox) return { scopes, typebox };
    }
  }
  return null;
}

const MODULES = piModules();
const skip = MODULES ? false : "the pi packages are not installed here; set PI_PACKAGES_DIR";

const FAKE_KITTEN = `#!/usr/bin/env node
const fs = require("node:fs");
const path = require("node:path");
const { spawn } = require("node:child_process");

const dir = process.env.FAKE_KITTY_DIR;
const [, ...args] = process.argv.slice(2); // drop "@"
const [, sub, ...rest] = args; // drop "--to=<socket>"
const file = (name) => path.join(dir, name);
const launched = 77;

fs.appendFileSync(file("calls.log"), JSON.stringify(args) + "\\n");

if (sub === "launch") {
  const command = [];
  const env = { ...process.env };
  for (let i = 0; i < rest.length; i++) {
    const arg = rest[i];
    if (arg.startsWith("--env=")) {
      const pair = arg.slice("--env=".length);
      const eq = pair.indexOf("=");
      if (eq > 0) env[pair.slice(0, eq)] = pair.slice(eq + 1);
      continue;
    }
    if (arg.startsWith("--")) continue;
    command.push(...rest.slice(i));
    break;
  }
  fs.writeFileSync(file("launch.argv"), JSON.stringify(rest));
  const child = spawn(command[0], command.slice(1), { detached: true, stdio: "ignore", env });
  child.unref();
  fs.writeFileSync(file("window.pid"), String(child.pid));
  process.stdout.write(String(launched) + "\\n");
} else if (sub === "ls") {
  process.stdout.write(JSON.stringify([{ id: 1, tabs: [{ id: 1, windows: [{ id: 42 }, { id: launched }] }] }]) + "\\n");
} else if (sub === "close-window") {
  fs.appendFileSync(file("closed.log"), rest.join(" ") + "\\n");
  try {
    process.kill(Number(fs.readFileSync(file("window.pid"), "utf8")), "SIGTERM");
  } catch {
    // already gone
  }
} else if (sub === "set-window-title") {
  fs.appendFileSync(file("titles.log"), rest.join(" ") + "\\n");
} else {
  process.stderr.write("unknown subcommand " + sub + "\\n");
  process.exit(2);
}
`;

/**
 * The stand-in for pi. A watched run tells it where to mirror, so it writes
 * its events and then waits for a `go` file in its working directory; the test
 * decides when the run ends. An unattended run gets no mirror path and answers
 * on stdout.
 */
const STAND_IN = `#!/bin/sh
if [ -n "$LOXPP_SUBAGENT_EVENTS" ]; then
  printf '%s\\n' '{"type":"agent_start"}' >> "$LOXPP_SUBAGENT_EVENTS"
  printf '%s\\n' '{"type":"message_end","message":{"role":"assistant","content":[{"type":"text","text":"watched answer"}],"usage":{"input":2,"output":3,"totalTokens":5}}}' >> "$LOXPP_SUBAGENT_EVENTS"
  printf '%s\\n' '{"type":"agent_end"}' >> "$LOXPP_SUBAGENT_EVENTS"
  i=0
  while [ ! -e go ] && [ "$i" -lt 500 ]; do i=$((i + 1)); sleep 0.02; done
  exit 0
fi
printf '%s\\n' '{"type":"message_end","message":{"role":"assistant","content":[{"type":"text","text":"unattended answer"}],"usage":{"input":2,"output":3,"totalTokens":5}}}'
exit 0
`;

interface Box {
  root: string;
  bin: string;
  work: string;
  kittenDir: string;
  ext: string;
  agentDir: string;
}

async function sandbox(): Promise<Box> {
  const root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-index-"));
  const bin = path.join(root, "bin");
  const work = path.join(root, "work");
  const kittenDir = path.join(root, "kitten");
  const ext = path.join(root, "ext");
  const agentDir = path.join(root, "agent");
  for (const dir of [bin, work, kittenDir, ext, path.join(agentDir, "agents")]) {
    await fs.promises.mkdir(dir, { recursive: true });
  }

  await fs.promises.writeFile(path.join(bin, "kitten"), FAKE_KITTEN, { mode: 0o700 });
  await fs.promises.writeFile(path.join(bin, "pi-stand-in"), STAND_IN, { mode: 0o700 });
  await fs.promises.writeFile(
    path.join(agentDir, "agents", "scout.md"),
    ["---", "name: scout", "description: Find the code that answers a question", "---", "You are a scout.", ""].join("\n"),
  );

  // A copy, so the pi packages resolve from this directory. A symlinked
  // extension would resolve its imports from the repository instead.
  for (const entry of await fs.promises.readdir(HERE)) {
    if (!entry.endsWith(".ts") || entry.endsWith(".test.ts")) continue;
    await fs.promises.copyFile(path.join(HERE, entry), path.join(ext, entry));
  }
  // A copy, so the pi packages resolve from this directory, and a link to the
  // installation's own copy of each one.
  const modules = path.join(root, "node_modules");
  const scopeDir = path.join(modules, "@earendil-works");
  await fs.promises.mkdir(scopeDir, { recursive: true });
  for (const scope of MODULES!.scopes) {
    for (const entry of await fs.promises.readdir(scope)) {
      const link = path.join(scopeDir, entry);
      if (fs.existsSync(link)) continue;
      await fs.promises.symlink(path.join(scope, entry), link, "dir");
    }
  }
  await fs.promises.symlink(MODULES!.typebox, path.join(modules, "typebox"), "dir");
  return { root, bin, work, kittenDir, ext, agentDir };
}

interface Harness {
  tool: {
    execute: (
      id: string,
      params: Record<string, unknown>,
      signal: AbortSignal | undefined,
      onUpdate: ((partial: unknown) => void) | undefined,
      ctx: Record<string, unknown>,
    ) => Promise<{ content: Array<{ type: string; text?: string }>; details?: unknown; isError?: boolean }>;
  };
  sent: Array<{ message: { customType: string; content: string; display: boolean; details: unknown }; options: unknown }>;
  restore: () => void;
}

async function register(box: Box, env: { kitty: boolean }): Promise<Harness> {
  const previous = { ...process.env };
  const listenOn = `unix:/tmp/loxpp-index-${process.pid}-${Math.random().toString(36).slice(2)}.sock`;
  process.env.PATH = `${box.bin}:${previous.PATH}`;
  process.env.FAKE_KITTY_DIR = box.kittenDir;
  process.env.LOXPP_SUBAGENT_PI = path.join(box.bin, "pi-stand-in");
  process.env.PI_CODING_AGENT_DIR = box.agentDir;
  if (env.kitty) {
    process.env.KITTY_WINDOW_ID = "42";
    process.env.KITTY_LISTEN_ON = listenOn;
  } else {
    delete process.env.KITTY_WINDOW_ID;
    delete process.env.KITTY_LISTEN_ON;
  }

  const { default: subagentExtension } = await import(pathToFileURL(path.join(box.ext, "index.ts")).href);
  const tools = new Map<string, Harness["tool"]>();
  const sent: Harness["sent"] = [];
  const handlers = new Map<string, (event: unknown, ctx: unknown) => unknown>();
  const api = {
    on: (event: string, handler: (event: unknown, ctx: unknown) => unknown) => {
      handlers.set(event, handler);
      return () => {};
    },
    registerTool: (tool: Harness["tool"]) => tools.set((tool as unknown as { name: string }).name, tool),
    registerMessageRenderer: () => {},
    sendMessage: (message: Harness["sent"][number]["message"], options: unknown) => sent.push({ message, options }),
  };
  subagentExtension(api as never);
  await handlers.get("session_start")?.({ type: "session_start", reason: "startup" }, ctx(box));
  const tool = tools.get("subagent");
  assert.ok(tool, "the tool is registered");
  return {
    tool,
    sent,
    restore: () => {
      process.env = previous;
    },
  };
}

const ctx = (box: Box, statuses?: Array<[string, string | undefined]>): Record<string, unknown> => ({
  cwd: box.work,
  hasUI: true,
  isProjectTrusted: () => true,
  ui: {
    setStatus: (key: string, text: string | undefined) => statuses?.push([key, text]),
    notify: () => {},
  },
});

const textOf = (result: { content: Array<{ type: string; text?: string }> }): string =>
  result.content.map((part) => part.text ?? "").join("\n");

async function waitFor(check: () => boolean, message: string, timeoutMs = 5000): Promise<void> {
  const until = Date.now() + timeoutMs;
  for (;;) {
    if (check()) return;
    if (Date.now() >= until) assert.fail(message);
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
}

async function withHarness(opts: { kitty: boolean }, body: (box: Box, harness: Harness) => Promise<void>): Promise<void> {
  const box = await sandbox();
  let harness: Harness | undefined;
  try {
    harness = await register(box, opts);
    await body(box, harness);
  } finally {
    harness?.restore();
    await fs.promises.rm(box.root, { recursive: true, force: true });
  }
}

test("a watched call returns at once, and its result arrives as a message", { skip, timeout: 10_000 }, async () => {
  await withHarness({ kitty: true }, async (box, harness) => {
    const statuses: Array<[string, string | undefined]> = [];
    const result = await harness.tool.execute(
      "call-1",
      { agent: "scout", task: "find it" },
      undefined,
      undefined,
      ctx(box, statuses),
    );

    assert.match(textOf(result), /Started subagent job #1: scout \(window 77\)/);
    assert.equal(result.isError, undefined);
    assert.equal(harness.sent.length, 0, "the launch did not wait for the run");

    // The run is still going: its window exists, and nobody has reported it.
    await waitFor(() => fs.existsSync(path.join(box.kittenDir, "launch.argv")), "the watched run opened a window");
    assert.equal(harness.sent.length, 0, "no result while the child is still running");

    // End the run, then the message that wakes the parent must arrive.
    await fs.promises.writeFile(path.join(box.work, "go"), "");
    await waitFor(() => harness.sent.length === 1, "the end of the run was reported");
    const [{ message, options }] = harness.sent;
    assert.equal(message.customType, "subagent-result");
    assert.equal(message.display, true);
    assert.deepEqual(options, { triggerTurn: true, deliverAs: "followUp" });
    assert.match(message.content, /completed/);
    assert.match(message.content, /watched answer/);

    // The footer follows the job: a running line, then no line.
    assert.ok(statuses.some(([key, text]) => key === "subagent" && Boolean(text?.includes("scout #1"))), "the footer named the job");
    assert.deepEqual(statuses.at(-1), ["subagent", undefined], "the footer is cleared when the job ends");
  });
});

test("an unattended call opens no window and returns its answer", { skip, timeout: 10_000 }, async () => {
  await withHarness({ kitty: true }, async (box, harness) => {
    const result = await harness.tool.execute(
      "call-2",
      { agent: "scout", task: "find it", interactive: false },
      undefined,
      undefined,
      ctx(box),
    );

    assert.match(textOf(result), /unattended answer/);
    assert.equal(fs.existsSync(path.join(box.kittenDir, "calls.log")), false, "an unattended run never asks kitty for a window");
    assert.equal(harness.sent.length, 0, "an unattended run reports through the tool result");
  });
});

test("a watched call outside kitty is refused with a way forward", { skip, timeout: 10_000 }, async () => {
  await withHarness({ kitty: false }, async (box, harness) => {
    const result = await harness.tool.execute("call-3", { agent: "scout", task: "find it" }, undefined, undefined, ctx(box));
    assert.equal(result.isError, true);
    assert.match(textOf(result), /kitty/);
    assert.match(textOf(result), /interactive: false/);
    assert.equal(harness.sent.length, 0);
  });
});

test("an unattended call works outside kitty", { skip, timeout: 10_000 }, async () => {
  await withHarness({ kitty: false }, async (box, harness) => {
    const result = await harness.tool.execute(
      "call-4",
      { agent: "scout", task: "find it", interactive: false },
      undefined,
      undefined,
      ctx(box),
    );
    assert.match(textOf(result), /unattended answer/);
    assert.equal(fs.existsSync(path.join(box.kittenDir, "calls.log")), false);
  });
});

test("an explicit interactive batch is refused before any window opens", { skip, timeout: 10_000 }, async () => {
  await withHarness({ kitty: true }, async (box, harness) => {
    const result = await harness.tool.execute(
      "call-5",
      { tasks: [{ agent: "scout", task: "a" }], interactive: true },
      undefined,
      undefined,
      ctx(box),
    );
    assert.equal(result.isError, true);
    assert.match(textOf(result), /interactive applies to a single task only/);
    assert.equal(fs.existsSync(path.join(box.kittenDir, "calls.log")), false);
  });
});

test("a session with no UI cannot start a watched run", { skip, timeout: 10_000 }, async () => {
  await withHarness({ kitty: true }, async (box, harness) => {
    const headless = { ...ctx(box), hasUI: false };
    const result = await harness.tool.execute("call-6", { agent: "scout", task: "find it" }, undefined, undefined, headless);
    assert.equal(result.isError, true);
    assert.match(textOf(result), /interactive: false/);
  });
});
