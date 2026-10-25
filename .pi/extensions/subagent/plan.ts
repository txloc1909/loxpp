/**
 * Which kind of run one tool call asks for.
 *
 * A watched run has a window and a person who can steer it. It lasts as long
 * as that person keeps it, so the tool call returns at once and the result
 * arrives later as a message. An unattended run has no window: the call waits
 * for the result, which is what a tool call normally does.
 *
 * Node builtins only, so the tests load this module directly.
 */

export interface PlanTask {
  agent: string;
  task: string;
  cwd?: string;
}

export interface PlanParams {
  agent?: string;
  task?: string;
  tasks?: PlanTask[];
  chain?: PlanTask[];
  interactive?: boolean;
}

export type RunPlan =
  | { kind: "watched"; explicit: boolean }
  | { kind: "unattended"; explicit: boolean }
  | { kind: "invalid"; problem: string };

export const MULTI_TASK_INTERACTIVE_PROBLEM =
  "interactive applies to a single task only: a chain hands each step's result to the next, and a parallel batch has no one window to steer. Call the tool once per task, or pass interactive: false.";

/**
 * A missing `interactive` means different things per mode: a person usually
 * wants to watch one task, and nobody steers a batch. An explicit `true` on a
 * batch is a mistake, not a default, so it is refused rather than ignored.
 */
export function planRun(params: PlanParams): RunPlan {
  const batch = (params.tasks?.length ?? 0) > 0 || (params.chain?.length ?? 0) > 0;
  if (batch) {
    if (params.interactive === true) return { kind: "invalid", problem: MULTI_TASK_INTERACTIVE_PROBLEM };
    return { kind: "unattended", explicit: params.interactive === false };
  }
  if (params.interactive === false) return { kind: "unattended", explicit: true };
  return { kind: "watched", explicit: params.interactive === true };
}
