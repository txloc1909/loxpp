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
 *
 * A run keeps writing into the temp dir until its last member is gone: the
 * script writes the exit code and the watcher writes its close call, both at
 * the moment the child ends. Every test that spawns a detached run ends that
 * run's whole process group before it returns, so no writer survives the temp
 * dir's removal.
 */

import assert from "node:assert/strict";
import { spawn, spawnSync, type ChildProcess } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { buildRunScript, closeWindowArgv } from "../subagent/kitty.ts";
import { buildPrintScript } from "../subagent/print.ts";
import { buildGateScript, gateRunArgv } from "./gate-run.ts";
import { shellQuote, WATCHDOG_MARKER } from "./watchdog.ts";

/** Fast enough for a test, and still several real poll cycles. */
const WATCH = { watchPollS: 0.05, watchGraceS: 0.3 };

/** The longest one wait may take. */
const TIMEOUT_MS = 8000;
/** Every wait in one test draws on this budget, so their sum cannot exceed it. */
const WAIT_BUDGET_MS = 45_000;

/**
 * A test that drives real processes. Its waits share one deadline, so no sum of
 * waits can reach the outer bound first: each wait asks for what is left and
 * fails with the cause it names. One wait is capped at `TIMEOUT_MS`, so a
 * single slow step cannot take the whole budget and hide a regression. The
 * outer bound holds the whole budget, which holds five capped waits and the
 * fixed sleeps, plus the setup and the teardown.
 */
function processTest(name: string, body: (left: () => number) => Promise<void>): void {
  test(name, { timeout: WAIT_BUDGET_MS + 15_000 }, async () => {
    const until = Date.now() + WAIT_BUDGET_MS;
    await body(() => Math.min(TIMEOUT_MS, Math.max(1, until - Date.now())));
  });
}

async function withTempDir(
  fn: (dir: string) => Promise<void>,
  // A parameter so a test can make the removal fail without depending on the
  // user id: `rm` cannot fail for root on a read-only directory.
  remove: (dir: string) => Promise<void> = removeTempDir,
): Promise<void> {
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-watchdog-"));
  let bodyFailed = false;
  try {
    await fn(dir);
  } catch (error) {
    bodyFailed = true;
    throw error;
  } finally {
    try {
      await remove(dir);
    } catch (error) {
      // The body's error says what the test proved wrong; a removal that raced
      // a writer must not replace it.
      if (!bodyFailed) throw error;
    }
  }
}

/**
 * `rm` unlinks the entries and then calls `rmdir`. A writer that is still live
 * leaves an entry behind between those two steps, and `ENOTEMPTY` names the
 * directory, not the writer. This error names the entries that were left, so
 * the next occurrence shows what wrote them.
 */
async function removeTempDir(dir: string): Promise<void> {
  try {
    await fs.promises.rm(dir, { recursive: true, force: true });
  } catch (error) {
    const left = fs.existsSync(dir) ? fs.readdirSync(dir).sort().join(", ") : "(the directory was already gone)";
    const detail = error instanceof Error ? error.message : String(error);
    throw new Error(`a process was still writing when the temp dir was removed: it held ${left}: ${detail}`);
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

/**
 * Wait for a spawned process to end by itself, so its last write has landed.
 * The listener goes on before the check: an `exit` that lands between a check
 * and the attach is never replayed, and a helper that removes a flake must not
 * add one.
 */
async function ended(child: ChildProcess, timeoutMs: number): Promise<void> {
  assert.ok(Number.isFinite(timeoutMs) && timeoutMs > 0, `a wait for the run script to end needs a finite limit, and got ${timeoutMs}`);
  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("timed out waiting for the run script to end")), timeoutMs);
    const done = (): void => {
      clearTimeout(timer);
      resolve();
    };
    child.once("exit", done);
    if (child.exitCode !== null || child.signalCode !== null) done();
  });
}

