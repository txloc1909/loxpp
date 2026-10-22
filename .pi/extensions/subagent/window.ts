/**
 * Running one subagent in one kitty window.
 *
 * The child owns its terminal, so it cannot also own a pipe to the parent.
 * stdout, stderr and the exit code go to files in a temporary directory and
 * this module tails them: the JSONL stream feeds progress, the exit file says
 * the run is over, and `kitten @ ls` catches a window the user closed by hand.
 *
 * Node builtins only, so the tests load this module directly.
 */

import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import {
  buildRunScript,
  closeWindowArgv,
  type KittySession,
  launchArgv,
  listWindowsArgv,
  setTitleArgv,
  windowIds,
} from "./kitty.ts";
import { EventStream, lastToolCall, type StreamState } from "./stream.ts";

export const POLL_MS = 150;
export const LIVENESS_MS = 2000;
export const DEAD_WINDOW_GRACE_MS = 1500;
export const KITTEN_TIMEOUT_MS = 10_000;

export interface ProcResult {
  code: number;
  stdout: string;
  stderr: string;
}

export function runProcess(argv: string[], timeoutMs = KITTEN_TIMEOUT_MS): Promise<ProcResult> {
  return new Promise((resolve) => {
    let stdout = "";
    let stderr = "";
    let settled = false;
    const finish = (result: ProcResult): void => {
      if (settled) return;
      settled = true;
      resolve(result);
    };
    const proc = spawn(argv[0], argv.slice(1), { stdio: ["ignore", "pipe", "pipe"] });
    const timer = setTimeout(() => {
      proc.kill("SIGKILL");
      finish({ code: 124, stdout, stderr: `${stderr}\n${argv[0]} timed out after ${timeoutMs} ms` });
    }, timeoutMs);
    proc.stdout.on("data", (data: Buffer) => {
      stdout += data.toString("utf8");
    });
    proc.stderr.on("data", (data: Buffer) => {
      stderr += data.toString("utf8");
    });
    proc.on("error", (error: Error) => {
      stderr += `\n${String(error)}`;
    });
    proc.on("close", (code) => {
      clearTimeout(timer);
      finish({ code: code ?? 0, stdout, stderr });
    });
  });
}

/**
 * One `kitten @ ls` per process: it proves remote control works and that pi's
 * window belongs to the kitty instance the socket reaches. Memoized because
 * every tool call would otherwise pay for it.
 */
let controlCheck: Promise<ProcResult> | null = null;

export function checkRemoteControl(): Promise<ProcResult> {
  controlCheck ??= runProcess(listWindowsArgv());
  return controlCheck;
}

export function describeControlProblem(result: ProcResult, session: KittySession): string {
  const reason = result.stderr.trim() || result.stdout.trim();
  if (result.code !== 0) {
    return [
      `kitty remote control is unavailable${reason ? `: ${reason}` : ""}.`,
      "",
      "Enable it in kitty.conf and restart kitty:",
      "  allow_remote_control yes",
      'or "allow_remote_control socket-only" together with a listen_on line.',
    ].join("\n");
  }
  if (!windowIds(result.stdout).includes(session.windowId)) {
    return `kitty window ${session.windowId} is not reachable through the current kitty socket, so a new window cannot open beside it.`;
  }
  return "";
}

export interface WindowRunOptions {
  session: KittySession;
  cwd: string;
  title: string;
  /** Lines printed in the new window before the child starts. */
  header: string[];
  command: string[];
  env: Record<string, string>;
  hold: boolean;
  keepFocus: boolean;
  signal: AbortSignal | undefined;
  onProgress: (state: StreamState) => void;
  kitten?: string;
  pollMs?: number;
  livenessMs?: number;
  deadWindowGraceMs?: number;
}

export interface WindowRunResult {
  windowId: string;
  exitCode: number;
  state: StreamState;
  stderr: string;
  closedByUser: boolean;
}

