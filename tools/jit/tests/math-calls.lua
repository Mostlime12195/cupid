-- Calls of math.floor, ceil, sqrt, abs, min and max are compiled to
-- WebAssembly instructions when the callee and arguments are what they
-- seem. Every other case must go through the real call.
local function show(...)
  local t = { ... }
  for i = 1, select("#", ...) do
    local v = t[i]
    if type(v) == "number" then
      if v ~= v then t[i] = "nan"
      elseif v == 0 then t[i] = (1 / v < 0) and "-0" or "0"
      else t[i] = string.format("%.14g", v) end
    else t[i] = tostring(v) end
  end
  print(table.concat(t, " "))
end

-- Through the math table, with every kind of number.
local values = { 0, -0, 1, -1, 0.5, -0.5, 2.5, -2.5, 1e308, -1e308, 2^53, 1/0, -1/0, 0/0, 16, 2, 1e-320 }
local function all(x, y)
  return math.floor(x), math.ceil(x), math.sqrt(x), math.abs(x), math.min(x, y), math.max(x, y),
         math.min(y, x), math.max(y, x)
end
for round = 1, 3 do
  for i = 1, #values do
    for j = 1, #values do show(all(values[i], values[j])) end
  end
end

-- Local and upvalue aliases, in a loop, with the result used as a number.
local floor, ceil, sqrt, abs, min, max = math.floor, math.ceil, math.sqrt, math.abs, math.min, math.max
local function sum(n)
  local s = 0
  for i = 1, n do
    local x = i * 0.37 - 20
    s = s + floor(x) + ceil(x) + sqrt(abs(x)) + min(x, 3) * max(x, -3) + floor(x / 3) % 5
  end
  return s
end
for i = 1, 60 do sum(10) end
show(sum(1000))

-- Arguments that aren't numbers: strings that convert, and errors.
local function f(x) return floor(x), sqrt(x), abs(x) end
for i = 1, 60 do f(i) end
show(f("16"))
show(pcall(f, nil))
show(pcall(f, {}))
show(pcall(f, "x"))
show(pcall(f))
local function g(x, y) return min(x, y), max(x, y) end
for i = 1, 60 do g(i, 3) end
show(g("7", 3))
show(g(7, "3"))
show(pcall(g, 1, nil))
show(pcall(g, nil, 1))
show(pcall(g, {}, 1))

-- Other numbers of arguments and results.
local function h(a, b, c)
  local r1 = min(a)
  local r2 = max(a, b, c)
  local r3, r4 = floor(a)
  local t = { floor(a, b), sqrt(b) }
  floor(a)
  return r1, r2, r3, r4, #t, t[1], t[2], (abs(c)), min(a, b, c), floor(max(a, b))
end
for i = 1, 60 do h(i, 2, 3) end
show(h(5.5, 9, -2))
local function v(...) return floor(...), (max(...)) end
for i = 1, 60 do v(i + 0.5) end
show(v(2.5))
show(pcall(v))
show(v(1.5, 7))

-- The function changes while the compiled code runs.
local function k(n)
  local s = 0
  for i = 1, n do
    s = s + math.floor(i / 2) + math.sqrt(i)
    if i == n / 2 then
      math.floor = function(x) return x * 10 end
      math.sqrt = setmetatable({}, { __call = function(_, x) return -x end })
    end
  end
  return s
end
for i = 1, 60 do math.floor, math.sqrt = floor, sqrt; k(8) end
math.floor, math.sqrt = floor, sqrt
show(k(8))
math.floor, math.sqrt = floor, sqrt

-- Something else stored under a math function's name.
local lib = { floor = function(x) return "floor " .. tostring(x) end, sqrt = "not callable", abs = math.ceil,
              min = math.max, max = function(...) return select("#", ...) end }
local function other(x)
  return lib.floor(x), lib.abs(x), lib.min(x, 2), lib.max(x, 2), pcall(function() return lib.sqrt(x) end)
end
for i = 1, 60 do other(i) end
show(other(1.5))
local swapped = math.ceil
local function up(x) return swapped(x) end
for i = 1, 60 do up(i + 0.5) end
swapped = math.floor
show(up(1.5))
swapped = function(x) return "lua " .. x end
show(up(1.5))

-- A call hook sees the calls.
local calls = 0
local function hooked(n)
  local s = 0
  for i = 1, n do s = s + floor(i / 3) + min(i, 7) end
  return s
end
for i = 1, 60 do hooked(5) end
debug.sethook(function() calls = calls + 1 end, "c")
local r = hooked(50)
debug.sethook()
show(r, calls)

-- Results of calls feeding speculation, then a non-number.
local function spec(xs)
  local s = 0
  for i = 1, #xs do
    local q = floor(xs[i])
    s = s + q * 2 + sqrt(q + 1)
  end
  return s
end
local xs = {}
for i = 1, 200 do xs[i] = i * 1.25 end
for i = 1, 5 do show(spec(xs)) end
xs[100] = "8"
show(spec(xs))
xs[100] = setmetatable({}, { __add = function() return 1 end })
show(pcall(spec, xs))

-- In coroutines, and with arguments computed by calls.
local co = coroutine.wrap(function(n)
  local s = 0
  for i = 1, n do
    s = s + floor(sqrt(i * i + 1)) + max(abs(-i), min(i, 2))
    if i % 25 == 0 then coroutine.yield(s) end
  end
  return s
end)
show(co(100), co(), co(), co(), co())

-- Functions held in locals of the calling function, named like the
-- functions, misleadingly named, and reassigned.
local function locals(n)
  local floor, sqrt, abs, min, max, ceil = math.floor, math.sqrt, math.abs, math.min, math.max, math.ceil
  local s = 0
  for i = 1, n do
    local x = i * 0.61 - 30
    s = s + floor(x) + sqrt(abs(x)) + min(x, 4) - max(x, -4) + ceil(x / 7)
  end
  local sqrt2, floor2 = floor, tostring  -- (names say nothing about these)
  do
    local floor, abs, min = math.ceil, function(x) return x + 100 end, math.max
    s = s + floor(2.5) + abs(-1) + min(1, 2)
    floor = "str"
    s = s + #floor
  end
  return s, sqrt2(7.9), floor2(1.5)
end
for i = 1, 60 do locals(5) end
show(locals(500))
