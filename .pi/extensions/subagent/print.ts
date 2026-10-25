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
    const child = spawn(opts.command, opts.args, {
      cwd: opts.cwd,
      env: { ...process.env, ...opts.env },
      stdio: ["ignore", "pipe", "pipe"],
    });

    const kill = (signal: NodeJS.Signals): void => {
      try {
        child.kill(signal);
      } catch {
        // Already gone; the close event is the only thing this run waits for.
      }
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
