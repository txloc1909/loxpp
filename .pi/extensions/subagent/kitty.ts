/**
 * Kitty plumbing for the subagent extension.
 *
 * Pure argv and string helpers only: no I/O and no third-party imports, so the
 * unit tests load this module with plain `node --test`.
 */

import { parentPidLines, shellQuote, stopWatcherLines, watchdogFunctions, watchdogLines } from "../lib/watchdog.ts";

export interface KittySession {
  /** The kitty window that runs pi. Subagent windows open beside it. */
  windowId: string;
  /**
   * Address of kitty's remote-control socket, from KITTY_LISTEN_ON.
   *
   * Without it, `kitten @` sends its request to the controlling terminal and
   * reads the reply from that same terminal, which pi reads for keystrokes.
   * The reply then arrives as input: a large listing lands in the prompt. The
   * extension refuses to run in that mode instead of racing.
   */
  listenOn: string | null;
}

/**
 * Kitty sets KITTY_WINDOW_ID for every process it starts, so its presence is
 * how the extension knows it may open windows at all. KITTY_LISTEN_ON is
 * present only when kitty listens on a socket, which needs a `listen_on` line.
 */
export function detectKitty(env: Record<string, string | undefined> = process.env): KittySession | null {
  const raw = env.KITTY_WINDOW_ID?.trim();
  if (!raw || !/^\d+$/.test(raw)) return null;
  const listenOn = env.KITTY_LISTEN_ON?.trim();
  return { windowId: raw, listenOn: listenOn ? listenOn : null };
}

/**
 * The socket address, named on every call. `kitten @` falls back to the
 * controlling terminal when it cannot find a socket, and that fallback writes
 * into the terminal pi reads its keyboard input from. Both entry points need
 * the address and neither may proceed without it.
 */
export function requireListenOn(session: KittySession): string {
  if (!session.listenOn) {
    throw new Error("kitty remote control needs a socket; add a listen_on line to kitty.conf");
  }
  return session.listenOn;
}

function targetArg(to: string): string {
  return `--to=${to}`;
}

export interface LaunchSpec {
  session: KittySession;
  cwd: string;
  title: string;
  command: string[];
  hold?: boolean;
  keepFocus?: boolean;
  env?: Record<string, string>;
}

/**
 * A window in the tab that holds the parent window, in the given directory.
 * This is the `--type=window` half of kitty's `new_window_with_cwd` action: a
 * split beside pi, never a new tab or a new OS window. Matching on the parent
 * window keeps the placement independent of which kitty window has focus.
 * The action itself can only open a shell, so it cannot run the child process
 * whose exit closes the window.
 *
 * Without `--hold`, kitty closes the window when the command exits.
 */
export function launchArgv(spec: LaunchSpec, kitten = "kitten"): string[] {
  const to = requireListenOn(spec.session);
  const argv = [
    kitten,
    "@",
    targetArg(to),
    "launch",
    `--match=window_id:${spec.session.windowId}`,
    "--type=window",
    `--cwd=${spec.cwd}`,
    `--title=${spec.title}`,
  ];
  if (spec.keepFocus) argv.push("--dont-take-focus");
  if (spec.hold) argv.push("--hold");
  const env = spec.env ?? {};
  for (const name of Object.keys(env).sort()) argv.push(`--env=${name}=${env[name]}`);
  argv.push(...spec.command);
  return argv;
}

export function closeWindowArgv(windowId: string, to: string, kitten = "kitten"): string[] {
  return [kitten, "@", targetArg(to), "close-window", `--match=id:${windowId}`];
}

export function setTitleArgv(windowId: string, title: string, to: string, kitten = "kitten"): string[] {
  return [kitten, "@", targetArg(to), "set-window-title", `--match=id:${windowId}`, title];
}

export function listWindowsArgv(to: string, kitten = "kitten"): string[] {
  return [kitten, "@", targetArg(to), "ls"];
}

/**
 * Window ids of a `kitten @ ls` listing. Only window entries count: an OS
 * window carries its own id in the same range, so a plain recursive search for
 * `id` would report a live window for an id that belongs to an OS window.
 */
export function windowIds(listing: string): string[] {
  let parsed: unknown;
  try {
    parsed = JSON.parse(listing);
  } catch {
    return [];
  }
  const ids: string[] = [];
  for (const osWindow of asArray(parsed)) {
    for (const tab of asArray(asRecord(osWindow).tabs)) {
      for (const win of asArray(asRecord(tab).windows)) {
        const id = asRecord(win).id;
        if (typeof id === "number" || typeof id === "string") ids.push(String(id));
      }
    }
  }
  return ids;
}

export interface RunScriptSpec {
  /** Lines printed in the new window before the child starts. */
  header: string[];
  cwd: string;
  command: string[];
  stderrPath: string;
  exitPath: string;
  /** What the watchdog calls to close this window when the parent is gone. */
  kitten: string;
  socket: string;
  watchPollS?: number;
  watchGraceS?: number;
}

/**
 * The script kitty runs in the new window. The child owns the terminal: stdin
 * and stdout stay attached, because its stdout is the TUI and its keyboard is
 * the window. Only stderr goes to a log. The exit code goes to a second file:
 * it is the parent's only proof that the run ended and how.
 *
 * The child runs in the background so a watcher can end it when pi dies: a
 * `SIGKILL` of pi runs no handler there. POSIX gives an asynchronous list
 * `/dev/null` on stdin when job control is off, so the terminal is kept on a
 * third descriptor first and handed to the child explicitly.
 *
 * Every path is absolute, so the script does not depend on the window's cwd.
 */
export function buildRunScript(spec: RunScriptSpec): string {
  const quote = shellQuote;
  const cdFailure = `loxpp subagent: cannot enter ${spec.cwd}`;
  return [
    "#!/bin/sh",
    "# Written by the Lox++ pi subagent extension; removed when the run ends.",
    "set -u",
    ...parentPidLines(),
    `printf '%s\\n' ${spec.header.map(quote).join(" ")} ''`,
    `cd ${quote(spec.cwd)} || { printf '%s\\n' ${quote(cdFailure)}; printf '%s\\n' 1 > ${quote(spec.exitPath)}; exit 1; }`,
    ...watchdogFunctions(),
    "exec 3<&0",
    `${spec.command.map(quote).join(" ")} 0<&3 2> ${quote(spec.stderrPath)} &`,
    "child=$!",
    ...watchdogLines({
      pollS: spec.watchPollS,
      graceS: spec.watchGraceS,
      // kitty runs this script under `kitten run-shell`, so it does not lead a
      // group of its own: the close call below is what ends the window.
      reach: "children",
      closeWindow: { kitten: spec.kitten, socket: spec.socket },
    }),
    'wait "$child"',
    "status=$?",
    ...stopWatcherLines(),
    `printf '%s\\n' "$status" > ${quote(spec.exitPath)}`,
    "",
  ].join("\n");
}

function asRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === "object" && !Array.isArray(value) ? (value as Record<string, unknown>) : {};
}

function asArray(value: unknown): unknown[] {
  return Array.isArray(value) ? value : [];
}