/**
 * End a run this test spawned detached, so the leader owns its group: the
 * watcher and the commands the child started are members of that group, and a
 * signal to the leader's pid alone leaves them behind. The wait returns only
 * when every member is gone, which is what makes the temp dir's removal safe.
 *
 * The leader is often already reaped when this runs, so the pid can be free.
 * A group signal to a freed pid is safe here because the kernel hands a pid out
 * again only after the whole pid space turns over (pid_max is 4 194 304 on this
 * host, and the allocator moved 7 640 pids in 5 s), while this call lives for
 * milliseconds. The signal is still needed then: a group can outlive its leader
 * and answer `kill -0`, and that is the case of a watcher that traps TERM.
 */
async function stopRun(run: ChildProcess, timeoutMs: number): Promise<void> {
  const group = run.pid === undefined ? undefined : -run.pid;
  if (group !== undefined) {
    try {
      process.kill(group, "SIGKILL");
    } catch {
      // The group is already gone.
    }
  }
  await waitFor("the run's process group to end", () => {
    if (group === undefined) return true;
    try {
      process.kill(group, 0);
      return false;
    } catch {
      return true;
    }
  }, timeoutMs);
}

/** The first letter of the process state: `Z` for an exited process. */
function state(pid: number): string {
  const out = spawnSync("ps", ["-o", "stat=", "-p", String(pid)], { encoding: "utf8" }).stdout;
  return out.trim().slice(0, 1);
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

async function waitFor(what: string, check: () => Promise<boolean> | boolean, timeoutMs: number): Promise<void> {
  // The argument is required, but `node --test` strips types without checking
  // them, so a call site that omits it must fail here and not hang.
  assert.ok(Number.isFinite(timeoutMs) && timeoutMs > 0, `a wait for ${what} needs a finite limit, and got ${timeoutMs}`);
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

async function readPid(file: string, timeoutMs: number): Promise<number> {
  let pid = 0;
  await waitFor(
    file,
    async () => {
      pid = Number.parseInt((await readOr(file, "")).trim(), 10);
      return Number.isInteger(pid) && pid > 1;
    },
    timeoutMs,
  );
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

test("a failed body keeps its own error when the removal fails", async () => {
  let removals = 0;
  const failingRemoval = async (dir: string): Promise<void> => {
    removals += 1;
    // Remove for real, so a passing test leaves nothing behind, and then fail
    // the way a removal that raced a writer does.
    await removeTempDir(dir);
    throw new Error("probe: the removal failed");
  };
  await assert.rejects(
    withTempDir(() => assert.fail("the body failed first"), failingRemoval),
    /the body failed first/,
    "the body's error is the one that names the defect",
  );
  assert.equal(removals, 1, "the cleanup ran, its failure was dropped, and nothing was left behind");
});

test("shellQuote survives a real shell", () => {
  const nasty = "it's $HOME `x` \"q\" \\ and\nnewline";
  const quoted = shellQuote(nasty);
  const result = spawnSync("/bin/sh", ["-c", `printf '%s' ${quoted}`], { encoding: "utf8" });
  assert.equal(result.status, 0);
  assert.equal(result.stdout, nasty);
});

test("every module in the extensions tree that starts a process is accounted for", () => {
  // Every module here runs inside pi, so every process it starts outlives a
  // SIGKILL of pi unless something watches pi. This list is the ground truth:
  // a new starter fails the test until it gets a decision.
  const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
  const modules: string[] = [];
  const walk = (dir: string): void => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) walk(full);
      else if (entry.name.endsWith(".ts") && !entry.name.endsWith(".test.ts")) modules.push(full);
    }
  };
  walk(root);
  const starters = modules
    .filter((file) => /\b(spawn|spawnSync|exec|execSync|execFile|execFileSync|fork)\(/.test(fs.readFileSync(file, "utf8")))
    .map((file) => path.relative(root, file).split(path.sep).join("/"))
    .sort();
  // Each entry names what watches pi for it:
  //   modes.ts             the gate wrapper in lib/gate-run.ts
  //   subagent/print.ts    the unattended wrapper it builds
  //   subagent/window.ts   the run script kitty runs (kitten calls are short)
  assert.deepEqual(starters, ["modes.ts", "subagent/print.ts", "subagent/window.ts"], "a module that starts a process needs a watchdog decision");
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
  const gate = buildGateScript({
    argv: gateRunArgv({ image: "loxpp-dev-env", cwd: "/repo", name: "loxpp-gate-1-abc", script: "true" }),
    name: "loxpp-gate-1-abc",
  });
  for (const [name, script] of [
    ["the watched run script", watched],
    ["the unattended wrapper", unattended],
    ["the gate wrapper", gate],
  ]) {
    assert.ok(script.includes(WATCHDOG_MARKER), `${name} watches the parent`);
  }
});

processTest("a watched run ends its child and closes its window when the parent dies", async (left) => {
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
      const childPid = await readPid(childPidFile, left());
      await new Promise((resolve) => setTimeout(resolve, 300));
      assert.equal(alive(childPid), true, "the child lives while the parent lives");

      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
      await waitFor("the window to close", async () => /close-window/.test(await readOr(path.join(dir, "kitten.log"), "")), left());
      const logged = await fs.promises.readFile(path.join(dir, "kitten.log"), "utf8");
      assert.match(logged, /--to=unix:\/tmp\/fake-kitty\.sock close-window/, "the close call names the socket");
      // The match flag is the one the parent builds, so a change to kitty's
      // syntax cannot drift between the two.
      const flag = closeWindowArgv("42", "unix:/tmp/fake-kitty.sock", kitten).find((arg) => arg.startsWith("--match="))!;
      assert.ok(logged.includes(flag), `the close call uses ${flag}`);
    } finally {
      // The stand-in parent holds this process open for 60 s, so it dies
      // first: a `stopRun` that throws must not skip it.
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
});

processTest("a child that ignores SIGTERM is killed after the grace", async (left) => {
  await withTempDir(async (dir) => {
    const kitten = await fakeKitten(dir);
    const childPidFile = path.join(dir, "child.pid");
    const exitPath = path.join(dir, "exit.code");
    const scriptPath = await watchedScript(dir, {
      child: `trap '' TERM; printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
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
      const childPid = await readPid(childPidFile, left());
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
      // The script records how the child ended only after `wait` returns, so
      // the code below is the test's proof of the signal that did the work: a
      // TERM the child ignores leaves it alive, and 137 is 128 plus SIGKILL.
      await ended(script, left());
      assert.equal((await fs.promises.readFile(exitPath, "utf8")).trim(), "137", "the KILL after the grace ended the child");
    } finally {
      // The stand-in parent holds this process open for 60 s, so it dies
      // first: a `stopRun` that throws must not skip it.
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
});

processTest("a run that ends on its own leaves its held window alone when the parent dies later", async (left) => {
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
      const finished = new Promise<number | null>((resolve) => script.once("exit", (code) => resolve(code)));
      assert.equal(await finished, 0, "the script itself exits 0");
      assert.equal((await fs.promises.readFile(exitPath, "utf8")).trim(), "7", "the child's code is kept");

      // A stale watcher would close a window that `hold` keeps open.
      await killParent(parent);
      await new Promise((resolve) => setTimeout(resolve, 500));
      assert.equal(await readOr(path.join(dir, "kitten.log"), ""), "", "no watcher ran after the run ended");
    } finally {
      // The stand-in parent holds this process open for 60 s, so it dies
      // first: a `stopRun` that throws must not skip it.
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
});

processTest("the unattended wrapper never signals a group it does not lead", async (left) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const script = buildPrintScript({
      command: ["/bin/sh", "-c", `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`],
      ...WATCH,
    });

    // Not detached: the wrapper shares this test's process group, so a group
    // signal would reach the test runner. The bystander makes that visible.
    const parent = standInParent();
    const bystander = standInParent();
    const wrapper = spawn("/bin/sh", ["-c", script, "loxpp-subagent", String(parent.pid)], { stdio: "ignore" });
    try {
      const childPid = await readPid(childPidFile, left());
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
      assert.equal(alive(bystander.pid!), true, "a process in the caller's group is untouched");
      // This wrapper shares the test's group, so it cannot be signalled as a
      // group: wait for it to end by itself, as its own `wait` does.
      await ended(wrapper, left());
    } finally {
      wrapper.kill("SIGKILL");
      bystander.kill("SIGKILL");
    }
  });
});

processTest("a ps that cannot answer does not make a live parent look dead", async (left) => {
  await withTempDir(async (dir) => {
    const kitten = await fakeKitten(dir);
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten,
      exitPath: path.join(dir, "exit.code"),
    });

    // A `ps` that fails for every pid. An empty answer means "gone" only when
    // `ps` can answer at all, or every live run looks orphaned.
    const bin = path.join(dir, "bin");
    await fs.promises.mkdir(bin);
    await fs.promises.writeFile(path.join(bin, "ps"), "#!/bin/sh\nexit 1\n", { mode: 0o700 });

    const parent = standInParent();
    const script = spawn("/bin/sh", [scriptPath, String(parent.pid)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, PATH: `${bin}:${process.env.PATH}`, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile, left());
      await new Promise((resolve) => setTimeout(resolve, 600));
      assert.equal(alive(childPid), true, "a parent that ps cannot read is not a dead parent");
      assert.equal(await readOr(path.join(dir, "kitten.log"), ""), "", "no close call without evidence of death");
    } finally {
      // The watcher cannot end by itself while `ps` fails: it reads every pid,
      // its own child included, as gone, and the safe direction keeps it
      // polling. The group is the only thing that ends it.
      // The stand-in parent holds this process open for 60 s, so it dies
      // first: a `stopRun` that throws must not skip it.
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
});

processTest("a zombie parent counts as dead, so its run ends", async (left) => {
  await withTempDir(async (dir) => {
    const kitten = await fakeKitten(dir);
    const childPidFile = path.join(dir, "child.pid");
    const zombiePidFile = path.join(dir, "zombie.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten,
      exitPath: path.join(dir, "exit.code"),
    });

    // A zombie exists only while its parent has not reaped it, and a shell
    // reaps a killed background child at once on some systems. A node process
    // that holds its event loop busy cannot reap at all, so the zombie lasts
    // and this precondition does not depend on the shell.
    const holder = spawn(
      process.execPath,
      [
        "-e",
        [
          "const { spawn } = require(\"node:child_process\");",
          `require("node:fs").writeFileSync(${JSON.stringify(zombiePidFile)}, String(spawn("sleep", ["300"], { stdio: "ignore" }).pid));`,
          "const until = Date.now() + 30000; while (Date.now() < until) {}",
        ].join(" "),
      ],
      { stdio: "ignore" },
    );
    try {
      const zombie = await readPid(zombiePidFile, left());
      process.kill(zombie, "SIGKILL");
      await waitFor("the state to become a zombie", () => /^Z/.test(state(zombie)), Math.min(5000, left()));
      assert.equal(alive(zombie), false, "the test helper counts a zombie as gone");
      assert.doesNotThrow(() => process.kill(zombie, 0), "the zombie still answers kill -0");

      // The watcher must read the state, not only `kill -0`, or it waits for a
      // parent that has already died.
      const script = spawn("/bin/sh", [scriptPath, String(zombie)], {
        detached: true,
        stdio: "ignore",
        env: { ...process.env, KITTY_WINDOW_ID: "42" },
      });
      try {
        const childPid = await readPid(childPidFile, left());
        await waitFor("the child to end", () => !alive(childPid), left());
      } finally {
        await stopRun(script, left());
      }
    } finally {
      holder.kill("SIGKILL");
    }
  });
});

processTest("an unattended wrapper ends its child and its grandchild when the parent dies", async (left) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const grandchildPidFile = path.join(dir, "grandchild.pid");
    const script = buildPrintScript({
      command: [
        "/bin/sh",
        "-c",
        `printf '%s' $$ > ${shellQuote(childPidFile)}; sh -c 'printf "%s" $$ > ${grandchildPidFile}; sleep 60' & sleep 60`,
      ],
      ...WATCH,
    });

    const parent = standInParent();
    // Detached, as print.ts spawns it: the wrapper leads its own process group.
    const wrapper = spawn("/bin/sh", ["-c", script, "loxpp-subagent", String(parent.pid)], {
      detached: true,
      stdio: "ignore",
    });
    try {
      const childPid = await readPid(childPidFile, left());
      const grandchildPid = await readPid(grandchildPidFile, left());
      await new Promise((resolve) => setTimeout(resolve, 300));
      assert.equal(alive(childPid), true, "the child lives while the parent lives");

      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
      await waitFor("the grandchild to end", () => !alive(grandchildPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(wrapper, left());
    }
  });
});
