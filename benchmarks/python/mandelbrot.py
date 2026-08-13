import time
def mandelbrot(size):
    total = 0
    y = 0
    while y < size:
        ci = (2.0 * y / size) - 1.0
        x = 0
        while x < size:
            zr = 0.0; zi = 0.0
            cr = (2.0 * x / size) - 1.5
            escape = False
            z = 0
            while z < 50:
                if not escape:
                    tr = zr * zr - zi * zi + cr
                    ti = 2.0 * zr * zi + ci
                    zr = tr; zi = ti
                    if zr * zr + zi * zi > 4.0:
                        escape = True
                z += 1
            if not escape:
                total += 1
            x += 1
        y += 1
    return total
start = time.perf_counter()
result = mandelbrot(100)
elapsed = time.perf_counter() - start
print(result)
print(elapsed)
