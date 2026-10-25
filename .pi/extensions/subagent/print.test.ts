/**
 * Tests for an unattended run: a child process with pipes, no window.
 *
 * The stand-in child is `/bin/sh`, so every case is a real process with real
 * pipe boundaries. A split multi-byte character gets its own case: pipe chunks
 * land wherever the kernel puts them, and a decoder that is wrong there
 * corrupts the answer.
 */

import assert from "node:assert/strict";
import { test } from "node:test";
import { runPrintChild } from "./print.ts";
import { finalOutput } from "./stream.ts";

const record = (text: string): string =>
  `${JSON.stringify({
    type: "message_end",
    message: { role: "assistant", content: [{ type: "text", text }], usage: { input: 3, output: 4, totalTokens: 7 } },
  })}\n`;

test("a child's events reach the caller with its exit code and stderr", async () => {
  const progress: number[] = [];
  const run = await runPrintChild({
    command: "/bin/sh",
    args: ["-c", `printf '%s' '${record("printed")}'; printf 'warn' >&2; exit 3`],
    cwd: process.cwd(),
    onProgress: (state) => {
      progress.push(state.usage.input);
    },
  });

  assert.equal(run.exitCode, 3);
  assert.equal(run.aborted, false);
  assert.equal(finalOutput(run.state), "printed");
  assert.equal(run.state.usage.input, 3);
  assert.equal(run.stderr, "warn");
  assert.ok(progress.length > 0, "progress reached the caller");
});

test("a child that exits 0 ends the run", async () => {
  const run = await runPrintChild({ command: "/bin/sh", args: ["-c", "exit 0"], cwd: process.cwd() });
  assert.equal(run.exitCode, 0);
  assert.equal(run.aborted, false);
});

test("a multi-byte character split across pipe chunks is not corrupted", async () => {
  // "café", with the two UTF-8 bytes of é written in separate writes 100 ms
  // apart, so the pipe read really sees half a character.
  const prefix = '{"type":"message_end","message":{"role":"assistant","content":[{"type":"text","text":"caf';
  const suffix = '"}]}}\n';
  const script = [
    `process.stdout.write(${JSON.stringify(prefix)});`,
    "process.stdout.write(Buffer.from([0xc3]));",
    `setTimeout(() => { process.stdout.write(Buffer.from([0xa9])); process.stdout.write(${JSON.stringify(suffix)}); }, 100);`,
  ].join(" ");
  const run = await runPrintChild({ command: process.execPath, args: ["-e", script], cwd: process.cwd() });
  assert.equal(finalOutput(run.state), "café", "a chunk boundary inside a character kept the text whole");
});

test("abort kills the child and marks the run aborted", async () => {
  const controller = new AbortController();
  const started = Date.now();
  const running = runPrintChild({
    command: "/bin/sh",
    args: ["-c", "sleep 30"],
    cwd: process.cwd(),
    signal: controller.signal,
    killGraceMs: 200,
  });
  setTimeout(() => controller.abort(), 100);
  const run = await running;
  assert.equal(run.aborted, true);
  assert.equal(run.exitCode, 130);
  assert.ok(Date.now() - started < 5000, "the tool call did not wait for the child's own exit");
});

test("abort ends the child and also the command that child started", { timeout: 5000 }, async () => {
  // The inner subshell keeps its own `sleep` and holds the pipe. A signal aimed
  // at the outer shell alone leaves the sleep behind, so the run never ends —
  // which is what a non-forwarding /bin/sh does to a single-process kill.
  const controller = new AbortController();
  const running = runPrintChild({
    command: "/bin/sh",
    args: ["-c", "( sleep 30; exit 0 ) & wait"],
    cwd: process.cwd(),
    signal: controller.signal,
    killGraceMs: 200,
  });
  setTimeout(() => controller.abort(), 100);
  const run = await running;
  assert.equal(run.aborted, true);
  assert.equal(run.exitCode, 130);
});

test("a child that ignores SIGTERM is killed after the grace", async () => {
  const controller = new AbortController();
  const running = runPrintChild({
    command: "/bin/sh",
    args: ["-c", "trap '' TERM; sleep 30"],
    cwd: process.cwd(),
    signal: controller.signal,
    killGraceMs: 100,
  });
  setTimeout(() => controller.abort(), 100);
  const run = await running;
  assert.equal(run.aborted, true);
});

test("a command that cannot start reports the failure instead of hanging", async () => {
  const run = await runPrintChild({ command: "/nonexistent/pi", args: [], cwd: process.cwd() });
  assert.notEqual(run.exitCode, 0);
  assert.match(run.stderr, /ENOENT|no such file/i);
});
