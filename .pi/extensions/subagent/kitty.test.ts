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
  windowIds,
} from "./kitty.ts";

const session = { windowId: "3", listenOn: "unix:/tmp/kitty.sock" };

test("detectKitty reads the window id and the socket address", () => {
  assert.deepEqual(detectKitty({ KITTY_WINDOW_ID: "3", KITTY_LISTEN_ON: "unix:/tmp/k.sock" }), {
    windowId: "3",
    listenOn: "unix:/tmp/k.sock",
  });
  assert.deepEqual(detectKitty({ KITTY_WINDOW_ID: " 12 " }), { windowId: "12", listenOn: null });
  assert.equal(detectKitty({}), null);
  assert.equal(detectKitty({ TERM: "xterm-kitty" }), null);
  assert.equal(detectKitty({ KITTY_WINDOW_ID: "" }), null);
  assert.equal(detectKitty({ KITTY_WINDOW_ID: "not-a-number" }), null);
  // `allow_remote_control yes` without a listen_on line leaves no socket, and
  // `kitten @` then falls back to the terminal, where pi reads its keys.
  assert.equal(detectKitty({ KITTY_WINDOW_ID: "3", KITTY_LISTEN_ON: "  " })?.listenOn, null);
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
    "--to=unix:/tmp/kitty.sock",
    "launch",
    "--match=window_id:3",
    "--type=window",
    "--cwd=/repo",
    "--title=pi subagent: scout",
    "/bin/sh",
    "/tmp/run.sh",
  ]);
});

test("launchArgv refuses to build a call without a socket", () => {
  assert.throws(
    () =>
      launchArgv({
        session: { windowId: "3", listenOn: null },
        cwd: "/repo",
        title: "t",
        command: ["cmd"],
      }),
    /socket/,
  );
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
  assert.deepEqual(argv.slice(5, 12), [
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
  assert.deepEqual(closeWindowArgv("9", "unix:/tmp/k.sock"), [
    "kitten",
    "@",
    "--to=unix:/tmp/k.sock",
    "close-window",
    "--match=id:9",
  ]);
  assert.deepEqual(setTitleArgv("9", "busy", "unix:/tmp/k.sock"), [
    "kitten",
    "@",
    "--to=unix:/tmp/k.sock",
    "set-window-title",
    "--match=id:9",
    "busy",
  ]);
  assert.deepEqual(listWindowsArgv("unix:/tmp/k.sock"), ["kitten", "@", "--to=unix:/tmp/k.sock", "ls"]);
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

test("the run script keeps stdin and stdout with the child and records stderr and the exit code", async () => {
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-test-"));
  try {
    const stderrPath = path.join(dir, "stderr.log");
    const exitPath = path.join(dir, "exit.code");
    const scriptPath = path.join(dir, "run.sh");
    await fs.promises.writeFile(
      scriptPath,
      buildRunScript({
        header: ["pi subagent: test", "this window closes when the subagent exits"],
        cwd: dir,
        command: ["/bin/sh", "-c", 'read line; printf \'got:%s\' "$line"; printf \'boom\' >&2; exit 7'],
        stderrPath,
        exitPath,
        kitten: "kitten",
        socket: "unix:/tmp/kitty.sock",
      }),
    );

    // The keyboard reaches the child and its output reaches the window: that
    // is what makes the window a live session instead of a log sink.
    const run = spawnSync("/bin/sh", [scriptPath, String(process.pid)], { encoding: "utf8", input: "hello\n" });
    assert.equal(run.status, 0, "the wrapper itself exits 0");
    assert.match(run.stdout, /got:hello/);
    assert.match(run.stdout, /pi subagent: test/);
    assert.match(run.stdout, /this window closes when the subagent exits/);
    assert.equal(fs.readFileSync(stderrPath, "utf8"), "boom");
    assert.equal(fs.readFileSync(exitPath, "utf8").trim(), "7");
    const script = fs.readFileSync(scriptPath, "utf8");
    assert.equal(script.includes("</dev/null"), false, "no stdin redirect");
    assert.match(script, /exec 3<&0/, "the terminal is kept on a third descriptor");
    assert.match(script, /0<&3/, "the child gets that descriptor, not /dev/null");
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
        stderrPath: path.join(dir, "stderr.log"),
        exitPath,
        kitten: "kitten",
        socket: "unix:/tmp/kitty.sock",
      }),
    );

    const run = spawnSync("/bin/sh", [scriptPath, String(process.pid)], { encoding: "utf8" });
    assert.equal(run.status, 1);
    assert.equal(fs.readFileSync(exitPath, "utf8").trim(), "1");
    assert.match(run.stdout, /cannot enter/);
  } finally {
    await fs.promises.rm(dir, { recursive: true, force: true });
  }
});
