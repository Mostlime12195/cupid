local V = {}
V.__index = V
V.__add = function(a, b) return setmetatable({ x = a.x + b.x }, V) end
V.__sub = function(a, b) return setmetatable({ x = a.x - (type(b) == "number" and b or b.x) }, V) end
V.__mul = function(a, b) return type(a) == "number" and a * b.x or a.x * b end
V.__unm = function(a) return setmetatable({ x = -a.x }, V) end
V.__eq = function(a, b) return a.x == b.x end
V.__lt = function(a, b) return a.x < b.x end
V.__le = function(a, b) return a.x <= b.x end
V.__len = function(a) return a.x * 10 end
V.__concat = function(a, b) return (type(a) == "table" and a.x or a) .. "|" .. (type(b) == "table" and b.x or b) end
V.__call = function(self, y) return self.x + y end
V.__tostring = function(a) return "V(" .. a.x .. ")" end
local function new(x) return setmetatable({ x = x }, V) end
local a, b = new(3), new(4)
print(tostring(a + b), tostring(a - b), tostring(a - 1), a * 2, 2 * a, tostring(-a))
print(a == b, a == new(3), a < b, b < a, a <= b, #a, a .. b, a .. "s", "s" .. b, a(10))
local proxy = setmetatable({}, { __index = function(t, k) return k .. "!" end, __newindex = function(t, k, v) rawset(t, k, v * 2) end })
proxy.y = 21
print(proxy.foo, proxy.y)
local count = 0
local tracked = setmetatable({}, { __newindex = function(t, k, v) count = count + 1; rawset(t, k, v) end })
for i = 1, 10 do tracked[i] = i end
print(count, #tracked)
local mt = { __index = { greet = function(self) return "hi " .. self.name end } }
local obj = setmetatable({ name = "bob" }, mt)
print(obj:greet())