export async function runInKittyWindow(opts: WindowRunOptions): Promise<WindowRunResult> {
  const kitten = opts.kitten ?? "kitten";
  const pollMs = opts.pollMs ?? POLL_MS;
  const livenessMs = opts.livenessMs ?? LIVENESS_MS;
  const deadWindowGraceMs = opts.deadWindowGraceMs ?? DEAD_WINDOW_GRACE_MS;

  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-"));
  const eventsPath = path.join(dir, "events.jsonl");
  const stderrPath = path.join(dir, "stderr.log");
  const exitPath = path.join(dir, "exit.code");
  const scriptPath = path.join(dir, "run.sh");

  await fs.promises.writeFile(
    scriptPath,
    buildRunScript({
      header: opts.header,
      cwd: opts.cwd,
      command: opts.command,
      eventsPath,
      stderrPath,
      exitPath,
    }),
    { mode: 0o700 },
  );

  const launched = await runProcess(
    launchArgv(
      {
        session: opts.session,
        cwd: opts.cwd,
        title: opts.title,
        command: ["/bin/sh", scriptPath],
        hold: opts.hold,
        keepFocus: opts.keepFocus,
        env: opts.env,
      },
      kitten,
    ),
  );
  if (launched.code !== 0) {
    await fs.promises.rm(dir, { recursive: true, force: true });
    const reason = launched.stderr.trim() || launched.stdout.trim() || `exit code ${launched.code}`;
    throw new Error(`kitty could not open a subagent window: ${reason}`);
  }

  const windowId = launched.stdout.trim().split(/\s+/).pop() ?? "";
  if (!/^\d+$/.test(windowId)) {
    await fs.promises.rm(dir, { recursive: true, force: true });
    throw new Error(`kitty did not report the new window id (got ${JSON.stringify(launched.stdout)})`);
  }

  const closeWindow = (): Promise<ProcResult> => runProcess(closeWindowArgv(windowId, kitten));
  const onAbort = (): void => {
    void closeWindow();
  };
  opts.signal?.addEventListener("abort", onAbort, { once: true });

  const stream = new EventStream();
  let offset = 0;
  let lastUpdate = 0;
  let lastLiveness = 0;
  let lastTitle = "";
  let goneSince: number | null = null;

  try {
    for (;;) {
      if (opts.signal?.aborted) throw new Error("Subagent aborted");

      const chunk = await readNew(eventsPath, offset);
      if (chunk.length > 0) {
        offset += chunk.length;
        if (stream.push(chunk.toString("utf8")) && Date.now() - lastUpdate >= UPDATE_MS) {
          lastUpdate = Date.now();
          opts.onProgress(stream.state);
        }
      }

      const exitCode = await readExitCode(exitPath);
      if (exitCode !== null) {
        const tail = await readNew(eventsPath, offset);
        if (tail.length > 0) {
          offset += tail.length;
          stream.push(tail.toString("utf8"));
        }
        stream.end();
        opts.onProgress(stream.state);
        return {
          windowId,
          exitCode,
          state: stream.state,
          stderr: await readTail(stderrPath),
          closedByUser: false,
        };
      }

      if (Date.now() - lastLiveness >= livenessMs) {
        lastLiveness = Date.now();
        const listing = await runProcess(listWindowsArgv(kitten));
        if (listing.code === 0) {
          if (windowIds(listing.stdout).includes(windowId)) {
            const title = progressTitle(opts.title, stream.state);
            if (title !== lastTitle) {
              lastTitle = title;
              void runProcess(setTitleArgv(windowId, title, kitten));
            }
          } else if (goneSince === null) {
            goneSince = Date.now();
          }
        }
      }

      // A window closed by hand takes the exit code with it. Report what the
      // stream holds instead of waiting for a file that cannot appear.
      if (goneSince !== null && Date.now() - goneSince >= deadWindowGraceMs) {
        stream.end();
        return {
          windowId,
          exitCode: 128,
          state: stream.state,
          stderr: await readTail(stderrPath),
          closedByUser: true,
        };
      }

      await sleep(pollMs);
    }
  } finally {
    opts.signal?.removeEventListener("abort", onAbort);
    // The window closes on its own when the child exits; this covers abort, a
    // crashed child, and any kitty that keeps the window after the exit.
    if (!opts.hold) await closeWindow();
    await fs.promises.rm(dir, { recursive: true, force: true });
  }
}

const UPDATE_MS = 250; // progress updates for the caller's UI
const STDERR_CAP = 4000;

export function progressTitle(base: string, state: StreamState): string {
  const tool = lastToolCall(state);
  const turns = state.usage.turns;
  return `${base}${turns ? ` · ${turns} turn${turns > 1 ? "s" : ""}` : ""}${tool ? ` · ${tool.name}` : ""}`;
}

async function readNew(file: string, offset: number): Promise<Buffer> {
  let handle: fs.promises.FileHandle | undefined;
  try {
    handle = await fs.promises.open(file, "r");
    const { size } = await handle.stat();
    if (size <= offset) return Buffer.alloc(0);
    const buffer = Buffer.alloc(size - offset);
    let filled = 0;
    while (filled < buffer.length) {
      const { bytesRead } = await handle.read(buffer, filled, buffer.length - filled, offset + filled);
      if (bytesRead <= 0) break;
      filled += bytesRead;
    }
    return buffer.subarray(0, filled);
  } catch {
    return Buffer.alloc(0);
  } finally {
    await handle?.close();
  }
}

async function readExitCode(file: string): Promise<number | null> {
  try {
    const text = (await fs.promises.readFile(file, "utf8")).trim();
    if (!text) return null;
    const code = Number.parseInt(text, 10);
    return Number.isNaN(code) ? null : code;
  } catch {
    return null;
  }
}

async function readTail(file: string): Promise<string> {
  try {
    const text = await fs.promises.readFile(file, "utf8");
    return text.length > STDERR_CAP ? `...${text.slice(-STDERR_CAP)}` : text;
  } catch {
    return "";
  }
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
