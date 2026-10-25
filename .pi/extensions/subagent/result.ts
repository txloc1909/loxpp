/**
 * Grading one subagent run.
 *
 * Closing an interactive child's window is how a person ends that run, so a
 * vanished window is a normal end there. A failed or interrupted turn is still
 * a failure: the parent must not get a green result over a child that errored,
 * and the stored error message must reach the tool result.
 *
 * Node builtins and type-only imports only, so the tests load this directly.
 */

import { finalOutput, type StreamState, type UsageStats } from "./stream.ts";

export interface SingleResult {
  agent: string;
  agentSource: "user" | "project" | "unknown";
  task: string;
  exitCode: number;
  messages: StreamState["messages"];
  stderr: string;
  usage: UsageStats;
  windowId?: string;
  model?: string;
  stopReason?: string;
  errorMessage?: string;
  closedByUser?: boolean;
  interactive?: boolean;
  step?: number;
}

export function isFailed(result: SingleResult): boolean {
  // The turn's own verdict comes first. A person who closes the window after a
  // failed or interrupted turn must not turn that turn green.
  if (result.stopReason === "error" || result.stopReason === "aborted") return true;
  // A closed window is the normal end of an interactive run, and a failure for
  // a print-mode run that would otherwise have ended by itself.
  if (result.closedByUser && result.interactive) return false;
  return result.exitCode !== 0;
}

/** The text the parent reports for one result, or the reason it failed. */
export function resultOutput(result: SingleResult): string {
  if (isFailed(result)) return result.errorMessage || result.stderr || finalOutput(result) || "(no output)";
  return finalOutput(result) || "(no output)";
}
