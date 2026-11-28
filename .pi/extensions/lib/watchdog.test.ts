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
import { startTimeOf } from "./parent-identity.ts";
import { PS_PROBE, shellQuote, WATCHDOG_MARKER } from "./watchdog.ts";

/** Fast enough for a test, and still several real poll cycles. */
const WATCH = { watchPollS: 0.05, watchGraceS: 0.3 };

/**
 * The longest one wait may take once the run is up. Such a wait measures the
 * watch, which polls every 50 ms, so a regression in the watch shows well
 * inside this limit; the rest is slack for a loaded host.
 */
const TIMEOUT_MS = 8000;
/**
 * The longest the wait for a run to show its child may take. That wait measures
 * the host, not the watch: a shell start and one short write, which a loaded
 * runner delays far past any interval that a regression in the watch needs.
 * `readPid` says which events end the wait before this limit does.
 */
const CHILD_START_TIMEOUT_MS = 30_000;
/**
 * How late a child start can be while its run is alive, in the test that proves
 * the start allowance. One second more than `TIMEOUT_MS`: a wait that kept the
 * watch's limit instead of the allowance fails at that limit.
 */
const SLOW_START_S = 9;
/**
 * How late a `ps` start can be, in the test that proves the read allowance. It
 * only has to outlast the limit `startTimeOf` gives its `ps` call, which is a
 * bound on a launch, not on a host: a read that kept that limit returns no
 * answer while this one gives the token.
 */
const SLOW_PS_S = 3;
/**
 * A limit short enough to probe the message of a `ps` call that gives no
 * answer, in a test whose fake `ps` exits at once.
 */
const PS_READ_PROBE_MS = 500;
/**
 * A limit short enough to probe the end of a wait, in the test that forces that
 * end: the run it passes starts no child, so the file can never arrive.
 */
const LIMIT_PROBE_MS = 200;
/** Every wait and every `ps` read in one test draws on this budget, so their sum cannot exceed it. */
const WAIT_BUDGET_MS = 90_000;

/**
 * A test that drives real processes. Its waits share one deadline, so no sum of
 * waits can reach the outer bound first: each wait asks for what is left and
 * fails with the cause it names. The body picks the limit its wait measures:
 * `startLeft` for host work that the host decides — a spawned run to come up,
 * or the `ps` start behind a start time — and `left` for a wait on the watch,
 * which is capped at `TIMEOUT_MS` so a single slow step cannot take the whole
 * budget and hide a regression. The outer bound holds the whole budget, which
 * holds the `ps` reads, the two start waits and the three watch waits that the
 * longest test uses, plus the fixed sleeps, the setup and the teardown.
 */
