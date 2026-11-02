-- goto and labels (LuaJIT / Lua 5.2 extension)
local function check(name, cond) print(name, cond and "ok" or "FAILED"); assert(cond, name) end

-- continue in loops
local s = 0
for i = 1, 10 do
  if i % 2 == 0 then goto continue end
  s = s + i
  ::continue::
end
check("for continue", s == 25)

local i, t = 0, {}
while i < 6 do
  i = i + 1
  if i == 3 then goto continue end
  t[#t + 1] = i
  ::continue::
end
check("while continue", table.concat(t, ",") == "1,2,4,5,6")

i, t = 0, {}
repeat
  i = i + 1
  if i == 2 then goto skip end
  t[#t + 1] = i
  ::skip::
until i >= 4
check("repeat continue", table.concat(t, ",") == "1,3,4")

-- leave nested loops
local found
for a = 1, 5 do
  for b = 1, 5 do
    if a * b == 12 then found = a .. "x" .. b; goto out end
  end
end
::out::
check("nested exit", found == "3x4")

-- backward goto: each pass gets a fresh local, captured separately
local fns = {}
do
  local n = 1
  ::top::
  local x = n
  fns[#fns + 1] = function() return x end
  n = n + 1
  if n <= 3 then goto top end
end
check("backward closures", fns[1]() == 1 and fns[2]() == 2 and fns[3]() == 3)

-- forward goto out of a loop must close captured locals
local caps = {}
for k = 1, 5 do
  local v = k * 10
  caps[k] = function() return v end
  if k == 3 then goto done end
end
::done::
local a1, a2, a3, a4, a5, a6 = 1, 2, 3, 4, 5, 6  -- reuse the stack slots
check("forward close", caps[3]() == 30 and caps[1]() == 10)

-- continue past a closure in the body
local fs = {}
for k = 1, 4 do
  local x = k
  fs[k] = function() return x end
  if k % 2 == 0 then goto continue end
  x = x + 100
  ::continue::
end
check("continue closures", fs[1]() == 101 and fs[2]() == 2 and fs[3]() == 103 and fs[4]() == 4)

-- skipping locals to the end of a block is allowed
do
  goto e
  local skipped = 1
  ::e::
end
check("end of block", true)

-- hot loop (compiled by the JIT)
local sum = 0
for k = 1, 200000 do
  if k % 3 == 0 then goto next end
  sum = sum + k
  ::next::
end
check("hot loop", sum == 13333466667)

-- compile errors
local function err(src)
  local f, e = loadstring(src)
  print((tostring(e):gsub("^[^:]*:%d+: ", "")))
  return f == nil
end
check("undefined label", err("goto nope"))
check("into scope", err("goto f; local a; ::f:: print(a)"))
check("duplicate", err("::a:: ::a::"))
check("into nested block", err("goto x; do ::x:: end"))
check("across functions", err("::l:: local function f() goto l end"))
check("goto is a keyword", err("local goto = 1"))
