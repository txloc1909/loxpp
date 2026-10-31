/**
 * Tests for the gate wrapper: one `podman run` client, watched by a script.
 *
 * The fake `podman` models the two facts measured on podman 6.1.4: the
 * container outlives a killed client, and only a removal ends it. Its `run`
 * keeps a marker file while the container lives, and its `rm` deletes it, so a
 * marker that survives is a container that nobody ended.
 */

import assert from "node:assert/strict";
import { spawn, spawnSync, type ChildProcess } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import { buildGateScript, gateContainerName, gateRunArgv } from "./gate-run.ts";
import { shellQuote, WATCHDOG_MARKER } from "./watchdog.ts";

const WATCH = { pollS: 0.05, graceS: 0.3 };
const TIMEOUT_MS = 8000;

const FAKE_PODMAN = `#!/bin/sh
# One log line per call: the gate script argument carries newlines of its own.
flat=$(printf '%s' "$*" | tr '\n' ' ')
case "$1" in
  --version) echo "podman version 6.1.4"; exit 0 ;;
  image) exit 0 ;;
  rm)
    printf 'rm %s\\n' "$flat" >> "$FAKE_LOG"
    rm -f "$FAKE_MARKER"
    exit 0 ;;
  run)
    printf 'run %s\\n' "$flat" >> "$FAKE_LOG"
    printf '%s\\n' "$FAKE_PID" > "$FAKE_PIDFILE"
    : > "$FAKE_MARKER"
    printf '%s\\n' "\${FAKE_OUT:-gate output}"
    [ -n "\${FAKE_IGNORE_TERM:-}" ] && trap '' TERM
    if [ -n "\${FAKE_SELF_EXIT:-}" ]; then
      sleep "\${FAKE_SELF_EXIT}"
      printf '%s\\n' "\${FAKE_ERR:-}" >&2
      exit "\${FAKE_CODE:-0}"
    fi
    i=0
    while [ -e "$FAKE_MARKER" ] && [ "$i" -lt 1200 ]; do i=$((i + 1)); sleep 0.05; done
    exit "\${FAKE_CODE:-0}" ;;
esac
exit 1
`;

interface Box {
  dir: string;
  env: Record<string, string>;
  script: string;
  marker: string;
  log: string;
}

