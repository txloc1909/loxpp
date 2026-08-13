import time
count = 0
def absn(n):
    return -n if n < 0 else n
def ok(c, prev, dist):
    return c != prev and absn(c - prev) != dist
def q8(c0,c1,c2,c3,c4,c5,c6):
    global count
    c = 0
    while c < 8:
        if ok(c,c6,1) and ok(c,c5,2) and ok(c,c4,3) and ok(c,c3,4) and ok(c,c2,5) and ok(c,c1,6) and ok(c,c0,7):
            count += 1
        c += 1
def q7(c0,c1,c2,c3,c4,c5):
    c = 0
    while c < 8:
        if ok(c,c5,1) and ok(c,c4,2) and ok(c,c3,3) and ok(c,c2,4) and ok(c,c1,5) and ok(c,c0,6):
            q8(c0,c1,c2,c3,c4,c5,c)
        c += 1
def q6(c0,c1,c2,c3,c4):
    c = 0
    while c < 8:
        if ok(c,c4,1) and ok(c,c3,2) and ok(c,c2,3) and ok(c,c1,4) and ok(c,c0,5):
            q7(c0,c1,c2,c3,c4,c)
        c += 1
def q5(c0,c1,c2,c3):
    c = 0
    while c < 8:
        if ok(c,c3,1) and ok(c,c2,2) and ok(c,c1,3) and ok(c,c0,4):
            q6(c0,c1,c2,c3,c)
        c += 1
def q4(c0,c1,c2):
    c = 0
    while c < 8:
        if ok(c,c2,1) and ok(c,c1,2) and ok(c,c0,3):
            q5(c0,c1,c2,c)
        c += 1
def q3(c0,c1):
    c = 0
    while c < 8:
        if ok(c,c1,1) and ok(c,c0,2):
            q4(c0,c1,c)
        c += 1
def q2(c0):
    c = 0
    while c < 8:
        if ok(c,c0,1):
            q3(c0,c)
        c += 1
def q1():
    c = 0
    while c < 8:
        q2(c)
        c += 1
start = time.perf_counter()
q1()
elapsed = time.perf_counter() - start
print(count)
print(elapsed)
