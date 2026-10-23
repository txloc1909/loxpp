/**
 * Subagent tool: delegate a task to a separate `pi` process that runs in a new
 * window in pi's kitty tab.
 *
 * The child owns its terminal, so the event stream, the stderr log and the
 * exit code travel through files that `window.ts` tails. When the child exits,
 * kitty closes the window.
 *
 * The extension registers nothing unless pi runs inside a kitty window.
 *
 * Modes: single ({ agent, task }), parallel ({ tasks: [...] }), and chain
 * ({ chain: [...] }, with a `{previous}` placeholder in later tasks).
 */

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import type { AgentToolResult } from "@earendil-works/pi-agent-core";
import { StringEnum } from "@earendil-works/pi-ai";
import { CONFIG_DIR_NAME, type ExtensionAPI, getAgentDir, getMarkdownTheme, type Theme } from "@earendil-works/pi-coding-agent";
import { Container, Markdown, Spacer, Text } from "@earendil-works/pi-tui";
import { Type } from "typebox";
import { type AgentConfig, type AgentScope, discoverAgents, formatAgentList } from "./agents.ts";
import { childArgs, childEnv, type Dispatch, piInvocation } from "./child.ts";
import { detectKitty, type KittySession } from "./kitty.ts";
import { addUsage, finalOutput, formatUsage, type StreamState, type UsageStats } from "./stream.ts";
import { checkRemoteControl, describeControlProblem, runInKittyWindow } from "./window.ts";

const MAX_PARALLEL_TASKS = 8;
const MAX_CONCURRENCY = 4;
const PER_TASK_OUTPUT_CAP = 50 * 1024;

interface SingleResult {
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
  step?: number;
}

interface SubagentDetails {
  mode: "single" | "parallel" | "chain";
  agentScope: AgentScope;
  projectAgentsDir: string | null;
  results: SingleResult[];
}

type OnUpdate = (partial: AgentToolResult<SubagentDetails>) => void;

function emptyUsage(): UsageStats {
  return { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, cost: 0, contextTokens: 0, turns: 0 };
}

async function runWindowAgent(opts: {
  session: KittySession;
  dispatch: Dispatch;
  agent: AgentConfig;
  task: string;
  cwd: string;
  hold: boolean;
  keepFocus: boolean;
  step?: number;
  signal: AbortSignal | undefined;
  onUpdate: OnUpdate | undefined;
  makeDetails: (results: SingleResult[]) => SubagentDetails;
}): Promise<SingleResult> {
  const result: SingleResult = {
    agent: opts.agent.name,
    agentSource: opts.agent.source,
    task: opts.task,
    exitCode: -1,
    messages: [],
    stderr: "",
    usage: emptyUsage(),
    step: opts.step,
  };

  const emit = (state: StreamState): void => {
    result.messages = state.messages;
    result.usage = state.usage;
    if (state.model) result.model = state.model;
    opts.onUpdate?.({
      content: [{ type: "text", text: finalOutput(state) || "(running...)" }],
      details: opts.makeDetails([result]),
    });
  };

  const promptDir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-prompt-"));
  try {
    let promptPath: string | undefined;
    if (opts.agent.systemPrompt.trim()) {
      promptPath = path.join(promptDir, `prompt-${opts.agent.name.replace(/[^\w.-]+/g, "_")}.md`);
      await fs.promises.writeFile(promptPath, opts.agent.systemPrompt, { encoding: "utf-8", mode: 0o600 });
    }

    const pi = piInvocation();
    const command = [pi.command, ...pi.args, ...childArgs(opts.agent, opts.task, promptPath, opts.dispatch)];

    const run = await runInKittyWindow({
      session: opts.session,
      cwd: opts.cwd,
      title: `pi subagent: ${opts.agent.name}`,
      header: [
        `pi subagent: ${opts.agent.name}`,
        opts.agent.description,
        `cwd: ${opts.cwd}`,
        "this window closes when the subagent exits",
      ],
      command,
      env: childEnv(),
      hold: opts.hold,
      keepFocus: opts.keepFocus,
      signal: opts.signal,
      onProgress: emit,
    });

    result.windowId = run.windowId;
    result.exitCode = run.exitCode;
    result.messages = run.state.messages;
    result.usage = run.state.usage;
    result.stderr = run.stderr;
    result.model = run.state.model ?? result.model;
    result.stopReason = run.state.stopReason;
    result.errorMessage = run.state.errorMessage;
    result.closedByUser = run.closedByUser;
    if (run.closedByUser) result.errorMessage = "the subagent window was closed before the run finished";
    return result;
  } finally {
    await fs.promises.rm(promptDir, { recursive: true, force: true });
  }
}

function isFailed(result: SingleResult): boolean {
  return result.exitCode !== 0 || result.stopReason === "error" || result.stopReason === "aborted";
}

function finalOutputFrom(result: SingleResult): string {
  return finalOutput({ messages: result.messages, usage: result.usage });
}

function resultOutput(result: SingleResult): string {
  if (isFailed(result)) return result.errorMessage || result.stderr || finalOutputFrom(result) || "(no output)";
  return finalOutputFrom(result) || "(no output)";
}

function capOutput(output: string): string {
  const bytes = Buffer.byteLength(output, "utf8");
  if (bytes <= PER_TASK_OUTPUT_CAP) return output;
  let truncated = output.slice(0, PER_TASK_OUTPUT_CAP);
  while (Buffer.byteLength(truncated, "utf8") > PER_TASK_OUTPUT_CAP) truncated = truncated.slice(0, -1);
  const omitted = bytes - Buffer.byteLength(truncated, "utf8");
  return `${truncated}\n\n[Output truncated: ${omitted} bytes omitted. Full output preserved in tool details.]`;
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
  cwd: Type.Optional(Type.String({ description: "Working directory for the subagent window" })),
});

