/**
 * Subagent tool: delegate a task to a separate `pi` process.
 *
 * Two kinds of run, and only one of them has a window.
 *
 * - A watched run (the default for a single task) opens a window in pi's kitty
 *   tab and runs a normal interactive pi session there, so a person can chat
 *   with it and redirect it. The tool call returns as soon as the window opens;
 *   the end of the run arrives later as a `subagent-result` message that wakes
 *   this session. Nothing here waits for a person.
 * - An unattended run (`interactive: false`, or a parallel/chain batch) opens no
 *   window: it is a child process with pipes, and the tool call waits for it.
 *
 * The parent never shares a watched child's terminal. That child mirrors its
 * messages to a file through `mirror.ts`, which `window.ts` tails. An
 * unattended child writes the same records to stdout, which `print.ts` reads.
 *
 * Modes: single ({ agent, task }), parallel ({ tasks: [...] }), and chain
 * ({ chain: [...] }, with a `{previous}` placeholder in later tasks).
 */

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import type { AgentToolResult } from "@earendil-works/pi-agent-core";
import { StringEnum } from "@earendil-works/pi-ai";
import {
  CONFIG_DIR_NAME,
  type ExtensionAPI,
  type ExtensionContext,
  getAgentDir,
  getMarkdownTheme,
  type Theme,
} from "@earendil-works/pi-coding-agent";
import { Box, Container, Markdown, Spacer, Text } from "@earendil-works/pi-tui";
import { Type } from "typebox";
import { type AgentConfig, type AgentScope, discoverAgents, formatAgentList } from "./agents.ts";
import { childArgs, childEnv, type Dispatch, piInvocation } from "./child.ts";
import { createJobManager, type JobView } from "./jobs.ts";
import { detectKitty, type KittySession } from "./kitty.ts";
import { RESULT_CUSTOM_TYPE, capText, deliverResult, launchText, statusText } from "./notify.ts";
import { planRun } from "./plan.ts";
import { runPrintChild } from "./print.ts";
import { isFailed, resultOutput, runResult, type SingleResult } from "./result.ts";
import { addUsage, finalOutput, formatUsage, type StreamState, type UsageStats } from "./stream.ts";
import { checkRemoteControl, runInKittyWindow } from "./window.ts";

const MAX_PARALLEL_TASKS = 8;
const MAX_CONCURRENCY = 4;
const STATUS_KEY = "subagent";

interface SubagentDetails {
  mode: "single" | "parallel" | "chain";
  agentScope: AgentScope;
  projectAgentsDir: string | null;
  results: SingleResult[];
  /** Set on a launch: the watched runs this call started. */
  jobs?: JobView[];
}

type OnUpdate = (partial: AgentToolResult<SubagentDetails>) => void;

function emptyUsage(): UsageStats {
  return { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, cost: 0, contextTokens: 0, turns: 0 };
}

/**
 * The mirror that feeds a watched child's events to the parent. Resolved from
 * this file so the child loads the copy that belongs to the running extension.
 */
function mirrorExtensionPath(): string {
  return fileURLToPath(new URL("./mirror.ts", import.meta.url));
}

function kittyRequiredMessage(): string {
  return [
    "A watched subagent needs a kitty window, and this pi session is not inside one.",
    "Pass interactive: false to run the task unattended: no window, and the tool call waits for the result.",
  ].join("\n");
}

async function mapWithConcurrency<T>(items: T[], concurrency: number, fn: (item: T, index: number) => Promise<void>): Promise<void> {
  let next = 0;
  const workers = Array.from({ length: Math.max(1, Math.min(concurrency, items.length)) }, async () => {
    for (;;) {
      const index = next++;
      if (index >= items.length) return;
      await fn(items[index], index);
    }
  });
  await Promise.all(workers);
}

// ---- registration ------------------------------------------------------

const TaskItem = Type.Object({
  agent: Type.String({ description: "Name of the agent to invoke" }),
  task: Type.String({ description: "Task to delegate to the agent" }),
  cwd: Type.Optional(Type.String({ description: "Working directory for the subagent" })),
});

