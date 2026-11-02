local worker = [[
local inbox, outbox = ...
while true do
  local job = inbox:demand()
  if job == "quit" then break end
  local sum = 0
  for i = 1, job do sum = sum + math.sqrt(i) end
  outbox:push(sum)
end
]]
local busy = [[
local ch = ...
local count = 0
while true do
  count = count + 1
  if count % 200000 == 0 then ch:push(count) end
  if love.thread.getChannel("ctl"):pop() == "stop" then ch:push(-count) return end
end
]]
local sleeper = [[
require('love.timer')
local ch = ...
for i = 1, 5 do love.timer.sleep(0.3); ch:push(i) end
]]

local lines = {}
local function log(s) lines[#lines + 1] = s; print(s) end

function love.load()
  inbox, outbox = love.thread.newChannel(), love.thread.newChannel()
  w = love.thread.newThread(worker); w:start(inbox, outbox)
  inbox:push(1000)
  log("sync demand from main: " .. string.format("%.3f", outbox:demand()))
  inbox:push(5e6)
  busyCh = love.thread.newChannel()
  b = love.thread.newThread(busy); b:start(busyCh)
  sleepCh = love.thread.newChannel()
  s = love.thread.newThread(sleeper); s:start(sleepCh)
  e = love.thread.newThread("\nerror('boom from thread')"); e:start()
  t0 = love.timer.getTime()
end

function love.threaderror(t, err) log("threaderror: " .. err:gsub("\n.*", "")) end

busyCount, sleeps = 0, 0
function love.update(dt)
  local r = outbox:pop()
  if r then log(("big job done after %.2fs: %.1f"):format(love.timer.getTime() - t0, r)) end
  while true do local v = busyCh:pop(); if not v then break end busyCount = v end
  while true do local v = sleepCh:pop(); if not v then break end sleeps = v end
end

function love.keypressed(k)
  if k == "s" then love.thread.getChannel("ctl"):push("stop") end
  if k == "w" then inbox:push("quit"); w:wait(); log("worker joined, running=" .. tostring(w:isRunning())) end
end

function love.draw()
  love.graphics.print(("FPS %d  busy=%d  sleeps=%d  busyRunning=%s  sleeperRunning=%s"):format(
    love.timer.getFPS(), busyCount, sleeps, tostring(b:isRunning()), tostring(s:isRunning())), 10, 10)
  for i, l in ipairs(lines) do love.graphics.print(l, 10, 20 + i * 18) end
end
