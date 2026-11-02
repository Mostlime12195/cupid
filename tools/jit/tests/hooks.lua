-- Hooks must work (functions with hooks set are interpreted, others compiled).
local function f(n) if n < 2 then return n end return f(n - 1) + f(n - 2) end
for i = 1, 60 do f(5) end  -- make f hot
local lines = 0
debug.sethook(function() lines = lines + 1 end, "l")
print(f(10))
debug.sethook()
print("line events", lines > 0)
local counts = 0
debug.sethook(function() counts = counts + 1 end, "", 100)
local s = 0
for i = 1, 100000 do s = s + i end
debug.sethook()
print(s, "count events", counts > 500)
local calls = 0
debug.sethook(function(ev) calls = calls + 1 end, "c")
f(8)
debug.sethook()
print("call events", calls)
-- a hook that yields-like behaviour: error from hook
local ok, e = pcall(function()
  debug.sethook(function() error("stop") end, "", 1000)
  local x = 0
  while true do x = x + 1 end
end)
debug.sethook()
print(ok, e:match("stop") ~= nil)
