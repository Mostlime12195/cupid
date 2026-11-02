-- Direct calls between compiled functions: depth limit, tail calls, yields
-- and errors in the middle of nested compiled frames.
local function depth(n) if n == 0 then return 0 end return 1 + depth(n - 1) end
print(depth(10), depth(500), depth(5000))
local function tdepth(n, acc) if n == 0 then return acc end return tdepth(n - 1, acc + 1) end
print(tdepth(200000, 0))
local function even(n) if n == 0 then return true end return odd(n - 1) end
function odd(n) if n == 0 then return false end return even(n - 1) end
print(even(1000), even(1001))
local function inner(x) if x == 3 then coroutine.yield("y" .. x) end return x * 2 end
local function middle(x) return inner(x) + 1 end
local co = coroutine.wrap(function()
  local s = 0
  for i = 1, 5 do s = s + middle(i) end
  return "sum " .. s
end)
print(co(), co())
local function thrower(n) if n == 0 then error("deep error") end return thrower(n - 1) + 1 end
for _ = 1, 3 do print(pcall(thrower, 100)) end
print(depth(30))
local function cfn_cb(t) table.sort(t, function(a, b) return depth(2) + a < depth(2) + b end) return t[1] end
print(cfn_cb({ 5, 3, 9, 1 }))
local mt = { __index = function(t, k) return depth(k) end }
print(setmetatable({}, mt)[7])
local function many(...) return select("#", ...), ... end
print(many(1, nil, 3))
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
print(fib(22))
