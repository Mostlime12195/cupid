-- Errors after one second, to check that LÖVE's error screen doesn't stall
-- the page (it calls love.event.pump(0.1) every frame).
local t = 0
function love.update(dt)
  t = t + dt
  if t > 1 then error("deliberate error for the error-screen test") end
end
function love.draw() love.graphics.print("erroring soon...", 10, 10) end
