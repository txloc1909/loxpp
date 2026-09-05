# Contributing to Lox++

[AGENTS.md](AGENTS.md) is the authoritative development reference — environment setup, build
presets, test and lint commands, branch/commit conventions, the full task loop, and cloud agent
configuration.

[TESTING.md](TESTING.md) covers how to write new GTest cases and use the VM test helpers.

**Dev environment:** the `Dockerfile` packages the full toolchain in two stages —
`dev` for C++ work, `dev-managed` when touching the JVM or CLR backends. Build the one
you need:

```bash
podman build --target dev -t loxpp-dev-env .                    # C++ work
podman build --target dev-managed -t loxpp-dev-env-managed .    # backend work
```

The `--target` matters: `dev-managed` is the last stage, so omitting it builds the
larger managed image regardless of what you tag it.

Agent containers share one compiler cache through the `loxpp-ccache` podman volume
mounted at `/ccache`; see [AGENTS.md](AGENTS.md) for the full `podman run` command.

---

## For AI agents

Read [AGENTS.md](AGENTS.md). The short version: plan before implementing, wait for explicit
approval, work in an isolated worktree and container, commit atomically with
[Conventional Commits](https://www.conventionalcommits.org/) subject lines, keep every commit
green.
