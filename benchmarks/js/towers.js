let movesDone = 0;
function hanoi(n, from, to, via) {
  if (n === 0) return;
  hanoi(n - 1, from, via, to);
  movesDone += 1;
  hanoi(n - 1, via, to, from);
}
const start = process.hrtime.bigint();
hanoi(20, 1, 3, 2);
const elapsed = Number(process.hrtime.bigint() - start) / 1e9;
console.log(movesDone);
console.log(elapsed);
