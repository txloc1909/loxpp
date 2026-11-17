import json, os, resource, subprocess, sys, time, statistics
import numpy as np
S = os.path.dirname(os.path.abspath(__file__))
BIN = "/home/user/loxpp/build/loxpp"
PROGS = ["live_then_churn", "list_write", "big_map", "long_chain", "list_move"]
MODES = ["stw", "dijkstra", "yuasa"]

def load(path):
    iv, kinds, start, end, peak, emerg = [], [], None, None, 0, 0
    for line in open(path):
        t = line.split()
        if not t: continue
        if t[0] == "start": start = int(t[1])
        elif t[0] == "end": end = int(t[1])
        elif t[0] == "peak_bytes": peak = int(t[1])
        elif t[0] == "emergency_steps": emerg = int(t[1])
        elif t[0] == "p": iv.append((int(t[1]), int(t[2]))); kinds.append(t[3])
    return start, end, peak, emerg, np.array(iv, dtype=np.int64).reshape(-1, 2), np.array(kinds)

def merge(iv):
    if len(iv) == 0: return iv
    iv = iv[np.argsort(iv[:, 0])]
    out = [list(iv[0])]
    for a, b in iv[1:]:
        if a <= out[-1][1]: out[-1][1] = max(out[-1][1], b)
        else: out.append([a, b])
    return np.array(out, dtype=np.int64)

def mmu(iv, start, end, w):
    if len(iv) == 0: return 1.0
    s, e = iv[:, 0], iv[:, 1]
    cum = np.concatenate(([0], np.cumsum(e - s)))
    def covered(a, b):
        i_hi = np.searchsorted(s, b, side="left"); i_lo = np.searchsorted(e, a, side="right")
        tot = cum[i_hi] - cum[i_lo]; has = i_hi > i_lo
        lt = np.where(has, np.maximum(0, a - s[np.minimum(i_lo, len(s) - 1)]), 0)
        rt = np.where(has, np.maximum(0, e[np.maximum(i_hi - 1, 0)] - b), 0)
        return tot - lt - rt
    a = np.concatenate((s, e - w)); a = a[(a >= start) & (a + w <= end)]
    if len(a) == 0: return 1.0
    return 1.0 - covered(a, a + w).max() / w

def metrics(path):
    start, end, peak, emerg, iv, kinds = load(path)
    wall = (end - start) / 1e6
    out = {"wall_ms": wall, "peak_bytes": peak, "emergency": emerg}
    gc = iv[kinds != "A"] if len(iv) else iv
    allm = merge(iv); gcm = merge(gc)
    if len(gc):
        d = gc[:, 1] - gc[:, 0]
        # single step lengths (not merged): what one step costs
        out.update(step_max_us=d.max()/1e3, step_p999_us=float(np.percentile(d, 99.9))/1e3,
                   step_p9999_us=float(np.percentile(d, 99.99))/1e3, steps=len(d))
        out["gc_share"] = float((gcm[:, 1] - gcm[:, 0]).sum()) / (end - start)
    for w in (1, 10, 100):
        out[f"mmu{w}_gc"] = mmu(gcm, start, end, w * 1_000_000)
        out[f"mmu{w}_all"] = mmu(allm, start, end, w * 1_000_000)
    if len(iv):
        out["stall_all_max_us"] = float((allm[:, 1] - allm[:, 0]).max()) / 1e3
    return out

def run(prog, mode, log):
    env = dict(os.environ, LOXPP_GC_MODE=mode)
    env.pop("LOXPP_GC_VERIFY", None)
    if log: env["LOXPP_GC_PAUSELOG"] = log
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t = time.time()
    p = subprocess.run([BIN, f"/home/user/loxpp/benchmarks/gc_large_heap/{prog}.lox"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=900)
    wall = time.time() - t
    return wall, p.returncode, p.stderr.decode()[-200:]

REPS = int(sys.argv[1]) if len(sys.argv) > 1 else 3
res = []
for prog in PROGS:
    for mode in MODES:
        logged = []
        for r in range(REPS):
            log = f"{S}/m.{prog}.{mode}.{r}.pl"
            run(prog, mode, log)
            logged.append(metrics(log))
        walls = []
        for r in range(REPS):
            w, rc, err = run(prog, mode, None)
            assert rc == 0, (prog, mode, err)
            walls.append(w)
        med = lambda k: statistics.median(m[k] for m in logged if k in m)
        row = {"prog": prog, "mode": mode, "wall_s_nolog": statistics.median(walls), "walls": walls}
        for k in ("step_max_us","step_p999_us","step_p9999_us","stall_all_max_us","gc_share","mmu1_gc","mmu10_gc","mmu100_gc","mmu10_all","mmu100_all","peak_bytes","emergency","steps"):
            row[k] = med(k)
        res.append(row)
        print(json.dumps(row), flush=True)
