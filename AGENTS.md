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

# 2. Build the image — pick the stage for the work
podman build --target dev -t loxpp-dev-env .                    # C++ work
podman build --target dev-managed -t loxpp-dev-env-managed .    # backend work

# 3. Start ephemeral container (:z needed on SELinux hosts e.g. Fedora),
#    using whichever image you built above
podman run -it --rm \
  -v /path/to/.claude/worktrees/loxpp-<type>-<desc>:/workspace:z \
  --name loxpp-<type>-<desc> loxpp-dev-env       # or loxpp-dev-env-managed

# 4. Build — also wires the pre-commit hook via cmake
cmake --preset debug && cmake --build build

# 5. Iterate: write code, test
ctest --test-dir build --output-on-failure -j$(nproc)

# 6. Format + lint
find src test -name '*.cpp' -o -name '*.h' | xargs clang-format -i
find src -name '*.cpp' | xargs clang-tidy -p build

# 7. Push + open PR
git push origin <branch>
gh pr create --base main --title "<title>" --body "<description>"

# 8. Watch CI
gh run watch <run-id> --repo txloc1909/loxpp
gh run view <run-id> --log-failed --repo txloc1909/loxpp   # on failure

# 9. Resolve review comments → push → re-watch → repeat until approved
gh pr view <pr-number> --repo txloc1909/loxpp --comments

# 10. Merge (squash)
gh pr merge <pr-number> --repo txloc1909/loxpp --squash

# 11. Teardown
git worktree remove .claude/worktrees/loxpp-<type>-<desc>
git branch -d <type>/<desc>
```

> Backend work: run `tools/build_lox_rt.sh && tools/check_managed_toolchains.sh`
> inside `dev-managed` before touching backend code. The check needs the JVM
> runtime jar, so build it first on a fresh worktree. This confirms the JVM
> and CLR toolchains are healthy, so any later failure points at generated
> bytecode rather than the image. Neither agent tag is `loxpp-dev` — that name
> belongs to the human's off-limits distrobox container.

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

**Comments:** let the code and the project structure speak for themselves.
Comment only the non-obvious *why* — a subtle invariant, a constraint not
visible locally, a deliberate trade-off. Don't narrate *what* the code already
states or restate the obvious; redundant comments are noise, not help.

A code comment must stand on its own for a reader who cannot see the pull
request or the review thread that produced it. If a comment needs that thread
to make sense, it is in the wrong place:

- The **invariant, constraint, or trade-off** goes in the code comment.
- The **reason a change happened at a particular time** — a review finding, a
  referee decision — goes in the commit message body and the PR reply, not in
  the source.

---

## Engineering rules

These apply to any contributor, human or agent, in any kind of work.

- **Prove that a new check can fail.** Remove the fix, run the test, watch it
  fail, then put the fix back. A check nobody has seen fail is unproven.
- **Call a new helper at every site that needs it.** A helper only some call
  sites use is not complete.
- **Connect each new probe to something that runs it.** A probe file that
  nothing reads catches nothing.
- **Do not report a tool result you did not just produce.** Run the tool; a
  remembered "it was clean last time" is not evidence.
- **If a check cannot be made to fail, say so, and name where the defect
  becomes reachable instead.** An honest gap beats a false completeness claim.
- **A reviewer must run the programs, not only read the diff.** Defects that
  only show up at runtime don't show up in a diff.
- **Commit when the code compiles. Push after each commit.** Work that sits
  uncommitted for hours is work that can be lost.

Any AI agent's external communication in this project — PR descriptions,
issue comments, commit messages, review replies, and so on, not only
messages between agents on a multi-agent run — uses ASD-STE100 Simplified
Technical English. If a person can read it, it must stay simple.

### Multi-agent conventions

When more than one agent works a task under the same GitHub account, every
public message (PR comment, review, commit trailer) is tagged with its
author's role: `[Implementer]`, `[Reviewer]`, or `[Researcher]`.

See `notes/multi-agent-playbook.md` for the full workflow these conventions
support (roles, escalation limits, node specification structure, referee
format).
