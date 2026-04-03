# AGENTS.md

## Project overview

Lox is a dynamically-typed scripting language.
Lox++ is a bytecode compiler & VM for Lox, written in C++17 (Clang, CMake, Ninja).

---

## Dev model

This project uses a **bare repo + git worktree + container-per-worktree** model:

```
loxpp.git/          ← bare repo (object store, no working tree)
loxpp-main/         ← human's worktree (branch: main)
loxpp-feat-foo/     ← agent worktree  (branch: feat/foo)
loxpp-fix-bar/      ← agent worktree  (branch: fix/bar)
```

- The bare repo is the single source of truth on disk.
- Every agent and the human each work in their own worktree on their own branch.
- Every worktree gets its own isolated container and its own `build/` directory.
- Containers all share the same `loxpp-dev-env` image; build it once.

> **The `loxpp-dev` distrobox container is the human's environment. Agents must
> not use or modify it.**

---

## One-time setup

### 1. Clone the bare repo

```bash
git clone --bare git@github.com:txloc1909/loxpp.git loxpp.git
```

### 2. Build the dev image

Run this once (or whenever the `Dockerfile` changes):

```bash
# From any worktree, or directly from the bare repo directory
podman build -t loxpp-dev-env <path-to-Dockerfile-dir>
# docker build -t loxpp-dev-env <path-to-Dockerfile-dir>   # docker also works
```

---

## Per-task setup (one worktree per agent)

### 1. Create a branch and worktree

```bash
# From the bare repo directory
git -C loxpp.git worktree add ../loxpp-<branch> -b <type>/<short-description>

# Example
git -C loxpp.git worktree add ../loxpp-feat-closures -b feat/closures
```

See [Conventions](#conventions) for branch naming rules.

### 2. Start a container for that worktree

Mount the worktree directory into the container as `/workspace`:

```bash
# podman (recommended)
podman run -it --rm \
  -v /path/to/loxpp-<branch>:/workspace:z \
  --name loxpp-<branch> \
  loxpp-dev-env

# docker (alternative)
docker run -it --rm \
  -v /path/to/loxpp-<branch>:/workspace \
  --name loxpp-<branch> \
  loxpp-dev-env
```

The container starts in `/workspace` (the worktree root).
The `:z` flag is needed for SELinux hosts (e.g. Fedora/RHEL); omit on non-SELinux hosts.

---

## Build, test, run (inside the container)

```bash
# Configure + build (debug preset — includes compile_commands.json)
cmake --preset debug && cmake --build build

# Run all tests
ctest --test-dir build --output-on-failure

# Run a specific test binary
./build/test_scanner

# Run the interpreter interactively
./build/loxpp

# Run a Lox script
./build/loxpp path/to/script.lox

# Release build
cmake --preset release && cmake --build build
```

Available presets: `debug`, `release` (see `CMakePresets.json`).

---

## CI feedback loop

Every push to any branch triggers the GitHub Actions CI pipeline
(`.github/workflows/ci.yml`), which runs:

- **Lint**: `clang-format` check
- **Build & Test**: both `debug` and `release` presets
- **Static Analysis**: `clang-tidy`

Watch CI status from anywhere with the `gh` CLI:

```bash
# List recent runs for your branch
gh run list --branch <your-branch> --repo txloc1909/loxpp

# Watch a run in real time
gh run watch <run-id> --repo txloc1909/loxpp

# Show failed job logs
gh run view <run-id> --log-failed --repo txloc1909/loxpp
```

**Feedback loop for agents:**
1. Edit code in the worktree.
2. Build and test locally inside the container.
3. `git push origin <branch>` — CI triggers automatically.
4. Monitor CI results with `gh run list`.
5. If CI fails, read the logs, fix locally, push again.

---

## Conventions

### Branch naming

```
<type>/<short-description>
```

| Type | Use for |
|---|---|
| `feat/` | New language features or VM capabilities |
| `fix/` | Bug fixes |
| `test/` | Test additions or improvements |
| `refactor/` | Refactoring without behaviour change |
| `ci/` | CI/tooling changes |
| `docs/` | Documentation only |

Examples: `feat/closures`, `fix/string-gc`, `test/scanner-edge-cases`

### Container & worktree naming

Use the branch name (with `/` replaced by `-`) for both the worktree directory
and the container name:

| Branch | Worktree dir | Container name |
|---|---|---|
| `feat/closures` | `loxpp-feat-closures/` | `loxpp-feat-closures` |
| `fix/string-gc` | `loxpp-fix-string-gc/` | `loxpp-fix-string-gc` |

### Isolation rules

- Each agent works **only** in its own worktree and its own container.
- Never commit directly to `main`. Always work on a feature branch and open a PR.
- The `build/` directory lives inside the worktree (`${sourceDir}/build` per
  `CMakePresets.json`) — it is automatically isolated between worktrees.

---

## Teardown

When the task is complete (branch merged or abandoned):

```bash
# Stop and remove the container (if not using --rm)
podman stop loxpp-<branch>

# Remove the worktree
git -C loxpp.git worktree remove ../loxpp-<branch>

# Optionally delete the branch
git -C loxpp.git branch -d <type>/<short-description>
```
