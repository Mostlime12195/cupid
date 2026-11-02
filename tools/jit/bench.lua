-- Pure-Lua benchmark (the Lua parts of tools/bench plus numeric kernels).
local clock = os.clock
local function bench(name, fn)
  fn()
  local best = math.huge
  for _ = 1, 5 do local t = clock(); fn(); best = math.min(best, clock() - t) end
  print(("%-24s %8.1f ms"):format(name, best * 1000))
end
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
bench("fib(27)", function() fib(27) end)
bench("tables+sort", function()
  local t = {}
  for i = 1, 200000 do t[i] = (i * 7919) % 100003 end
  table.sort(t)
end)
bench("strings", function()
  local parts = {}
  for i = 1, 50000 do parts[#parts + 1] = string.format("%d:%s", i, ("x"):rep(i % 10)) end
  local s = table.concat(parts, ",")
  local n = 0
  for _ in s:gmatch("%d+") do n = n + 1 end
end)
bench("closures/oop", function()
  local V = {}; V.__index = V
  local function new(x, y) return setmetatable({ x = x, y = y }, V) end
  function V.add(a, b) return new(a.x + b.x, a.y + b.y) end
  local acc = new(0, 0)
  for i = 1, 300000 do acc = acc:add(new(i, -i)) end
end)
bench("mandelbrot 200", function()
  local n, count = 200, 0
  for y = 0, n - 1 do
    local ci = 2 * y / n - 1
    for x = 0, n - 1 do
      local cr, zr, zi = 2 * x / n - 1.5, 0, 0
      local i = 0
      while i < 50 and zr * zr + zi * zi < 4 do
        zr, zi = zr * zr - zi * zi + cr, 2 * zr * zi + ci
        i = i + 1
      end
      if i == 50 then count = count + 1 end
    end
  end
end)
bench("particles update 10k", function()
  local ps = {}
  for i = 1, 10000 do ps[i] = { x = i, y = i * 2, vx = 1, vy = -1, life = 3 } end
  for _ = 1, 20 do
    for i = 1, #ps do
      local p = ps[i]
      p.x = p.x + p.vx * 0.016
      p.y = p.y + p.vy * 0.016
      p.vy = p.vy + 9.8 * 0.016
      p.life = p.life - 0.016
    end
  end
end)
bench("sieve 1e6", function()
  local n, flags = 1000000, {}
  for i = 2, n do flags[i] = true end
  for i = 2, math.floor(math.sqrt(n)) do
    if flags[i] then for j = i * i, n, i do flags[j] = false end end
  end
end)
