#!/usr/bin/env python3
"""Reconstruct mission state from GitHub, then print the exact Workflow args
needed to continue.

Truth order: merged commits on origin/main > open PR state. Nothing here
trusts a hand-written progress file, so it stays correct after a crash, a
reboot, or a session that ended mid-node.

  plan_resume.py <mission-issue>           human-readable plan
  plan_resume.py <mission-issue> --json    machine state, written into every snapshot

<mission-issue> is the tracking issue number that sequences this mission's
nodes (see notes/multi-agent-playbook.md, "The harness" and "Node
specification structure"). It may also be given as the MISSION_ISSUE
environment variable. The script reads the issue body directly from GitHub —
it never reads a local mission directory or a plan.json, and it needs no git
checkout: every call here is a `gh` call against the repo's GitHub state.

The tracking issue's body must group its node checklist under `### Wave N`
headings, each `- [ ] #<node-issue> — <description>` line naming one node's
GitHub issue (the shape issue #261 already uses). checkbox state itself is
never read as truth — only decoration for a human skimming the issue — node
state always comes from live PR bodies, never from the checkbox.

A node is identified by its own GitHub issue number, not by a branch name:
this script finds a node's PR by matching the literal phrase "Closes #<n>"
against every PR body (GitHub's own closing-issue syntax, and the exact
phrase every implementer prompt in backend-dag.js is required to write). No
branch-naming convention is tracked anywhere for this purpose.

This match is done locally against fetched PR bodies with a plain regex, not
with `gh search prs`: that command ranks by relevance, not by literal
substring, and was measured returning a PR for "Closes #240" whose body does
not contain the string "240" anywhere — a false "merged" for a node that was
still open. Never switch this back to search without re-proving that failure
mode is gone.

KNOWN GAP: this script cannot see a branch that was pushed but has no PR yet
(a search over PR bodies finds nothing until a PR exists to search). That
case is not silently lost — an implementer resuming a node runs its own
`git ls-remote --heads origin <branch>` / `wip/<branch>` check as the first
step of its own prompt (backend-dag.js's step 0), which is the authoritative
resume path for exactly this case. This script's per-node table is for
human and orchestrator visibility between runs, not the sole resume source.
"""

import json
import os
import re
import subprocess
import sys

GH_REPO = os.environ.get("LOXPP_GH_REPO", "txloc1909/loxpp")

WAVE_RE = re.compile(r"^#{2,4}\s*wave\s*(\d+)", re.IGNORECASE)
ITEM_RE = re.compile(r"^-\s*\[.\]\s*#(\d+)\b(?:\s*[—–:-]\s*(.*))?$")


def _mission_issue() -> str:
    positional = [a for a in sys.argv[1:] if not a.startswith("--")]
    raw = positional[0] if positional else os.environ.get("MISSION_ISSUE")
    if not raw:
        sys.exit(
            "plan_resume.py: mission tracking issue number required — pass it "
            "as the first argument or set MISSION_ISSUE."
        )
    if not raw.lstrip("#").isdigit():
        sys.exit(f"plan_resume.py: {raw!r} does not look like an issue number.")
    return raw.lstrip("#")


MISSION_ISSUE = _mission_issue()


def run(cmd, **kw):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60, **kw)
        return r.stdout if r.returncode == 0 else ""
    except Exception:
        return ""


def tracking_issue():
    out = run(["gh", "issue", "view", MISSION_ISSUE, "--repo", GH_REPO,
               "--json", "body,title,state"])
    if not out:
        sys.exit(
            f"plan_resume.py: could not read issue #{MISSION_ISSUE} from "
            f"{GH_REPO} — check the number and `gh auth status`."
        )
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        sys.exit(f"plan_resume.py: issue #{MISSION_ISSUE} returned unparsable JSON.")


def parse_stages(body):
    """Group `- [ ] #<n> — ...` lines by the `### Wave N` heading above them.

    A checklist line with no wave heading above it yet goes into stage 0
    (still sequential, just ungrouped) — a malformed tracking issue should
    not silently drop nodes.
    """
    stages = {}
    order = []
    current = 0
    for line in body.splitlines():
        wm = WAVE_RE.match(line.strip())
        if wm:
            current = int(wm.group(1))
            if current not in order:
                order.append(current)
            continue
        im = ITEM_RE.match(line.strip())
        if im:
            if current not in stages:
                stages[current] = []
                if current not in order:
                    order.append(current)
            stages[current].append(im.group(1))
    return [stages[w] for w in sorted(order)]


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


