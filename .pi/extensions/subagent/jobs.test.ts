/**
 * Tests for watched jobs.
 *
 * The load-bearing case is the first one: a launch must return while its run is
 * still going. That is the whole reason this module exists, so replacing the
 * race in `launch` with an await on the run fails here.
 */

import assert from "node:assert/strict";
import { test } from "node:test";
import { createJobManager, type JobView } from "./jobs.ts";
import type { SingleResult } from "./result.ts";
import { emptyState } from "./stream.ts";

const usage = { input: 1, output: 2, cacheRead: 0, cacheWrite: 0, cost: 0, contextTokens: 0, turns: 1 };

const resultOf = (overrides: Partial<SingleResult> = {}): SingleResult => ({
  agent: "scout",
  agentSource: "user",
  task: "t",
  exitCode: 0,
  messages: [{ role: "assistant", content: [{ type: "text", text: "the answer" }] }],
  stderr: "",
  usage,
  interactive: true,
  idle: true,
  ...overrides,
});

const tick = (): Promise<void> => new Promise((resolve) => setImmediate(resolve));

function deferred<T>(): { promise: Promise<T>; resolve: (value: T) => void; reject: (error: unknown) => void } {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

test("a launch returns when the window opens, not when the run ends", async () => {
  const finish = deferred<SingleResult>();
  const manager = createJobManager({ notify: () => assert.fail("no notification before the run ends") });

  const launched = await manager.launch({
    agent: "scout",
    task: "find it",
    cwd: "/tmp",
    run: ({ onWindow }) => {
      onWindow("42");
      return finish.promise;
    },
  });

  assert.equal(launched.windowId, "42");
  assert.equal(launched.status, "running");
  assert.equal(manager.running().length, 1, "the run is still going");
  finish.resolve(resultOf());
});

test("the end of a run notifies once, with the answer and the grade", async () => {
  const notified: JobView[] = [];
  const finish = deferred<SingleResult>();
  const manager = createJobManager({ notify: (view) => notified.push({ ...view }) });

  await manager.launch({
    agent: "scout",
    task: "find it",
    cwd: "/tmp",
    run: ({ onWindow }) => {
      onWindow("42");
      return finish.promise;
    },
  });
  assert.equal(notified.length, 0, "nothing to report while the run is going");

  finish.resolve(resultOf());
  await tick();
  assert.equal(notified.length, 1);
  assert.equal(notified[0].status, "completed");
  assert.equal(notified[0].output, "the answer");
  assert.equal(notified[0].usage?.turns, 1);
  assert.deepEqual(manager.running(), []);
});

test("a run cut off by its window is reported as stopped, not completed", async () => {
  const notified: JobView[] = [];
  const manager = createJobManager({ notify: (view) => notified.push({ ...view }) });
  await manager.launch({
    agent: "scout",
    task: "find it",
    cwd: "/tmp",
    run: async ({ onWindow }) => {
      onWindow("42");
      return resultOf({ closedByUser: true, idle: false, exitCode: 128 });
    },
  });
  await tick();
  assert.equal(notified.length, 1);
  assert.equal(notified[0].status, "stopped");
});

test("a run that fails before its window opens rejects the launch and sends no message", async () => {
  const notified: JobView[] = [];
  const manager = createJobManager({ notify: (view) => notified.push(view) });
  await assert.rejects(
    manager.launch({
      agent: "scout",
      task: "find it",
      cwd: "/tmp",
      run: async () => {
        throw new Error("kitty could not open a subagent window: no socket");
      },
    }),
    /kitty could not open a subagent window/,
  );
  await tick();
  assert.equal(notified.length, 0, "the tool call carries this failure, not a message");
  assert.deepEqual(manager.running(), []);
});

test("a run that opens its window and then fails still notifies", async () => {
  const notified: JobView[] = [];
  const manager = createJobManager({ notify: (view) => notified.push({ ...view }) });
  await manager.launch({
    agent: "scout",
    task: "find it",
    cwd: "/tmp",
    run: async ({ onWindow }) => {
      onWindow("42");
      throw new Error("the child crashed");
    },
  });
  await tick();
  assert.equal(notified.length, 1);
  assert.equal(notified[0].status, "failed");
  assert.equal(notified[0].errorMessage, "the child crashed");
});

test("abandonAll aborts every run and sends nothing", async () => {
  const notified: JobView[] = [];
  let aborted = false;
  const manager = createJobManager({ notify: (view) => notified.push(view) });
  await manager.launch({
    agent: "scout",
    task: "find it",
    cwd: "/tmp",
    run: ({ signal, onWindow }) => {
      onWindow("42");
      return new Promise<SingleResult>((resolve) => {
        signal.addEventListener("abort", () => {
          aborted = true;
          resolve(resultOf({ closedByUser: true, idle: true, exitCode: 128 }));
        });
      });
    },
  });

  manager.abandonAll();
  assert.equal(aborted, true, "the abort reached the run");
  await tick();
  assert.equal(notified.length, 0, "a replaced session gets no stale result");
  assert.deepEqual(manager.running(), []);
});

test("progress reports the running set and the current tool", async () => {
  const seen: number[] = [];
  const { promise, resolve } = deferred<SingleResult>();
  const manager = createJobManager({ notify: () => {}, onProgress: (running) => seen.push(running.length) });
  await manager.launch({
    agent: "scout",
    task: "find it",
    cwd: "/tmp",
    run: ({ onWindow, onProgress }) => {
      onWindow("42");
      const state = emptyState();
      state.usage.turns = 3;
      state.messages = [{ role: "assistant", content: [{ type: "toolCall", name: "bash", arguments: {} }] }];
      onProgress(state);
      return promise;
    },
  });
  const [running] = manager.running();
  assert.equal(running.turns, 3);
  assert.equal(running.tool, "bash");
  assert.ok(seen.includes(1), "the footer was told a job is running");
  resolve(resultOf());
});
