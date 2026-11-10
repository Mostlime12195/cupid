-- Objects of every small size, growing and shrinking, freed in different
-- orders (the allocator keeps free lists by size: lauxlib.c).
local function checksum(t)
  local s = 0
  for i = 1, #t do
    local v = t[i]
    if type(v) == "string" then s = (s * 31 + #v + v:byte(1) + v:byte(-1)) % 2147483647
    elseif type(v) == "table" then s = (s * 31 + #v + (v.n or 0)) % 2147483647
    elseif type(v) == "function" then s = (s * 31 + v()) % 2147483647 end
  end
  return s
end

-- Strings of every length up to past the small sizes.
local strs = {}
for n = 1, 600 do strs[n] = string.rep(string.char(65 + n % 26), n) .. n end
print(checksum(strs))
for n = 1, 600, 2 do strs[n] = false end
collectgarbage()
for n = 1, 600, 2 do strs[n] = string.rep("z", 601 - n) .. n end
print(checksum(strs))

-- Tables that grow from the small sizes into malloc's and back.
local tabs = {}
for i = 1, 300 do
  local t = { n = i }
  for j = 1, i do t[j] = j end
  for k = 1, i % 40 do t["k" .. k] = k end
  tabs[i] = t
end
print(checksum(tabs))
for i = 1, 300 do
  local t = tabs[i]
  for j = #t, i % 7 + 1, -1 do t[j] = nil end
  for k = 1, i % 40 do t["k" .. k] = nil end
end
collectgarbage()
for i = 1, 300 do tabs[i].extra = i; tabs[i][#tabs[i] + 1] = i end
print(checksum(tabs))

-- Closures with 0 to 40 upvalues, userdata-free churn, coroutines.
local fns = {}
for i = 1, 200 do
  local a, b, c, d, e, f, g, h = i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7
  if i % 4 == 0 then fns[i] = function() return a end
  elseif i % 4 == 1 then fns[i] = function() return a + b + c end
  elseif i % 4 == 2 then fns[i] = function() return a + b + c + d + e + f end
  else fns[i] = function() return a + b + c + d + e + f + g + h end end
end
print(checksum(fns))
local total = 0
for round = 1, 20 do
  local cos = {}
  for i = 1, 50 do
    cos[i] = coroutine.wrap(function(x)
      local t = {}
      for j = 1, x do t[j] = { j, tostring(j), function() return j end } end
      coroutine.yield(#t)
      return t[x][3]()
    end)
  end
  for i = 1, 50 do total = total + cos[i](i) end
  for i = 1, 50, 2 do total = total + cos[i]() end
  if round % 5 == 0 then collectgarbage() end
end
print(total)

-- Many short-lived objects with steps of the collector in between.
local acc = 0
for i = 1, 200000 do
  local v = { x = i, y = -i, name = (i % 100 == 0) and ("n" .. i) or nil }
  local w = { v, v.x + v.y }
  acc = acc + #w + w[2]
  if i % 50000 == 0 then collectgarbage("step", 100) end
end
print(acc)
collectgarbage()
collectgarbage()
print(checksum(strs), checksum(tabs), checksum(fns))

-- Other states (coroutines are threads of this one; this makes sure a big
-- string table and its strings survive a full cycle).
local keep = {}
for i = 1, 5000 do keep[i] = "s" .. i end
collectgarbage()
local n = 0
for i = 1, 5000 do if keep[i] == "s" .. i then n = n + 1 end end
print(n)
