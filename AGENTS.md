# AGENTS.md

## Source of truth

`spec/` is the canonical definition of Lox++ semantics. **When spec and
implementation conflict, fix the implementation.** Always update `spec/` in the
same PR as any language change.

Decision priority: **spec** (`spec/`) > **implementation** (`src/`) > **design
notes** (`notes/`)

Consult `notes/` when planning — it captures future direction and helps avoid
conflicting designs.

---

## Planning policy

**Always plan before implementing.**

1. Post a plan (approach, files affected, open questions) as an issue comment
   or PR description.
2. Wait for explicit approval before writing any code or opening a PR.
3. If the plan changes, acknowledge the revision before proceeding.

> Trivial one-liners may skip this. When in doubt, plan first.

---

## Dev model

Each agent works in its own worktree and ephemeral container — isolated from
the human's environment and from other agents:

```
loxpp/
  .claude/worktrees/
    loxpp-feat-foo/    ← agent worktree (branch: feat/foo), own build/
    loxpp-fix-bar/     ← agent worktree (branch: fix/bar), own build/
```

> The human's `loxpp-dev` distrobox container is off-limits — never use or
> modify it.

---

## Task loop

```bash
# 1. Create worktree + branch (from repo root)
git worktree add .claude/worktrees/loxpp-<type>-<desc> -b <type>/<desc>

# 2. Start ephemeral container (:z needed on SELinux hosts e.g. Fedora)
podman run -it --rm \
  -v /path/to/.claude/worktrees/loxpp-<type>-<desc>:/workspace:z \
  --name loxpp-<type>-<desc> loxpp-dev-env

# 3. Build — also wires the pre-commit hook via cmake
cmake --preset debug && cmake --build build

# 4. Iterate: write code, test
ctest --test-dir build --output-on-failure -j$(nproc)

# 5. Format + lint
find src test -name '*.cpp' -o -name '*.h' | xargs clang-format -i
find src -name '*.cpp' | xargs clang-tidy -p build

# 6. Push + open PR
git push origin <branch>
gh pr create --base main --title "<title>" --body "<description>"

# 7. Watch CI
gh run watch <run-id> --repo txloc1909/loxpp
gh run view <run-id> --log-failed --repo txloc1909/loxpp   # on failure

# 8. Resolve review comments → push → re-watch → repeat until approved
gh pr view <pr-number> --repo txloc1909/loxpp --comments

# 9. Merge (squash)
gh pr merge <pr-number> --repo txloc1909/loxpp --squash

# 10. Teardown
git worktree remove .claude/worktrees/loxpp-<type>-<desc>
git branch -d <type>/<desc>
```

---

## Conventions

**Branch naming:** `<type>/<short-description>`

| Type | Use for |
|---|---|
| `feat/` | New language features or VM capabilities |
| `fix/` | Bug fixes |
| `test/` | Test additions or improvements |
| `refactor/` | Refactoring without behaviour change |
| `ci/` | CI/tooling changes |
| `docs/` | Documentation only |

**Commit discipline:** atomic and often — one concern per commit, green on
every commit. Use [Conventional Commits](https://www.conventionalcommits.org/):

```
feat: add constant folding for binary arithmetic
fix: prevent double-free in ObjString destructor
```

If you'd write "and" in the subject, split the commit. Add a body only when the
*why* needs explaining.
