/**
 * How the subagent's pi process is started.
 *
 * Node builtins only (the `agents.ts` import is type-only), so the tests load
 * this module directly.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import type { AgentConfig } from "./agents.ts";

export interface Dispatch {
  model?: string;
  thinking?: string;
}

export interface Invocation {
  command: string;
  args: string[];
}

export interface ChildMode {
  /**
   * An interactive child owns the terminal in its window, so a person can chat
   * with it and direct the work. It ends when that person quits it. Without
   * this flag the child runs `--mode json -p` and ends by itself.
   */
  interactive: boolean;
  /**
   * The extension that mirrors the child's session events to `EVENTS_ENV`. An
   * interactive child needs it: its stdout carries the TUI, so the parent has
   * no event stream to read there.
   */
  mirrorPath?: string;
}

/**
 * The test hook: a path to a stand-in executable, so the window plumbing can
 * run end to end without a model call.
 */
export const PI_OVERRIDE_ENV = "LOXPP_SUBAGENT_PI";

/**
 * The event file, set by `window.ts` on launch. The mirror extension writes
 * there; the parent tails it. A child that does not load the mirror ignores it.
 */
export const EVENTS_ENV = "LOXPP_SUBAGENT_EVENTS";

/**
 * Resolve how the child starts pi. A real script path or a compiled binary
 * keeps the child on the same build as the parent; a plain node/bun runtime
 * means pi is an npm-style install found on PATH.
 */
export function piInvocation(env: NodeJS.ProcessEnv = process.env, argv: string[] = process.argv): Invocation {
  const override = env[PI_OVERRIDE_ENV];
  if (override) return { command: override, args: [] };

  const script = argv[1];
  const virtualScript = script?.startsWith("/$bunfs/root/");
  if (script && !virtualScript && fs.existsSync(script)) return { command: process.execPath, args: [script] };

  const execName = path.basename(process.execPath).toLowerCase();
  if (!/^(node|bun)(\.exe)?$/.test(execName)) return { command: process.execPath, args: [] };
  return { command: "pi", args: [] };
}

/**
 * kitty spawns the child from its own environment, not from pi's, so PATH and
 * pi's own configuration variables travel with the launch. Everything else is
 * left out: the child gets an isolated context by design.
 */
export function childEnv(env: NodeJS.ProcessEnv = process.env): Record<string, string> {
  const result: Record<string, string> = {};
  for (const [name, value] of Object.entries(env)) {
    if (!value) continue;
    if (name === "PATH" || name.startsWith("PI_")) result[name] = value;
  }
  return result;
}

export function childArgs(
  agent: AgentConfig,
  task: string,
  promptPath: string | undefined,
  dispatch: Dispatch,
  mode: ChildMode = { interactive: false },
): string[] {
  // An interactive child is a normal pi session: a person reads it, types into
  // it, and resumes it later, so it saves its session and carries a name. The
  // print-mode child reports through stdout and keeps no session.
  const args = mode.interactive ? ["--name", `subagent: ${agent.name}`] : ["--mode", "json", "-p", "--no-session"];
  if (mode.interactive && mode.mirrorPath) args.push("--extension", mode.mirrorPath);
  const model = agent.model ?? dispatch.model;
  if (model) args.push("--model", model);
  // A thinking level belongs to the model it was set on, so it follows only
  // when the model itself is inherited.
  if (!agent.model && dispatch.thinking) args.push("--thinking", dispatch.thinking);
  if (agent.tools?.length) args.push("--tools", agent.tools.join(","));
  if (promptPath) args.push("--append-system-prompt", promptPath);
  args.push(`Task: ${task}`);
  return args;
}
