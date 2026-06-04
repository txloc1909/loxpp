# Contributing to Lox++

[AGENTS.md](AGENTS.md) is the authoritative development reference — environment setup, build
presets, test and lint commands, branch/commit conventions, the full task loop, and cloud agent
configuration.

[TESTING.md](TESTING.md) covers how to write new GTest cases and use the VM test helpers.

**Dev environment:** the `Dockerfile` packages the full toolchain. Build it once:

```bash
podman build -t loxpp-dev-env .
```

---

## For AI agents

Read [AGENTS.md](AGENTS.md). The short version: plan before implementing, wait for explicit
approval, work in an isolated worktree and container, commit atomically with
[Conventional Commits](https://www.conventionalcommits.org/) subject lines, keep every commit
green.
