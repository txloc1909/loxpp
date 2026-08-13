var mandelbrot = Fn.new { |size|
  var sum = 0
  var y = 0
  while (y < size) {
    var ci = (2.0 * y / size) - 1.0
    var x = 0
    while (x < size) {
      var zr = 0.0
      var zi = 0.0
      var cr = (2.0 * x / size) - 1.5
      var escape = false
      var z = 0
      while (z < 50) {
        if (!escape) {
          var tr = zr * zr - zi * zi + cr
          var ti = 2.0 * zr * zi + ci
          zr = tr
          zi = ti
          if (zr * zr + zi * zi > 4.0) escape = true
        }
        z = z + 1
      }
      if (!escape) sum = sum + 1
      x = x + 1
    }
    y = y + 1
  }
  return sum
}
var start = System.clock
var result = mandelbrot.call(100)
var elapsed = System.clock - start
System.print(result)
System.print(elapsed)