function processTest(
  name: string,
  body: (left: () => number, startLeft: () => number) => Promise<void>,
  skip: false | string = false,
): void {
  test(name, { skip, timeout: WAIT_BUDGET_MS + 15_000 }, async () => {
    const until = Date.now() + WAIT_BUDGET_MS;
    await body(
      () => Math.min(TIMEOUT_MS, Math.max(1, until - Date.now())),
      () => Math.min(CHILD_START_TIMEOUT_MS, Math.max(1, until - Date.now())),
    );
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
 * The two arguments a run script takes: the parent pid, and the parent's start
 * time, which production reads with `parentStartTime` while the parent is
 * certainly alive.
 */
function parentArgs(parent: ChildProcess): string[] {
  const pid = parent.pid;
  assert.ok(pid !== undefined, "the stand-in parent has a pid");
  return [String(pid), startTimeOf(pid)];
}

/**
 * The raw `ps` answer for one pid under one time zone, normalized as both sides
 * of the watch normalize it, read within `timeoutMs`.
 *
 * A read that gives no answer fails with the call it names, because a starved
 * host loses this read first and a message about the run the test was setting up
 * does not say which read gave up.
 */
function rawStart(pid: number, tz: string, timeoutMs: number): string {
  const r = spawnSync("ps", ["-o", "lstart=", "-p", String(pid)], {
    encoding: "utf8",
    env: { ...process.env, LC_ALL: "C", TZ: tz },
    timeout: timeoutMs,
  });
  const answer = (r.stdout ?? "").replace(/\s+/g, "");
  assert.ok(
    r.error === undefined && r.status === 0 && answer !== "",
    `ps gave no answer for the start time of pid ${pid} in ${tz} within ${timeoutMs} ms (${psFailure(r)})`,
  );
  return answer;
}

/** What a `ps` call that gave no answer reported, for the message that names it. */
function psFailure(r: { error?: Error; signal: NodeJS.Signals | null; status: number | null }): string {
  const cause = r.error === undefined ? "" : `: ${r.error.message}`;
  return r.signal !== null ? `signal ${r.signal}${cause}` : `exit ${r.status}${cause}`;
}

/**
 * A time zone that prints a different start time than the `TZ=UTC` both sides
 * use, or undefined when this host's zone database offers none. A run started in
 * such a zone is the test of the token's zone: a token that carried the caller's
 * zone would read a live parent as dead.
 *
 * Each read draws on `psLeft`, the test's allowance for host work, so the reads
 * of the search share the budget with the waits that follow. A read that gives
 * no answer names itself instead of reading as a zone that differs.
 */
function differingZone(pid: number, psLeft: () => number): string | undefined {
  const utc = rawStart(pid, "UTC", psLeft());
  for (const zone of ["Asia/Tokyo", "America/Los_Angeles", "Pacific/Kiritimati"]) {
    if (rawStart(pid, zone, psLeft()) !== utc) return zone;
  }
  return undefined;
}

/**
 * The parent's start time as production reads it, within the test's allowance
 * for host work. The `ps` start is a fork and an exec, which a loaded runner
 * delays past the limit that bounds production's read, so this read gets the
 * allowance and its failure names the call.
 */
function parentStart(pid: number, timeoutMs: number): string {
  const answer = startTimeOf(pid, timeoutMs);
  assert.notEqual(answer, "", `ps gave no answer for the start time of pid ${pid} within ${timeoutMs} ms`);
  return answer;
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

/**
 * The pid a run's child writes into `file`.
 *
 * Two events end this wait. The pid landing is the progress it exists for. The
 * end of `run` is a proven failure: the caller passes the process whose end
 * means that no writer of the file is left. That holds for a file the run
 * writes itself and for a file of the run's own child, because that child ends
 * with its run. The wait then fails at once and names the run's exit instead of
 * sitting out the limit.
 *
 * A file that a descendant writes needs no such process, and `undefined` says
 * so: a grandchild outlives the shell that starts it, so its file can arrive
 * after the run ends, and only the limit then bounds the wait.
 *
 * Until one of the two events happens the run is alive and working, so a host
 * that is slow to start its shell keeps the wait. The limit is therefore the
 * test's start allowance, not the one a wait on the watch uses.
 *
 * The listener goes on before the first check, because an `exit` that lands
 * between a check and the attach is never replayed.
 */
async function readPid(file: string, run: ChildProcess | undefined, timeoutMs: number): Promise<number> {
  // The argument is required, but `node --test` strips types without checking
  // them, so a call site that omits it must fail here and not hang.
  assert.ok(Number.isFinite(timeoutMs) && timeoutMs > 0, `a wait for ${file} needs a finite limit, and got ${timeoutMs}`);
  let ended = false;
  if (run !== undefined) {
    run.once("exit", () => {
      ended = true;
    });
    if (run.exitCode !== null || run.signalCode !== null) ended = true;
  }
  const until = Date.now() + timeoutMs;
  for (;;) {
    const pid = Number.parseInt((await readOr(file, "")).trim(), 10);
    if (Number.isInteger(pid) && pid > 1) return pid;
    if (ended && run !== undefined) assert.fail(`the run ended (${run.exitCode ?? run.signalCode}) before its child wrote ${file}`);
    if (Date.now() >= until) assert.fail(`timed out waiting for ${file}`);
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
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
  //   lib/parent-identity.ts  one `ps -o lstart=` call, which reads pi's own
  //                        start time and returns at once
  //   modes.ts             the gate wrapper in lib/gate-run.ts
  //   subagent/print.ts    the unattended wrapper it builds
  //   subagent/window.ts   the run script kitty runs (kitten calls are short)
  assert.deepEqual(starters, ["lib/parent-identity.ts", "modes.ts", "subagent/print.ts", "subagent/window.ts"], "a module that starts a process needs a watchdog decision");
});

test("every script builder carries the watchdog", () => {
  const kitten = "/opt/kitty/bin/kitten";
  const watched = buildRunScript({
    header: ["h"],
    cwd: "/",
    command: ["/bin/sh", "-c", "true"],
    stderrPath: "/tmp/err.log",
    exitPath: "/tmp/exit.code",
    kitten,
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
    assert.ok(script.includes(PS_PROBE), `${name} asks ps whether it can answer at all`);
    // The state check alone cannot tell the parent from a process that took its
    // pid, so every builder must carry the identity comparison too.
    for (const needle of ["parent_start=${2:-}", "process_start() {", 'parent_gone() {', 'pid_gone "$parent_pid" && return 0']) {
      assert.ok(script.includes(needle), `${name} carries ${needle}, so a reused pid reads as a dead parent`);
    }
    for (const needle of ["command tr", "command head", "command sleep", "command kill", "command printf", "command [", "command wait", "unset -f command"]) {
      assert.ok(script.includes(needle), `${name} runs \`${needle}\` through command, so no function shadows it`);
    }
  }
  // Every site, not only the ones with a name: no line the builders write may
  // use a guarded command without `command` before it. This is the check the
  // named needles cannot give, because one prefix removed from a helper leaves
  // another site's prefix in the text. Its limits: a word inside a quoted
  // payload, an `eval` string, and a command named by a variable are not seen,
  // and a payload word after a space would be flagged; no builder does any of
  // those today.
  const guarded = ["ps", "tr", "head", "sleep", "kill", "printf", "wait", "cd", "["];
  for (const [label, script] of [
    ["the watched run script", watched],
    ["the unattended wrapper", unattended],
    ["the gate wrapper", gate],
  ]) {
    for (const line of script.split("\n")) {
      if (/^\s*#/.test(line)) continue;
      for (const word of guarded) {
        const token = word === "[" ? "\\[" : `\\b${word}\\b`;
        const pattern = new RegExp(`(^|[\\s;&|({])${token}`, "g");
        for (let match = pattern.exec(line); match !== null; match = pattern.exec(line)) {
          const before = line.slice(0, match.index + match[1].length);
          assert.ok(/(^|[\s$(])command\s+$/.test(before), `${label} uses \`${word}\` without command: ${line.trim()}`);
        }
      }
    }
    // The gate's removal is the wrapper's own command, and its binary is
    // quoted, so the word scan cannot see the prefix. Every removal must go
    // through `command`.
    for (const line of script.split("\n")) {
      if (!line.includes("rm -f --time 0")) continue;
      assert.ok(/command\s+\S+\s+rm -f --time 0/.test(line), `${label} removes a container without command: ${line.trim()}`);
    }
  }
  // The close call and the entry directory belong to the watched run only, and
  // the close call's path is quoted, so it is checked by the path.
  assert.ok(watched.includes(`command '${kitten}'`), "the close call runs kitten through command");
  assert.ok(watched.includes("command cd"), "the watched run enters the caller's directory through command");
});

processTest("a watched run ends its child and closes its window when the parent dies", async (left, startLeft) => {
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
    const script = spawn("/bin/sh", [scriptPath, ...parentArgs(parent)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
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

processTest("a run that ends before its child writes the pid fails with that cause", async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten: await fakeKitten(dir),
      exitPath: path.join(dir, "exit.code"),
    });

    // No parent pid: the script refuses to run and starts no child, so the pid
    // file can never appear. The wait must name the run's end rather than sit
    // out the load allowance: a run that is gone cannot write the file, and the
    // allowance is for a run that is alive and slow to start its shell.
    const script = spawn("/bin/sh", [scriptPath], { detached: true, stdio: "ignore" });
    try {
      await assert.rejects(readPid(childPidFile, script, startLeft()), /the run ended \(1\) before its child wrote/);
    } finally {
      await stopRun(script, left());
    }
  });
});

processTest("the wait for a run's child ends at its limit, and says so", async (left) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    // A run that starts no child keeps answering, so the file can never appear
    // and no process end can prove it gone: only the limit can end this wait.
    const run = spawn("/bin/sh", ["-c", "sleep 60"], { detached: true, stdio: "ignore" });
    try {
      await assert.rejects(readPid(childPidFile, run, LIMIT_PROBE_MS), /timed out waiting for .*child\.pid/);
    } finally {
      await stopRun(run, left());
    }
  });
});

processTest("a run that is slow to start its child keeps its wait", async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten: await fakeKitten(dir),
      exitPath: path.join(dir, "exit.code"),
    });
    // The run #1305 lost was alive and its child came late, which is what a
    // loaded host does to a shell start. The delay stands in for that load and
    // outlasts the limit a wait on the watch uses, so a wait that kept the
    // watch's limit instead of the start allowance fails here.
    const slowPath = path.join(dir, "slow.sh");
    await fs.promises.writeFile(
      slowPath,
      ["#!/bin/sh", `sleep ${SLOW_START_S}`, `exec /bin/sh ${shellQuote(scriptPath)} "$@"`, ""].join("\n"),
      { mode: 0o700 },
    );

    const parent = standInParent();
    const script = spawn("/bin/sh", [slowPath, ...parentArgs(parent)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, KITTY_WINDOW_ID: "42" },
    });
    try {
      const began = Date.now();
      const childPid = await readPid(childPidFile, script, startLeft());
      assert.ok(Date.now() - began > TIMEOUT_MS, "the child came after the limit a wait on the watch uses");
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
});

