# Subagent extension (kitty)

Delegate a task to a separate `pi` process with its own context. The child runs
in a new window in the same kitty tab as pi. By default that window holds a
normal interactive pi session: chat with the child there, watch its work, and
direct it while it runs. The call returns when the child exits, and the window
closes with it.

The extension is active only when pi runs inside a kitty window. Outside kitty
it registers no tool.

## Requirements

pi must reach kitty's remote control **through a socket**. In
`~/.config/kitty/kitty.conf`:

```
allow_remote_control socket-only
listen_on unix:/tmp/kitty-{kitty_pid}
```

Restart kitty after the change. A socket address must be in `KITTY_LISTEN_ON`;
`listen_on` is what creates the socket.

Use a literal path. kitty expands `$VAR` in `listen_on`, but it leaves an unset
variable in the path as text, so the copy of kitty's man page that uses
`${TEMP}` makes an invalid line on most Linux systems: `TEMP` is not set there
and the socket never appears. kitty appends `-<pid>` to a `unix:` path that has
no `{kitty_pid}`, so the address it reports is not the text above.

Do not use `allow_remote_control yes` without a `listen_on` line. That setting
opens the terminal channel instead: `kitten @` writes the request to the
controlling terminal and reads kitty's reply from it, and pi reads its keys
from that same terminal. The reply then arrives as input, so a large listing
such as `ls` lands in the prompt. The extension refuses to run in that mode and
says which two lines to add.

Every call names the socket with `--to`. The code never relies on a fallback.

## Agents

An agent is a markdown file with YAML frontmatter:

```markdown
---
name: scout
description: Fast codebase reconnaissance
tools: read, grep, find, ls, bash
model: anthropic/claude-haiku-4-5
---

You are a scout. Find the code that answers the question, and report file paths
and line numbers. Do not change files.
```

`tools` and `model` are optional. Without `model`, the child inherits the
dispatching session's model and thinking level.

Pi reads agents from:

| Directory | Loaded when |
|---|---|
| `~/.pi/agent/agents/*.md` | always |
| `.pi/agents/*.md` | only with `agentScope: "project"` or `"both"` |

A project agent replaces a user agent of the same name. Project agents are
repository-controlled prompts. The tool asks for confirmation before it runs
one in an untrusted project.

## Use

The model calls the `subagent` tool. A person can ask for it:

```
Use scout to find every place that opens a config file.
Run scout twice in parallel: one for the parser, one for the resolver.
Chain: scout finds the read tool, then worker adds the line range it names.
```

| Mode | Parameters | Notes |
|---|---|---|
| single | `agent`, `task` | one child |
| parallel | `tasks: [{ agent, task }]` | at most 8 tasks, 4 at a time |
| chain | `chain: [{ agent, task }]` | sequential; a later task may use `{previous}` |

Other parameters: `agentScope`, `confirmProjectAgents`, `cwd`, `interactive`,
`hold`, `keepFocus`.

- `cwd` sets the child's working directory. The default is the pi session's
  directory.
- `interactive` (default `true`) gives the child the window's terminal, so a
  person can chat with it. The call returns when that person quits the child
  (`Ctrl+D` or `/quit`) or closes its window. Set it to `false` for an
  unattended run: the child then ends as soon as its task is done, prints no
  TUI, and the window closes by itself. Parallel and chain runs are usually
  unattended, so pass `interactive: false` there.
- `hold: true` keeps the window open at a shell after the child exits. Use it
  to read the child's log. The default is to close the window.
- `keepFocus: true` leaves the keyboard in pi's window.

## How a run works

1. `kitten @ --to=<socket> launch --match=window_id=<pi window> --type=window
   --cwd=<cwd>` opens the child window as a split in pi's tab. The match on
   pi's own window keeps the placement independent of which kitty window has
   focus. The socket address keeps kitty's reply out of pi's terminal.
2. kitty runs a small shell script. The script enters the working directory and
   starts the child.
3. An interactive child keeps the terminal: its keys and its TUI stay in the
   window, and only its stderr goes to a log file. It saves a session named
   `subagent: <agent>`. The parent cannot read that terminal, so the child
   loads `mirror.ts`, which appends three record types to the event file:
   `message_end` for each finished message, and `agent_start`/`agent_end`
   around each run. They use the shape `pi --mode json` gives the same records.
4. pi tails the event file. Progress and token counts stream into the pi
   session, and the child window's title shows the turn count, the current
   tool, or `waiting for you` when the child is back at its prompt.
5. The child exits, and the exit code goes to a third file. kitty closes the
   window at once. pi also sends `close-window` as a safety net, so an aborted
   or crashed run leaves no window behind. `hold: true` turns both closes off.
6. The tool result is the child's last assistant text plus its usage. For an
   interactive child that is the answer after the last instruction a person
   gave it. Closing the window by hand is a normal end for an interactive run;
   `/quit` returns exit code 0.

With `interactive: false` steps 2 and 3 differ: the child runs `--mode json -p`
with stdout, stderr and the exit code in files, and the window shows a short
header. Closing that window by hand ends the run as aborted.

## Tests

```bash
# Unit tests. A fake `kitten` stands in for kitty, so no terminal is needed.
node --test .pi/extensions/subagent/*.test.ts

# Live tests. These need a running kitty with remote control over a socket: run
# them from a shell inside a kitty window, so KITTY_LISTEN_ON is inherited. The
# split they open lands in that window's tab, not in another tab. The address
# there is the one kitty reports, which may differ from the listen_on text:
# kitty appends -<pid> when the line has no {kitty_pid}.
LOXPP_SUBAGENT_LIVE=1 node --test .pi/extensions/subagent/live.test.ts
```

`LOXPP_SUBAGENT_PI=/path/to/program` makes the extension launch that program
instead of pi. The end-to-end tests use it to run the window plumbing without a
model call.
