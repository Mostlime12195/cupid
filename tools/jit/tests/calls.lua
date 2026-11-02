local function va(...)
  local n = select("#", ...)
  local t = { ... }
  return n, #t, ...
end
print(va())
print(va(1, nil, 3))
print((va(1, 2)))
local function pass(...) return ... end
print(pass(1, 2, 3), pass(4, 5))
print(({ pass(1, 2, 3) })[3], select(2, pass("a", "b", "c")))
local function tail(n, acc) if n == 0 then return acc end return tail(n - 1, acc + n) end
print(tail(100000, 0))
local function counter()
  local c = 0
  return function() c = c + 1; return c end, function() return c end
end
local inc, get = counter()
for i = 1, 5 do inc() end
print(get())
local fns = {}
for i = 1, 5 do fns[i] = function() return i end end
local s = ""
for i = 1, 5 do s = s .. fns[i]() end
print(s)
local k = 0
while true do
  k = k + 1
  local captured = k
  fns[k] = function() return captured * 2 end
  if k >= 3 then break end
end
print(fns[1](), fns[2](), fns[3]())
local t = { 1, 2, 3, pass(4, 5, 6) }
print(#t, t[6])
local big = {}
for i = 1, 120 do big[i] = i end
local copy = { unpack(big) }
print(#copy, copy[1], copy[120])
local lit = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, x = 1 }
print(#lit, lit[55], lit.x)
print(string.format("%5.2f|%d|%s", 3.14159, 42, "x"), ("abc"):upper(), #"hello")
print(math.max(1, 5, 3), math.floor(-2.5), select("#", nil, nil))
local a, b, c = (function() return 1, 2 end)()
print(a, b, c)
local x, y = 1
print(x, y)
