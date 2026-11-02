for i = 10, 1, -3 do io.write(i, " ") end print()
for i = 1, 2, 0.25 do io.write(i, " ") end print()
for i = 1, 0 do print("never") end
for i = 1.5, -1.5, -0.5 do io.write(i, " ") end print()
print(pcall(function() for i = "a", 2 do end end))
print(pcall(function() for i = 1, {} do end end))
local t = { a = 1, b = 2, c = 3 }
local keys = {}
for k, v in pairs(t) do keys[#keys + 1] = k .. "=" .. v end
table.sort(keys)
print(table.concat(keys, ","))
for i, v in ipairs({ "x", "y", nil, "z" }) do io.write(i, v, " ") end print()
local n = 0
repeat n = n + 1 until n >= 5
print(n)
local w = 0
while w < 100 do w = w + 7 if w % 2 == 0 then w = w + 1 end end
print(w)
local function cls(x)
  if x == nil then return "nil" elseif x == false then return "false" elseif not x then return "?" end
  return x and "truthy" or "falsy"
end
print(cls(nil), cls(false), cls(0), cls(""), cls({}) )
local a = nil
local b = a or "default"
local c = false and 1 or 2
local d = 1 and nil
print(b, c, d, nil and 1, false or nil)
for i = 1, 3 do
  for j = 1, 3 do
    if j == 2 then break end
    io.write(i, j, " ")
  end
end
print()
local function gen(n)
  local i = 0
  return function() i = i + 1; if i <= n then return i, i * i end end
end
for i, sq in gen(4) do io.write(i, ":", sq, " ") end print()
goto_like = 0
