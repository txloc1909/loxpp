#!/usr/bin/env python3
"""Emit one line per meaningful event on a mission PR.

Usage: watch_pr.py <node-id> <pr-number> [heartbeat-minutes]

Events emitted (one stdout line each, so one notification each):
  push      head commit changed  -> new commits with subjects
  comment   new issue comment    -> role tag + first line
  inline    new review comment   -> file:line + first line
  state     merged / closed      -> then exit
  heartbeat every N minutes      -> compact status, only while nothing else moved

Exits 0 when the PR is merged or closed.
"""
import json
import os
import subprocess
import sys
import time

REPO = os.environ.get("LOXPP_GH_REPO", "txloc1909/loxpp")
POLL = 90


def gh(*args):
    try:
        out = subprocess.run(
            ["gh", *args], capture_output=True, text=True, timeout=60
        )
        if out.returncode != 0:
            return None
        return json.loads(out.stdout) if out.stdout.strip() else None
    except Exception:
        return None


def first_line(body, limit=150):
    for ln in (body or "").splitlines():
        ln = ln.strip().lstrip("#* ").strip()
        if ln:
            return ln[:limit]
    return "(empty)"


def main():
    node = sys.argv[1]
    pr = sys.argv[2]
    hb_min = int(sys.argv[3]) if len(sys.argv) > 3 else 20

    # A branch name instead of a number: wait until its PR exists. This lets
    # the watcher start the moment a node spawns, before its implementer has
    # opened anything.
    if not pr.isdigit():
        branch, pr = pr, None
        while pr is None:
            found = gh(
                "pr", "list", "--repo", REPO, "--head", branch,
                "--state", "all", "--json", "number",
            )
            if found:
                pr = str(found[0]["number"])
                print(f"{node} PR #{pr} opened on {branch}", flush=True)
            else:
                time.sleep(POLL)

    tag = f"{node} #{pr}"
    seen_comments = set()
    seen_inline = set()
    head = None
    primed = False
    last_change = time.time()
    last_hb = time.time()

    while True:
        info = gh(
            "pr", "view", pr, "--repo", REPO, "--json",
            "state,headRefOid,comments,commits,mergedAt",
        )
        inline = gh("api", f"repos/{REPO}/pulls/{pr}/comments", "--paginate")

        if info is None:
            time.sleep(POLL)
            continue

        events = []

        new_head = info.get("headRefOid")
        if head is not None and new_head != head:
            subjects = [
                c["messageHeadline"] for c in (info.get("commits") or [])
            ][-3:]
            events.append(
                f"{tag} PUSH {new_head[:7]} :: " + " | ".join(subjects)
            )
        head = new_head

        for c in info.get("comments") or []:
            key = c.get("id") or c.get("url")
            if key in seen_comments:
                continue
            seen_comments.add(key)
            if primed:
                events.append(f"{tag} COMMENT {first_line(c.get('body'))}")

        for c in inline or []:
            key = c.get("id")
            if key in seen_inline:
                continue
            seen_inline.add(key)
            if primed:
                loc = f"{c.get('path')}:{c.get('line') or c.get('original_line')}"
                events.append(
                    f"{tag} INLINE {loc} :: {first_line(c.get('body'), 110)}"
                )

        state = info.get("state")
        if state != "OPEN":
            verb = "MERGED" if info.get("mergedAt") else "CLOSED"
            print(f"{tag} {verb} — node done, watcher exits", flush=True)
            return 0

        if events:
            for e in events:
                print(e, flush=True)
            last_change = time.time()
            last_hb = time.time()
        elif primed and time.time() - last_hb >= hb_min * 60:
            quiet = int((time.time() - last_change) / 60)
            print(
                f"{tag} heartbeat: open at {head[:7]}, "
                f"{len(seen_comments)} comments / {len(seen_inline)} inline, "
                f"quiet {quiet} min",
                flush=True,
            )
            last_hb = time.time()

        primed = True
        time.sleep(POLL)


if __name__ == "__main__":
    sys.exit(main())
