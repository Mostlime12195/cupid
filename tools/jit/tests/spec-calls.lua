-- Calls and returns take fast paths in compiled code (frame setup in the
-- call helper, returns inline): every case they don't cover must still
-- work like the interpreter.
local function one(x) return x end
local function two(x) return x, x * 2 end
local function none() return end
local function capture(x)
  local function get() return x end   -- an upvalue open in this frame
  return x + 1, get
end
local acc = {}
for i = 1, 200 do
  local a = one(i)                 -- wants 1, gets 1
  local b, c = one(i)              -- wants 2, gets 1 (c is nil)
  local d = two(i)                 -- wants 1, gets 2
  local t = { two(i) }             -- wants all
  local e = none()                 -- wants 1, gets 0
  local f, g = capture(i)          -- closes an upvalue on return
  if i % 50 == 0 then
    acc[#acc + 1] = table.concat({ a, tostring(c), d, #t, tostring(e), f, g() }, ",")
  end
end
print(table.concat(acc, " | "))

-- Return and call hooks set while compiled code runs.
local events = 0
local function leaf(x) return x * 2 end
local function caller(n)
  local s = 0
  for i = 1, n do
    s = s + leaf(i)
    if i == n - 10 then debug.sethook(function(ev) events = events + 1 end, "cr") end
  end
  debug.sethook()
  return s
end
for i = 1, 60 do caller(20) end
events = 0
print(caller(100), events)

-- Varargs, tail calls and C functions in the same loops.
local function va(...) return select("#", ...), ... end
local function tail(n) if n == 0 then return "done" end return tail(n - 1) end
local function tailc(x) return math.floor(x) end
local r = {}
for i = 1, 120 do
  local n, x, y = va(i, nil, i)
  r[1] = n; r[2] = x; r[3] = y
  r[4] = tail(i % 7)
  r[5] = tailc(i + 0.5)
end
print(r[1], r[2], r[3], r[4], r[5])

-- Deep recursion beyond the direct-call depth, and an error at the bottom.
local function depth(n) if n == 0 then error("bottom") end return depth(n - 1) + 1 end
local function sum(n) if n == 0 then return 0 end return n + sum(n - 1) end
print(sum(5000), pcall(depth, 300))

-- A coroutine yielding from a callee several compiled frames deep.
local function inner(i) if i % 10 == 0 then coroutine.yield(i) end return i end
local function middle(i) return inner(i) * 2 end
local co = coroutine.wrap(function()
  local s = 0
  for i = 1, 100 do s = s + middle(i) end
  return "sum " .. s
end)
local seen = {}
for i = 1, 11 do seen[#seen + 1] = tostring(co()) end
print(table.concat(seen, " "))
