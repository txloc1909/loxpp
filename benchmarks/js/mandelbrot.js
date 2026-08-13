function mandelbrot(size) {
  let sum = 0;
  let y = 0;
  while (y < size) {
    const ci = (2.0 * y / size) - 1.0;
    let x = 0;
    while (x < size) {
      let zr = 0.0, zi = 0.0;
      const cr = (2.0 * x / size) - 1.5;
      let escape = false;
      let z = 0;
      while (z < 50) {
        if (!escape) {
          const tr = zr * zr - zi * zi + cr;
          const ti = 2.0 * zr * zi + ci;
          zr = tr; zi = ti;
          if (zr * zr + zi * zi > 4.0) escape = true;
        }
        z += 1;
      }
      if (!escape) sum += 1;
      x += 1;
    }
    y += 1;
  }
  return sum;
}
const start = process.hrtime.bigint();
const result = mandelbrot(100);
const elapsed = Number(process.hrtime.bigint() - start) / 1e9;
console.log(result);
console.log(elapsed);
