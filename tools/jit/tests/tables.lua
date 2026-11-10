-- Tables made by constructors keep their first hash entries in the same
-- block as the table (ltable.c). Growing, shrinking, clearing and
-- collecting them must behave as always.
local function count(t) local n = 0; for _ in pairs(t) do n = n + 1 end; return n end
local function sum(t)
  local s = 0
  for k, v in pairs(t) do
    if type(v) == "number" then s = s + v end
    if type(k) == "number" then s = s + k * 1000 end
  end
  return s
end

-- Constructors with 0 to 20 fields, with and without an array part.
local makers = {
  function() return {} end,
  function() return { a = 1 } end,
  function() return { a = 1, b = 2 } end,
  function() return { a = 1, b = 2, c = 3 } end,
  function() return { a = 1, b = 2, c = 3, d = 4 } end,
  function() return { a = 1, b = 2, c = 3, d = 4, e = 5 } end,
  function() return { a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8 } end,
  function() return { a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8, i = 9 } end,
  function() return { a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8, i = 9, j = 10, k = 11, l = 12,
                      m = 13, n = 14, o = 15, p = 16, q = 17, r = 18, s = 19, t = 20 } end,
  function() return { 10, 20, 30, a = 1 } end,
  function() return { 10, 20, 30, a = 1, b = 2, c = 3, d = 4, e = 5 } end,
  function() return { [1] = 1, [2] = 2, [100] = 3, x = 4 } end,
  function() return { [1.5] = 1, [true] = 2, x = 3 } end,
}
for round = 1, 3 do
  local line = {}
  for i = 1, #makers do
    local t = makers[i]()
    line[#line + 1] = count(t) .. ":" .. sum(t)
  end
  print(table.concat(line, " "))
end

-- Growing past the inline entries one key at a time, then removing them.
for i = 1, #makers do
  local t = makers[i]()
  local before = sum(t)
  for k = 1, 40 do t["key" .. k] = k end
  local grown = count(t) .. ":" .. sum(t)
  for k = 1, 40 do t["key" .. k] = nil end
  for k = 1, 5 do t[#t + 1] = k end
  t.a = nil; t.b = nil
  t.z = 99
  print(i, before, grown, count(t), sum(t), t.c, t.z, #t)
end

-- Many of them, kept, dropped and collected at different times; some grow
-- after others around them have been freed.
local keep = {}
local total = 0
for i = 1, 20000 do
  local t = makers[i % #makers + 1]()
  t.id = i
  if i % 3 == 0 then keep[#keep + 1] = t end
  if i % 7 == 0 then for k = 1, i % 30 do t[k] = k end end
  if i % 11 == 0 then t.a, t.b, t.c, t.d, t.e, t.f, t.g, t.h, t.i, t.j = nil end
  total = total + count(t)
  if i % 5000 == 0 then collectgarbage() end
end
collectgarbage()
local s = 0
for i = 1, #keep do s = s + sum(keep[i]) + keep[i].id end
print(total, #keep, s)
for i = 1, #keep, 2 do keep[i] = false end
collectgarbage()
for i = 2, #keep, 2 do for k = 1, 12 do keep[i]["extra" .. k] = k end end
collectgarbage()
s = 0
for i = 2, #keep, 2 do s = s + sum(keep[i]) end
print(s)

-- Metatables, weak tables and rawset on tables with inline entries.
local mt = { __index = function(_, k) return "missing " .. tostring(k) end, __newindex = function(t, k, v) rawset(t, k, v * 2) end }
local obj = setmetatable({ x = 1, y = 2 }, mt)
obj.w = 5
print(obj.x, obj.y, obj.w, obj.q, rawget(obj, "q"), count(obj))
local weak = setmetatable({ a = {}, b = {}, c = "str" }, { __mode = "v" })
local strong = weak.a
collectgarbage()
print(weak.a == strong, weak.b, weak.c, count(weak))
local weakk = setmetatable({ x = 1 }, { __mode = "k" })
for i = 1, 20 do weakk[{}] = i end
weakk[strong] = "kept"
collectgarbage()
print(count(weakk), weakk[strong], weakk.x)

-- next() while clearing fields, and table.insert/remove on mixed tables.
local t = { a = 1, b = 2, c = 3, d = 4, 1, 2, 3 }
local seen = 0
for k in pairs(t) do t[k] = nil; seen = seen + 1 end
print(seen, next(t))
t = { n = 0, name = "list" }
for i = 1, 100 do table.insert(t, i); t.n = t.n + 1 end
for i = 1, 50 do table.remove(t, 1) end
print(#t, t[1], t[50], t.n, t.name)

-- Objects in a hot loop: created, read and written by compiled code.
local function vec(x, y) return { x = x, y = y } end
local function step(n)
  local p = vec(0, 0)
  for i = 1, n do
    local v = vec(i, -i)
    p = vec(p.x + v.x, p.y + v.y * 0.5)
    if i % 100 == 0 then p.extra = i; p.more = i; p.evenmore = i end
  end
  return p.x, p.y, p.extra, p.more
end
for i = 1, 60 do step(10) end
print(step(100000))
local function fill(n)
  local objs = {}
  for i = 1, n do
    local o = { id = i, hp = 3, x = i * 2, y = i * 3, alive = true }
    if i % 2 == 0 then o.shield = 1 end
    if i % 4 == 0 then o.a, o.b, o.c, o.d = 1, 2, 3, 4 end
    objs[i] = o
  end
  local s = 0
  for i = 1, n do
    local o = objs[i]
    o.hp = o.hp - 1
    s = s + o.x + o.y + o.hp + (o.shield or 0) + (o.d or 0)
  end
  return s
end
for i = 1, 60 do fill(8) end
print(fill(5000))