_ALL_PRS = None


def all_prs():
    """Every PR in the repo, with body text, fetched once and cached.
    Local regex matching against this beats `gh search prs`, whose relevance
    ranking is not a literal substring match — see the module docstring."""
    global _ALL_PRS
    if _ALL_PRS is None:
        out = run(["gh", "pr", "list", "--repo", GH_REPO, "--state", "all",
                   "--limit", "1000",
                   "--json", "number,state,mergedAt,body"])
        try:
            _ALL_PRS = json.loads(out) if out else []
        except json.JSONDecodeError:
            _ALL_PRS = []
    return _ALL_PRS


def find_pr(node_issue):
    """The PR that closes this node issue, or None."""
    pat = re.compile(r"\bcloses\s+#" + re.escape(node_issue) + r"\b", re.IGNORECASE)
    cands = [p for p in all_prs() if pat.search(p.get("body") or "")]
    if not cands:
        return None
    # newest PR wins, same tie-break as before
    return sorted(cands, key=lambda p: p["number"])[-1]


def node_state(node_issue):
    entry = {"issue": node_issue, "pr": 0, "state": "not_started"}
    cand = find_pr(node_issue)
    if cand:
        entry["pr"] = cand["number"]
        if cand.get("mergedAt"):
            entry["state"] = "merged"
        elif cand.get("state") == "OPEN":
            entry["state"] = "merge_pending" if approved(cand["number"]) else "in_review"
        else:
            entry["state"] = "closed_unmerged"
        return entry

    # No PR found by search. Cross-check the issue's own state: a closed
    # issue with no PR we could find is a surprise, not "not started" — say
    # so rather than reporting a false "not_started".
    iss = json.loads(run(["gh", "issue", "view", node_issue, "--repo", GH_REPO,
                           "--json", "state"]) or "{}")
    if iss.get("state") == "CLOSED":
        entry["state"] = "closed_no_pr_found"
    return entry


def main():
    as_json = "--json" in sys.argv

    tracking = tracking_issue()
    stages = parse_stages(tracking.get("body") or "")
    if not stages:
        sys.exit(
            f"plan_resume.py: found no `### Wave N` / `- [ ] #<issue>` "
            f"checklist in issue #{MISSION_ISSUE}'s body. Nothing to resume."
        )

    all_nodes = [n for stage in stages for n in stage]
    nodes = {n: node_state(n) for n in all_nodes}
    done = {n for n, e in nodes.items() if e["state"] == "merged"}

    remaining, resume = [], {}
    for stage in stages:
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
        "mission_issue": MISSION_ISSUE,
        "nodes": nodes,
        "merged": sorted(done),
        "remaining_stages": remaining,
        "workflow_args": {"stages": remaining, "resume": resume},
        "complete": not remaining,
    }

    if as_json:
        print(json.dumps(state, indent=2))
        return

    print(f"MISSION STATE for #{MISSION_ISSUE}  (source of truth: origin/main + GitHub PR bodies)\n")
    for n in all_nodes:
        e = nodes[n]
        pr = f"#{e['pr']}" if e["pr"] else "-"
        mark = {"merged": "[x]", "merge_pending": "[~]", "in_review": "[>]",
                "closed_unmerged": "[!]", "closed_no_pr_found": "[?]",
                "not_started": "[ ]"}[e["state"]]
        print(f"  {mark} #{n:<6} {pr:<6} {e['state']}")
    print(f"\n  merged {len(done)}/{len(all_nodes)}")
    if state["complete"]:
        print("\nAll nodes merged.")
        return
    print("\nRemaining stages: " + " -> ".join("+".join(s) for s in remaining))
    print("\nResume with:\n")
    resume_args = dict(state["workflow_args"])
    resume_args["missionIssue"] = int(MISSION_ISSUE)
    print("Workflow({")
    print('  scriptPath: ".claude/workflows/backend-dag.js",')
    print("  args: " + json.dumps(resume_args))
    print("})")
    print()
    print("(this reconstructs only the dynamic stages/resume state, keyed by node")
    print(" issue number. Merge in your mission's static `nodes` map too — for each")
    print(" issue number above, supply { branch, title, issue } under whatever")
    print(" mnemonic id you want the harness to log (match it to `issue`, not to")
    print(" this script's key) — plus `repo`, `githubRepo`, `briefPath`, `dagDoc`,")
    print(" `opcodeDoc`, and `targetLabel`.)")


if __name__ == "__main__":
    main()
