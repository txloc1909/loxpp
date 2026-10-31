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
 * passes one, and it removes a leftover of that name before it starts, so a
 * container from an earlier crash cannot refuse this run.
 *
 * Pure argv and string helpers only, so the tests load this module directly.
 */

import { parentPidLines, shellQuote, stopWatcherLines, watchdogFunctions, watchdogLines } from "./watchdog.ts";

/** The container name prefix. The rest is pi's pid and a counter. */
export const GATE_NAME_PREFIX = "loxpp-gate";

/**
 * A name that is unique to one call and visible in `podman ps`: pi's pid plus a
 * counter, so two runs from one session cannot pick the same name.
 */
export function gateContainerName(pid: number, index: number): string {
  return `${GATE_NAME_PREFIX}-${pid}-${index}`;
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
    // An earlier run can leave a container with this name behind, because the
    // pid and the counter belong to a dead session. A leftover name refuses a
    // new run, so it goes first.
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
    }),
    "",
    "# An abort must end the run too, so it does what the watcher does: forward",
    "# the signal, then remove the container, because a signalled client does",
    "# not stop its container.",
    "abort() {",
    '  command kill -TERM "$child" 2>/dev/null',
    `  ${remove}`,
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
