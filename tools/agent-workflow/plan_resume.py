#!/usr/bin/env python3
"""Reconstruct mission state from GitHub and git, then print the exact
Workflow args needed to continue.

Truth order: merged commits on origin/main > open PR state > local branches.
Nothing here trusts a hand-written progress file, so it stays correct after a
crash, a reboot, or a session that ended mid-node.

  plan_resume.py <mission-dir>           human-readable plan
  plan_resume.py <mission-dir> --json    machine state, written into every snapshot

<mission-dir> may also be given as the MISSION_DIR environment variable. It
must hold plan.json (stages + branches for this run) — see
notes/multi-agent-playbook.md for the node/mission directory shape. This
script never guesses a mission directory: an unreadable plan.json is a hard
failure, not a silent one.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

REPO = os.environ.get("LOXPP_REPO", "/var/home/loctran/personal/loxpp")
GH_REPO = os.environ.get("LOXPP_GH_REPO", "txloc1909/loxpp")


def _mission_dir() -> Path:
    positional = [a for a in sys.argv[1:] if not a.startswith("--")]
    raw = positional[0] if positional else os.environ.get("MISSION_DIR")
    if not raw:
        sys.exit(
            "plan_resume.py: mission directory required — pass it as the "
            "first argument or set MISSION_DIR."
        )
    d = Path(raw)
    if not (d / "plan.json").is_file():
        sys.exit(f"plan_resume.py: {d}/plan.json not found or unreadable.")
    return d


MISSION = _mission_dir()
PLAN = json.loads((MISSION / "plan.json").read_text())
STAGES = PLAN["stages"]
BRANCHES = PLAN["branches"]


def run(cmd, **kw):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60, **kw)
        return r.stdout if r.returncode == 0 else ""
    except Exception:
        return ""


def gh_prs():
    out = run(["gh", "pr", "list", "--repo", GH_REPO, "--state", "all", "--limit", "60",
               "--json", "number,title,headRefName,state,mergedAt,reviewDecision"])
    try:
        return json.loads(out) if out else []
    except json.JSONDecodeError:
        return []


MARKER = "[Reviewer] APPROVED"


def approved(pr_number):
    """The reviewer cannot use gh's approve, so approval is a comment whose
    first line is '[Reviewer] APPROVED'.

    Tolerant on purpose: a reviewer may wrap the marker in a markdown heading
    or bold it. Resume correctness must not hinge on that formatting. But the
    marker still has to be its own line, so a comment that merely *discusses*
    approval ("I cannot post [Reviewer] APPROVED yet") is not mistaken for one.
    """
    out = run(["gh", "pr", "view", str(pr_number), "--repo", GH_REPO,
               "--json", "comments"])
    try:
        comments = json.loads(out).get("comments", [])
    except Exception:
        return False
    for c in comments:
        for line in (c.get("body") or "").splitlines():
            if not line.strip():
                continue                      # skip leading blank lines
            bare = line.strip().lstrip("#").strip().strip("*_` ").strip()
            return_this = bare == MARKER
            if return_this:
                return True
            break                             # only the first real line counts
    return False


def main():
    as_json = "--json" in sys.argv

    run(["git", "-C", REPO, "fetch", "origin", "--prune"])
    remote_heads = run(["git", "-C", REPO, "ls-remote", "--heads", "origin"])
    remote_branches = {ln.split("refs/heads/")[-1]
                       for ln in remote_heads.splitlines() if "refs/heads/" in ln}

    prs = gh_prs()
    by_branch = {}
    for p in prs:
        by_branch.setdefault(p["headRefName"], []).append(p)

    nodes = {}
    for node, branch in BRANCHES.items():
        entry = {"branch": branch, "pr": 0, "state": "not_started"}
        cands = by_branch.get(branch, [])
        if cands:
            # newest PR for the branch wins
            p = sorted(cands, key=lambda x: x["number"])[-1]
            entry["pr"] = p["number"]
            if p.get("mergedAt"):
                entry["state"] = "merged"
            elif p["state"] == "OPEN":
                entry["state"] = "merge_pending" if approved(p["number"]) else "in_review"
            else:
                entry["state"] = "closed_unmerged"
        elif branch in remote_branches:
            entry["state"] = "branch_pushed_no_pr"
        elif f"wip/{branch}" in remote_branches:
            entry["state"] = "wip_only"
        nodes[node] = entry

    done = {n for n, e in nodes.items() if e["state"] == "merged"}

    remaining, resume = [], {}
    for stage in STAGES:
        todo = [n for n in stage if n not in done]
        if todo:
            remaining.append(todo)
    for node, e in nodes.items():
        if e["state"] == "merged":
            resume[node] = {"merged": True, "pr": e["pr"]}
        elif e["state"] == "merge_pending":
            resume[node] = {"pr": e["pr"], "phase": "merge"}
        elif e["state"] == "in_review":
            resume[node] = {"pr": e["pr"], "phase": "review"}

    # Nodes with no open PR are omitted from resume, so the implementer runs and
    # its step 0 resume check picks up any pushed branch or wip/ twin.
    resume = {k: v for k, v in resume.items() if not v.get("merged")}

    state = {
        "nodes": nodes,
        "merged": sorted(done),
        "remaining_stages": remaining,
        "workflow_args": {"stages": remaining, "resume": resume},
        "complete": not remaining,
    }

    if as_json:
        print(json.dumps(state, indent=2))
        return

    print("MISSION STATE  (source of truth: origin/main + GitHub PRs)\n")
    order = [n for st in STAGES for n in st]
    for n in order:
        e = nodes[n]
        pr = f"#{e['pr']}" if e["pr"] else "-"
        mark = {"merged": "[x]", "merge_pending": "[~]", "in_review": "[>]",
                "branch_pushed_no_pr": "[.]", "wip_only": "[.]",
                "closed_unmerged": "[!]", "not_started": "[ ]"}[e["state"]]
        print(f"  {mark} {n:<4} {e['branch']:<32} {pr:<6} {e['state']}")
    print(f"\n  merged {len(done)}/{len(order)}")
    if state["complete"]:
        print("\nAll nodes merged. Verify the deliverable gate:")
        for d in PLAN["deliverable"]:
            print("  - " + d)
        return
    print("\nRemaining stages: " + " -> ".join("+".join(s) for s in remaining))
    print("\nResume with:\n")
    resume_args = dict(state["workflow_args"])
    resume_args["missionDir"] = str(MISSION)
    print("Workflow({")
    print(f'  scriptPath: "{REPO}/.claude/workflows/backend-dag.js",')
    print("  args: " + json.dumps(resume_args))
    print("})")
    print()
    print("(this reconstructs only the dynamic stages/resume state — merge in")
    print(" your mission's static `nodes`, `repo`, `githubRepo`, `dagDoc`,")
    print(" `opcodeDoc`, and `targetLabel` args too.)")


if __name__ == "__main__":
    main()
