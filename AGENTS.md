# AGENTS.md

## Project overview

Lox is a dynamically-typed scripting language.
Lox++ is a bytecode compiler & VM for Lox, written in C++17 (Clang, CMake, Ninja).

This project uses **trunk-based development**: all work targets `main` via short-lived
branches. PRs should be small and merged quickly — no long-running feature branches.

---

## Dev model

The repo is a **regular clone** with a dedicated directory for agent worktrees:

```
loxpp/                        ← human's clone (branch: main)
loxpp/agents-worktree/        ← all agent worktrees live here (gitignored)
  loxpp-feat-foo/             ← agent worktree (branch: feat/foo)
  loxpp-fix-bar/              ← agent worktree (branch: fix/bar)
```

- `agents-worktree/` is listed in `.gitignore` — its contents never appear in commits.
- Every agent works in its own worktree on its own branch, with its own `build/` directory.
- Every worktree gets its own ephemeral container (started with `--rm`).
- Containers all share the same `loxpp-dev-env` image; build it once.

> **The `loxpp-dev` distrobox container is the human's environment. Agents must
> not use or modify it.**

---

## One-time setup

### 1. Build the dev image

Run this once (or whenever the `Dockerfile` changes):

```bash
# From the repo root
podman build -t loxpp-dev-env .
# docker build -t loxpp-dev-env .   # docker also works
```

---

## Copilot cloud agent

When running as a **Copilot cloud agent** (GitHub Actions), the `loxpp-dev:latest`
image is pre-built by `.github/workflows/copilot-setup-steps.yml` before the agent
starts. Use `docker run` for all build, test, and lint commands — do not install the
toolchain directly on the runner.

The `Dockerfile` is the single source of truth for the dev environment. Any toolchain
change must go there; the setup steps and CI will automatically pick it up.

### Build and test (cloud agent)

```bash
# Build (debug)
docker run --rm -v $PWD:/workspace -w /workspace loxpp-dev:latest \
  bash -c "cmake --preset debug && cmake --build build"

# Run tests
docker run --rm -v $PWD:/workspace -w /workspace loxpp-dev:latest \
  bash -c "ctest --test-dir build --output-on-failure"

# Check formatting
docker run --rm -v $PWD:/workspace -w /workspace loxpp-dev:latest \
  bash -c "find src test -name '*.cpp' -o -name '*.h' | xargs clang-format --dry-run --Werror"

# Run clang-tidy
docker run --rm -v $PWD:/workspace -w /workspace loxpp-dev:latest \
  bash -c "find src -name '*.cpp' | xargs clang-tidy -p build"
```

---

## Agent task loop

Every agent task follows this loop end-to-end:

### 1. Create a branch and worktree

```bash
# From the repo root (loxpp/)
git worktree add agents-worktree/loxpp-<branch> -b <type>/<short-description>

# Example
git worktree add agents-worktree/loxpp-feat-closures -b feat/closures
```

See [Conventions](#conventions) for branch naming rules.

### 2. Start an ephemeral container for that worktree

Mount the worktree directory into the container as `/workspace`:

```bash
# podman (recommended)
podman run -it --rm \
  -v /path/to/loxpp/agents-worktree/loxpp-<branch>:/workspace:z \
  --name loxpp-<branch> \
  loxpp-dev-env

# docker (alternative)
docker run -it --rm \
  -v /path/to/loxpp/agents-worktree/loxpp-<branch>:/workspace \
  --name loxpp-<branch> \
  loxpp-dev-env
```

The container starts in `/workspace` (the worktree root).
The `:z` flag is needed for SELinux hosts (e.g. Fedora/RHEL); omit on non-SELinux hosts.

### 3. Build and test locally (inside the container)

Iterate here until everything passes:

```bash
cmake --preset debug && cmake --build build
ctest --test-dir build --output-on-failure
```

Fix any failures before pushing.

### 4. Push and open a PR

```bash
git push origin <branch>
gh pr create --base main --title "<title>" --body "<description>"
```

### 5. Watch CI

```bash
# List recent runs for your branch
gh run list --branch <your-branch> --repo txloc1909/loxpp

# Watch a run in real time
gh run watch <run-id> --repo txloc1909/loxpp

# Show failed job logs
gh run view <run-id> --log-failed --repo txloc1909/loxpp
```

CI runs: **Lint** (clang-format), **Build & Test** (debug + release), **Static Analysis** (clang-tidy).

If CI fails (for non-intentional reasons): fix locally inside the container, push again, re-watch.

### 6. Resolve review comments

Poll for PR review comments:

```bash
gh pr view <pr-number> --repo txloc1909/loxpp --comments
```

For each comment: fix inside the container → build/test → push → re-watch CI → repeat until approved.

### 7. Merge

Once CI is green and the PR is approved, merge to `main`:

```bash
gh pr merge <pr-number> --repo txloc1909/loxpp --squash
```

### 8. Teardown

```bash
# Remove the worktree
git worktree remove agents-worktree/loxpp-<branch>

# Delete the local branch
git branch -d <type>/<short-description>

# The container is already gone (started with --rm)
```

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
| `feat/closures` | `agents-worktree/loxpp-feat-closures/` | `loxpp-feat-closures` |
| `fix/string-gc` | `agents-worktree/loxpp-fix-string-gc/` | `loxpp-fix-string-gc` |

### Isolation rules

- Each agent works **only** in its own worktree and its own container.
- Never commit directly to `main`. Always work on a feature branch and open a PR.
- The `build/` directory lives inside the worktree (`${sourceDir}/build` per
  `CMakePresets.json`) — it is automatically isolated between worktrees.

