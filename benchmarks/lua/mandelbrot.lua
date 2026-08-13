local function mandelbrot(size)
  local sum = 0
  local y = 0
  while y < size do
    local ci = (2.0 * y / size) - 1.0
    local x = 0
    while x < size do
      local zr = 0.0; local zi = 0.0
      local cr = (2.0 * x / size) - 1.5
      local escape = false
      local z = 0
      while z < 50 do
        if not escape then
          local tr = zr * zr - zi * zi + cr
          local ti = 2.0 * zr * zi + ci
          zr = tr; zi = ti
          if zr * zr + zi * zi > 4.0 then escape = true end
        end
        z = z + 1
      end
      if not escape then sum = sum + 1 end
      x = x + 1
    end
    y = y + 1
  end
  return sum
end
local start = os.clock()
local result = mandelbrot(100)
local elapsed = os.clock() - start
print(result)
print(elapsed)
