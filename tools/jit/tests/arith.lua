local vals = { 0, 1, -1, 2.5, -7.25, 1e300, -1e-300, 3, "10", "0x10", " 5 ", 1/0, -1/0 }
for _, a in ipairs(vals) do
  for _, b in ipairs(vals) do
    local ok, r = pcall(function()
      return { a + b, a - b, a * b, a / b, a % b, a ^ b, -a, a == b, tostring(type(a) == type(b) and (a < b)) }
    end)
    if ok then
      local t = {}
      for i = 1, 9 do t[i] = tostring(r[i]) end
      print(tostring(a), tostring(b), table.concat(t, " "))
    else
      print(tostring(a), tostring(b), "ERR", r)
    end
  end
end
-- constants on either side
local x = 7
print(x + 1, 1 + x, x - 1, 1 - x, x * 2, 2 * x, x / 2, 2 / x, x % 3, 3 % x, x ^ 2, 2 ^ x)
print(x % -3, -x % 3, 5.5 % 2, -5.5 % 2, 0/0 ~= 0/0)
-- comparisons
for _, p in ipairs({ { 1, 2 }, { 2, 1 }, { 2, 2 }, { "a", "b" }, { "b", "a" }, { "a", "a" } }) do
  local a, b = p[1], p[2]
  print(a < b, a <= b, a > b, a >= b, a == b, a ~= b, not (a < b))
end
print(pcall(function() return 1 < "2" end))
print(pcall(function() return {} < {} end))
print(1 == "1", nil == false, 0 == -0)
