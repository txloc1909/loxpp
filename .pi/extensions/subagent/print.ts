/**
 * A subagent run with no window: a plain child process and pipes.
 *
 * An unattended run has nobody at a keyboard, so it needs no terminal and no
 * kitty window. The child writes its `--mode json` event stream to stdout,
 * which this module reads incrementally; stderr stays in memory and the exit
 * code ends the run. This also works outside kitty.
 *
 * Node builtins and type-only imports only, so the tests load this directly.
 */

import { spawn } from "node:child_process";
import { StringDecoder } from "node:string_decoder";
import { EventStream, type StreamState } from "./stream.ts";

export const STDERR_CAP = 4000;
export const KILL_GRACE_MS = 2000;
const PROGRESS_MS = 250;

/**
 * Children of running jobs, so a pi exit takes them with it. A detached child
 * leads its own process group, and nothing else would signal that group when
 * this process goes away.
 */
const live = new Set<number>();
let exitGuardInstalled = false;

function killGroup(pid: number, signal: NodeJS.Signals): void {
  try {
    process.kill(-pid, signal);
  } catch {
    // The group is already gone; the child's close event is what ends the run.
  }
}

function guardExits(): void {
  if (exitGuardInstalled) return;
  exitGuardInstalled = true;
  process.once("exit", () => {
    for (const pid of live) killGroup(pid, "SIGKILL");
  });
}

export interface PrintRunOptions {
  command: string;
  args: string[];
  cwd: string;
  env?: NodeJS.ProcessEnv;
  signal?: AbortSignal;
  onProgress?: (state: StreamState) => void;
  killGraceMs?: number;
}

export interface PrintRunResult {
  exitCode: number;
  state: StreamState;
  stderr: string;
  aborted: boolean;
}

export function runPrintChild(opts: PrintRunOptions): Promise<PrintRunResult> {
  const stream = new EventStream();
  const decoder = new StringDecoder("utf8");
  let stderr = "";
  let aborted = false;
  let lastUpdate = 0;

  return new Promise((resolve) => {
    // Detached, so the child leads a process group: an abort then reaches the
    // commands the child started, not only the child. A shell that does not
    // forward signals would otherwise leave its own child holding the pipe.
    const child = spawn(opts.command, opts.args, {
      cwd: opts.cwd,
      env: { ...process.env, ...opts.env },
      stdio: ["ignore", "pipe", "pipe"],
      detached: true,
    });
    const pid = child.pid;
    if (pid !== undefined) {
      live.add(pid);
      guardExits();
    }

    const kill = (signal: NodeJS.Signals): void => {
      if (pid !== undefined) killGroup(pid, signal);
    };
    const onAbort = (): void => {
      aborted = true;
      kill("SIGTERM");
      // A child that ignores SIGTERM must not hold the tool call forever.
      const timer = setTimeout(() => kill("SIGKILL"), opts.killGraceMs ?? KILL_GRACE_MS);
      timer.unref();
    };
    opts.signal?.addEventListener("abort", onAbort, { once: true });

    child.stdout.on("data", (data: Buffer) => {
      // A chunk can split a multi-byte character; the decoder holds the bytes
      // until the character is complete, so no line is corrupted at the seam.
      if (!stream.push(decoder.write(data))) return;
      const now = Date.now();
      if (now - lastUpdate < PROGRESS_MS) return;
      lastUpdate = now;
      opts.onProgress?.(stream.state);
    });

    child.stderr.on("data", (data: Buffer) => {
      stderr = `${stderr}${data.toString("utf8")}`.slice(-STDERR_CAP);
    });

    child.on("error", (error: Error) => {
      stderr = `${stderr}\n${String(error)}`.slice(-STDERR_CAP);
    });

    child.on("close", (code) => {
      opts.signal?.removeEventListener("abort", onAbort);
      if (pid !== undefined) live.delete(pid);
      stream.push(decoder.end());
      stream.end();
      opts.onProgress?.(stream.state);
      resolve({
        exitCode: aborted ? 130 : (code ?? 128),
        state: stream.state,
        stderr,
        aborted,
      });
    });
  });
}
