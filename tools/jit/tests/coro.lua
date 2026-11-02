local co = coroutine.create(function(a, b)
  print("start", a, b)
  local c = coroutine.yield(a + b)
  print("got", c)
  local s = 0
  for i = 1, 10 do
    s = s + i
    if i % 3 == 0 then s = s + coroutine.yield(s) end
  end
  return "done", s
end)
print(coroutine.resume(co, 1, 2))
print(coroutine.resume(co, "x"))
for i = 1, 5 do print(coroutine.resume(co, i)) end
print(coroutine.status(co))
local gen = coroutine.wrap(function() for k, v in pairs({ 10, 20, 30 }) do coroutine.yield(k, v) end end)
print(gen()) print(gen()) print(gen())
local function producer()
  return coroutine.wrap(function()
    for i = 1, 3 do
      local inner = coroutine.wrap(function() for j = 1, 2 do coroutine.yield(i * 10 + j) end end)
      coroutine.yield(inner()) coroutine.yield(inner())
    end
  end)
end
local p = producer()
local out = {}
for _ = 1, 6 do out[#out + 1] = p() end
print(table.concat(out, " "))
print(coroutine.resume(coroutine.create(function() error("in coro") end)))
local errco = coroutine.create(function() local t = nil; return t.x end)
print(coroutine.resume(errco))
