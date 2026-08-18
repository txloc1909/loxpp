#!/usr/bin/env python3
"""Digest a backend-DAG workflow run: one block per agent, newest activity first.

Usage:  status.py [--run <wf_id>] [--json] [--filter <substr>]
Reads the workflow transcript dir, works out each subagent's node + role from
its opening prompt, and reports what it is doing right now.

Workflow runs are searched across every Claude Code session for this repo
(PROJECT_ROOT below), not one hardcoded session id — a mission can span
several sessions, and a session id from a prior mission is never valid again.
"""

import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

PROJECT_ROOT = Path(
    os.environ.get(
        "LOXPP_CLAUDE_PROJECT_DIR",
        "/var/home/loctran/.claude/projects/-var-home-loctran-personal-loxpp",
    )
)
REPO = os.environ.get("LOXPP_GH_REPO", "txloc1909/loxpp")
PR_FILTER = os.environ.get("MISSION_BRANCH_FILTER", "")

ROLE_ICON = {"IMPLEMENTER": "IMPL", "REVIEWER": "REVW", "RESEARCHER": "RSCH"}


def newest_run() -> Path:
    runs = sorted(
        PROJECT_ROOT.glob("*/subagents/workflows/wf_*"),
        key=lambda p: p.stat().st_mtime,
    )
    if not runs:
        sys.exit("no workflow runs found under " + str(PROJECT_ROOT))
    return runs[-1]


