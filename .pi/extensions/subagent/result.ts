/**
 * Grading one subagent run.
 *
 * Closing a watched child's window is how a person ends that run, so a
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
  /**
   * The child was at its prompt when the run ended. A window that vanishes in
   * this state ended a wait, not a turn.
   */
  idle?: boolean;
}

export function isFailed(result: SingleResult): boolean {
  // The turn's own verdict comes first. A person who closes the window after a
  // failed or interrupted turn must not turn that turn green.
  if (result.stopReason === "error" || result.stopReason === "aborted") return true;
  // A closed window is the normal end of a watched run. An unattended run
  // never has a window to close.
  if (result.closedByUser && result.interactive) return false;
  return result.exitCode !== 0;
}

export type RunStatus = "completed" | "stopped" | "failed";

/**
 * How a run ended, for a reader who did not watch it.
 *
 * A window closed while a turn is still running cuts the child off: the run
 * did end, but not at its own end, and only the messages the child finished
 * before that point are in the result. Calling that "completed" would let the
 * parent read an old answer as the answer to its task.
 */
export function runStatus(result: SingleResult): RunStatus {
  if (isFailed(result)) return "failed";
  if (result.closedByUser && result.idle === false) return "stopped";
  return "completed";
}

/** One result, built from what a runner saw of the child. */
export function runResult(input: {
  agent: string;
  agentSource: SingleResult["agentSource"];
  task: string;
  exitCode: number;
  state: StreamState;
  stderr: string;
  windowId?: string;
  interactive: boolean;
  closedByUser?: boolean;
  step?: number;
}): SingleResult {
  return {
    agent: input.agent,
    agentSource: input.agentSource,
    task: input.task,
    exitCode: input.exitCode,
    messages: input.state.messages,
    stderr: input.stderr,
    usage: input.state.usage,
    windowId: input.windowId,
    model: input.state.model,
    stopReason: input.state.stopReason,
    errorMessage: input.state.errorMessage,
    closedByUser: input.closedByUser,
    interactive: input.interactive,
    step: input.step,
    idle: input.state.idle,
  };
}

/** The text the parent reports for one result, or the reason it failed. */
export function resultOutput(result: SingleResult): string {
  if (isFailed(result)) return result.errorMessage || result.stderr || finalOutput(result) || "(no output)";
  return finalOutput(result) || "(no output)";
}
