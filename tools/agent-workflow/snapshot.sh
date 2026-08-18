#!/usr/bin/env bash
# Persist every piece of mission state that a reboot would destroy.
#
#   snapshot.sh <mission-dir> [--filter <substr>] [--push-wip]
#
#   <mission-dir>     required. A durable directory (never /tmp) that will
#                     hold snapshots/. May also come from the MISSION_DIR
#                     env var.
#   --filter <substr> only worktrees/PRs whose branch name contains this
#                     substring are snapshotted. Defaults to <mission-dir>'s
#                     basename with a leading "loxpp-" and trailing
#                     "-mission" stripped (e.g. "loxpp-clr-mission" -> "clr").
#                     Override it if your branch names don't share that
#                     substring.
#   --push-wip        also commits and pushes uncommitted work to
#                     origin/wip/<branch>. Run this ONLY after the workflow
#                     is stopped, because it mutates worktrees.
#
# Everything lands in $MISSION/snapshots/<stamp>/ plus a stable `latest` link.
set -uo pipefail

MISSION="${1:-${MISSION_DIR:-}}"
if [ -z "$MISSION" ] || [ "$MISSION" = "--push-wip" ]; then
  echo "snapshot.sh: mission directory required as the first argument or MISSION_DIR." >&2
  exit 1
fi
shift || true

REPO="${LOXPP_REPO:-/var/home/loctran/personal/loxpp}"
GH_REPO="${LOXPP_GH_REPO:-txloc1909/loxpp}"
BASENAME="$(basename "$MISSION")"
FILTER="${BASENAME#loxpp-}"
FILTER="${FILTER%-mission}"
PUSH_WIP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --push-wip) PUSH_WIP=1 ;;
    --filter) shift; FILTER="${1:-}" ;;
  esac
  shift || true
done

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$MISSION/snapshots/$STAMP"
mkdir -p "$OUT/worktrees" "$OUT/github"

say() { printf '%s\n' "$*"; }

# --- 1. environment preconditions ------------------------------------------
{
  say "stamp:        $STAMP"
  say "gh user:      $(gh api user --jq .login 2>/dev/null || echo UNKNOWN)"
  say "gh perm:      $(gh repo view "$GH_REPO" --json viewerPermission -q .viewerPermission 2>/dev/null || echo UNKNOWN)"
  say "push url:     $(git -C "$REPO" remote get-url --push origin 2>/dev/null)"
  say "fetch url:    $(git -C "$REPO" remote get-url origin 2>/dev/null)"
  say "cred helper:  $(git config --global --get credential.https://github.com.helper 2>/dev/null)"
  say "podman image: $(podman images --format '{{.Repository}}:{{.Tag}} {{.ID}}' 2>/dev/null | grep loxpp-dev-env-managed || echo MISSING)"
  say "main head:    $(git -C "$REPO" rev-parse origin/main 2>/dev/null)"
} > "$OUT/environment.txt" 2>&1

# --- 2. git topology --------------------------------------------------------
git -C "$REPO" fetch origin --prune >/dev/null 2>&1
git -C "$REPO" worktree list                 > "$OUT/worktree-list.txt" 2>&1
git -C "$REPO" branch -vv                    > "$OUT/local-branches.txt" 2>&1
git -C "$REPO" ls-remote --heads origin      > "$OUT/remote-branches.txt" 2>&1
git -C "$REPO" log --oneline -40 origin/main > "$OUT/main-log.txt" 2>&1

# --- 3. per-worktree working state -----------------------------------------
# Only this mission's worktrees, matched by $FILTER. The human's other
# worktrees are never touched.
while read -r WT; do
  [ -d "$WT" ] || continue
  BR="$(git -C "$WT" rev-parse --abbrev-ref HEAD 2>/dev/null)"
  case "$BR" in
    *"$FILTER"*) ;;
    *) continue ;;
  esac
  SAFE="$(printf '%s' "$BR" | tr '/' '-')"
  D="$OUT/worktrees/$SAFE"
  mkdir -p "$D"
  {
    say "path:   $WT"
    say "branch: $BR"
    say "head:   $(git -C "$WT" rev-parse HEAD 2>/dev/null)"
    say "upstream: $(git -C "$WT" rev-parse --abbrev-ref '@{u}' 2>/dev/null || echo none)"
  } > "$D/info.txt"
  git -C "$WT" status --porcelain=v1 -uall > "$D/status.txt" 2>&1
  git -C "$WT" diff HEAD                   > "$D/tracked.patch" 2>&1
  git -C "$WT" log --oneline -30           > "$D/log.txt" 2>&1
  # New files git does not track yet are the ones a patch cannot carry.
  git -C "$WT" ls-files --others --exclude-standard -z 2>/dev/null \
    | tar --null -C "$WT" -czf "$D/untracked.tar.gz" --files-from=- 2>/dev/null

  if [ "$PUSH_WIP" = "1" ]; then
    if [ -n "$(git -C "$WT" status --porcelain 2>/dev/null)" ]; then
      git -C "$WT" add -A
      git -C "$WT" -c core.hooksPath=/dev/null \
        commit -q -m "wip: snapshot before suspend" 2>>"$D/wip.log"
    fi
    git -C "$WT" push -q --force-with-lease \
      "https://github.com/$GH_REPO.git" "HEAD:refs/heads/wip/$BR" 2>>"$D/wip.log" \
      && say "pushed wip/$BR" >> "$D/wip.log"
  fi
done < <(git -C "$REPO" worktree list --porcelain | awk '/^worktree /{print $2}')

# --- 4. GitHub state (the durable record of every review thread) ------------
gh pr list --repo "$GH_REPO" --state all --limit 50 \
  --json number,title,headRefName,state,isDraft,mergedAt,updatedAt \
  > "$OUT/github/pr-list.json" 2>/dev/null

python3 - "$OUT/github/pr-list.json" "$FILTER" <<'PY' > "$OUT/github/mission-prs.txt" 2>/dev/null
import json, sys
try:
    prs = json.load(open(sys.argv[1]))
except Exception:
    prs = []
needle = sys.argv[2]
for p in prs:
    if needle in p.get("headRefName", ""):
        print(p["number"])
PY

while read -r N; do
  [ -n "$N" ] || continue
  gh pr view "$N" --repo "$GH_REPO" --comments > "$OUT/github/pr-$N-thread.md" 2>&1
  gh pr diff "$N" --repo "$GH_REPO"            > "$OUT/github/pr-$N.diff"    2>&1
done < "$OUT/github/mission-prs.txt"

# --- 5. derived mission state ----------------------------------------------
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$SELF_DIR/plan_resume.py" "$MISSION" --json > "$OUT/state.json" 2>"$OUT/state.err"

# --- 6. workflow transcripts (agent reasoning, already in $HOME) ------------
WF="/var/home/loctran/.claude/projects/-var-home-loctran-personal-loxpp"
say "workflow transcripts live under: $WF/*/subagents/workflows/" > "$OUT/transcripts.txt"
ls -d "$WF"/*/subagents/workflows/wf_* >> "$OUT/transcripts.txt" 2>/dev/null

ln -sfn "$OUT" "$MISSION/snapshots/latest"
say "snapshot written: $OUT"
say "resume plan:"
sed -n '1,40p' "$OUT/state.json" 2>/dev/null
