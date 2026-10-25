/**
 * What the parent session is told about a watched job.
 *
 * A watched run outlives the tool call, so its result cannot be a tool result.
 * It arrives as a custom message that starts a new turn (`triggerTurn`) and
 * waits behind a running turn (`followUp`): the parent finishes what it is
 * doing, then reads the answer.
 *
 * Node builtins and type-only imports only, so the tests load this directly.
 */

import type { JobView } from "./jobs.ts";
import { formatUsage } from "./stream.ts";

export const RESULT_CUSTOM_TYPE = "subagent-result";

/** The delivery that wakes the parent without cutting into a running turn. */
export const RESULT_DELIVERY: { triggerTurn: boolean; deliverAs: "followUp" } = { triggerTurn: true, deliverAs: "followUp" };

export const OUTPUT_CAP = 50 * 1024;
const TASK_PREVIEW = 200;

export function capText(text: string, cap = OUTPUT_CAP): string {
  const bytes = Buffer.byteLength(text, "utf8");
  if (bytes <= cap) return text;
  let truncated = text.slice(0, cap);
  while (Buffer.byteLength(truncated, "utf8") > cap) truncated = truncated.slice(0, -1);
  const omitted = bytes - Buffer.byteLength(truncated, "utf8");
  return `${truncated}\n\n[Output truncated: ${omitted} bytes omitted.]`;
}

function oneLine(text: string, max = TASK_PREVIEW): string {
  const flat = text.replace(/\s+/g, " ").trim();
  return flat.length > max ? `${flat.slice(0, max)}...` : flat;
}

export function launchText(view: JobView): string {
  const where = view.windowId ? ` (window ${view.windowId})` : "";
  return [
    `Started subagent job #${view.id}: ${view.agent}${where}.`,
    `Task: ${oneLine(view.task)}`,
    "This run waits for a person in its window. It ends when that person quits or closes the window.",
    `Keep working, or end your turn: a "${RESULT_CUSTOM_TYPE}" message arrives when the run ends. Do not guess its answer before that message arrives.`,
  ].join("\n");
}

const STATUS_HEAD: Record<"completed" | "stopped" | "failed", (view: JobView) => string> = {
  completed: (view) => `Subagent job #${view.id} (${view.agent}) completed.`,
  stopped: (view) => `Subagent job #${view.id} (${view.agent}) was stopped: its window closed while a turn was still running.`,
  failed: (view) => `Subagent job #${view.id} (${view.agent}) failed.`,
};

/**
 * The message the parent's model reads. It must not let an old answer look like
 * the answer to the task: a person may have redirected the run, and a run that
 * was cut off holds only the messages the child finished before that point.
 */
export function resultText(view: JobView): string {
  const status = view.status === "completed" || view.status === "stopped" ? view.status : "failed";
  const lines = [STATUS_HEAD[status](view), `Task: ${oneLine(view.task)}`];
  if (view.errorMessage) lines.push(`Reason: ${view.errorMessage}`);
  const output = capText(view.output ?? "");
  lines.push(output ? `Answer:\n${output}` : "Answer: (no output)");
  if (status === "stopped") {
    lines.push("Only the messages the child finished before the window closed are included.");
  }
  lines.push("A person may have redirected this run in its window: read this as the child's last word, not as an answer to the task alone.");
  const usage = formatUsage(view.usage, view.model);
  if (usage) lines.push(`Usage: ${usage}`);
  return lines.join("\n");
}

export interface ResultMessage {
  customType: string;
  content: string;
  display: boolean;
  details: unknown;
}

export interface ResultSender {
  (message: ResultMessage, options: { triggerTurn: boolean; deliverAs: "followUp" }): void;
}

/** Send the end of a watched run to the session that started it. */
export function deliverResult(send: ResultSender, view: JobView): void {
  send({ customType: RESULT_CUSTOM_TYPE, content: resultText(view), display: true, details: view }, RESULT_DELIVERY);
}

/** The footer line for the jobs that are still running. */
export function statusText(running: JobView[]): string | undefined {
  if (running.length === 0) return undefined;
  const [first] = running;
  const extra = running.length > 1 ? ` +${running.length - 1}` : "";
  const where = first.windowId ? ` · window ${first.windowId}` : "";
  const tool = first.tool ? ` · ${first.tool}` : "";
  return `subagent ${first.agent} #${first.id}${where}${tool}${extra}`;
}