const SubagentParams = Type.Object({
  agent: Type.Optional(Type.String({ description: "Agent name (single mode)" })),
  task: Type.Optional(Type.String({ description: "Task to delegate (single mode)" })),
  tasks: Type.Optional(Type.Array(TaskItem, { description: "Parallel tasks" })),
  chain: Type.Optional(Type.Array(TaskItem, { description: "Sequential tasks; later tasks may use {previous}" })),
  agentScope: Type.Optional(
    StringEnum(["user", "project", "both"] as const, {
      description: `Which agent directories to use. Default "user" (${path.join(getAgentDir(), "agents")}). Use "both" to include project-local ${CONFIG_DIR_NAME}/agents.`,
      default: "user",
    }),
  ),
  confirmProjectAgents: Type.Optional(
    Type.Boolean({ description: "Ask before running repo-controlled project agents in an untrusted project", default: true }),
  ),
  cwd: Type.Optional(Type.String({ description: "Working directory for the subagent window. Default the session cwd" })),
  hold: Type.Optional(
    Type.Boolean({ description: "Keep the window open at a shell after the subagent exits. Default false: it closes", default: false }),
  ),
  keepFocus: Type.Optional(Type.Boolean({ description: "Leave focus in pi's window. Default false", default: false })),
});

export default function kittySubagent(pi: ExtensionAPI): void {
  const session = detectKitty();
  // The extension exists to open kitty windows. Outside kitty it registers no
  // tool at all, rather than one that can only fail.
  if (!session) return;

  pi.on("session_start", async (_event, ctx) => {
    const problem = describeControlProblem(await checkRemoteControl(), session);
    if (problem && ctx.hasUI) ctx.ui.notify(problem, "error");
  });

  pi.registerTool({
    name: "subagent",
    label: "Subagent",
    description: [
      "Delegate a task to a subagent: a separate pi process with its own context, running in a new kitty window.",
      "The window closes when the subagent exits, and its progress streams back into this session.",
      "Modes: single (agent + task), parallel (tasks array, max 8), chain (sequential, {previous} placeholder).",
      'Default agent scope is "user"; set agentScope: "both" to allow project-local agents.',
    ].join(" "),
    promptSnippet: "delegate a task to a subagent in a new kitty window",
    parameters: SubagentParams,

    async execute(_toolCallId, params, signal, onUpdate, ctx) {
      const problem = describeControlProblem(await checkRemoteControl(), session);
      if (problem) return { content: [{ type: "text" as const, text: problem }], details: undefined, isError: true };

      const agentScope: AgentScope = params.agentScope ?? "user";
      const hold = params.hold ?? false;
      const keepFocus = params.keepFocus ?? false;
      const discovery = discoverAgents(ctx.cwd, agentScope);
      const agents = discovery.agents;
      const baseCwd = params.cwd ? path.resolve(ctx.cwd, params.cwd) : ctx.cwd;

      const mode = params.chain?.length ? "chain" : params.tasks?.length ? "parallel" : "single";
      const makeDetails = (results: SingleResult[]): SubagentDetails => ({
        mode,
        agentScope,
        projectAgentsDir: discovery.projectAgentsDir,
        results,
      });

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

      const run = (
        agentName: string,
        task: string,
        cwd: string | undefined,
        step: number | undefined,
        update: OnUpdate | undefined,
      ): Promise<SingleResult> => {
        const agent = agents.find((candidate) => candidate.name === agentName);
        if (!agent) {
          return Promise.resolve({
            agent: agentName,
            agentSource: "unknown",
            task,
            exitCode: 1,
            messages: [],
            stderr: `Unknown agent "${agentName}". Available agents: ${formatAgentList(agents)}.`,
            usage: emptyUsage(),
            step,
          });
        }
        return runWindowAgent({
          session,
          dispatch,
          agent,
          task,
          cwd: cwd ? path.resolve(baseCwd, cwd) : baseCwd,
          hold,
          keepFocus,
          step,
          signal,
          onUpdate: update,
          makeDetails,
        });
      };

      if (mode === "chain") {
        const chain = params.chain!;
        const results: SingleResult[] = [];
        let previous = "";
        for (let i = 0; i < chain.length; i++) {
          const step = chain[i];
          const result = await run(
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
          previous = finalOutputFrom(result);
        }
        return {
          content: [{ type: "text" as const, text: capOutput(finalOutputFrom(results[results.length - 1])) || "(no output)" }],
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
          slots[index] = await run(
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
          return `### [${slot.agent}] ${status}\n\n${capOutput(resultOutput(slot))}`;
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

      const result = await run(params.agent, params.task, params.cwd, undefined, onUpdate);
      if (isFailed(result)) {
        return {
          content: [{ type: "text" as const, text: `Agent ${result.stopReason || "failed"}: ${resultOutput(result)}` }],
          details: makeDetails([result]),
          isError: true,
        };
      }
      return {
        content: [{ type: "text" as const, text: capOutput(finalOutputFrom(result)) || "(no output)" }],
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
      const text =
        theme.fg("toolTitle", theme.bold("subagent ")) +
        theme.fg("accent", args.agent ?? "...") +
        theme.fg("muted", ` [${scope}]`) +
        `\n  ${theme.fg("dim", preview(args.task, 60))}`;
      return new Text(text, 0, 0);
    },

    renderResult(result, { expanded }, theme, _context) {
      const details = result.details as SubagentDetails | undefined;
      if (!details || details.results.length === 0) {
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
        const output = finalOutput({ messages: entry.messages, usage: entry.usage }).trim();
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
