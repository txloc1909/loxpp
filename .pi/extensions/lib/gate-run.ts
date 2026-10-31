/**
 * One gate run, in its container, watched by a shell script.
 *
 * `podman run` is a client: the container lives on its own. A `SIGKILL` of pi
 * runs no handler in pi, so nothing ends the client, and the container keeps
 * building and testing with nobody to read the result. `--rm` does not help
 * either: the client is what removes the container, and that client is gone.
 *
 * Measured on podman 6.1.4 (rootless) while planning this change:
 *
 * - `SIGKILL` of the client leaves the container `Up`, and `--rm` never
 *   removes it;
 * - `SIGTERM` to the client neither stopped the container nor ended the client;
 * - `podman rm -f --time 0 <name>` ended and removed a running container.
 *
 * So the wrapper removes the container itself. It knows the name because it
 * passes one, and every call's name is unique, so a leftover of that exact name
 * belongs to a dead run and removing it cannot touch a live one. `podman run`
 * refuses a name that is already taken, so the removal also unblocks this run.
 *
 * Pure argv and string helpers only, so the tests load this module directly.
 */

import { parentPidLines, shellQuote, stopWatcherLines, watchdogFunctions, watchdogLines } from "./watchdog.ts";

/** The container name prefix. The rest is pi's pid and a value made unique. */
export const GATE_NAME_PREFIX = "loxpp-gate";

/**
 * A name that is unique to one call and visible in `podman ps`: pi's pid plus a
 * value the caller makes unique.
 *
 * Not a module-level counter: pi can load an extension again, which resets the
 * module and restarts the counter, and two live runs then share a name. The
 * stale-container removal would then remove the other run's container.
 */
export function gateContainerName(pid: number, unique: string): string {
  return `${GATE_NAME_PREFIX}-${pid}-${unique}`;
}

/** The line a failed gate adds, so a stuck container is one command away. */
export function gateFailureNote(name: string): string {
  return `container: ${name} (remove a stuck run with: podman rm -f --time 0 ${name})`;
}

export interface GateArgvSpec {
  image: string;
  /** The absolute checkout: podman makes a relative host path a named volume. */
  cwd: string;
  name: string;
  script: string;
}

/**
 * The container that runs one gate. `--rm` covers the normal end; the wrapper
 * removes the container when the normal end cannot happen.
 */
export function gateRunArgv(spec: GateArgvSpec): string[] {
  return [
    "podman",
    "run",
    "--rm",
    `--name=${spec.name}`,
    "-v",
    `${spec.cwd}:/workspace:z`,
    "-v",
    "loxpp-ccache:/ccache",
    spec.image,
    "bash",
    "-lc",
    spec.script,
  ];
}

export interface GateScriptSpec {
  /** The container command, from `gateRunArgv`. */
  argv: string[];
  /** The container name in that argv, so the wrapper can remove it. */
  name: string;
  /** Seconds between liveness polls, and between the TERM and the KILL. */
  pollS?: number;
  graceS?: number;
}

/**
 * The script that starts the client and watches pi.
 *
 * The client keeps this script's stdout and stderr, so the caller still reads
 * the whole gate output, and the script exits with the client's code, so the
 * caller still reads pass or fail. On an abort the shell forwards the signal
 * and removes the container, because a signalled client does neither.
 */
export function buildGateScript(spec: GateScriptSpec): string {
  const remove = `command ${shellQuote(spec.argv[0])} rm -f --time 0 ${shellQuote(spec.name)} >/dev/null 2>&1`;
  return [
    "#!/bin/sh",
    "# Written by the Lox++ pi modes extension: one gate run in its container.",
    "set -u",
    ...parentPidLines(),
    ...watchdogFunctions(),
    // A name is unique per call, so a container that already holds this name
    // belongs to a dead run, and `podman run` would refuse the name. It goes
    // first.
    remove,
    `${spec.argv.map(shellQuote).join(" ")} </dev/null &`,
    "child=$!",
    ...watchdogLines({
      pollS: spec.pollS,
      graceS: spec.graceS,
      // The client is this script's only child. A group signal here would reach
      // pi's own group, so the child is the right scope.
      reach: "children",
      // The removal is what ends the container; the signals alone do not.
      afterSignal: [remove],
      afterGrace: [remove],
    }),
    "",
    "# An abort must end the run too, so it does what the watcher does: forward",
    "# the signal, then remove the container, because a signalled client does",
    "# not stop its container.",
    "abort() {",
    '  command kill -TERM "$child" 2>/dev/null',
    `  ${remove}`,
    // A removal can fail, for example when podman cannot answer. The client
    // must still end, or the tool call waits forever.
    `  sleep ${spec.graceS ?? 2}`,
    // The same race as in the watcher: `podman run` can create the container
    // after the first removal landed, so a second removal follows the grace.
    `  ${remove}`,
    '  pid_gone "$child" || command kill -KILL "$child" 2>/dev/null',
    "}",
    "trap 'abort' TERM INT HUP",
    // `wait` returns on the trap, so it repeats until the client is gone.
    "while :; do",
    '  wait "$child"',
    "  status=$?",
    '  pid_gone "$child" && break',
    "done",
    ...stopWatcherLines(),
    'exit "$status"',
    "",
  ].join("\n");
}
