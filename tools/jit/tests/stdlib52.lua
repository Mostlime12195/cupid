-- LuaJIT standard-library extensions
-- load with strings, modes and env
print(load("return 1 + 1")())
print(load("return x", "chunk", "t", { x = 42 })())
print(load("return 1", "=bin", "b"))
local parts = { "return ", "'reader'" }
print(load(function() return table.remove(parts, 1) end)())
print(loadstring("return ...", "c")(1, 2))
-- xpcall passes arguments
print(xpcall(function(a, b) return a + b end, print, 2, 3))
print(xpcall(function(a) error(a) end, function(e) return "handled " .. e end, "x"))
-- math.log with a base
print(math.log(8, 2), math.log(1000, 10), math.log(81, 3), math.log(math.exp(2)))
-- string.rep with a separator
print(("ab"):rep(3, ","), ("x"):rep(1, "-"), ("x"):rep(0, "-"))
-- %s uses tostring; %a
local mt = setmetatable({}, { __tostring = function() return "custom" end })
print(string.format("%s %s %s %s", mt, nil, true, 12))
print(string.format("%5s|%-5s|", "ab", "cd"))
print(string.format("%a", 1), string.format("%.3a", 0.5))
-- table.move
local t = { 1, 2, 3, 4, 5 }
table.move(t, 2, 4, 1); print(table.concat(t, ","))
t = { 1, 2, 3, 4, 5 }
table.move(t, 1, 3, 3); print(table.concat(t, ","))
print(table.concat(table.move({ 1, 2, 3 }, 1, 3, 2, { "a" }), ","))
-- table.new / table.clear via require
local tnew = require("table.new")
local n = tnew(10, 2); n[1] = 1; print(#n, table.new == tnew)
local tclear = require("table.clear")
local c = { 1, 2, x = 3 }; tclear(c); print(next(c), #c)
-- coroutine.isyieldable
print(coroutine.isyieldable(), coroutine.wrap(function() return coroutine.isyieldable() end)())
-- package.searchpath
print(package.searchpath("no.such.mod", "./?.lua;./?/init.lua"))
