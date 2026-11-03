-- The JIT speculates that arithmetic and comparisons see numbers and keeps
-- such registers in wasm locals. Every way that can go wrong must behave
-- exactly like the interpreter.

-- Operand types changing mid-loop: tables with metamethods, strings that
-- coerce, then an error (with the right line).
local V = setmetatable({}, { __add = function(a, b) return 1000 end,
                             __unm = function(a) return -7 end,
                             __lt = function(a, b) return true end })
local function sum(xs)
  local s, neg, lt = 0, 0, 0
  for i = 1, #xs do
    s = s + xs[i]
    neg = neg + -xs[i]
    if xs[i] < 5 then lt = lt + 1 end
  end
  return s, neg, lt
end
local xs = {}
for i = 1, 300 do xs[i] = i % 7 end
print(sum(xs))
xs[150] = V
print(pcall(sum, xs))
xs[150] = "12"
print(pcall(sum, xs))
xs[150] = nil
print(pcall(sum, xs))
xs[150] = 3
print(sum(xs))

-- A register that is a number at every back edge but not on entry.
local function count(n, start)
  local x = start
  for i = 1, n do x = (x or 0) + i end
  return x
end
for i = 1, 100 do count(10, i) end
print(count(10, nil), count(10, 5), pcall(count, 10, "a"), count(10, "2"))

-- A called function changes the caller's numeric local with debug.setlocal.
local function poke(level, idx, v) debug.setlocal(level + 1, idx, v) end
local function victim(n, switch_at, newv)
  local acc = 0
  for i = 1, n do
    acc = acc + i
    if i == switch_at then poke(1, 4, newv) end  -- local 4 is acc
  end
  return acc
end
for i = 1, 60 do victim(100, -1) end
print(victim(100, 50, 0.5), pcall(victim, 100, 50, "str"), pcall(victim, 100, 50, {}))

-- An upvalue alias: a closure called in the loop rewrites the local.
local function aliased(n)
  local x = 0
  local function set(v) x = v end
  for i = 1, n do
    x = x + 1
    if i == n - 5 then set(1e6) end
    if i == n - 2 then set(V) end
  end
  return x
end
print(pcall(aliased, 100))
print(pcall(aliased, 100))

-- Comparisons that switch to strings and to mixed types (an error).
local function less(a, b, n)
  local c = 0
  for i = 1, n do if a < b then c = c + 1 end end
  return c
end
for i = 1, 60 do less(1, 2, 10) end
print(less("a", "b", 10), less(2, 1, 10), pcall(less, 1, "x", 3))

-- Errors inside speculated code report the right line.
local function boom(t)
  local s = 0
  for i = 1, 200 do
    s = s + i * 2
    s = s - t[i]          -- t[150] is a boolean
  end
  return s
end
local t = {}
for i = 1, 200 do t[i] = i end
print(boom(t))
t[150] = true
print(pcall(boom, t))

-- Hooks set while compiled code runs (the frame goes to the interpreter).
local function hooked(n)
  local x = 0.5
  for i = 1, n do
    x = x * 1.0001 + i
    if i == math.floor(n / 2) then
      debug.sethook(function() end, "", 1000)
    end
  end
  debug.sethook()
  return x
end
print(hooked(20000))

-- Coroutines yielding from inside a hot numeric loop, with the suspended
-- coroutine's locals changed from outside before it resumes.
local co = coroutine.create(function(n)
  local acc = 0
  for i = 1, n do
    acc = acc + i * 0.5
    if i % 100 == 0 then coroutine.yield(acc) end
  end
  return acc
end)
local results = {}
for round = 1, 12 do
  local ok, v = coroutine.resume(co, 1000)
  results[#results + 1] = tostring(ok) .. ":" .. (type(v) == "table" and "table" or tostring(v))
  if round == 5 then debug.setlocal(co, 1, 2, 1e9) end    -- acc
  if round == 8 then debug.setlocal(co, 1, 2, "9") end    -- acc becomes a string
  if round == 10 then debug.setlocal(co, 1, 2, V) end     -- then a table with __add
end
print(table.concat(results, " "))

-- Numeric edge cases: NaN, -0, infinities, modulo and division by zero.
local function edges(a, b)
  local r = {}
  for i = 1, 3 do
    r[#r + 1] = tostring(a / b) .. " " .. tostring(a % b) .. " " .. tostring(-a) ..
      " " .. tostring(a < b) .. " " .. tostring(a <= b) .. " " .. tostring(a == b)
  end
  return r[3]
end
for i = 1, 60 do edges(i, 3) end
print(edges(0, 0), edges(-0, 1), edges(1, 0), edges(-1, 0), edges(0 / 0, 1), edges(math.huge, -math.huge))

-- for loops with string bounds and a non-number step (errors).
local function loops(a, b, c)
  local s = 0
  for i = a, b, c do s = s + i end
  return s
end
for i = 1, 60 do loops(1, 10, 1) end
print(loops("1", "10", "2"), pcall(loops, 1, 10, {}), loops(10, 1, -3), loops(1, 0, 1))

-- Repeated deopts until the function is compiled without speculation.
local function flip(v, n)
  local s = 0
  for i = 1, n do s = s + v end
  return s
end
local out = {}
for round = 1, 40 do
  local v = (round % 2 == 0) and V or round
  local ok, r = pcall(flip, v, 80)
  out[#out + 1] = tostring(r)
end
print(table.concat(out, ","))
