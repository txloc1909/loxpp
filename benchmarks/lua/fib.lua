local function fibonacci(n)
  if n > 1 then return fibonacci(n - 1) + fibonacci(n - 2) end
  return n
end
local start = os.clock()
local result = fibonacci(35)
local elapsed = os.clock() - start
print(result)
print(elapsed)
