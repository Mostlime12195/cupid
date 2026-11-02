-- yielding inside pcall/xpcall (as in LuaJIT)
local co = coroutine.wrap(function(a)
  local ok, v = pcall(function(x)
    local y = coroutine.yield(x + 1)
    return y * 2
  end, a)
  return ok, v
end)
print(co(1))
print(co(10))

-- an error after a yield is still caught
local co2 = coroutine.create(function()
  local ok, err = pcall(function() coroutine.yield("in"); error("boom") end)
  coroutine.yield(ok, err)
  return "end"
end)
print(coroutine.resume(co2))
print(coroutine.resume(co2))
print(coroutine.resume(co2))
print(coroutine.status(co2))

-- xpcall: the handler runs at the error and sees the error's stack
local co3 = coroutine.wrap(function()
  return xpcall(function(a, b)
    coroutine.yield(a + b)
    local function inner() error({ code = 7 }) end
    inner()
  end, function(e)
    local tb = debug.traceback()
    return e.code .. (tb:find("inner") and " with stack" or " without stack")
  end, 2, 3)
end)
print(co3())
print(co3())

-- running/status/isyieldable from inside pcall
local outer
outer = coroutine.create(function()
  return pcall(function()
    return coroutine.running() == outer, coroutine.status(outer), coroutine.isyieldable()
  end)
end)
print(coroutine.resume(outer))

-- nested pcalls and many yields
local gen = coroutine.wrap(function()
  for i = 1, 3 do
    pcall(function()
      pcall(function() coroutine.yield(i) end)
      coroutine.yield(-i)
    end)
  end
  return "done"
end)
local out = {}
for _ = 1, 7 do out[#out + 1] = tostring(gen()) end
print(table.concat(out, " "))

-- C functions and callable tables through pcall
local co4 = coroutine.wrap(function()
  local ok, v = pcall(coroutine.yield, "c-yield")
  local callable = setmetatable({}, { __call = function(_, x) return coroutine.yield(x) end })
  local ok2, v2 = pcall(callable, "callable")
  return ok, v, ok2, v2
end)
print(co4())
print(co4("r1"))
print(co4("r2"))

-- error levels and messages match a plain pcall
local function check(x) if not x then error("bad arg", 2) end end
print(pcall(check, nil))
print(coroutine.wrap(function() return pcall(check, nil) end)())
print(coroutine.wrap(function() return pcall(nil) end)())
print(coroutine.wrap(function() return pcall(error) end)())

-- main thread: unchanged, and pcall is still the C function
print(debug.getinfo(pcall, "S").what, debug.getinfo(xpcall, "S").what)
print(pcall(coroutine.yield, 1))
print(xpcall(error, function(e) return "h:" .. tostring(e) end, "m"))

-- yielding across a metamethod still fails, with a hint
local mtco = coroutine.wrap(function()
  local obj = setmetatable({}, { __index = function() return coroutine.yield() end })
  return pcall(function() return obj.x end)
end)
local ok, msg = mtco()
print(ok, (msg:gsub("^[^:]*:%d+: ", "")))

-- lots of pcalls inside a coroutine (allocation / GC)
local many = coroutine.wrap(function()
  local s = 0
  for i = 1, 20000 do
    local ok, v = pcall(function(k) return k * 2 end, i)
    s = s + v
  end
  return s
end)
print(many())
