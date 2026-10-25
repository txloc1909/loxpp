# Subagent extension (kitty)

Delegate a task to a separate `pi` process with its own context.

There are two kinds of run, and only one of them has a window.

| Run kind | When | Window | The tool call | The result arrives as |
|---|---|---|---|---|
| **watched** | one task, `interactive` not set to `false` | yes, a new kitty window in pi's tab | returns as soon as the window opens | a `subagent-result` message that wakes the session |
| **unattended** | `interactive: false`, or `tasks`/`chain` | none | waits for the run | the tool result |

A watched run is a normal interactive pi session in its window: chat with it,
watch its work, and redirect it while it runs. The parent never waits for that
person, so it keeps working, or ends its turn, and the end of the run arrives
later as a message.

An unattended run is a child process with pipes. It has no terminal and no
window, it needs no kitty, and it works in any terminal. Parallel and chain
runs are unattended by nature: a chain hands one step's result to the next, and
a batch has no one window to steer.

## Requirements

A watched run needs pi inside a kitty window, and kitty's remote control
**through a socket**. In `~/.config/kitty/kitty.conf`:

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
such as `ls` lands in the prompt. The extension refuses to run a watched job in
that mode and says which two lines to add.

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
| parallel | `tasks: [{ agent, task }]` | at most 8 tasks, 4 at a time; unattended |
| chain | `chain: [{ agent, task }]` | sequential; a later task may use `{previous}`; unattended |

Other parameters: `agentScope`, `confirmProjectAgents`, `cwd`, `interactive`,
`hold`, `keepFocus`.

- `cwd` sets the child's working directory. The default is the pi session's
  directory.
- `interactive` (default `true` for one task) runs the child as a watched job:
  a person can chat with it, and the call returns as soon as its window opens.
  Set it to `false` for an unattended run: no window, and the call waits.
  `interactive: true` together with `tasks` or `chain` is refused, because a
  batch has no one window to steer.
- `hold: true` keeps a watched window open at a shell after the child exits.
  Use it to read the child's log. The default is to close the window.
- `keepFocus: true` leaves the keyboard in pi's window.

## How a watched run works

1. `kitten @ --to=<socket> launch --match=window_id=<pi window> --type=window
   --cwd=<cwd>` opens the child window as a split in pi's tab. The match on
   pi's own window keeps the placement independent of which kitty window has
   focus. The socket address keeps kitty's reply out of pi's terminal.
2. kitty runs a small shell script. The script enters the working directory and
   starts the child.
3. The child keeps the terminal: its keys and its TUI stay in the window, and
   only its stderr goes to a log file. It saves a session named
   `subagent: <agent>`. The parent cannot read that terminal, so the child
   loads `mirror.ts`, which appends three record types to the event file:
   `message_end` for each finished message, and `agent_start`/`agent_end`
   around each run.
4. The tool call returns. The parent keeps working, and a footer line names the
   job, its window and its current tool. pi tails the event file, and the
   window's title shows the turn count, the current tool, or `waiting for you`
   when the child is back at its prompt.
5. The child exits, and the exit code goes to a third file. kitty closes the
   window at once, and pi sends `close-window` as a safety net. `hold: true`
   turns both closes off.
6. The parent gets a `subagent-result` message with the child's last assistant
   text and its usage. That message starts a new turn in the parent session, so
   the parent reads the answer and continues. It waits behind a running turn
   rather than cutting into it. The tool call itself only says that the job
   started, and the model is told not to guess the answer before the message
   arrives.

### Closing the window early

A person can end a watched run at any moment:

- Quit the child (`Ctrl+D`, `/quit`) or close the window while it waits at its
  prompt: the run completed, and the answer is its last message.
- Close the window while a turn is still running: kitty kills the child. The
  parent notices within about three seconds (a two-second liveness poll plus a
  grace period), keeps what the child had already finished, and reports the run
  as **stopped**, not completed. The message says that only finished messages
  are included, so the parent does not read an older answer as the answer to
  its task.
- The last turn itself failed or was interrupted: the run is a failure, even if
  the person closes the window afterwards.

No notification is sent to a session that has ended: a job belongs to the
session that started it, and every shutdown path stops its jobs.

## How an unattended run works

The child runs `--mode json -p --no-session` as a plain subprocess: stdout
carries the event stream, stderr is kept for the tool result, and the exit code
ends the run. Progress streams into the tool call. There is no window, no kitty
call, and no temporary event file. An abort ends the child: `SIGTERM` first,
then `SIGKILL` after two seconds.

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

`index.test.ts` loads the real `index.ts` against a fake `ExtensionAPI`, a fake
kitty and a stand-in child. That needs the pi packages, which `index.ts`
imports: the test skips where they are not installed. `PI_PACKAGES_DIR` points
at a pi installation's `node_modules` when none of the usual places has it.

The two rules that file carries:

- a watched call returns while its child is still running, and the run's end
  arrives as a message that wakes the session;
- an unattended call never calls kitty.