def read_jsonl(path: Path):
    out = []
    with path.open(encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def journal_state(journal):
    """agentId -> {'done': bool, 'result': dict|None}.

    The workflow journal writes 'started' then 'result'. It never writes
    'completed', so keying on that word marks every finished agent as live.
    The 'result' record also carries the agent's validated return value, which
    is more reliable than scraping StructuredOutput out of the transcript.
    """
    state = {}
    for j in journal:
        aid = j.get("agentId")
        if not aid:
            continue
        entry = state.setdefault(aid, {"done": False, "result": None})
        if j.get("type") in ("result", "completed", "failed"):
            entry["done"] = True
            if isinstance(j.get("result"), dict):
                entry["result"] = j["result"]
    return state


def blocks(rec):
    msg = rec.get("message") or {}
    content = msg.get("content")
    return content if isinstance(content, list) else []


def tool_gist(name, inp):
    """One short line describing a tool call."""
    if not isinstance(inp, dict):
        return name
    if name == "Bash":
        d = inp.get("description") or ""
        c = " ".join((inp.get("command") or "").split())
        return f"{d} :: {c[:110]}" if d else c[:130]
    for key in ("file_path", "path", "pattern", "query", "url"):
        if key in inp:
            return f"{name} {inp[key]}"
    if name == "StructuredOutput":
        return "StructuredOutput (returning result)"
    return name


def age(ts: str) -> str:
    try:
        then = datetime.fromisoformat(ts.replace("Z", "+00:00"))
    except Exception:
        return "?"
    secs = int((datetime.now(timezone.utc) - then).total_seconds())
    if secs < 90:
        return f"{secs}s ago"
    if secs < 5400:
        return f"{secs // 60}m ago"
    return f"{secs // 3600}h{(secs % 3600) // 60:02d}m ago"


def describe(agent_id: str, path: Path, meta: dict, done: bool):
    recs = read_jsonl(path)
    info = {
        "id": agent_id[:8],
        "model": meta.get("model", "?"),
        "node": "?",
        "role": "?",
        "done": done,
        "turns": 0,
        "tools": 0,
        "last_ts": "",
        "last_tool": "",
        "last_text": "",
        "recent": [],
        "result": None,
    }
    if not recs:
        return info

    # Node + role come from the opening prompt.
    first = ""
    for r in recs[:3]:
        for b in blocks(r):
            if b.get("type") == "text":
                first += b.get("text", "")
        if isinstance((r.get("message") or {}).get("content"), str):
            first += (r.get("message") or {}).get("content", "")
    m = re.search(r"YOUR NODE:\s*(\S+)", first)
    if m:
        info["node"] = m.group(1)
    m = re.search(r"ROLE:\s*([A-Z]+)", first)
    if m:
        info["role"] = m.group(1)
    if "acting as REFEREE" in first:
        info["role"] = "RESEARCHER"
        info["node"] += "/referee"

    for r in recs:
        ts = r.get("timestamp") or ""
        if ts:
            info["last_ts"] = ts
        if r.get("type") == "assistant":
            info["turns"] += 1
            for b in blocks(r):
                if b.get("type") == "text" and b.get("text", "").strip():
                    info["last_text"] = " ".join(b["text"].split())
                elif b.get("type") == "tool_use":
                    info["tools"] += 1
                    g = tool_gist(b.get("name", "?"), b.get("input"))
                    info["last_tool"] = g
                    info["recent"].append(g)
                    if b.get("name") == "StructuredOutput":
                        info["result"] = b.get("input")
    info["recent"] = info["recent"][-6:]
    return info


def gh_prs():
    try:
        out = subprocess.run(
            ["gh", "pr", "list", "--repo", REPO, "--state", "all", "--limit", "25",
             "--json", "number,title,headRefName,state,reviewDecision,updatedAt"],
            capture_output=True, text=True, timeout=30,
        )
        if out.returncode != 0:
            return []
        prs = json.loads(out.stdout)
        if PR_FILTER:
            prs = [p for p in prs if PR_FILTER in p["headRefName"]]
        return prs
    except Exception:
        return []


def _find_run(wf_id: str) -> Path:
    matches = list(PROJECT_ROOT.glob(f"*/subagents/workflows/{wf_id}"))
    if not matches:
        sys.exit(f"no workflow run {wf_id!r} found under {PROJECT_ROOT}")
    return matches[0]


def main():
    argv = sys.argv[1:]
    run = newest_run()
    if "--run" in argv:
        run = _find_run(argv[argv.index("--run") + 1])

    journal = read_jsonl(run / "journal.jsonl")
    state = journal_state(journal)

    agents = []
    for meta_path in sorted(run.glob("agent-*.meta.json")):
        aid = meta_path.name[len("agent-"):-len(".meta.json")]
        jsonl = run / f"agent-{aid}.jsonl"
        if not jsonl.exists():
            continue
        meta = json.loads(meta_path.read_text())
        st = state.get(aid) or {"done": False, "result": None}
        info = describe(aid, jsonl, meta, st["done"])
        if st["result"]:
            info["result"] = st["result"]   # journal beats transcript scraping
        agents.append((jsonl.stat().st_mtime, info))

    agents.sort(key=lambda t: -t[0])
    prs = gh_prs()

    if "--json" in argv:
        print(json.dumps({"run": run.name, "agents": [a for _, a in agents], "prs": prs}, indent=1))
        return

    print(f"RUN {run.name}   agents={len(agents)}  live={sum(1 for _, a in agents if not a['done'])}")
    print()
    for _, a in agents:
        flag = "DONE" if a["done"] else "LIVE"
        head = (f"[{flag}] {a['node']:<12} {ROLE_ICON.get(a['role'], a['role']):<4} "
                f"{a['model']:<7} turns={a['turns']:<3} tools={a['tools']:<4} {age(a['last_ts'])}")
        print(head)
        if a["last_text"]:
            print(f"       say : {a['last_text'][:200]}")
        if a["last_tool"]:
            print(f"       do  : {a['last_tool'][:200]}")
        if a["result"]:
            r = a["result"]
            keys = [k for k in ("status", "approved", "merged", "resolved", "mission_status", "pr") if k in r]
            print("       RES : " + ", ".join(f"{k}={r[k]}" for k in keys))
            if r.get("summary"):
                print(f"       sum : {' '.join(str(r['summary']).split())[:300]}")
        print()

    if prs:
        print("PULL REQUESTS")
        for p in prs:
            print(f"  #{p['number']:<4} {p['state']:<6} {p['headRefName']:<32} {p['title'][:60]}")


if __name__ == "__main__":
    main()
