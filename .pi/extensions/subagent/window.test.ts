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
import { checkRemoteControl, runInKittyWindow } from "./window.ts";
import { finalOutput } from "./stream.ts";

const FAKE_KITTEN = `#!/usr/bin/env node
const fs = require("node:fs");
const path = require("node:path");
const { spawn } = require("node:child_process");

const dir = process.env.FAKE_KITTY_DIR;
const [, ...args] = process.argv.slice(2); // drop "@"
const [, sub, ...rest] = args; // drop "--to=<socket>"
const file = (name) => path.join(dir, name);
const windowId = 42;

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
  session: { windowId: "1", listenOn: "unix:/tmp/fake-kitty.sock" },
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
    const windows: string[] = [];
    const run = await runInKittyWindow({
      ...baseOptions(kitten, cwd),
      command: [
        "/bin/sh",
        "-c",
        `printf '%s\\n' '{"type":"message_end","message":{"role":"assistant","content":[{"type":"text","text":"done"}],"usage":{"input":7,"output":3,"totalTokens":10}}}' >> "$LOXPP_SUBAGENT_EVENTS"; printf 'warn' >&2; exit 3`,
      ],
      onWindow: (windowId) => {
        windows.push(windowId);
      },
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
    assert.deepEqual(windows, ["42"], "the window id reaches the caller before the run ends");

    const dir = process.env.FAKE_KITTY_DIR!;
    const argv = JSON.parse(await fs.promises.readFile(path.join(dir, "launch.argv"), "utf8"));
    assert.ok(argv.includes("--match=window_id:1"));
    assert.ok(argv.includes("--cwd=" + cwd));
    assert.ok(argv.includes("--dont-take-focus"));
    assert.equal(argv.includes("--hold"), false);
    assert.match(String(argv.at(-2)), /[/\\]run\.sh$/, "kitty runs the script the extension wrote");
    assert.equal(argv.at(-1), String(process.pid), "the run script watches this process");

    // A `kitten @` without a socket writes into pi's terminal, so every call
    // must name the socket itself.
    const calls = (await fs.promises.readFile(path.join(dir, "calls.log"), "utf8"))
      .trim()
      .split("\n")
      .map((line) => JSON.parse(line) as string[]);
    assert.ok(calls.length > 0);
    for (const call of calls) {
      assert.equal(call[0], "--to=unix:/tmp/fake-kitty.sock", `kitten ${call[1]} names the socket`);
    }
    assert.ok(
      calls.some((call) => call[1] === "close-window"),
      "the auto-close also goes through the socket",
    );

    const closed = await fs.promises.readFile(path.join(dir, "closed.log"), "utf8");
    assert.match(closed, /--match=id:42/, "the window is closed even when kitty already would");
  });
});

test("tty-only remote control is refused before any kitten call", async () => {
  await withFakeKitten(async (_cwd, kitten) => {
    const check = await checkRemoteControl({ windowId: "1", listenOn: null }, kitten);
    assert.equal(check.ok, false);
    if (check.ok) return;
    assert.match(check.problem, /listen_on/);
    assert.match(check.problem, /socket/);
    assert.equal(
      fs.existsSync(path.join(process.env.FAKE_KITTY_DIR!, "calls.log")),
      false,
      "no kitten ran: without a socket it would write kitty's reply into pi's terminal",
    );
  });
});

test("a socket that answers and holds pi's window passes the check", async () => {
  await withFakeKitten(async (_cwd, kitten) => {
    const check = await checkRemoteControl({ windowId: "42", listenOn: "unix:/tmp/fake-kitty.sock" }, kitten);
    assert.deepEqual(check, { ok: true });
  });
});

test("a run refuses a session with no socket before it writes anything", async () => {
  await withFakeKitten(async (cwd, kitten) => {
    await assert.rejects(
      runInKittyWindow({
        ...baseOptions(kitten, cwd),
        session: { windowId: "1", listenOn: null },
        command: ["/bin/sh", "-c", "exit 0"],
        onProgress: () => {},
      }),
      /needs a socket/,
    );
    assert.equal(
      fs.existsSync(path.join(process.env.FAKE_KITTY_DIR!, "calls.log")),
      false,
      "no kitten ran without a socket",
    );
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

    // This run outlives a liveness interval, so the poll path is covered: its
    // listing must name the socket like every other call.
    const calls = (await fs.promises.readFile(path.join(process.env.FAKE_KITTY_DIR!, "calls.log"), "utf8"))
      .trim()
      .split("\n")
      .map((line) => JSON.parse(line) as string[]);
    assert.ok(calls.some((call) => call[1] === "ls"), "the liveness poll ran");
    for (const call of calls) {
      assert.equal(call[0], "--to=unix:/tmp/fake-kitty.sock", `kitten ${call[1]} names the socket`);
    }
  });
});

test("a watched child keeps the terminal and writes its own events", async () => {
  await withFakeKitten(async (cwd, kitten) => {
    // What mirror.ts writes in the child: the parent gets no stdout from a
    // watched child, so the launch flag carries the path.
    const records = [
      { type: "agent_start" },
      { type: "message_end", message: { role: "assistant", content: [{ type: "text", text: "from the mirror" }], usage: { input: 4 } } },
      { type: "agent_end" },
    ];
    await fs.promises.writeFile(path.join(cwd, "mirror.jsonl"), `${records.map((r) => JSON.stringify(r)).join("\n")}\n`);

    const dir = process.env.FAKE_KITTY_DIR!;
    const running = runInKittyWindow({
      ...baseOptions(kitten, cwd),
      // The child waits for the test, so the script can be read while it lives.
      // The wait is bounded: a failed assertion above must not leave it behind.
      command: [
        "/bin/sh",
        "-c",
        'cat mirror.jsonl >> "$LOXPP_SUBAGENT_EVENTS"; i=0; while [ ! -e go ] && [ "$i" -lt 200 ]; do i=$((i + 1)); sleep 0.02; done; exit 0',
      ],
      onProgress: () => {},
    });

    const argv = (await waitForFile(path.join(dir, "launch.argv"), (text) => JSON.parse(text) as string[])) as string[];
    const eventFlag = argv.find((arg) => arg.startsWith("--env=LOXPP_SUBAGENT_EVENTS="));
    assert.ok(eventFlag, "the child is told where to write its events");

    // The script the window runs must leave stdin and stdout alone: that is
    // what makes the window a live session instead of a log sink.
    const eventsPath = eventFlag.slice("--env=LOXPP_SUBAGENT_EVENTS=".length);
    const script = await fs.promises.readFile(path.join(path.dirname(eventsPath), "run.sh"), "utf8");
    assert.equal(script.includes("</dev/null"), false, "stdin stays with the terminal");
    assert.equal(script.includes(`> ${eventsPath}`), false, "stdout stays with the terminal");
    assert.match(script, /0<&3/, "the child keeps the terminal on its stdin");
    assert.match(script, /2> /, "stderr still goes to the log");

    await fs.promises.writeFile(path.join(cwd, "go"), "");
    const run = await running;
    assert.equal(run.exitCode, 0);
    assert.equal(finalOutput(run.state), "from the mirror");
    assert.equal(run.state.idle, true);
    assert.equal(run.state.usage.input, 4);
  });
});

/** Poll until the fake kitten has recorded the launch, then parse it. */
async function waitForFile<T>(file: string, parse: (text: string) => T, timeoutMs = 3000): Promise<T> {
  const until = Date.now() + timeoutMs;
  for (;;) {
    try {
      return parse(await fs.promises.readFile(file, "utf8"));
    } catch (error) {
      if (Date.now() >= until) throw error;
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
  }
}
