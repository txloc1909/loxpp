/**
 * Tests for the window plumbing. A fake `kitten` stands in for kitty: it
 * spawns the command, lists a single window, and honours close-window. This
 * keeps the launch, tailing, exit-code, auto-close and hand-closed paths
 * covered without a terminal.
 */

import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import { runInKittyWindow } from "./window.ts";

const FAKE_KITTEN = `#!/usr/bin/env node
const fs = require("node:fs");
const path = require("node:path");
const { spawn } = require("node:child_process");

const dir = process.env.FAKE_KITTY_DIR;
const [, sub, ...rest] = process.argv.slice(2);
const file = (name) => path.join(dir, name);
const windowId = 42;

if (sub === "launch") {
  const command = [];
  for (let i = 0; i < rest.length; i++) {
    if (rest[i].startsWith("--")) continue;
    command.push(...rest.slice(i));
    break;
  }
  fs.writeFileSync(file("launch.argv"), JSON.stringify(rest));
  const child = spawn(command[0], command.slice(1), { detached: true, stdio: "ignore" });
  child.unref();
  fs.writeFileSync(file("window.pid"), String(child.pid));
  process.stdout.write(String(windowId) + "\\n");
} else if (sub === "ls") {
  if (fs.existsSync(file("hide"))) process.stdout.write("[]\\n");
  else process.stdout.write(JSON.stringify([{ id: 1, tabs: [{ id: 1, windows: [{ id: windowId }] }] }]) + "\\n");
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

async function withFakeKitten(fn: (dir: string, kitten: string) => Promise<void>): Promise<void> {
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-window-"));
  const kitten = path.join(dir, "kitten");
  await fs.promises.writeFile(kitten, FAKE_KITTEN, { mode: 0o700 });
  const previous = process.env.FAKE_KITTY_DIR;
  process.env.FAKE_KITTY_DIR = dir;
  const work = path.join(dir, "work");
  await fs.promises.mkdir(work);
  try {
    await fn(work, kitten);
  } finally {
    if (previous === undefined) delete process.env.FAKE_KITTY_DIR;
    else process.env.FAKE_KITTY_DIR = previous;
    await fs.promises.rm(dir, { recursive: true, force: true });
  }
}

const baseOptions = (kitten: string, cwd: string) => ({
  session: { windowId: "1" },
  cwd,
  title: "pi subagent: test",
  header: ["pi subagent: test"],
  env: {},
  hold: false,
  keepFocus: true,
  signal: undefined,
  kitten,
  pollMs: 20,
  livenessMs: 100,
  deadWindowGraceMs: 100,
});

test("a finished run reports its events, stderr and exit code", async () => {
  await withFakeKitten(async (cwd, kitten) => {
    const progress: number[] = [];
    const run = await runInKittyWindow({
      ...baseOptions(kitten, cwd),
      command: [
        "/bin/sh",
        "-c",
        `printf '%s\\n' '{"type":"message_end","message":{"role":"assistant","content":[{"type":"text","text":"done"}],"usage":{"input":7,"output":3,"totalTokens":10}}}'; printf 'warn' >&2; exit 3`,
      ],
      onProgress: (state) => {
        progress.push(state.usage.turns);
      },
    });

    assert.equal(run.exitCode, 3);
    assert.equal(run.closedByUser, false);
    assert.equal(run.windowId, "42");
    assert.equal(run.state.usage.input, 7);
    assert.equal(run.stderr, "warn");
    assert.deepEqual(run.state.messages[0].content, [{ type: "text", text: "done" }]);
    assert.ok(progress.length > 0, "progress reached the caller");

    const argv = JSON.parse(await fs.promises.readFile(path.join(process.env.FAKE_KITTY_DIR!, "launch.argv"), "utf8"));
    assert.ok(argv.includes("--match=window_id:1"));
    assert.ok(argv.includes("--cwd=" + cwd));
    assert.ok(argv.includes("--dont-take-focus"));
    assert.equal(argv.includes("--hold"), false);

    const closed = await fs.promises.readFile(path.join(process.env.FAKE_KITTY_DIR!, "closed.log"), "utf8");
    assert.match(closed, /--match=id:42/, "the window is closed even when kitty already would");
  });
});

test("a held window is not closed", async () => {
  await withFakeKitten(async (cwd, kitten) => {
    await runInKittyWindow({
      ...baseOptions(kitten, cwd),
      hold: true,
      command: ["/bin/sh", "-c", "exit 0"],
      onProgress: () => {},
    });
    const dir = process.env.FAKE_KITTY_DIR!;
    assert.equal(fs.existsSync(path.join(dir, "closed.log")), false);
    const argv = JSON.parse(await fs.promises.readFile(path.join(dir, "launch.argv"), "utf8"));
    assert.ok(argv.includes("--hold"));
  });
});

test("a failed launch reports why", async () => {
  await withFakeKitten(async (cwd, kitten) => {
    await assert.rejects(
      runInKittyWindow({
        ...baseOptions(kitten, cwd),
        command: ["/bin/sh", "-c", "exit 0"],
        onProgress: () => {},
        // A subcommand the fake does not know, so it exits 2 with stderr.
        kitten: kitten + "-missing",
      }),
      /kitty could not open a subagent window/,
    );
  });
});

test("aborting closes the window and rejects", async () => {
  await withFakeKitten(async (cwd, kitten) => {
    const controller = new AbortController();
    const running = runInKittyWindow({
      ...baseOptions(kitten, cwd),
      signal: controller.signal,
      command: ["/bin/sh", "-c", "sleep 5"],
      onProgress: () => {},
    });
    setTimeout(() => controller.abort(), 300);
    await assert.rejects(running, /Subagent aborted/);
    const closed = await fs.promises.readFile(path.join(process.env.FAKE_KITTY_DIR!, "closed.log"), "utf8");
    assert.match(closed, /--match=id:42/);
  });
});

test("a window closed by hand ends the run instead of hanging", async () => {
  await withFakeKitten(async (cwd, kitten) => {
    await fs.promises.writeFile(path.join(process.env.FAKE_KITTY_DIR!, "hide"), "");
    const run = await runInKittyWindow({
      ...baseOptions(kitten, cwd),
      command: ["/bin/sh", "-c", "sleep 5"],
      onProgress: () => {},
    });
    assert.equal(run.closedByUser, true);
    assert.equal(run.exitCode, 128);
  });
});
