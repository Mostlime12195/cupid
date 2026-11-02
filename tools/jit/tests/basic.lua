local s = 0
for i = 1, 10 do s = s + i end
print("sum", s)
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
print("fib", fib(20))