processTest("a parent pid that another process holds does not keep the run alive", async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const kitten = await fakeKitten(dir);
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten,
      exitPath: path.join(dir, "exit.code"),
    });

    const parent = standInParent();
    const start = parentStart(parent.pid!, startLeft());

    // The kernel hands a dead process's number to a new one, and the new owner's
    // `S` state is what a state-only watch reads as a live parent: the wrapper
    // then polls for as long as the stranger lives. A `ps` that answers with
    // another process's start time for the parent pid is what the reuse looks
    // like to the watch, and the stand-in parent keeps answering `kill -0`
    // throughout, so nothing but the identity check can end this run.
    const other = "Mon Jan  1 00:00:00 2001";
    const bin = path.join(dir, "bin");
    await fs.promises.mkdir(bin);
    await fs.promises.writeFile(
      path.join(bin, "ps"),
      [
        "#!/bin/sh",
        'case "$*" in',
        `  *"-o lstart= -p ${parent.pid}"*) printf '%s\\n' ${shellQuote(other)}; exit 0 ;;`,
        "esac",
        'exec /bin/ps "$@"',
        "",
      ].join("\n"),
      { mode: 0o700 },
    );

    const script = spawn("/bin/sh", [scriptPath, String(parent.pid), start], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, PATH: `${bin}:${process.env.PATH}`, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
      await waitFor("the child to end", () => !alive(childPid), left());
      assert.equal(alive(parent.pid!), true, "the run ended while the pid it watched still answered");
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
});

