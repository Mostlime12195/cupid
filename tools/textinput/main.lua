-- Text input checks: plain typing, IME composition (love.textedited), emoji
-- pickers, dictation and on-screen keyboards should all reach love.textinput
-- exactly once, and love.keypressed should keep working.
--   F1: toggle love.keyboard.setTextInput (tap the game on a phone to get
--       the on-screen keyboard while it's on)
--   F2: text input on, with the input rectangle around the text box
--   Backspace deletes, Escape clears.
-- Every event is printed ("TEXT ..." lines) for checking from the console.
local utf8 = require("utf8")

local text = ""
local composing, compStart = "", 0
local log = {}
local box = { x = 40, y = 120, w = 560, h = 40 }

local function say(fmt, ...)
  local line = fmt:format(...)
  log[#log + 1] = line
  if #log > 18 then table.remove(log, 1) end
  print(line)
end

function love.load()
  love.keyboard.setKeyRepeat(true)
  say("TEXT start hasTextInput=%s", tostring(love.keyboard.hasTextInput()))
end

function love.textinput(t)
  text = text .. t
  say("TEXT textinput %q", t)
end

function love.textedited(t, start, length)
  composing, compStart = t, start
  say("TEXT textedited %q %d %d", t, start, length)
end

function love.keypressed(key, scancode, isrepeat)
  say("TEXT keypressed %s %s%s", key, scancode, isrepeat and " repeat" or "")
  if key == "backspace" then
    local offset = utf8.offset(text, -1)
    if offset then text = text:sub(1, offset - 1) end
  elseif key == "escape" then
    text = ""
  elseif key == "f1" then
    love.keyboard.setTextInput(not love.keyboard.hasTextInput())
    say("TEXT hasTextInput=%s", tostring(love.keyboard.hasTextInput()))
  elseif key == "f2" then
    love.keyboard.setTextInput(true, box.x, box.y, box.w, box.h)
    say("TEXT hasTextInput=%s (with rect)", tostring(love.keyboard.hasTextInput()))
  end
end

function love.keyreleased(key)
  say("TEXT keyreleased %s", key)
end

function love.draw()
  love.graphics.setColor(0.8, 0.8, 0.8)
  love.graphics.print(("F1 toggle text input (now %s), F2 with rect. Backspace, Escape clear.")
    :format(love.keyboard.hasTextInput() and "on" or "off"), box.x, 40)
  love.graphics.setColor(0.2, 0.2, 0.25)
  love.graphics.rectangle("fill", box.x, box.y, box.w, box.h)
  love.graphics.setColor(1, 1, 1)
  local shown = text .. composing
  love.graphics.print(shown, box.x + 8, box.y + 12)
  if composing ~= "" then
    local w = love.graphics.getFont():getWidth(text)
    love.graphics.line(box.x + 8 + w, box.y + 30, box.x + 8 + w + love.graphics.getFont():getWidth(composing), box.y + 30)
  end
  love.graphics.setColor(0.7, 0.9, 0.7)
  for i, line in ipairs(log) do love.graphics.print(line, box.x, 190 + (i - 1) * 16) end
end
