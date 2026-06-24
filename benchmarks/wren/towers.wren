var movesDone = 0
var hanoi
hanoi = Fn.new { |n, from, to, via|
  if (n == 0) return
  hanoi.call(n - 1, from, via, to)
  movesDone = movesDone + 1
  hanoi.call(n - 1, via, to, from)
}
var start = System.clock
hanoi.call(20, 1, 3, 2)
var elapsed = System.clock - start
System.print(movesDone)
System.print(elapsed)