const SubagentParams = Type.Object({
  agent: Type.Optional(Type.String({ description: "Agent name (single mode)" })),
  task: Type.Optional(Type.String({ description: "Task to delegate (single mode)" })),
  tasks: Type.Optional(Type.Array(TaskItem, { description: "Parallel tasks; always unattended" })),
  chain: Type.Optional(Type.Array(TaskItem, { description: "Sequential tasks; later tasks may use {previous}. Always unattended" })),
  agentScope: Type.Optional(
    StringEnum(["user", "project", "both"] as const, {
      description: `Which agent directories to use. Default "user" (${path.join(getAgentDir(), "agents")}). Use "both" to include project-local ${CONFIG_DIR_NAME}/agents.`,
      default: "user",
    }),
  ),
  confirmProjectAgents: Type.Optional(
    Type.Boolean({ description: "Ask before running repo-controlled project agents in an untrusted project", default: true }),
  ),
  cwd: Type.Optional(Type.String({ description: "Working directory for the subagent. Default the session cwd" })),
  interactive: Type.Optional(
    Type.Boolean({
      description:
        "Run one task as a watched job in a kitty window: a person can steer it, the call returns as soon as the window opens, and the result arrives later as a subagent-result message that wakes this session. Default true for a single task. Set false for an unattended run: no window, and the call waits for the result. An explicit true with tasks or chain is refused, because a batch has no one window to steer.",
    }),
  ),
  hold: Type.Optional(
    Type.Boolean({ description: "Watched runs only: keep the window open at a shell after the child exits. Default false", default: false }),
  ),
  keepFocus: Type.Optional(Type.Boolean({ description: "Watched runs only: leave focus in pi's window. Default false", default: false })),
});

