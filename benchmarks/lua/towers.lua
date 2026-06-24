local movesDone = 0
local function hanoi(n, from, to, via)
  if n == 0 then return end
  hanoi(n - 1, from, via, to)
  movesDone = movesDone + 1
  hanoi(n - 1, via, to, from)
end
local start = os.clock()
hanoi(20, 1, 3, 2)
local elapsed = os.clock() - start
print(movesDone)
print(elapsed)
