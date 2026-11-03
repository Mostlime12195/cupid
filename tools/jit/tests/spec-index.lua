-- Field reads through __index tables (followed inline by the JIT) must see
-- every change: metatables set or removed later, __index switching between
-- tables and functions, fields shadowed, removed and added, and chains
-- deeper than the inline levels.
local Base = { kind = "base" }
Base.__index = Base
function Base.hello(self) return "base " .. tostring(self.name) end
local Class = setmetatable({ kind = "class" }, Base)
Class.__index = Class
local obj = { name = "o" }

local function read(o) return o.kind, o.hello and o:hello(), o.missing, o.name end

local log = {}
for i = 1, 200 do
  if i == 20 then setmetatable(obj, Class) end           -- metatable appears
  if i == 40 then Class.kind = nil end                     -- falls through to Base
  if i == 60 then obj.kind = "own" end                     -- shadowed by the object
  if i == 80 then obj.kind = nil end
  if i == 100 then Class.__index = function(t, k) return "fn:" .. k end end
  if i == 120 then Class.__index = Class end
  if i == 130 then Base.__index = nil end                  -- end of the chain
  if i == 140 then Base.__index = { kind = "deep", hello = function() return "deep" end } end
  if i == 150 then setmetatable(Base.__index, { __index = { missing = "deeper" } }) end
  if i == 160 then setmetatable(obj, nil) end              -- metatable removed
  if i == 170 then setmetatable(obj, { __index = 42 }) end -- __index not a table
  local ok, a, b, c, d = pcall(read, obj)
  local line = tostring(ok) .. " " .. tostring(a) .. " " .. tostring(b) .. " " .. tostring(c) .. " " .. tostring(d)
  if log[#log] ~= line then log[#log + 1] = line; print(i, line) end
end

-- Array-part reads: nil slots with and without a metatable.
local arr = { 1, 2, nil, 4 }
local function geti(t, i) return t[i] end
for i = 1, 100 do
  if i == 50 then setmetatable(arr, { __index = function(t, k) return k * 100 end }) end
  local v = geti(arr, 3)
  if i == 1 or i == 50 or i == 100 then print("arr", i, v, geti(arr, 1), geti(arr, 9)) end
end

-- Methods through a class while the class table is rehashed (fields added).
local C = {}
C.__index = C
function C:get() return self.v end
local objs = {}
for i = 1, 50 do objs[i] = setmetatable({ v = i }, C) end
local sum = 0
for round = 1, 20 do
  for i = 1, #objs do sum = sum + objs[i]:get() end
  C["extra" .. round] = round  -- grows and rehashes the class
end
print("sum", sum)

-- Globals through a metatable on the environment.
local env = setmetatable({}, { __index = _G })
local function g() return print ~= nil, undefinedglobal end
setfenv(g, env)
for i = 1, 60 do
  if i == 30 then getmetatable(env).__index = function(_, k) return "dyn " .. k end end
  local a, b = g()
  if i == 1 or i == 30 or i == 60 then print("env", i, a, b) end
end
