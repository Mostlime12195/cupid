-- Pure-Lua benchmark (the Lua parts of tools/bench plus numeric kernels).
-- Optional arguments: a filter on benchmark names, and the number of timed
-- runs (more when profiling one benchmark).
local clock = os.clock
local only, runs = arg and arg[1], tonumber(arg and arg[2]) or 5
local function bench(name, fn)
  if only and not name:find(only, 1, true) then return end
  fn()
  local best = math.huge
  for _ = 1, runs do local t = clock(); fn(); best = math.min(best, clock() - t) end
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
-- Game-like workloads.
bench("entities update 5k", function()
  -- Objects with methods and an inherited method, field reads and writes.
  local Entity = {}; Entity.__index = Entity
  function Entity.new(x, y) return setmetatable({ x = x, y = y, vx = 1, vy = 0.5, alive = true }, Entity) end
  function Entity:speed() return math.sqrt(self.vx * self.vx + self.vy * self.vy) end
  function Entity:update(dt)
    self.x = self.x + self.vx * dt
    self.y = self.y + self.vy * dt
    if self.x > 800 then self.x = 0 end
    if self:speed() > 100 then self.alive = false end
  end
  local Enemy = setmetatable({}, { __index = Entity }); Enemy.__index = Enemy
  function Enemy.new(x, y) local e = Entity.new(x, y); e.hp = 3; return setmetatable(e, Enemy) end
  local es = {}
  for i = 1, 5000 do es[i] = (i % 2 == 0) and Entity.new(i, i) or Enemy.new(i, i) end
  for _ = 1, 20 do
    for i = 1, #es do es[i]:update(0.016) end
  end
end)
bench("vector math 100k", function()
  -- Small tables as 2D vectors: allocation, calls, math.sqrt.
  local function vec(x, y) return { x = x, y = y } end
  local function add(a, b) return vec(a.x + b.x, a.y + b.y) end
  local function scale(a, s) return vec(a.x * s, a.y * s) end
  local function len(a) return math.sqrt(a.x * a.x + a.y * a.y) end
  local function normalize(a) local l = len(a); return l > 0 and scale(a, 1 / l) or a end
  local p, total = vec(0, 0), 0
  for i = 1, 100000 do
    local d = normalize(vec(i % 7 - 3, i % 5 - 2))
    p = add(p, scale(d, 0.5))
    total = total + len(p)
  end
end)
bench("collisions 300", function()
  -- All-pairs AABB overlap tests over objects with fields.
  local boxes = {}
  for i = 1, 300 do
    boxes[i] = { x = (i * 37) % 640, y = (i * 91) % 480, w = 16 + i % 9, h = 16 + i % 7, hits = 0 }
  end
  for _ = 1, 5 do
    for i = 1, #boxes do
      local a = boxes[i]
      for j = i + 1, #boxes do
        local b = boxes[j]
        if a.x < b.x + b.w and b.x < a.x + a.w and a.y < b.y + b.h and b.y < a.y + a.h then
          a.hits = a.hits + 1
          b.hits = b.hits + 1
        end
      end
    end
  end
end)
