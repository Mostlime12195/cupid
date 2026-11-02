local env = { print = print, x = 5 }
local f = setfenv(function() return x end, env)
print(f())
local g = loadstring("local a, b = ... return a * b, y")
print(g(6, 7))
y = "global y"
print(g(1, 1))
local big = {}
for i = 1, 1000 do big[#big + 1] = { id = i, name = "n" .. i } end
local sum = 0
for _, v in ipairs(big) do sum = sum + v.id end
print(sum, #big, big[500].name)
collectgarbage()
local weak = setmetatable({}, { __mode = "k" })
weak[{}] = 1
collectgarbage()
local c = 0 for _ in pairs(weak) do c = c + 1 end
print("weak", c)
local nested = function(a) return function(b) return function(c) return a + b + c end end end
print(nested(1)(2)(3))
local r = {}
for i = 1, 30 do
  local v = i
  r[i] = function() v = v + 1; return v end
end
print(r[1](), r[1](), r[30]())
print(bit and bit.band(12, 10), select("#", unpack({ 1, 2, nil, 4 }, 1, 4)))
