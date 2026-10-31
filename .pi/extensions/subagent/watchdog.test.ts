/**
 * Tests for the watchdog: the shell text that ends a run when the parent pi
 * process dies.
 *
 * A `SIGKILL` of pi runs no handler in pi, so the parent cannot close a run it
 * started. What stays behind is the defect in #898: a window that still runs,
 * and a child whose pipes or session have nobody to report to.
 *
 * Each case uses a real stand-in parent process and a real child process, so
 * the signal path is the one production takes. The stand-in parent is a process
 * the test may kill, which the test process itself is not.
 */

import assert from "node:assert/strict";
import { spawn, spawnSync, type ChildProcess } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import { buildRunScript, closeWindowArgv } from "./kitty.ts";
import { buildPrintScript } from "./print.ts";
import { shellQuote, WATCHDOG_MARKER } from "./watchdog.ts";

/** Fast enough for a test, and still several real poll cycles. */
const WATCH = { watchPollS: 0.05, watchGraceS: 0.3 };
const TIMEOUT_MS = 8000;

async function withTempDir(fn: (dir: string) => Promise<void>): Promise<void> {
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-watchdog-"));
  try {
    await fn(dir);
  } finally {
    await fs.promises.rm(dir, { recursive: true, force: true });
  }
}

/** A process the test can kill, standing in for the parent pi process. */
function standInParent(): ChildProcess {
  return spawn(process.execPath, ["-e", "setTimeout(() => {}, 60000)"], { stdio: "ignore" });
}

async function killParent(parent: ChildProcess): Promise<void> {
  const exited = new Promise<void>((resolve) => parent.once("exit", () => resolve()));
  parent.kill("SIGKILL");
  // Wait for the exit, so nothing answers `kill -0` for a zombie afterwards.
  await exited;
}

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

async function waitFor(what: string, check: () => Promise<boolean> | boolean, timeoutMs = TIMEOUT_MS): Promise<void> {
  const until = Date.now() + timeoutMs;
  for (;;) {
    if (await check()) return;
    if (Date.now() >= until) assert.fail(`timed out waiting for ${what}`);
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
}

async function readOr(file: string, fallback: string): Promise<string> {
  try {
    return await fs.promises.readFile(file, "utf8");
  } catch {
    return fallback;
  }
}

async function readPid(file: string): Promise<number> {
  let pid = 0;
  await waitFor(file, async () => {
    pid = Number.parseInt((await readOr(file, "")).trim(), 10);
    return Number.isInteger(pid) && pid > 1;
  });
  return pid;
}

/** A `kitten` stand-in that records the calls the watcher makes. */
async function fakeKitten(dir: string): Promise<string> {
  const keeper = path.join(dir, "kitten");
  await fs.promises.writeFile(keeper, `#!/bin/sh\nprintf '%s\\n' "$*" >> ${shellQuote(path.join(dir, "kitten.log"))}\n`, {
    mode: 0o700,
  });
  return keeper;
}

async function watchedScript(dir: string, options: { child: string; kitten: string; exitPath: string }): Promise<string> {
  const scriptPath = path.join(dir, "run.sh");
  await fs.promises.writeFile(
    scriptPath,
    buildRunScript({
      header: ["pi subagent: watchdog test"],
      cwd: dir,
      command: ["/bin/sh", "-c", options.child],
      stderrPath: path.join(dir, "stderr.log"),
      exitPath: options.exitPath,
      kitten: options.kitten,
      socket: "unix:/tmp/fake-kitty.sock",
      ...WATCH,
    }),
    { mode: 0o700 },
  );
  return scriptPath;
}

test("shellQuote survives a real shell", () => {
  const nasty = "it's $HOME `x` \"q\" \\ and\nnewline";
  const quoted = shellQuote(nasty);
  const result = spawnSync("/bin/sh", ["-c", `printf '%s' ${quoted}`], { encoding: "utf8" });
  assert.equal(result.status, 0);
  assert.equal(result.stdout, nasty);
});

test("every script builder carries the watchdog", () => {
  const watched = buildRunScript({
    header: ["h"],
    cwd: "/",
    command: ["/bin/sh", "-c", "true"],
    stderrPath: "/tmp/err.log",
    exitPath: "/tmp/exit.code",
    kitten: "kitten",
    socket: "unix:/tmp/sock",
  });
  const unattended = buildPrintScript({ command: ["/bin/sh", "-c", "true"] });
  for (const [name, script] of [
    ["the watched run script", watched],
    ["the unattended wrapper", unattended],
  ]) {
    assert.ok(script.includes(WATCHDOG_MARKER), `${name} watches the parent`);
  }
});

test("a watched run ends its child and closes its window when the parent dies", { timeout: 20000 }, async () => {
  await withTempDir(async (dir) => {
    const kitten = await fakeKitten(dir);
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten,
      exitPath: path.join(dir, "exit.code"),
    });

    const parent = standInParent();
    // Detached, as kitty starts a window's command: the script leads its own
    // process group, so a group signal cannot reach this test process.
    const script = spawn("/bin/sh", [scriptPath, String(parent.pid)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile);
      await new Promise((resolve) => setTimeout(resolve, 300));
      assert.equal(alive(childPid), true, "the child lives while the parent lives");

      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid));
      await waitFor("the window to close", async () => /close-window/.test(await readOr(path.join(dir, "kitten.log"), "")));
      const logged = await fs.promises.readFile(path.join(dir, "kitten.log"), "utf8");
      assert.match(logged, /--to=unix:\/tmp\/fake-kitty\.sock close-window/, "the close call names the socket");
      // The match flag is the one the parent builds, so a change to kitty's
      // syntax cannot drift between the two.
      const flag = closeWindowArgv("42", "unix:/tmp/fake-kitty.sock", kitten).find((arg) => arg.startsWith("--match="))!;
      assert.ok(logged.includes(flag), `the close call uses ${flag}`);
    } finally {
      script.kill("SIGKILL");
      parent.kill("SIGKILL");
    }
  });
});

