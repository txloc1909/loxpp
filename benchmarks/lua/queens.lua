local count = 0
local function absn(n) if n < 0 then return -n end return n end
local function ok(c, prev, dist) return c ~= prev and absn(c - prev) ~= dist end
local function q8(c0,c1,c2,c3,c4,c5,c6)
  local c = 0
  while c < 8 do
    if ok(c,c6,1) and ok(c,c5,2) and ok(c,c4,3) and ok(c,c3,4) and ok(c,c2,5) and ok(c,c1,6) and ok(c,c0,7) then count = count + 1 end
    c = c + 1
  end
end
local function q7(c0,c1,c2,c3,c4,c5)
  local c = 0
  while c < 8 do
    if ok(c,c5,1) and ok(c,c4,2) and ok(c,c3,3) and ok(c,c2,4) and ok(c,c1,5) and ok(c,c0,6) then q8(c0,c1,c2,c3,c4,c5,c) end
    c = c + 1
  end
end
local function q6(c0,c1,c2,c3,c4)
  local c = 0
  while c < 8 do
    if ok(c,c4,1) and ok(c,c3,2) and ok(c,c2,3) and ok(c,c1,4) and ok(c,c0,5) then q7(c0,c1,c2,c3,c4,c) end
    c = c + 1
  end
end
local function q5(c0,c1,c2,c3)
  local c = 0
  while c < 8 do
    if ok(c,c3,1) and ok(c,c2,2) and ok(c,c1,3) and ok(c,c0,4) then q6(c0,c1,c2,c3,c) end
    c = c + 1
  end
end
local function q4(c0,c1,c2)
  local c = 0
  while c < 8 do
    if ok(c,c2,1) and ok(c,c1,2) and ok(c,c0,3) then q5(c0,c1,c2,c) end
    c = c + 1
  end
end
local function q3(c0,c1)
  local c = 0
  while c < 8 do
    if ok(c,c1,1) and ok(c,c0,2) then q4(c0,c1,c) end
    c = c + 1
  end
end
local function q2(c0)
  local c = 0
  while c < 8 do
    if ok(c,c0,1) then q3(c0,c) end
    c = c + 1
  end
end
local function q1()
  local c = 0
  while c < 8 do q2(c) c = c + 1 end
end
local start = os.clock()
q1()
local elapsed = os.clock() - start
print(count)
print(elapsed)
