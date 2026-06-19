var count = 0
var absn = Fn.new { |n| n < 0 ? -n : n }
var ok = Fn.new { |c, prev, dist| c != prev && absn.call(c - prev) != dist }
var q8 = Fn.new { |c0,c1,c2,c3,c4,c5,c6|
  var c = 0
  while (c < 8) {
    if (ok.call(c,c6,1) && ok.call(c,c5,2) && ok.call(c,c4,3) && ok.call(c,c3,4) && ok.call(c,c2,5) && ok.call(c,c1,6) && ok.call(c,c0,7)) count = count + 1
    c = c + 1
  }
}
var q7 = Fn.new { |c0,c1,c2,c3,c4,c5|
  var c = 0
  while (c < 8) {
    if (ok.call(c,c5,1) && ok.call(c,c4,2) && ok.call(c,c3,3) && ok.call(c,c2,4) && ok.call(c,c1,5) && ok.call(c,c0,6)) q8.call(c0,c1,c2,c3,c4,c5,c)
    c = c + 1
  }
}
var q6 = Fn.new { |c0,c1,c2,c3,c4|
  var c = 0
  while (c < 8) {
    if (ok.call(c,c4,1) && ok.call(c,c3,2) && ok.call(c,c2,3) && ok.call(c,c1,4) && ok.call(c,c0,5)) q7.call(c0,c1,c2,c3,c4,c)
    c = c + 1
  }
}
var q5 = Fn.new { |c0,c1,c2,c3|
  var c = 0
  while (c < 8) {
    if (ok.call(c,c3,1) && ok.call(c,c2,2) && ok.call(c,c1,3) && ok.call(c,c0,4)) q6.call(c0,c1,c2,c3,c)
    c = c + 1
  }
}
var q4 = Fn.new { |c0,c1,c2|
  var c = 0
  while (c < 8) {
    if (ok.call(c,c2,1) && ok.call(c,c1,2) && ok.call(c,c0,3)) q5.call(c0,c1,c2,c)
    c = c + 1
  }
}
var q3 = Fn.new { |c0,c1|
  var c = 0
  while (c < 8) {
    if (ok.call(c,c1,1) && ok.call(c,c0,2)) q4.call(c0,c1,c)
    c = c + 1
  }
}
var q2 = Fn.new { |c0|
  var c = 0
  while (c < 8) {
    if (ok.call(c,c0,1)) q3.call(c0,c)
    c = c + 1
  }
}
var q1 = Fn.new {
  var c = 0
  while (c < 8) {
    q2.call(c)
    c = c + 1
  }
}
var start = System.clock
q1.call()
var elapsed = System.clock - start
System.print(count)
System.print(elapsed)
