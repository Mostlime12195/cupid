-- A tail call made a few compiled frames deep, to a function that isn't
-- compiled yet. ljit_h_enter used to read the LJIT_TAILCALL passed up from a
-- deeper frame as "the frame I entered was replaced": once the tail-called
-- function got compiled on the way up, it ran it in place of a frame that was
-- still on the stack, and the caller then continued in the wrong frame
-- ("attempt to call a table value" at line 0, wrong results).

local function B(f, x) return f(x) end  -- the tail call
local function A(f, x) local r = B(f, x); return r * 2 end
local function Z1(f, x) local r = A(f, x); return r + 100 end
local function Z2(f, x) local r = Z1(f, x); return r + 1000 end
local function Z3(f, x) local r = Z2(f, x); return r + 10000 end

local warm = function(x) return x end
for i = 1, 60 do Z3(warm, i) end  -- everything above is compiled now

-- each round tail-calls a new, cold function (its own prototype)
local results = {}
for round = 1, 60 do
  local cold = loadstring("return function(x) return x + " .. round .. " end")()
  local t = {}
  for i = 1, 8 do
    local v = Z3(cold, i)
    t[#t + 1] = v
    local check = { "table", i }  -- uses the caller's registers after the call
    assert(check[2] == i)
  end
  results[#results + 1] = table.concat(t, ",")
end
print(results[1])
print(results[60])
local sum = 0
for _, r in ipairs(results) do for v in r:gmatch("%d+") do sum = sum + v end end
print("sum", sum)

-- the same through a method call in a rarely taken branch, as in a game's
-- update: errors raised afterwards must name the right line
local Obj = {}
Obj.__index = Obj
function Obj:hurt(f) return f(self) end
function Obj:update(f, n)
  if n % 7 == 0 then self:hurt(f) end
  self.n = (self.n or 0) + n
  return self.n
end
local o = setmetatable({}, Obj)
for n = 1, 200 do
  local cold = n % 7 == 0 and loadstring("return function(s) return s end")() or nil
  o:update(cold, n)
end
print("n", o.n)
local ok, err = pcall(function()
  local bad = {}
  bad()  -- line 54
end)
print(ok, err)