export default function subagentExtension(pi: ExtensionAPI): void {
  // A window is only needed for a watched run, so the tool registers even
  // outside kitty: an unattended run works in any terminal.
  const session: KittySession | null = detectKitty();
  let ui: ExtensionContext["ui"] | undefined;

  const jobs = createJobManager({
    notify: (view) => deliverResult((message, options) => pi.sendMessage(message, options), view),
    onProgress: (running) => {
      try {
        ui?.setStatus(STATUS_KEY, statusText(running));
      } catch {
        // A session replacement invalidates the captured ctx. The footer is not
        // worth a failed run, and session_shutdown already stopped the jobs.
        ui = undefined;
      }
    },
  });

  pi.on("session_start", async (_event, ctx) => {
    ui = ctx.ui;
    if (!session) return;
    const check = await checkRemoteControl(session);
    if (!check.ok && ctx.hasUI) ctx.ui.notify(check.problem, "error");
  });

  // A job belongs to the session that started it: its result must not land in
  // the session that replaced it, so every shutdown path stops and forgets it.
  pi.on("session_shutdown", () => {
    jobs.abandonAll();
    ui = undefined;
  });

  pi.registerMessageRenderer<JobView>(RESULT_CUSTOM_TYPE, (message, { expanded, outputPad }, theme) => {
    const view = message.details;
    const status = view?.status ?? "completed";
    const icon =
      status === "failed" ? theme.fg("error", "✗") : status === "stopped" ? theme.fg("warning", "⏹") : theme.fg("success", "✓");
    const name = view ? `${theme.fg("toolTitle", theme.bold(view.agent))} ${theme.fg("muted", `#${view.id}`)}` : "subagent";
    const usage = view?.usage ? formatUsage(view.usage, view.model) : "";
    const lines = [`${icon} ${name}${usage ? ` ${theme.fg("dim", usage)}` : ""}`];
    const output = (view?.output ?? "").trim();
    if (output) {
      const all = output.split("\n");
      const shown = expanded ? all : all.slice(0, 4);
      for (const line of shown) lines.push(theme.fg("toolOutput", line));
      if (shown.length < all.length) lines.push(theme.fg("dim", `... ${all.length - shown.length} more lines`));
    } else if (view?.errorMessage) {
      lines.push(theme.fg("error", view.errorMessage));
    }
    const box = new Box(outputPad, 1, (text) => theme.bg("customMessageBg", text));
    box.addChild(new Text(lines.join("\n"), 0, 0));
    return box;
  });

  pi.registerTool({
    name: "subagent",
    label: "Subagent",
    description: [
      "Delegate a task to a subagent: a separate pi process with its own context.",
      "A single task by default runs as a watched job in a new kitty window, where a person can chat with it and redirect it. That call returns as soon as the window opens, so keep working or end your turn; a subagent-result message arrives when the run ends and wakes this session. Never report a watched run's answer before that message arrives.",
      "Set interactive: false for an unattended run: no window, and the call waits for the result. Parallel (tasks) and chain runs are always unattended, and the call waits for them.",
      "Modes: single (agent + task), parallel (tasks array, max 8), chain (sequential, {previous} placeholder).",
      'Default agent scope is "user"; set agentScope: "both" to allow project-local agents.',
    ].join(" "),
    promptSnippet: "delegate a task to a subagent; a watched run returns at once and its result wakes you later",
    parameters: SubagentParams,

    async execute(_toolCallId, params, signal, onUpdate, ctx) {
      // A fresh ctx is the only one the footer can trust after a session
      // replacement; session_start sets it first, and a reload clears it.
      ui = ctx.ui;
      const plan = planRun(params);
      if (plan.kind === "invalid") {
        return { content: [{ type: "text" as const, text: plan.problem }], details: undefined, isError: true };
      }
      const watched = plan.kind === "watched";

      const agentScope: AgentScope = params.agentScope ?? "user";
      const hold = params.hold ?? false;
      const keepFocus = params.keepFocus ?? false;
      const discovery = discoverAgents(ctx.cwd, agentScope);
      const agents = discovery.agents;
      const baseCwd = params.cwd ? path.resolve(ctx.cwd, params.cwd) : ctx.cwd;
      const mode = params.chain?.length ? "chain" : params.tasks?.length ? "parallel" : "single";
      const makeDetails = (results: SingleResult[], jobList?: JobView[]): SubagentDetails => ({
        mode,
        agentScope,
        projectAgentsDir: discovery.projectAgentsDir,
        results,
        ...(jobList && jobList.length > 0 ? { jobs: jobList } : {}),
      });

      if (watched) {
        if (!session) {
          return { content: [{ type: "text" as const, text: kittyRequiredMessage() }], details: makeDetails([]), isError: true };
        }
        if (!ctx.hasUI) {
          return {
            content: [
              {
                type: "text" as const,
                text: "A watched subagent needs an interactive session: this one has no UI, so nobody could steer the run. Pass interactive: false.",
              },
            ],
            details: makeDetails([]),
            isError: true,
          };
        }
        const check = await checkRemoteControl(session);
        if (!check.ok) return { content: [{ type: "text" as const, text: check.problem }], details: makeDetails([]), isError: true };
      }

      const wanted = new Set<string>();
      for (const step of params.chain ?? []) wanted.add(step.agent);
      for (const task of params.tasks ?? []) wanted.add(task.agent);
      if (params.agent) wanted.add(params.agent);
      const repoAgents = agents.filter((agent) => agent.source === "project" && wanted.has(agent.name));
      if (repoAgents.length > 0 && (params.confirmProjectAgents ?? true) && ctx.hasUI && !ctx.isProjectTrusted()) {
        const names = repoAgents.map((agent) => agent.name).join(", ");
        const ok = await ctx.ui.confirm(
          "Run project-local agents?",
          `Agents: ${names}\nSource: ${discovery.projectAgentsDir ?? "(unknown)"}\n\nProject agents are repo-controlled. Continue only for a trusted repository.`,
        );
        if (!ok) {
          return { content: [{ type: "text" as const, text: "Canceled: project-local agents not approved." }], details: makeDetails([]) };
        }
      }

      const dispatch: Dispatch = {
        model: ctx.model ? `${ctx.model.provider}/${ctx.model.id}` : undefined,
        thinking: ctx.thinkingLevel,
      };

      /**
       * Start the child and wait for it. `onWindow` reports the window id of a
       * watched run before the run ends; `onProgress` feeds both the tool card
       * (unattended) and the footer (watched).
       */
      const startChild = async (
        agent: AgentConfig,
        task: string,
        cwd: string,
        opts: {
          title: string;
          signal: AbortSignal | undefined;
          onWindow?: (windowId: string) => void;
          onProgress?: (state: StreamState) => void;
        },
      ): Promise<SingleResult> => {
        const promptDir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-prompt-"));
        try {
          let promptPath: string | undefined;
          if (agent.systemPrompt.trim()) {
            promptPath = path.join(promptDir, `prompt-${agent.name.replace(/[^\w.-]+/g, "_")}.md`);
            await fs.promises.writeFile(promptPath, agent.systemPrompt, { encoding: "utf-8", mode: 0o600 });
          }
          const pi = piInvocation();
          const command = [
            pi.command,
            ...pi.args,
            ...childArgs(agent, task, promptPath, dispatch, {
              interactive: watched,
              mirrorPath: watched ? mirrorExtensionPath() : undefined,
            }),
          ];

          if (watched) {
            const run = await runInKittyWindow({
              session: session!,
              cwd,
              title: opts.title,
              header: [
                opts.title,
                agent.description,
                `cwd: ${cwd}`,
                hold
                  ? "chat here to direct the subagent; quit it (Ctrl+D) or close this window to send its last answer back, and this window stays open"
                  : "chat here to direct the subagent; quit it (Ctrl+D) or close this window to send its last answer back",
              ],
              command,
              env: childEnv(),
              hold,
              keepFocus,
              signal: opts.signal,
              onWindow: opts.onWindow,
              onProgress: opts.onProgress ?? (() => {}),
            });
            return runResult({
              agent: agent.name,
              agentSource: agent.source,
              task,
              exitCode: run.exitCode,
              state: run.state,
              stderr: run.stderr,
              windowId: run.windowId,
              interactive: true,
              closedByUser: run.closedByUser,
            });
          }

          const run = await runPrintChild({
            command: command[0],
            args: command.slice(1),
            cwd,
            signal: opts.signal,
            onProgress: opts.onProgress,
          });
          const result = runResult({
            agent: agent.name,
            agentSource: agent.source,
            task,
            exitCode: run.exitCode,
            state: run.state,
            stderr: run.stderr,
            interactive: false,
          });
          // An abort kills the child, so its exit code alone does not say why.
          if (run.aborted) result.errorMessage = "the run was aborted";
          return result;
        } finally {
          await fs.promises.rm(promptDir, { recursive: true, force: true });
        }
      };

      const unknownAgent = (agentName: string, task: string, step?: number): SingleResult => ({
        agent: agentName,
        agentSource: "unknown",
        task,
        exitCode: 1,
        messages: [],
        stderr: `Unknown agent "${agentName}". Available agents: ${formatAgentList(agents)}.`,
        usage: emptyUsage(),
        step,
      });

      const runUnattended = (
        agentName: string,
        task: string,
        cwd: string | undefined,
        step: number | undefined,
        update: OnUpdate | undefined,
      ): Promise<SingleResult> => {
        const agent = agents.find((candidate) => candidate.name === agentName);
        if (!agent) return Promise.resolve(unknownAgent(agentName, task, step));
        const preview: SingleResult = {
          agent: agentName,
          agentSource: agent.source,
          task,
          exitCode: -1,
          messages: [],
          stderr: "",
          usage: emptyUsage(),
          step,
        };
        const emit = update
          ? (state: StreamState): void => {
              preview.messages = state.messages;
              preview.usage = state.usage;
              preview.model = state.model;
              update({
                content: [{ type: "text", text: finalOutput(state) || "(running...)" }],
                details: makeDetails([preview]),
              });
            }
          : undefined;
        return startChild(agent, task, cwd ? path.resolve(baseCwd, cwd) : baseCwd, {
          title: `pi subagent: ${agent.name}`,
          signal,
          onProgress: emit,
        });
      };

      if (mode === "chain") {
        const chain = params.chain!;
        const results: SingleResult[] = [];
        let previous = "";
        for (let i = 0; i < chain.length; i++) {
          const step = chain[i];
          const result = await runUnattended(
            step.agent,
            step.task.replace(/\{previous\}/g, previous),
            step.cwd,
            i + 1,
            onUpdate
              ? (partial) => {
                  const current = partial.details?.results[0];
                  if (current) onUpdate({ content: partial.content, details: makeDetails([...results, current]) });
                }
              : undefined,
          );
          results.push(result);
          if (isFailed(result)) {
            return {
              content: [{ type: "text" as const, text: `Chain stopped at step ${i + 1} (${step.agent}): ${resultOutput(result)}` }],
              details: makeDetails(results),
              isError: true,
            };
          }
          previous = finalOutput(result);
        }
        return {
          content: [{ type: "text" as const, text: capText(finalOutput(results[results.length - 1])) || "(no output)" }],
          details: makeDetails(results),
        };
      }

      if (mode === "parallel") {
        const tasks = params.tasks!;
        if (tasks.length > MAX_PARALLEL_TASKS) {
          return {
            content: [{ type: "text" as const, text: `Too many parallel tasks (${tasks.length}). Max is ${MAX_PARALLEL_TASKS}.` }],
            details: makeDetails([]),
          };
        }
        const slots: SingleResult[] = tasks.map((task) => ({
          agent: task.agent,
          agentSource: "unknown" as const,
          task: task.task,
          exitCode: -1,
          messages: [],
          stderr: "",
          usage: emptyUsage(),
        }));
        const emit = (): void => {
          if (!onUpdate) return;
          const done = slots.filter((slot) => slot.exitCode !== -1).length;
          onUpdate({
            content: [{ type: "text", text: `Parallel: ${done}/${slots.length} done, ${slots.length - done} running...` }],
            details: makeDetails([...slots]),
          });
        };

        await mapWithConcurrency(tasks, MAX_CONCURRENCY, async (task, index) => {
          slots[index] = await runUnattended(
            task.agent,
            task.task,
            task.cwd,
            undefined,
            onUpdate
              ? (partial) => {
                  const current = partial.details?.results[0];
                  if (current) {
                    slots[index] = current;
                    emit();
                  }
                }
              : undefined,
          );
          emit();
        });

        const succeeded = slots.filter((slot) => !isFailed(slot)).length;
        const summaries = slots.map((slot) => {
          const status = isFailed(slot) ? `failed${slot.stopReason && slot.stopReason !== "end" ? ` (${slot.stopReason})` : ""}` : "completed";
          return `### [${slot.agent}] ${status}\n\n${capText(resultOutput(slot))}`;
        });
        return {
          content: [{ type: "text" as const, text: `Parallel: ${succeeded}/${slots.length} succeeded\n\n${summaries.join("\n\n---\n\n")}` }],
          details: makeDetails(slots),
        };
      }

      if (!params.agent || !params.task) {
        return {
          content: [{ type: "text" as const, text: `Provide one mode. Available agents: ${formatAgentList(agents)}` }],
          details: makeDetails([]),
        };
      }

      const agent = agents.find((candidate) => candidate.name === params.agent);
      if (!agent) {
        return {
          content: [{ type: "text" as const, text: `Unknown agent "${params.agent}". Available agents: ${formatAgentList(agents)}.` }],
          details: makeDetails([]),
          isError: true,
        };
      }
      const cwd = params.cwd ? path.resolve(baseCwd, params.cwd) : baseCwd;
      const task = params.task;

      if (watched) {
        try {
          const view = await jobs.launch({
            agent: agent.name,
            task,
            cwd,
            run: ({ id, signal: jobSignal, onWindow, onProgress }) =>
              startChild(agent, task, cwd, {
                title: `pi subagent: ${agent.name} #${id}`,
                signal: jobSignal,
                onWindow,
                onProgress,
              }),
          });
          return {
            content: [{ type: "text" as const, text: launchText(view) }],
            details: makeDetails([], [view]),
          };
        } catch (error) {
          const reason = error instanceof Error ? error.message : String(error);
          return {
            content: [{ type: "text" as const, text: `Could not start the subagent: ${reason}` }],
            details: makeDetails([]),
            isError: true,
          };
        }
      }

      const result = await runUnattended(params.agent, params.task, params.cwd, undefined, onUpdate);
      if (isFailed(result)) {
        return {
          content: [{ type: "text" as const, text: `Agent ${result.stopReason || "failed"}: ${resultOutput(result)}` }],
          details: makeDetails([result]),
          isError: true,
        };
      }
      return {
        content: [{ type: "text" as const, text: capText(finalOutput(result)) || "(no output)" }],
        details: makeDetails([result]),
      };
    },

    renderCall(args, theme, _context) {
      const scope: AgentScope = args.agentScope ?? "user";
      const preview = (text: string | undefined, max: number): string =>
        !text ? "..." : text.length > max ? `${text.slice(0, max)}...` : text;

      if (args.chain?.length) {
        let text =
          theme.fg("toolTitle", theme.bold("subagent ")) +
          theme.fg("accent", `chain (${args.chain.length} steps)`) +
          theme.fg("muted", ` [${scope}]`);
        for (const step of args.chain.slice(0, 3)) {
          const task = preview(step.task.replace(/\{previous\}/g, "").trim(), 40);
          text += `\n  ${theme.fg("accent", step.agent)}${theme.fg("dim", ` ${task}`)}`;
        }
        return new Text(text, 0, 0);
      }
      if (args.tasks?.length) {
        let text =
          theme.fg("toolTitle", theme.bold("subagent ")) +
          theme.fg("accent", `parallel (${args.tasks.length} tasks)`) +
          theme.fg("muted", ` [${scope}]`);
        for (const task of args.tasks.slice(0, 3)) {
          text += `\n  ${theme.fg("accent", task.agent)}${theme.fg("dim", ` ${preview(task.task, 40)}`)}`;
        }
        return new Text(text, 0, 0);
      }
      const kind = args.interactive === false ? "unattended" : "watched";
      const text =
        theme.fg("toolTitle", theme.bold("subagent ")) +
        theme.fg("accent", args.agent ?? "...") +
        theme.fg("muted", ` [${scope}]`) +
        theme.fg("muted", ` ${kind}`) +
        `\n  ${theme.fg("dim", preview(args.task, 60))}`;
      return new Text(text, 0, 0);
    },

    renderResult(result, { expanded }, theme, _context) {
      const details = result.details as SubagentDetails | undefined;
      if (!details || (details.results.length === 0 && !details.jobs?.length)) {
        const first = result.content[0];
        return new Text(first?.type === "text" ? first.text : "(no output)", 0, 0);
      }

      const icon = (entry: SingleResult): string =>
        entry.exitCode === -1 ? theme.fg("muted", "⏳") : isFailed(entry) ? theme.fg("error", "✗") : theme.fg("success", "✓");
      const label = (entry: SingleResult): string => {
        const parts = [icon(entry), theme.fg("toolTitle", theme.bold(entry.agent))];
        if (entry.windowId) parts.push(theme.fg("muted", `window ${entry.windowId}`));
        const usage = formatUsage(entry.usage, entry.model);
        if (usage) parts.push(theme.fg("dim", usage));
        return parts.join(" ");
      };

      if (details.jobs?.length) {
        const lines = details.jobs.map((view) => {
          const where = view.windowId ? ` ${theme.fg("muted", `window ${view.windowId}`)}` : "";
          return `⏳ ${theme.fg("toolTitle", theme.bold(view.agent))} ${theme.fg("muted", `#${view.id}`)}${where}`;
        });
        lines.push(theme.fg("dim", "the result arrives as a message when the run ends"));
        return new Text(lines.join("\n"), 0, 0);
      }

      if (!expanded) {
        if (details.results.length === 1) {
          const output = resultOutput(details.results[0]).split("\n").slice(0, 3).join("\n");
          return new Text(`${label(details.results[0])}\n  ${theme.fg("toolOutput", output.slice(0, 300))}`, 0, 0);
        }
        const ok = details.results.filter((entry) => !isFailed(entry)).length;
        return new Text(
          `${details.mode} ${details.results.length} · ${ok} ok\n` + details.results.map((entry) => `  ${label(entry)}`).join("\n"),
          0,
          0,
        );
      }

      const markdown = getMarkdownTheme();
      const container = new Container();
      for (const entry of details.results) {
        container.addChild(new Text(label(entry), 0, 0));
        if (entry.step) container.addChild(new Text(theme.fg("muted", `step ${entry.step}: `) + theme.fg("dim", entry.task), 0, 0));
        for (const message of entry.messages) {
          if (message.role !== "assistant") continue;
          for (const part of message.content) {
            if (part.type === "toolCall" && part.name) {
              container.addChild(new Text(theme.fg("muted", "→ ") + formatToolCall(part.name, part.arguments ?? {}, theme), 0, 0));
            }
          }
        }
        const output = finalOutput(entry).trim();
        if (output) {
          container.addChild(new Spacer(1));
          container.addChild(new Markdown(output, 0, 0, markdown));
        }
        container.addChild(new Spacer(1));
      }

      let total = emptyUsage();
      for (const entry of details.results) total = addUsage(total, entry.usage);
      const usage = formatUsage(total);
      if (usage) container.addChild(new Text(theme.fg("dim", `Total: ${usage}`), 0, 0));
      return container;
    },
  });
}

function formatToolCall(name: string, args: Record<string, unknown>, theme: Theme): string {
  const text = (value: unknown): string => (typeof value === "string" ? value : "");
  const file = text(args.path) || text(args.file_path) || text(args.filePath);
  const call = (detail: string): string => `${theme.fg("accent", name)} ${theme.fg("dim", detail)}`;
  switch (name) {
    case "bash":
      return call(text(args.command));
    case "read":
    case "write":
    case "edit":
      return call(file);
    case "grep":
    case "find":
    case "ls":
    case "glob":
      return call(text(args.pattern) || text(args.path) || file);
    default:
      return theme.fg("accent", name);
  }
}
