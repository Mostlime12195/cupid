-- Stores into tables while the incremental GC runs (write barriers).
collectgarbage("setpause", 50)
collectgarbage("setstepmul", 400)
local big, map = {}, {}
for i = 1, 20000 do
  big[i] = { id = i, name = "n" .. i }
  map["k" .. (i % 500)] = big[i]
  if i % 1000 == 0 then
    big[i - 1].name = "renamed" .. i
    collectgarbage("step")
  end
end
local holder = { slot = false }
for i = 1, 5000 do
  holder.slot = { i }           -- string-keyed store of a new table
  big[(i % 100) + 1] = { i }    -- array store of a new table
  big[(i % 100) + 101].name = "s" .. i
end
collectgarbage()
local sum = 0
for i = 1, #big do sum = sum + (big[i].id or big[i][1]) end
local count = 0
for k, v in pairs(map) do count = count + 1 end
print(sum, count, holder.slot[1], big[150].name, big[1999].name)