processTest("a parent token read in another time zone still names the same process", async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten: await fakeKitten(dir),
      exitPath: path.join(dir, "exit.code"),
    });

    const parent = standInParent();
    const start = parentStart(parent.pid!, startLeft());
    // kitty forwards a short list of environment keys, so a `TZ` that reaches
    // pi need not reach the window's script. `ps -o lstart=` answers in local
    // time, so both sides of the comparison set TZ=UTC; without that, a zone
    // difference reads a live parent as dead. A host whose zone database cannot
    // print a different answer cannot show the difference, and this test would
    // prove nothing.
    const zone = differingZone(parent.pid!, startLeft);
    if (zone === undefined) return;

    const script = spawn("/bin/sh", [scriptPath, String(parent.pid), start], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, TZ: zone, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
      await new Promise((resolve) => setTimeout(resolve, 300));
      assert.equal(alive(childPid), true, `the run outlives its parent in ${zone}`);
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
});

processTest("a ps that is slow to start still gives the parent's start time", async (left, startLeft) => {
  await withTempDir(async (dir) => {
    // A loaded host delays the fork and exec of `ps` itself. `startTimeOf`'s
    // default limit bounds what that delay may cost a launch, so a read that
    // kept it returns no answer here; the test's read draws on the start
    // allowance instead, which fits the host it measures.
    const bin = path.join(dir, "bin");
    await fs.promises.mkdir(bin);
    await fs.promises.writeFile(
      path.join(bin, "ps"),
      ["#!/bin/sh", `command sleep ${SLOW_PS_S}`, 'exec /bin/ps "$@"', ""].join("\n"),
      { mode: 0o700 },
    );

    const before = process.env.PATH;
    let slow: string;
    process.env.PATH = `${bin}:${before}`;
    try {
      slow = parentStart(process.pid, startLeft());
    } finally {
      process.env.PATH = before;
    }
    // The delay changes when the answer arrives, not what it says: the token is
    // the one a `ps` that answered at once gives.
    const direct = rawStart(process.pid, "UTC", startLeft());
    assert.equal(slow, direct, "the late answer names the same start time");
  });
});

