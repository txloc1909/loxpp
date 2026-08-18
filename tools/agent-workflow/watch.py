#!/usr/bin/env python3
"""Emit one line per meaningful backend-DAG mission transition.

Selective by design: agent start, agent finish (with its returned status),
PR opened/merged/closed, reviewer approval, plus a heartbeat so silence is
never ambiguous. Everything else stays quiet.

Set MISSION_BRANCH_FILTER to restrict PR tracking to this mission's branches
(e.g. "clr"); empty means track every PR in the repo.
"""

import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import status as S  # noqa: E402

POLL = 60
HEARTBEAT = 900
REPO = os.environ.get("LOXPP_GH_REPO", "txloc1909/loxpp")
PR_FILTER = os.environ.get("MISSION_BRANCH_FILTER", "")


def emit(line):
    print(line, flush=True)


def snapshot():
    run = S.newest_run()
    state = S.journal_state(S.read_jsonl(run / "journal.jsonl"))

    agents = {}
    for meta_path in run.glob("agent-*.meta.json"):
        aid = meta_path.name[len("agent-"):-len(".meta.json")]
        jsonl = run / f"agent-{aid}.jsonl"
        if not jsonl.exists():
            continue
        meta = json.loads(meta_path.read_text())
        st = state.get(aid) or {"done": False, "result": None}
        info = S.describe(aid, jsonl, meta, st["done"])
        if st["result"]:
            info["result"] = st["result"]
        agents[aid] = info
    return run.name, agents


def pr_snapshot():
    out = {}
    try:
        r = subprocess.run(
            ["gh", "pr", "list", "--repo", REPO, "--state", "all", "--limit", "30",
             "--json", "number,title,headRefName,state"],
            capture_output=True, text=True, timeout=40,
        )
        if r.returncode == 0:
            for p in json.loads(r.stdout):
                if not PR_FILTER or PR_FILTER in p["headRefName"]:
                    out[p["number"]] = (p["state"], p["headRefName"], p["title"])
    except Exception:
        pass
    return out


def result_line(a):
    r = a.get("result") or {}
    bits = []
    for k in ("status", "approved", "merged", "resolved", "mission_status", "pr"):
        if k in r:
            bits.append(f"{k}={r[k]}")
    tail = " ".join(str(r.get("summary", "")).split())[:160]
    return (", ".join(bits) + (" | " + tail if tail else "")) or "no structured result"


def main():
    seen_agents = {}
    seen_prs = pr_snapshot()
    last_beat = time.time()

    run, agents = snapshot()
    for aid, a in agents.items():
        seen_agents[aid] = a["done"]
    emit(f"WATCH armed on {run}: {len(agents)} agent(s), {len(seen_prs)} PR(s)")

    while True:
        time.sleep(POLL)
        try:
            run, agents = snapshot()
        except Exception as e:  # never die on a transient read
            emit(f"WATCH read error: {e}")
            continue

        for aid, a in agents.items():
            tag = f"{a['node']}/{a['role'][:4]}"
            if aid not in seen_agents:
                emit(f"START  {tag} ({a['model']}) began work")
                seen_agents[aid] = a["done"]
                continue
            if a["done"] and not seen_agents[aid]:
                emit(f"FINISH {tag} after {a['turns']} turns / {a['tools']} tools -> {result_line(a)}")
            seen_agents[aid] = a["done"]

        prs = pr_snapshot()
        for num, (st, branch, title) in prs.items():
            if num not in seen_prs:
                emit(f"PR     #{num} OPENED  {branch} :: {title[:70]}")
            elif seen_prs[num][0] != st:
                emit(f"PR     #{num} {st}  {branch}")
        if prs:
            seen_prs = prs

        if time.time() - last_beat >= HEARTBEAT:
            last_beat = time.time()
            live = [a for a in agents.values() if not a["done"]]
            if live:
                parts = [f"{a['node']}/{a['role'][:4]} t{a['turns']}" for a in live]
                emit("STATUS live: " + ", ".join(parts))
            else:
                emit("STATUS no live agents (workflow may be between stages)")


if __name__ == "__main__":
    main()
