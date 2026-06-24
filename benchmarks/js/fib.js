function fibonacci(n) {
  if (n > 1) return fibonacci(n - 1) + fibonacci(n - 2);
  return n;
}
const start = process.hrtime.bigint();
const result = fibonacci(35);
const elapsed = Number(process.hrtime.bigint() - start) / 1e9;
console.log(result);
console.log(elapsed);
