local parts = {}
for i = 1, 200 do parts[#parts + 1] = ("%d:%s"):format(i, ("ab"):rep(i % 4)) end
local s = table.concat(parts, ",")
print(#s, s:sub(1, 40))
local n = 0
for w in s:gmatch("%d+") do n = n + tonumber(w) end
print(n)
print(("hello world"):gsub("o", "0"), ("x=1, y=2"):match("y=(%d)"))
print(string.byte("ABC", 1, -1), string.char(72, 105))
local acc = ""
for i = 1, 50 do acc = acc .. i .. (i % 2 == 0 and "e" or "o") end
print(acc)
print(tostring(1e15), tostring(1e16), tostring(0.1), 2^53, -0.0)
local t = {}
for i = 1, 100 do t[i] = (i * 7919) % 101 end
table.sort(t, function(a, b) return a > b end)
print(t[1], t[50], t[100])
