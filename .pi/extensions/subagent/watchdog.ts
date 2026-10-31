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
 * state field of `/proc/<pid>/stat` is read where `/proc` exists.
 */
export function watchdogFunctions(): string[] {
  return [
    "parent_alive() {",
    '  if [ -r "/proc/$parent_pid/stat" ]; then',
    '    case "$(cat "/proc/$parent_pid/stat" 2>/dev/null)" in',
    "      *') Z'*) return 1 ;;",
    "    esac",
    "    return 0",
    "  fi",
    '  kill -0 "$parent_pid" 2>/dev/null',
    "}",
    "",
    "# The group signal must reach the child's own commands, and it must not",
    "# reach a caller that shares this script's group. It is used only when the",
    "# script leads its group: kitty makes a window's command a session leader,",
    "# and print.ts starts the wrapper detached.",
    "leads_group() {",
    `  [ "$(ps -o pgid= -p "$$" 2>/dev/null | tr -d ' ')" = "$$" ]`,
    "}",
  ];
}

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
}

/**
 * The watcher is a background subshell, because the child keeps the script's
 * foreground: `wait` below cannot poll. Its own output goes to /dev/null, so it
 * never holds the window's stdout open after the child is gone.
 */
export function watchdogLines(opts: WatchOptions = {}): string[] {
  const poll = opts.pollS ?? WATCH_POLL_S;
  const grace = opts.graceS ?? WATCH_GRACE_S;
  const group = (signal: string): string =>
    `  if leads_group; then kill -${signal} "-$$" 2>/dev/null; else kill -${signal} "$child" 2>/dev/null; fi`;
  const close = opts.closeWindow
    ? [
        // kitty's match flag separates the field from the value with a colon,
        // as `closeWindowArgv` does.
        `  ${shellQuote(opts.closeWindow.kitten)} @ --to=${shellQuote(opts.closeWindow.socket)} close-window --match=id:"\${KITTY_WINDOW_ID:-}" >/dev/null 2>&1`,
      ]
    : [];
  return [
    "(",
    `  ${WATCHDOG_MARKER}`,
    `  while parent_alive && kill -0 "$child" 2>/dev/null; do sleep ${poll}; done`,
    "  # The run ended on its own: a held window stays, and nothing closes it.",
    '  kill -0 "$child" 2>/dev/null || exit 0',
    // This subshell sends the group signal, so it must survive that signal to
    // send the KILL after it.
    "  trap '' TERM",
    group("TERM"),
    ...close,
    `  sleep ${grace}`,
    group("KILL"),
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
