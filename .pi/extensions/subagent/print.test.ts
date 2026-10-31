/**
 * Tests for an unattended run: a child process with pipes, no window.
 *
 * The stand-in child is `/bin/sh`, so every case is a real process with real
 * pipe boundaries. A split multi-byte character gets its own case: pipe chunks
 * land wherever the kernel puts them, and a decoder that is wrong there
 * corrupts the answer.
 */

import assert from "node:assert/strict";
import { spawn, type ChildProcess } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
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
  assert.equal(run.exitCode, 127);
  assert.match(run.stderr, /cannot start \/nonexistent\/pi/);
});

/** An exited process that nobody has reaped yet still answers `kill -0`. */
function alive(pid: number): boolean {
  try {
    process.kill(pid, 0);
  } catch {
    return false;
  }
  if (!fs.existsSync("/proc/self/stat")) return true;
  try {
    return !/\) Z /.test(fs.readFileSync(`/proc/${pid}/stat`, "utf8"));
  } catch {
    return false;
  }
}

async function waitFor(what: string, check: () => boolean, timeoutMs = 8000): Promise<void> {
  const until = Date.now() + timeoutMs;
  for (;;) {
    if (check()) return;
    if (Date.now() >= until) assert.fail(`timed out waiting for ${what}`);
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
}

test("a run ends its child when the parent pi process is killed", { timeout: 20000 }, async () => {
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-watchdog-"));
  const childPidFile = path.join(dir, "child.pid");
  // The caller of runPrintChild is the process whose death the wrapper must
  // notice, so it runs in a stand-in that the test can kill with SIGKILL.
  const caller = path.join(dir, "caller.ts");
  await fs.promises.writeFile(
    caller,
    [
      `import { runPrintChild } from ${JSON.stringify(path.resolve(".pi/extensions/subagent/print.ts"))};`,
      `const run = runPrintChild({`,
      `  command: "/bin/sh",`,
      `  args: ["-c", ${JSON.stringify(`printf '%s' $$ > ${childPidFile}; sleep 60`)}],`,
      `  cwd: ${JSON.stringify(dir)},`,
      `  watchPollS: 0.05,`,
      `  watchGraceS: 0.3,`,
      `});`,
      `run.then((result) => process.exit(result.exitCode));`,
    ].join("\n"),
  );

  const parent: ChildProcess = spawn(process.execPath, [caller], { stdio: "ignore" });
  try {
    let childPid = 0;
    await waitFor("the child to start", () => {
      try {
        childPid = Number.parseInt(fs.readFileSync(childPidFile, "utf8").trim(), 10);
      } catch {
        return false;
      }
      return Number.isInteger(childPid) && childPid > 1;
    });
    await new Promise((resolve) => setTimeout(resolve, 300));
    assert.equal(alive(childPid), true, "the child lives while the parent lives");

    const exited = new Promise<void>((resolve) => parent.once("exit", () => resolve()));
    parent.kill("SIGKILL");
    await exited;
    await waitFor("the child to end", () => !alive(childPid));
  } finally {
    parent.kill("SIGKILL");
    await fs.promises.rm(dir, { recursive: true, force: true });
  }
});
