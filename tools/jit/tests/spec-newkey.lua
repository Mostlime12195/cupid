-- New string keys are added inline when their main position is free; every
-- other case (collisions, empty tables, metatables with __newindex, tables
-- the GC has already marked) must still behave like the interpreter.
local function make(x, y) return { x = x, y = y } end
local function fill(t, v)
  t.a = v; t.b = v; t.c = v; t.d = v; t.e = v; t.f = v; t.g = v; t.h = v
  return t
end
local total = 0
for i = 1, 300 do
  local p = make(i, -i)
  total = total + p.x + p.y
  local t = fill({}, i)                         -- empty: grows through rehash
  local u = fill({ a = 0, q = 1 }, i)           -- some keys present, some collide
  total = total + t.a + t.h + u.a + u.h + u.q
end
print(total)

-- Assigning nil to an absent key creates nothing visible.
local function setnil(t) t.k = nil; t.k2 = nil; return t end
for i = 1, 100 do
  local t = setnil({ k = 1 })
  if i == 100 then print(t.k, t.k2, next(t)) end
end

-- __newindex appearing later, then removed.
local log = {}
local mt = { __newindex = function(t, k, v) log[#log + 1] = k .. "=" .. tostring(v); rawset(t, k, v) end }
local function put(t, v) t.fresh = v; t.other = v end
for i = 1, 120 do
  local t = {}
  if i > 60 and i <= 90 then setmetatable(t, mt) end
  put(t, i)
  if t.fresh ~= i or t.other ~= i then print("lost", i) end
end
print(#log, log[1], log[#log])

-- New keys with fresh (white) strings and tables as values, in old tables,
-- while the incremental GC is running: values must survive collection.
collectgarbage("collect")
collectgarbage("setpause", 100)
collectgarbage("setstepmul", 200)
local keep = {}
for i = 1, 400 do keep[i] = { id = i } end
local function decorate(o, i)
  o.name = "obj" .. i       -- a new string
  o.child = { v = i * 2 }   -- a new table
  o.tag = o.name .. "!"
end
for round = 1, 3 do
  for i = 1, #keep do
    decorate(keep[i], i + round)
    if i % 50 == 0 then collectgarbage("step", 1) end
  end
end
collectgarbage("collect")
local ok, sum = true, 0
for i = 1, #keep do
  local o = keep[i]
  if o.name ~= "obj" .. (i + 3) or o.tag ~= o.name .. "!" or o.child.v ~= (i + 3) * 2 then ok = false end
  sum = sum + o.child.v
end
print(ok, sum)

-- Removed keys (nil values) are reused; iteration order matches.
local function churn(t, i)
  t.a = i; t.b = nil; t.c = i; t.a = nil; t.b = i
end
local t = { a = 1, b = 2, c = 3 }
for i = 1, 100 do churn(t, i) end
local keys = {}
for k, v in pairs(t) do keys[#keys + 1] = k .. "=" .. tostring(v) end
print(table.concat(keys, " "))

-- Globals created inline (the environment is a table too).
local function defglobals(i) newglobal_a = i; newglobal_b = (newglobal_b or 0) + i end
for i = 1, 100 do defglobals(i) end
print(newglobal_a, newglobal_b)