processTest("a ps that gives no answer names the read that gave it", async () => {
  await withTempDir(async (dir) => {
    // A `ps` that is not installed, or one that a policy stops, answers
    // nothing. Each read the test makes must then say which one it was: the
    // parent's start time, or one zone of the search.
    const bin = path.join(dir, "bin");
    await fs.promises.mkdir(bin);
    await fs.promises.writeFile(path.join(bin, "ps"), ["#!/bin/sh", "exit 3", ""].join("\n"), { mode: 0o700 });

    const before = process.env.PATH;
    process.env.PATH = `${bin}:${before}`;
    try {
      assert.throws(
        () => parentStart(process.pid, PS_READ_PROBE_MS),
        new RegExp(`ps gave no answer for the start time of pid ${process.pid} within ${PS_READ_PROBE_MS} ms`),
        "the read of the parent's start time names itself",
      );
      assert.throws(
        () => differingZone(process.pid, () => PS_READ_PROBE_MS),
        new RegExp(`ps gave no answer for the start time of pid ${process.pid} in UTC within ${PS_READ_PROBE_MS} ms`),
        "the search names the read that failed, and its zone",
      );
    } finally {
      process.env.PATH = before;
    }
  });
});

processTest("a run started without a parent start time still ends when the parent dies", async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten: await fakeKitten(dir),
      exitPath: path.join(dir, "exit.code"),
    });

    const parent = standInParent();
    // A caller whose `ps` could not read the parent passes the pid alone. The
    // state check is then the only evidence, which is what the watch had before
    // the identity arrived: a dead parent still ends the run.
    const script = spawn("/bin/sh", [scriptPath, String(parent.pid)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
});