async function box(extra: Record<string, string> = {}): Promise<Box> {
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-gate-run-"));
  const bin = path.join(dir, "bin");
  await fs.promises.mkdir(bin);
  await fs.promises.writeFile(path.join(bin, "podman"), FAKE_PODMAN, { mode: 0o700 });
  const marker = path.join(dir, "container");
  const log = path.join(dir, "calls.log");
  const pidfile = path.join(dir, "client.pid");
  const name = gateContainerName(process.pid, 1);
  const script = buildGateScript({
    argv: gateRunArgv({ image: "loxpp-dev-env", cwd: dir, name, script: "set -e\ncd /workspace" }),
    name,
    ...WATCH,
  });
  return {
    dir,
    script,
    marker,
    log,
    env: {
      ...process.env,
      PATH: `${bin}:${process.env.PATH}`,
      FAKE_LOG: log,
      FAKE_MARKER: marker,
      FAKE_PIDFILE: pidfile,
      FAKE_PID: "$$",
      ...extra,
    },
  };
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

const readOr = async (file: string, fallback = ""): Promise<string> => {
  try {
    return await fs.promises.readFile(file, "utf8");
  } catch {
    return fallback;
  }
};

const calls = async (b: Box): Promise<string[]> => (await readOr(b.log)).trim().split("\n").filter(Boolean);

function wrapper(b: Box, parentPid: number): ChildProcess {
  return spawn("/bin/sh", ["-c", b.script, "loxpp-gate", String(parentPid)], {
    detached: true,
    stdio: ["ignore", "pipe", "pipe"],
    env: b.env,
  });
}

async function finish(proc: ChildProcess): Promise<{ code: number | null; stdout: string; stderr: string }> {
  let stdout = "";
  let stderr = "";
  proc.stdout?.on("data", (d: Buffer) => (stdout += d.toString()));
  proc.stderr?.on("data", (d: Buffer) => (stderr += d.toString()));
  const code = await new Promise<number | null>((resolve) => proc.once("close", (c) => resolve(c)));
  return { code, stdout, stderr };
}

function standInParent(): ChildProcess {
  return spawn(process.execPath, ["-e", "setTimeout(() => {}, 60000)"], { stdio: "ignore" });
}

async function killParent(parent: ChildProcess): Promise<void> {
  const exited = new Promise<void>((resolve) => parent.once("exit", () => resolve()));
  parent.kill("SIGKILL");
  await exited;
}

const clientPidOf = async (b: Box): Promise<number> => Number.parseInt((await readOr(path.join(b.dir, "client.pid"))).trim(), 10);

test("the gate argv names the container and the wrapper removes it", () => {
  const name = gateContainerName(4242, 7);
  assert.equal(name, "loxpp-gate-4242-7");
  assert.notEqual(gateContainerName(4242, 8), name, "the counter keeps two runs apart");

  const argv = gateRunArgv({ image: "loxpp-dev-env", cwd: "/repo", name, script: "make" });
  assert.ok(argv.includes("--rm"));
  assert.ok(argv.includes(`--name=${name}`));
  assert.ok(argv.includes("/repo:/workspace:z"));
  assert.deepEqual(argv.slice(-3), ["bash", "-lc", "make"]);

  const script = buildGateScript({ argv, name });
  for (const needle of [WATCHDOG_MARKER, "parent_pid=${1:-}", `rm -f --time 0 '${name}'`, "trap 'abort' TERM INT HUP", 'exit "$status"']) {
    assert.ok(script.includes(needle), `the wrapper contains ${needle}`);
  }
});

test("a gate that ends by itself passes its code and its output through", { timeout: 20000 }, async () => {
  const b = await box({ FAKE_SELF_EXIT: "0.1" });
  const done = await finish(wrapper(b, process.pid));
  assert.equal(done.code, 0);
  assert.match(done.stdout, /gate output/);
  const log = await calls(b);
  assert.deepEqual(
    log.map((line) => line.split(" ")[0]),
    ["rm", "run"],
    "the leftover removal runs before the gate, and nothing is removed after it",
  );
});

test("a failing gate keeps its code and its stderr", { timeout: 20000 }, async () => {
  const b = await box({ FAKE_SELF_EXIT: "0.1", FAKE_CODE: "3", FAKE_ERR: "test failed" });
  const done = await finish(wrapper(b, process.pid));
  assert.equal(done.code, 3);
  assert.match(done.stderr, /test failed/);
});

test("the container ends when the parent dies", { timeout: 20000 }, async () => {
  const b = await box();
  const parent = standInParent();
  const proc = wrapper(b, parent.pid!);
  const done = finish(proc);
  try {
    await waitFor("the container to start", () => fs.existsSync(b.marker));
    const clientPid = await clientPidOf(b);
    await killParent(parent);
    await waitFor("the container to be removed", () => !fs.existsSync(b.marker));
    await waitFor("the client to end", () => !alive(clientPid));
    const log = await calls(b);
    assert.deepEqual(log.map((line) => line.split(" ")[0]), ["rm", "run", "rm"], "the removal follows the gate");
  } finally {
    proc.kill("SIGKILL");
    parent.kill("SIGKILL");
    await done;
  }
});

test("a client that ignores SIGTERM still loses its container", { timeout: 20000 }, async () => {
  const b = await box({ FAKE_IGNORE_TERM: "1" });
  const parent = standInParent();
  const proc = wrapper(b, parent.pid!);
  const done = finish(proc);
  try {
    await waitFor("the container to start", () => fs.existsSync(b.marker));
    const clientPid = await clientPidOf(b);
    await killParent(parent);
    await waitFor("the container to be removed", () => !fs.existsSync(b.marker));
    await waitFor("the client to end", () => !alive(clientPid));
  } finally {
    proc.kill("SIGKILL");
    parent.kill("SIGKILL");
    await done;
  }
});

test("an abort ends the run instead of waiting for it", { timeout: 20000 }, async () => {
  const b = await box();
  const proc = wrapper(b, process.pid);
  const done = finish(proc);
  try {
    await waitFor("the container to start", () => fs.existsSync(b.marker));
    const clientPid = await clientPidOf(b);
    const started = Date.now();
    proc.kill("SIGTERM");
    await waitFor("the wrapper to end", async () => !alive(proc.pid!));
    assert.ok(Date.now() - started < 5000, "the abort did not wait for the container's own end");
    assert.equal(fs.existsSync(b.marker), false, "the container is removed");
    assert.equal(alive(clientPid), false, "the client is gone");
  } finally {
    proc.kill("SIGKILL");
    await done;
  }
});

test("a leftover container of the same name is removed before the run", () => {
  const name = gateContainerName(1, 1);
  const script = buildGateScript({ argv: ["podman", "run", `--name=${name}`], name });
  const removeAt = script.indexOf("rm -f");
  assert.ok(removeAt > 0 && removeAt < script.indexOf("'podman' 'run'"), "the removal comes before the run");
  assert.equal(spawnSync("sh", ["-n", "-c", script]).status, 0, "the wrapper is valid shell");
  assert.ok(shellQuote(name).includes(name));
});
