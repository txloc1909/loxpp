import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import {
  buildRunScript,
  closeWindowArgv,
  detectKitty,
  launchArgv,
  listWindowsArgv,
  setTitleArgv,
  shellQuote,
  windowIds,
} from "./kitty.ts";

const session = { windowId: "3" };

test("detectKitty reads KITTY_WINDOW_ID only", () => {
  assert.deepEqual(detectKitty({ KITTY_WINDOW_ID: "3" }), { windowId: "3" });
  assert.deepEqual(detectKitty({ KITTY_WINDOW_ID: " 12 " }), { windowId: "12" });
  assert.equal(detectKitty({}), null);
  assert.equal(detectKitty({ TERM: "xterm-kitty" }), null);
  assert.equal(detectKitty({ KITTY_WINDOW_ID: "" }), null);
  assert.equal(detectKitty({ KITTY_WINDOW_ID: "not-a-number" }), null);
});

test("shellQuote survives a real shell", () => {
  const nasty = "it's $HOME `x` \"q\" \\ and\nnewline";
  const quoted = shellQuote(nasty);
  const result = spawnSync("/bin/sh", ["-c", `printf '%s' ${quoted}`], { encoding: "utf8" });
  assert.equal(result.status, 0);
  assert.equal(result.stdout, nasty);
});

test("launchArgv opens a window in the parent's tab", () => {
  const argv = launchArgv({
    session,
    cwd: "/repo",
    title: "pi subagent: scout",
    command: ["/bin/sh", "/tmp/run.sh"],
  });
  assert.deepEqual(argv, [
    "kitten",
    "@",
    "launch",
    "--match=window_id:3",
    "--type=window",
    "--cwd=/repo",
    "--title=pi subagent: scout",
    "/bin/sh",
    "/tmp/run.sh",
  ]);
});

test("launchArgv adds hold, focus and env in a stable order", () => {
  const argv = launchArgv({
    session,
    cwd: "/repo",
    title: "t",
    command: ["cmd"],
    hold: true,
    keepFocus: true,
    env: { PATH: "/bin", PI_OFFLINE: "1" },
  });
  assert.deepEqual(argv.slice(4, 11), [
    "--type=window",
    "--cwd=/repo",
    "--title=t",
    "--dont-take-focus",
    "--hold",
    "--env=PATH=/bin",
    "--env=PI_OFFLINE=1",
  ]);
  assert.equal(argv.at(-1), "cmd");
});

test("window helper argv point at one window", () => {
  assert.deepEqual(closeWindowArgv("9"), ["kitten", "@", "close-window", "--match=id:9"]);
  assert.deepEqual(setTitleArgv("9", "busy"), ["kitten", "@", "set-window-title", "--match=id:9", "busy"]);
  assert.deepEqual(listWindowsArgv(), ["kitten", "@", "ls"]);
});

test("windowIds reads window ids, not OS window or tab ids", () => {
  const listing = JSON.stringify([
    {
      id: 7,
      tabs: [
        { id: 11, windows: [{ id: 9 }, { id: 12 }] },
        { id: 13, windows: [] },
      ],
    },
  ]);
  assert.deepEqual(windowIds(listing), ["9", "12"]);
  assert.deepEqual(windowIds("not json"), []);
  assert.deepEqual(windowIds("[]"), []);
});

test("buildRunScript reports events, stderr and the exit code", async () => {
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-test-"));
  try {
    const eventsPath = path.join(dir, "events.jsonl");
    const stderrPath = path.join(dir, "stderr.log");
    const exitPath = path.join(dir, "exit.code");
    const scriptPath = path.join(dir, "run.sh");
    await fs.promises.writeFile(
      scriptPath,
      buildRunScript({
        header: ["pi subagent: test", "this window closes when the subagent exits"],
        cwd: dir,
        command: ["/bin/sh", "-c", "printf '{\"type\":\"x\"}\\n'; printf 'boom' >&2; exit 7"],
        eventsPath,
        stderrPath,
        exitPath,
      }),
    );

    const run = spawnSync("/bin/sh", [scriptPath], { encoding: "utf8" });
    assert.equal(run.status, 0, "the wrapper itself exits 0");
    assert.equal(fs.readFileSync(eventsPath, "utf8"), '{"type":"x"}\n');
    assert.equal(fs.readFileSync(stderrPath, "utf8"), "boom");
    assert.equal(fs.readFileSync(exitPath, "utf8").trim(), "7");
    assert.match(run.stdout, /pi subagent: test/);
    assert.match(run.stdout, /this window closes when the subagent exits/);
  } finally {
    await fs.promises.rm(dir, { recursive: true, force: true });
  }
});

test("buildRunScript reports a cwd it cannot enter", async () => {
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-test-"));
  try {
    const exitPath = path.join(dir, "exit.code");
    const scriptPath = path.join(dir, "run.sh");
    await fs.promises.writeFile(
      scriptPath,
      buildRunScript({
        header: ["h"],
        cwd: path.join(dir, "missing"),
        command: ["/bin/sh", "-c", "exit 0"],
        eventsPath: path.join(dir, "events.jsonl"),
        stderrPath: path.join(dir, "stderr.log"),
        exitPath,
      }),
    );

    const run = spawnSync("/bin/sh", [scriptPath], { encoding: "utf8" });
    assert.equal(run.status, 1);
    assert.equal(fs.readFileSync(exitPath, "utf8").trim(), "1");
    assert.match(run.stdout, /cannot enter/);
    assert.equal(fs.existsSync(path.join(dir, "events.jsonl")), false);
  } finally {
    await fs.promises.rm(dir, { recursive: true, force: true });
  }
});