test("a child that ignores SIGTERM is killed after the grace", { timeout: 20000 }, async () => {
  await withTempDir(async (dir) => {
    const kitten = await fakeKitten(dir);
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `trap '' TERM; printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten,
      exitPath: path.join(dir, "exit.code"),
    });

    const parent = standInParent();
    const script = spawn("/bin/sh", [scriptPath, String(parent.pid)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile);
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid));
    } finally {
      script.kill("SIGKILL");
      parent.kill("SIGKILL");
    }
  });
});

test("a run that ends on its own leaves its held window alone when the parent dies later", { timeout: 20000 }, async () => {
  await withTempDir(async (dir) => {
    const kitten = await fakeKitten(dir);
    const exitPath = path.join(dir, "exit.code");
    const scriptPath = await watchedScript(dir, {
      child: "sleep 0.1; exit 7",
      kitten,
      exitPath,
    });

    const parent = standInParent();
    const script = spawn("/bin/sh", [scriptPath, String(parent.pid)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, KITTY_WINDOW_ID: "42" },
    });
    try {
      const ended = new Promise<number | null>((resolve) => script.once("exit", (code) => resolve(code)));
      assert.equal(await ended, 0, "the script itself exits 0");
      assert.equal((await fs.promises.readFile(exitPath, "utf8")).trim(), "7", "the child's code is kept");

      // A stale watcher would close a window that `hold` keeps open.
      await killParent(parent);
      await new Promise((resolve) => setTimeout(resolve, 500));
      assert.equal(await readOr(path.join(dir, "kitten.log"), ""), "", "no watcher ran after the run ended");
    } finally {
      script.kill("SIGKILL");
      parent.kill("SIGKILL");
    }
  });
});

test("a script that does not lead its group never signals the group", { timeout: 20000 }, async () => {
  await withTempDir(async (dir) => {
    const kitten = await fakeKitten(dir);
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten,
      exitPath: path.join(dir, "exit.code"),
    });

    // Not detached: the script shares this test's process group, so a group
    // signal would reach the test runner. The bystander makes that visible.
    const parent = standInParent();
    const bystander = standInParent();
    const script = spawn("/bin/sh", [scriptPath, String(parent.pid)], {
      stdio: "ignore",
      env: { ...process.env, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile);
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid));
      assert.equal(alive(bystander.pid!), true, "a process in the caller's group is untouched");
    } finally {
      script.kill("SIGKILL");
      bystander.kill("SIGKILL");
    }
  });
});

test("an unattended wrapper ends its child when the parent dies", { timeout: 20000 }, async () => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const script = buildPrintScript({
      command: ["/bin/sh", "-c", `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`],
      ...WATCH,
    });

    const parent = standInParent();
    // Detached, as print.ts spawns it: the wrapper leads its own process group.
    const wrapper = spawn("/bin/sh", ["-c", script, "loxpp-subagent", String(parent.pid)], {
      detached: true,
      stdio: "ignore",
    });
    try {
      const childPid = await readPid(childPidFile);
      await new Promise((resolve) => setTimeout(resolve, 300));
      assert.equal(alive(childPid), true, "the child lives while the parent lives");

      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid));
    } finally {
      wrapper.kill("SIGKILL");
      parent.kill("SIGKILL");
    }
  });
});
