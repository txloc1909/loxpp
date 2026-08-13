import sys, time
sys.setrecursionlimit(100000)
moves_done = 0
def hanoi(n, frm, to, via):
    global moves_done
    if n == 0:
        return
    hanoi(n - 1, frm, via, to)
    moves_done += 1
    hanoi(n - 1, via, to, frm)
start = time.perf_counter()
hanoi(20, 1, 3, 2)
elapsed = time.perf_counter() - start
print(moves_done)
print(elapsed)
