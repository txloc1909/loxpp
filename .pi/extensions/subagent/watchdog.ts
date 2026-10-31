/**
 * The shell text that ends a run when the parent pi process disappears.
 *
 * A `SIGKILL` of pi runs no handler there, so the parent cannot close a run it
 * started: the child keeps its window, or its process group, and its result has
 * nowhere to arrive. The script that starts the child watches the parent
 * instead. It polls the parent's pid and signals the run when the pid is gone.
 *
 * Both run kinds share this text: the script kitty runs in a watched window,
 * and the wrapper `print.ts` spawns. The script receives the parent pid as its
 * first argument, because kitty starts it from its own environment.
 *
 * Pure string helpers only, so the tests load this module directly.
 */

/** Seconds between liveness polls, and between the TERM and the KILL. */
export const WATCH_POLL_S = 1;
export const WATCH_GRACE_S = 2;

/**
 * A test asserts that every script builder writes this marker, so a builder
 * cannot appear without the watchdog.
 */
export const WATCHDOG_MARKER = "# loxpp subagent watchdog";

/** One POSIX single-quoted word. A literal quote closes, escapes, and reopens. */
export function shellQuote(value: string): string {
  return `'${value.split("'").join("'\\''")}'`;
}

/** The first argument is the parent pid. Without it nothing can watch. */
export function parentPidLines(): string[] {
  return [
    "parent_pid=${1:-}",
    'if [ -z "$parent_pid" ]; then',
    "  printf '%s\\n' 'loxpp subagent: the run script needs the parent process id' >&2",
    "  exit 1",
    "fi",
  ];
}

/**
 * `kill -0` also succeeds for a zombie that nobody has reaped yet, so the
 * process state decides. `ps` answers on every platform kitty runs on; reading
 * `/proc` would leave the zombie to the fallback and keep the run alive forever.
 *
 * An empty answer means one of two things: the pid is gone, or `ps` cannot
 * answer at all. The two are told apart by asking `ps` about this script's own
 * pid, which certainly exists. Without that check a `ps` that fails makes every
 * run look orphaned, and the watcher then kills a live run and its work. The
 * safe direction is to kill only on evidence of death: when `ps` cannot answer,
 * the run is left alone, which is the behaviour before this watch existed.
 */
export function watchdogFunctions(): string[] {
  return [
    "parent_alive() {",
    `  state=$(ps -o stat= -p "$parent_pid" 2>/dev/null | tr -d '[:space:]')`,
    "  if [ -z \"$state\" ]; then",
    `    [ -n "$(ps -o stat= -p "$$" 2>/dev/null | tr -d '[:space:]')" ] || return 0`,
    "    return 1",
    "  fi",
    "  case $state in",
    "    Z*) return 1 ;;",
    "  esac",
    "  return 0",
    "}",
    "",
    "# A group signal must reach the commands the child started, and it must not",
    "# reach a caller that shares this script's group. It is used only when the",
    "# script leads its group, which print.ts arranges by spawning it detached.",
    "leads_group() {",
    `  [ "$(ps -o pgid= -p "$$" 2>/dev/null | tr -d ' ')" = "$$" ]`,
    "}",
  ];
}

/**
 * How far the signals reach.
 *
 * - `children`: the child process only. kitty runs the window's command under
 *   `kitten run-shell`, so the script shares a group with that shell and does
 *   not lead it. The close call below is what ends the window and the rest of
 *   what runs in it.
 * - `group-when-led`: the script's group when the script leads it. print.ts
 *   spawns the wrapper detached, so that is the normal path there, and the
 *   group signal reaches the commands the child started.
 */
export type WatchReach = "children" | "group-when-led";

export interface WatchOptions {
  /** Poll interval, in seconds. */
  pollS?: number;
  /** Time between the TERM and the KILL, in seconds. */
  graceS?: number;
  /**
   * A watched run closes its own window: kitty holds it after the child exits,
   * and the parent that would close it is gone. The window id comes from
   * KITTY_WINDOW_ID, which kitty sets in the window the script runs in.
   */
  closeWindow?: { kitten: string; socket: string };
  /**
   * Required, with no default: a call site that forgets it would silently stop
   * at the child and leave the commands the child started behind.
   */
  reach: WatchReach;
}

/**
 * The watcher is a background subshell, because the child keeps the script's
 * foreground: `wait` below cannot poll. Its own output goes to /dev/null, so it
 * never holds the window's stdout open after the child is gone.
 */
export function watchdogLines(opts: WatchOptions = {}): string[] {
  const poll = opts.pollS ?? WATCH_POLL_S;
  const grace = opts.graceS ?? WATCH_GRACE_S;
  // An omitted reach falls to the group branch, which `leads_group` guards, so
  // a caller that forgets it cannot lose the commands the child started.
  const signals = (signal: string): string =>
    opts.reach === "children"
      ? `  kill -${signal} "$child" 2>/dev/null`
      : `  if leads_group; then kill -${signal} "-$$" 2>/dev/null; else kill -${signal} "$child" 2>/dev/null; fi`;
  const close = opts.closeWindow
    ? [
        // The window holds the rest of the run: kitty kills what lives in the
        // window it closes. kitty's match flag separates the field from the
        // value with a colon, as `closeWindowArgv` does.
        `  ${shellQuote(opts.closeWindow.kitten)} @ --to=${shellQuote(opts.closeWindow.socket)} close-window --match=id:"\${KITTY_WINDOW_ID:-}" >/dev/null 2>&1`,
      ]
    : [];
  return [
    "(",
    `  ${WATCHDOG_MARKER}`,
    `  while parent_alive && kill -0 "$child" 2>/dev/null; do sleep ${poll}; done`,
    "  # The run ended on its own: a held window stays, and nothing closes it.",
    '  kill -0 "$child" 2>/dev/null || exit 0',
    // The TERM below may reach this subshell when it signals a group, so it
    // must survive that signal to close the window and to send the KILL.
    "  trap '' TERM",
    signals("TERM"),
    `  sleep ${grace}`,
    signals("KILL"),
    ...close,
    ") >/dev/null 2>&1 &",
    "watcher=$!",
  ];
}

/**
 * The exit paths of a run share one order: the child is signalled, the window
 * (if any) is closed, and the KILL follows the grace. `watcher` is stopped
 * before the script exits, so no stale watcher closes a held window later.
 */
export function stopWatcherLines(): string[] {
  return ['kill "$watcher" 2>/dev/null'];
}
