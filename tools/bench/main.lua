-- Benchmark that runs unchanged on LÖVE 11.4 (love.js) and LÖVE 12.
local getTime = love.timer.getTime
local results = {}
local function report(name, ms) results[#results + 1] = ("%-22s %8.1f ms"):format(name, ms); print("BENCH " .. name .. " " .. ("%.1f"):format(ms)) end
local function bench(name, fn)
  fn() -- warm up
  local best = math.huge
  for _ = 1, 3 do local t = getTime(); fn(); best = math.min(best, (getTime() - t) * 1000) end
  report(name, best)
end

local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end

function love.load()
  print("BENCH start")
  bench("lua fib(27)", function() fib(27) end)
  bench("lua tables+sort", function()
    local t = {}
    for i = 1, 200000 do t[i] = (i * 7919) % 100003 end
    table.sort(t)
  end)
  bench("lua strings", function()
    local parts = {}
    for i = 1, 50000 do parts[#parts + 1] = string.format("%d:%s", i, ("x"):rep(i % 10)) end
    local s = table.concat(parts, ",")
    local n = 0
    for _ in s:gmatch("%d+") do n = n + 1 end
  end)
  bench("lua closures/oop", function()
    local V = {}; V.__index = V
    local function new(x, y) return setmetatable({ x = x, y = y }, V) end
    function V.add(a, b) return new(a.x + b.x, a.y + b.y) end
    local acc = new(0, 0)
    for i = 1, 300000 do acc = acc:add(new(i, -i)) end
  end)
  local d = love.image.newImageData(16, 16)
  d:mapPixel(function(x, y) return x / 15, y / 15, 1, 1 end)
  img = love.graphics.newImage(d)
  bench("imagedata mapPixel 512^2", function()
    local big = love.image.newImageData(512, 512)
    big:mapPixel(function(x, y, r, g, b, a) return x / 511, y / 511, 0.5, 1 end)
  end)
end

local SPRITES, RECTS = 20000, 5000
local frame, drawTimes, frameTimes = 0, {}, {}
local function median(t) table.sort(t); return t[math.floor(#t / 2) + 1] end

function love.run()
  love.load()
  love.timer.step()
  return function()
    love.event.pump()
    for name, a in love.event.poll() do if name == "quit" then return a or 0 end end
    local t0 = getTime()
    love.graphics.origin()
    love.graphics.clear(0.1, 0.1, 0.12)
    for i = 1, SPRITES do
      local a = i * 0.618
      love.graphics.setColor((i % 7) / 7, (i % 5) / 5, (i % 3) / 3)
      love.graphics.draw(img, 400 + math.cos(a + frame * 0.01) * (i % 300), 300 + math.sin(a) * (i % 280), a, 1, 1, 8, 8)
    end
    for i = 1, RECTS do love.graphics.rectangle("fill", (i * 37) % 800, (i * 53) % 600, 6, 6) end
    love.graphics.setColor(1, 1, 1)
    for i, r in ipairs(results) do love.graphics.print(r, 10, 10 + i * 16) end
    local t1 = getTime()
    love.graphics.present()
    local t2 = getTime()
    frame = frame + 1
    if frame > 10 then drawTimes[#drawTimes + 1] = (t1 - t0) * 1000; frameTimes[#frameTimes + 1] = (t2 - t0) * 1000 end
    if frame == 70 then
      report(("draw %dk sprites+%dk rects"):format(SPRITES / 1000, RECTS / 1000), median(drawTimes))
      report("frame incl. present", median(frameTimes))
      print("BENCH done")
    end
  end
end
