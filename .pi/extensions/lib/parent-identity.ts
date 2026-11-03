/**
 * The identity a run wrapper compares against the parent pid, so a pid that the
 * kernel handed to a different process does not read as the parent.
 *
 * A pid's state says nothing about which process holds the number: after the
 * parent dies, the allocator gives the number out again, and the new owner's
 * `S` state is what a state-only check reads as "the parent is alive". The start
 * time is what tells the two apart, and the wrapper receives the parent's as
 * its second argument, recorded while the parent was certainly alive.
 *
 * `ps` answers this on every platform kitty runs on. `LC_ALL=C` is set on both
 * sides of the comparison, here and in the wrapper: the answer carries localized
 * month and day names, and the wrapper's environment is not pi's. `TZ=UTC` is
 * set on both sides for the same reason: the answer is in local time, and kitty
 * forwards a short list of environment keys, so a `TZ` that reaches pi need not
 * reach the window's script. Without it, a run whose two sides read different
 * zones would read a live parent as dead. The whitespace is removed here because
 * `ps` pads the field, and the wrapper's `tr` removes it there, so the two tokens
 * are compared in one shape.
 *
 * The limit: the answer has one-second resolution, so a pid reused inside the
 * same second still reads as the parent. A pid is reused only after the whole
 * number space turns over (about 45 minutes on a host that moves 7 640 pids in
 * 5 s), so a real reuse always lands in a later second.
 */

import { spawnSync } from "node:child_process";

/** The longest one `ps` call may take before its answer counts as unreadable. */
export const START_TIMEOUT_MS = 2000;

/**
 * The start time of one process, or an empty string when `ps` cannot read it.
 * An empty answer is not evidence of anything, so callers keep the state-only
 * check for it.
 */
export function startTimeOf(pid: number): string {
  const r = spawnSync("ps", ["-o", "lstart=", "-p", String(pid)], {
    encoding: "utf8",
    env: { ...process.env, LC_ALL: "C", TZ: "UTC" },
    // A `ps` that hangs must not hold the run that is about to start.
    timeout: START_TIMEOUT_MS,
  });
  if (r.error !== undefined || r.status !== 0) return "";
  return r.stdout.replace(/\s+/g, "");
}

let own: string | undefined;

/**
 * pi's own start time, which the wrapper watches. Cached because a live
 * process's start time cannot change. An empty answer is not cached: a stale
 * empty token would turn the identity check off for the rest of this pi.
 */
export function parentStartTime(): string {
  if (own === undefined) own = startTimeOf(process.pid) || undefined;
  return own ?? "";
}
