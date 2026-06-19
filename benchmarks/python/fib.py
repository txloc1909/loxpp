import sys, time
sys.setrecursionlimit(100000)
def fibonacci(n):
    if n > 1:
        return fibonacci(n - 1) + fibonacci(n - 2)
    return n
start = time.perf_counter()
result = fibonacci(35)
elapsed = time.perf_counter() - start
print(result)
print(elapsed)