processTest("a child that ignores SIGTERM is killed after the grace", async (left, startLeft) => {
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
    const script = spawn("/bin/sh", [scriptPath, ...parentArgs(parent)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
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

processTest("a ps that cannot read pid 1 still lets the run end", async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const kitten = await fakeKitten(dir);
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten,
      exitPath: path.join(dir, "exit.code"),
    });

    // A `hidepid` mount: `ps` lists processes and reads this user's own pids,
    // but pid 1 belongs to another user, and cannot be read. A probe that asks
    // about pid 1 then turns the watch off for every run.
    const bin = path.join(dir, "bin");
    await fs.promises.mkdir(bin);
    await fs.promises.writeFile(
      path.join(bin, "ps"),
      ["#!/bin/sh", 'case "$*" in', '  *"-p 1") exit 0 ;;', "esac", 'exec /bin/ps "$@"', ""].join("\n"),
      { mode: 0o700 },
    );

    const parent = standInParent();
    const script = spawn("/bin/sh", [scriptPath, ...parentArgs(parent)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, PATH: `${bin}:${process.env.PATH}`, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
});

processTest("a watcher ends when its script is signalled alone", async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const kitten = await fakeKitten(dir);
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 0.3`,
      kitten,
      exitPath: path.join(dir, "exit.code"),
    });

    const parent = standInParent();
    const script = spawn("/bin/sh", [scriptPath, ...parentArgs(parent)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, KITTY_WINDOW_ID: "42" },
    });
    try {
      await readPid(childPidFile, script, startLeft());
      // The script dies alone here, as it did when a fake kitten closed a
      // window by its pid. Its watcher must still end when its child does:
      // `pid_gone` asks `ps` to list any pid, not about the script, because a
      // script that is gone cannot answer for itself.
      script.kill("SIGTERM");
      await waitFor("the run's process group to end", () => {
        try {
          process.kill(-script.pid!, 0);
          return false;
        } catch {
          return true;
        }
      }, left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
});

/**
 * Whether the shell a run starts in imports an exported function at all. Where
 * `/bin/sh` is bash it does, and in POSIX mode it refuses one for a special
 * builtin; where it is dash there is no way to export a function, so no shadow
 * can arrive and the tests that build one skip.
 */
const SHADOWS_ARRIVE =
  spawnSync("/bin/sh", ["-c", 'probe_fn() { :; }; export -f probe_fn; /bin/sh -c "type -t probe_fn"'], {
    encoding: "utf8",
  }).stdout.trim() === "function";
const NO_SHADOWS = SHADOWS_ARRIVE ? false : "this shell imports no exported function, so no shadow can arrive";

/**
 * Start a run whose shell has the given functions exported first, the way a
 * parent shell can hand them to pi. The run keeps the shell it has in
 * production, `/bin/sh`.
 */
function shadowedRun(scriptPath: string, args: string[], shadow: string[]): ChildProcess {
  // The script starts as `/bin/sh` in production too, so the test uses that
  // shell: where it is bash, bash runs in POSIX mode, imports a function for a
  // regular builtin and refuses one for a special builtin, exactly as it does
  // for a real run. Where `/bin/sh` is dash, `export -f` kills this script
  // before the run starts, so `SHADOWS_ARRIVE` is false and the tests skip.
  const script = [...shadow, 'exec /bin/sh "$0" "$@"'].join("\n");
  return spawn("/bin/sh", ["-c", script, scriptPath, ...args], {
    detached: true,
    stdio: "ignore",
    env: { ...process.env, KITTY_WINDOW_ID: "42" },
  });
}

/** One hostile function, exported. */
function oneShadow(name: string, body: string): string[] {
  return [`${name}() { ${body}; }`, `export -f ${name}`];
}

processTest(
  "a shadowed tr does not make every pid read as gone",
  async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten: await fakeKitten(dir),
      exitPath: path.join(dir, "exit.code"),
    });
    const parent = standInParent();
    // A `tr` that prints nothing leaves `process_state` empty for every pid, so
    // `pid_gone` answers "gone" for all of them and the watcher exits at its
    // first poll: the run then outlives the parent.
    const script = shadowedRun(scriptPath, parentArgs(parent), oneShadow("tr", ":"));
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
  },
  NO_SHADOWS,
);

processTest(
  "a shadowed head does not turn the watch off",
  async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten: await fakeKitten(dir),
      exitPath: path.join(dir, "exit.code"),
    });
    const parent = standInParent();
    // A `head` that prints nothing makes `ps` read as unable to answer, which
    // the fallback takes as "leave the run alone".
    const script = shadowedRun(scriptPath, parentArgs(parent), oneShadow("head", ":"));
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
  },
  NO_SHADOWS,
);

processTest(
  "a shadowed sleep does not stop the poll",
  async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten: await fakeKitten(dir),
      exitPath: path.join(dir, "exit.code"),
    });
    const parent = standInParent();
    // A `sleep` that blocks forever stops the poll, so the watcher never sees
    // the dead parent.
    const script = shadowedRun(scriptPath, parentArgs(parent), oneShadow("sleep", "command sleep 60"));
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
  },
  NO_SHADOWS,
);

processTest(
  "a shadowed printf does not lose the run's exit code",
  async (left) => {
  await withTempDir(async (dir) => {
    const exitPath = path.join(dir, "exit.code");
    const scriptPath = await watchedScript(dir, {
      child: "exit 7",
      kitten: await fakeKitten(dir),
      exitPath,
    });
    const parent = standInParent();
    // A `printf` that prints nothing would leave the exit file empty, and the
    // parent reads that file as the run's result.
    const script = shadowedRun(scriptPath, parentArgs(parent), oneShadow("printf", ":"));
    try {
      await waitFor("the script to end", () => script.exitCode !== null || script.signalCode !== null, left());
      assert.equal((await fs.promises.readFile(exitPath, "utf8")).trim(), "7", "the child's code reached the file");
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
  },
  NO_SHADOWS,
);

processTest(
  "a shadowed command does not turn the watch off",
  async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten: await fakeKitten(dir),
      exitPath: path.join(dir, "exit.code"),
    });
    const parent = standInParent();
    // A `command` function shadows the builtin every other guard depends on.
    // The script clears it with `unset -f command`, and that line cannot be
    // shadowed: in POSIX mode bash does not import a function for a special
    // builtin, and `unset` is one.
    const script = shadowedRun(scriptPath, parentArgs(parent), oneShadow("command", ":"));
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
  },
  NO_SHADOWS,
);

processTest(
  "a shadowed [, wait or cd does not change the run",
  async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const cwdFile = path.join(dir, "cwd");
    const scriptPath = await watchedScript(dir, {
      child: `pwd > ${shellQuote(cwdFile)}; printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten: await fakeKitten(dir),
      exitPath: path.join(dir, "exit.code"),
    });
    const parent = standInParent();
    // `[` decides every test in the watcher, `wait` holds the script at the
    // child, and `cd` puts the child where the caller asked. bash imports a
    // function for each of the three.
    const script = shadowedRun(scriptPath, parentArgs(parent), [
      "[() { return 1; }",
      "wait() { return 0; }",
      "cd() { return 0; }",
      "export -f '[' wait cd",
    ]);
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
      assert.equal((await fs.promises.readFile(cwdFile, "utf8")).trim(), dir, "the child runs where the caller asked");
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(script, left());
    }
  });
  },
  NO_SHADOWS,
);

