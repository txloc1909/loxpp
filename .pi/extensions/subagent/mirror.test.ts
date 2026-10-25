/**
 * Tests for the child-side event mirror. The mirror is what lets the parent
 * follow an interactive child, whose stdout belongs to the TUI.
 *
 * Run with `node --test .pi/extensions/subagent/mirror.test.ts`.
 */

import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import mirror, { EVENTS_ENV } from "./mirror.ts";
import { EventStream, finalOutput } from "./stream.ts";

type Handler = (event: unknown) => void;
type ExtensionApi = Parameters<typeof mirror>[0];

/** Stands in for pi's extension API: records the handlers the mirror registers. */
function fakePi(): { api: ExtensionApi; handlers: Map<string, Handler> } {
  const handlers = new Map<string, Handler>();
  const api = {
    on: (name: string, handler: Handler) => {
      handlers.set(name, handler);
    },
  };
  return { api: api as unknown as ExtensionApi, handlers };
}

async function withEventsPath(fn: (eventsPath: string) => void): Promise<void> {
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-mirror-"));
  const previous = process.env[EVENTS_ENV];
  const eventsPath = path.join(dir, "events.jsonl");
  process.env[EVENTS_ENV] = eventsPath;
  try {
    await fn(eventsPath);
  } finally {
    if (previous === undefined) delete process.env[EVENTS_ENV];
    else process.env[EVENTS_ENV] = previous;
    await fs.promises.rm(dir, { recursive: true, force: true });
  }
}

test("the mirror writes records the parent's stream parses", async () => {
  await withEventsPath(async (eventsPath) => {
    const { api, handlers } = fakePi();
    mirror(api);

    handlers.get("agent_start")?.({ type: "agent_start" });
    handlers.get("message_end")?.({
      type: "message_end",
      message: {
        role: "assistant",
        content: [{ type: "text", text: "mirrored answer" }],
        usage: { input: 11, output: 5, totalTokens: 16 },
        model: "p/m",
        stopReason: "end",
      },
    });
    handlers.get("agent_end")?.( { type: "agent_end" });

    // One format for both kinds of child: the parent's own reader must accept
    // these lines, not a private shape of the mirror's.
    const stream = new EventStream();
    assert.equal(stream.push(await fs.promises.readFile(eventsPath, "utf8")), true);
    assert.equal(finalOutput(stream.state), "mirrored answer");
    assert.equal(stream.state.usage.input, 11);
    assert.equal(stream.state.usage.turns, 1);
    assert.equal(stream.state.model, "p/m");
    assert.equal(stream.state.idle, true);
  });
});

test("no events variable means no handlers and no file", async () => {
  const { api, handlers } = fakePi();
  const previous = process.env[EVENTS_ENV];
  delete process.env[EVENTS_ENV];
  try {
    mirror(api);
    assert.equal(handlers.size, 0, "the mirror registers nothing without a destination");
  } finally {
    if (previous !== undefined) process.env[EVENTS_ENV] = previous;
  }
});

test("a write that fails does not break the child's session", async () => {
  const previous = process.env[EVENTS_ENV];
  process.env[EVENTS_ENV] = path.join(os.tmpdir(), "loxpp-subagent-mirror-missing", "events.jsonl");
  try {
    const { api, handlers } = fakePi();
    mirror(api);
    assert.doesNotThrow(() => {
      handlers.get("agent_start")?.({});
      handlers.get("message_end")?.({ message: { role: "assistant", content: [{ type: "text", text: "x" }] } });
    });
  } finally {
    if (previous === undefined) delete process.env[EVENTS_ENV];
    else process.env[EVENTS_ENV] = previous;
  }
});
