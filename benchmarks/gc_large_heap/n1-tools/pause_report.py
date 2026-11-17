import sys
import numpy as np

def load(path):
    iv = []
    kinds = []
    start = end = None
    peak = 0
    with open(path) as f:
        for line in f:
            t = line.split()
            if not t:
                continue
            if t[0] == "start":
                start = int(t[1])
            elif t[0] == "end":
                end = int(t[1])
            elif t[0] == "peak_bytes":
                peak = int(t[1])
            elif t[0] == "p":
                iv.append((int(t[1]), int(t[2])))
                kinds.append(t[3] if len(t) > 3 else "?")
    return start, end, peak, np.array(iv, dtype=np.int64), kinds

def mmu(iv, start, end, w):
    # worst window of length w: it starts at a pause start or ends at a pause end
    if len(iv) == 0:
        return 1.0
    s, e = iv[:, 0], iv[:, 1]
    cum = np.concatenate(([0], np.cumsum(e - s)))
    def covered(a, b):
        # total pause time inside [a, b) for sorted disjoint intervals
        # intervals fully before b: index by searchsorted on starts
        i_hi = np.searchsorted(s, b, side="left")      # intervals with start < b
        i_lo = np.searchsorted(e, a, side="right")     # intervals with end > a
        tot = cum[i_hi] - cum[i_lo]
        # trim the partial at the left edge and at the right edge
        has = i_hi > i_lo
        left_trim = np.where(has, np.maximum(0, a - s[np.minimum(i_lo, len(s) - 1)]), 0)
        right_trim = np.where(has, np.maximum(0, e[np.maximum(i_hi - 1, 0)] - b), 0)
        return tot - left_trim - right_trim
    a1 = s.copy()
    a2 = e - w
    a = np.concatenate((a1, a2))
    a = a[(a >= start) & (a + w <= end)]
    if len(a) == 0:
        return 1.0 - (e - s).sum() / max(1, end - start)
    worst = covered(a, a + w).max()
    return 1.0 - worst / w

def main(path):
    start, end, peak, iv, kinds = load(path)
    if len(iv):
        dd = iv[:,1]-iv[:,0]
        for k in sorted(set(kinds)):
            m = np.array([x==k for x in kinds])
            print(f"  kind {k}: n={m.sum()} max={dd[m].max()/1e3:.0f}us total={dd[m].sum()/1e6:.0f}ms")
    # merge overlapping or touching intervals
    if len(iv):
        iv = iv[np.argsort(iv[:, 0])]
        merged = [list(iv[0])]
        for a, b in iv[1:]:
            if a <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], b)
            else:
                merged.append([a, b])
        iv = np.array(merged, dtype=np.int64)
    d = (iv[:, 1] - iv[:, 0]) if len(iv) else np.array([0])
    wall = (end - start) / 1e6
    print(f"wall {wall:.0f} ms  pauses {len(iv)}  total {d.sum()/1e6:.0f} ms ({100*d.sum()/(end-start):.1f}%)")
    print(f"pause max {d.max()/1e3:.0f} us  p99 {np.percentile(d,99)/1e3:.0f} us  p50 {np.percentile(d,50)/1e3:.1f} us")
    print("MMU " + "  ".join(f"{w}ms:{100*mmu(iv,start,end,w*1_000_000):.1f}%" for w in (1, 10, 100)))
    print(f"peak heap {peak/1e6:.0f} MB")

main(sys.argv[1])