processTest(
  "a shadowed [ does not lose the parent pid check",
  async (left) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const scriptPath = await watchedScript(dir, {
      child: `printf '%s' $$ > ${shellQuote(childPidFile)}; sleep 60`,
      kitten: await fakeKitten(dir),
      exitPath: path.join(dir, "exit.code"),
    });
    // No parent pid, and a `[` that answers 1: the script must still refuse to
    // run, and must start no child. Without the guard it reads the empty pid as
    // a valid one, starts the child, and then loses it.
    const script = shadowedRun(scriptPath, [], oneShadow("[", "return 1"));
    try {
      await waitFor("the script to end", () => script.exitCode !== null || script.signalCode !== null, left());
      assert.equal(script.exitCode, 1, "the script refuses to run without the parent pid");
      assert.equal(fs.existsSync(childPidFile), false, "no child is started");
    } finally {
      await stopRun(script, left());
    }
  });
  },
  NO_SHADOWS,
);

processTest(
  "a shadowed [ does not lose the group signal",
  async (left, startLeft) => {
  await withTempDir(async (dir) => {
    const childPidFile = path.join(dir, "child.pid");
    const grandchildPidFile = path.join(dir, "grandchild.pid");
    const wrapperPath = path.join(dir, "wrapper.sh");
    await fs.promises.writeFile(
      wrapperPath,
      buildPrintScript({
        command: [
          "/bin/sh",
          "-c",
          `printf '%s' $$ > ${shellQuote(childPidFile)}; sh -c 'printf "%s" $$ > ${shellQuote(grandchildPidFile)}; sleep 60' & sleep 60`,
        ],
        ...WATCH,
      }),
      { mode: 0o700 },
    );
    const parent = standInParent();
    // The wrapper leads its own group, so `leads_group` decides whether the
    // signal reaches the grandchild. A `[` that answers 1 makes it read "not
    // leading" and signal the child alone.
    const wrapper = shadowedRun(wrapperPath, parentArgs(parent), oneShadow("[", "return 1"));
    try {
      const childPid = await readPid(childPidFile, wrapper, startLeft());
      // A grandchild outlives the shell that starts it, so the wrapper's end
      // proves nothing about this file.
      const grandchildPid = await readPid(grandchildPidFile, undefined, startLeft());
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
      await waitFor("the grandchild to end", () => !alive(grandchildPid), left());
    } finally {
      parent.kill("SIGKILL");
      await stopRun(wrapper, left());
    }
  });
  },
  NO_SHADOWS,
);

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
    const script = spawn("/bin/sh", [scriptPath, ...parentArgs(parent)], {
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

processTest("the unattended wrapper never signals a group it does not lead", async (left, startLeft) => {
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
    const wrapper = spawn("/bin/sh", ["-c", script, "loxpp-subagent", ...parentArgs(parent)], { stdio: "ignore" });
    try {
      const childPid = await readPid(childPidFile, wrapper, startLeft());
      await killParent(parent);
      await waitFor("the child to end", () => !alive(childPid), left());
      assert.equal(alive(bystander.pid!), true, "a process in the caller's group is untouched");
      // This wrapper shares the test's group, so it cannot be signalled as a
      // group: wait for it to end by itself, as its own `wait` does.
      await ended(wrapper, left());
    } finally {
      // The stand-in parent holds this process open for 60 s, so it dies even
      // if an earlier `await` threw.
      parent.kill("SIGKILL");
      wrapper.kill("SIGKILL");
      bystander.kill("SIGKILL");
    }
  });
});

