/**
 * Watched subagent jobs.
 *
 * A watched run has a window and a person who can steer it. It lasts as long
 * as that person keeps it, so the tool call must not wait for it: this manager
 * starts the run, hands back a view of it as soon as the window opens, and
 * turns the run's end into one notification.
 *
 * A run that never opened its window is reported by the tool call itself, and
 * a run that the session abandoned sends nothing at all.
 *
 * Node builtins and type-only imports only, so the tests load this directly.
 */

import { type SingleResult, runStatus } from "./result.ts";
import { finalOutput, lastToolCall, type StreamState, type UsageStats } from "./stream.ts";

export type JobStatus = "running" | "completed" | "stopped" | "failed" | "abandoned";

export interface JobView {
  id: number;
  agent: string;
  task: string;
  cwd: string;
  startedAt: number;
  status: JobStatus;
  /** Set once kitty reports the window. A launch waits for this, nothing else. */
  windowId?: string;
  exitCode?: number;
  model?: string;
  usage?: UsageStats;
  turns: number;
  /** The tool the child called last, for the footer line. */
  tool?: string;
  /** The child's last assistant text, uncapped. */
  output?: string;
  errorMessage?: string;
}

export interface JobRunContext {
  id: number;
  signal: AbortSignal;
  onWindow: (windowId: string) => void;
  onProgress: (state: StreamState) => void;
}

export interface JobSpec {
  agent: string;
  task: string;
  cwd: string;
  run: (ctx: JobRunContext) => Promise<SingleResult>;
}

export interface JobManagerDeps {
  /** One call per job that opened a window and then ended. */
  notify: (view: JobView) => void;
  /** Called when the set of running jobs changes. */
  onProgress?: (running: JobView[]) => void;
  now?: () => number;
}

export interface JobManager {
  /** Resolves when the window opens; rejects when the run fails before that. */
  launch(spec: JobSpec): Promise<JobView>;
  running(): JobView[];
  get(id: number): JobView | undefined;
  /** Session teardown: abort every run, send no notification, forget it. */
  abandonAll(): void;
}

export function createJobManager(deps: JobManagerDeps): JobManager {
  const now = deps.now ?? Date.now;
  const jobs = new Map<number, JobView>();
  const controllers = new Map<number, AbortController>();
  let nextId = 1;

  const running = (): JobView[] => [...jobs.values()].filter((job) => job.status === "running");
  const emit = (): void => deps.onProgress?.(running());

  function finish(view: JobView, result: SingleResult): void {
    if (view.status === "abandoned") return;
    view.status = runStatus(result);
    view.output = finalOutput(result);
    view.errorMessage = result.errorMessage;
    view.exitCode = result.exitCode;
    view.model = result.model;
    view.usage = result.usage;
    view.turns = result.usage.turns;
    controllers.delete(view.id);
    // Only a run that opened a window is out of the caller's hands. One that
    // failed earlier is already the launch's answer.
    if (view.windowId !== undefined) deps.notify(view);
    emit();
  }

  function finishWithError(view: JobView, error: unknown): void {
    if (view.status === "abandoned") return;
    view.status = "failed";
    view.errorMessage = error instanceof Error ? error.message : String(error);
    controllers.delete(view.id);
    if (view.windowId !== undefined) deps.notify(view);
    emit();
  }

  return {
    async launch(spec: JobSpec): Promise<JobView> {
      const id = nextId++;
      const view: JobView = {
        id,
        agent: spec.agent,
        task: spec.task,
        cwd: spec.cwd,
        startedAt: now(),
        status: "running",
        turns: 0,
      };
      jobs.set(id, view);
      const controller = new AbortController();
      controllers.set(id, controller);

      let opened: () => void = () => {};
      const windowOpened = new Promise<void>((resolve) => {
        opened = resolve;
      });

      const done = (async () =>
        spec.run({
          id,
          signal: controller.signal,
          onWindow: (windowId) => {
            view.windowId = windowId;
            opened();
          },
          onProgress: (state) => {
            view.turns = state.usage.turns;
            view.tool = lastToolCall(state)?.name;
            emit();
          },
        }))();
      const settled = done.then(
        (result) => finish(view, result),
        (error) => finishWithError(view, error),
      );

      // The window, not the run, is what the caller waits for. If the run ends
      // first, its failure is the launch's answer.
      await Promise.race([windowOpened, settled]);
      if (view.status === "running" || view.status === "abandoned") return view;
      throw new Error(view.errorMessage ?? "the subagent ended before its window opened");
    },

    running,

    get(id: number): JobView | undefined {
      return jobs.get(id);
    },

    abandonAll(): void {
      for (const [id, controller] of controllers) {
        const view = jobs.get(id);
        if (view) view.status = "abandoned";
        controller.abort();
      }
      controllers.clear();
      jobs.clear();
      deps.onProgress?.([]);
    },
  };
}
