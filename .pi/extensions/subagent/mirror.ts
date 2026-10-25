/**
 * Event mirror for a watched subagent child.
 *
 * A watched child draws its TUI on stdout and reads the keyboard from the same
 * terminal, so the parent cannot read its event stream. This extension
 * writes three record types to the file named by `LOXPP_SUBAGENT_EVENTS`:
 * `message_end` for each finished message, and `agent_start`/`agent_end`
 * around each run. They use the shape `pi --mode json` gives the same records,
 * so the parent parses one format for both kinds of child.
 *
 * The parent loads this file with `--extension`. Without the environment
 * variable it registers nothing, so loading it by mistake does no harm.
 *
 * Node builtins and type-only imports only, so the tests load it directly.
 */

import * as fs from "node:fs";
import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { EVENTS_ENV } from "./child.ts";

export { EVENTS_ENV };

export default function mirror(pi: ExtensionAPI): void {
  const eventsPath = process.env[EVENTS_ENV];
  if (!eventsPath) return;

  const write = (record: unknown): void => {
    try {
      fs.appendFileSync(eventsPath, `${JSON.stringify(record)}\n`);
    } catch {
      // The parent may already be gone and its temporary directory with it. A
      // failed mirror must not take down a session the person is still using.
    }
  };

  pi.on("agent_start", () => write({ type: "agent_start" }));
  // Not a final-idle signal in general, but the parent only uses it to say the
  // child is back at its prompt, and another agent_start corrects it.
  pi.on("agent_end", () => write({ type: "agent_end" }));
  pi.on("message_end", (event) => write({ type: "message_end", message: event.message }));
}