processTest("a ps that cannot answer does not make a live parent look dead", async (left, startLeft) => {
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
    const script = spawn("/bin/sh", [scriptPath, ...parentArgs(parent)], {
      detached: true,
      stdio: "ignore",
      env: { ...process.env, PATH: `${bin}:${process.env.PATH}`, KITTY_WINDOW_ID: "42" },
    });
    try {
      const childPid = await readPid(childPidFile, script, startLeft());
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

processTest("a zombie parent counts as dead, so its run ends", async (left, startLeft) => {
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
      const zombie = await readPid(zombiePidFile, holder, startLeft());
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
        const childPid = await readPid(childPidFile, script, startLeft());
        await waitFor("the child to end", () => !alive(childPid), left());
      } finally {
        await stopRun(script, left());
      }
    } finally {
      holder.kill("SIGKILL");
    }
  });
});

processTest("an unattended wrapper ends its child and its grandchild when the parent dies", async (left, startLeft) => {
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
    const wrapper = spawn("/bin/sh", ["-c", script, "loxpp-subagent", ...parentArgs(parent)], {
      detached: true,
      stdio: "ignore",
    });
    try {
      const childPid = await readPid(childPidFile, wrapper, startLeft());
      // A grandchild outlives the shell that starts it, so the wrapper's end
      // proves nothing about this file.
      const grandchildPid = await readPid(grandchildPidFile, undefined, startLeft());
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
