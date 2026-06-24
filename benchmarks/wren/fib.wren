var fibonacci
fibonacci = Fn.new { |n|
  if (n > 1) return fibonacci.call(n - 1) + fibonacci.call(n - 2)
  return n
}
var start = System.clock
var result = fibonacci.call(35)
var elapsed = System.clock - start
System.print(result)
System.print(elapsed)
