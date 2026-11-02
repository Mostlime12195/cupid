-- love.window.showFileDialog checks.
--   o: open a file     m: open several    f: open a folder
--   s: save a file     l: open a file 6 seconds later (outside the key
--                         press's user activation, so the player's bar shows)
-- Every result is printed ("DIALOG ..." lines) so it can be checked from the
-- browser console.
local log = {}
local saveCount = 0
local pendingTimer = nil

local function say(fmt, ...)
  local line = fmt:format(...)
  log[#log + 1] = line
  if #log > 28 then table.remove(log, 1) end
  print(line)
end

local function describe(path)
  -- Read the picked file the way desktop games do: as a native path.
  local file, err = love.filesystem.openNativeFile(path, "r")
  if not file then return "openNativeFile failed: " .. tostring(err) end
  local data = file:read()
  file:close()
  local hash = love.data.encode("string", "hex", love.data.hash("string", "md5", data))
  return ("%d bytes md5 %s"):format(#data, hash)
end

local function report(kind)
  return function(files, filtername, err)
    say("DIALOG %s: %d file(s), filter=%s, err=%s", kind, #files, tostring(filtername), tostring(err))
    for i, path in ipairs(files) do
      say("DIALOG %s file %d: %s (%s)", kind, i, path, kind == "folder" and "folder" or describe(path))
    end
    if kind == "folder" and files[1] then
      -- Mount the folder and list it, as a desktop game would.
      local ok = love.filesystem.mountFullPath(files[1], "picked", "read")
      say("DIALOG folder mount: %s", tostring(ok))
      local function walk(dir)
        for _, item in ipairs(love.filesystem.getDirectoryItems(dir)) do
          local p = dir .. "/" .. item
          local info = love.filesystem.getInfo(p)
          if info.type == "directory" then walk(p) else say("DIALOG folder item: %s (%d bytes)", p, info.size) end
        end
      end
      if ok then walk("picked") love.filesystem.unmountFullPath(files[1]) end
    end
  end
end

local filters = { ["Images"] = "png;jpg", ["Text"] = "txt;lua" }

local function open(kind)
  if kind == "open" then
    love.window.showFileDialog("openfile", report("open"), { title = "Open a file", filters = filters })
  elseif kind == "multi" then
    love.window.showFileDialog("openfile", report("multi"), { multiselect = true })
  elseif kind == "folder" then
    love.window.showFileDialog("openfolder", report("folder"))
  elseif kind == "save" then
    love.window.showFileDialog("savefile", function(files, filtername, err)
      say("DIALOG save: %d file(s), filter=%s, err=%s", #files, tostring(filtername), tostring(err))
      if files[1] then
        saveCount = saveCount + 1
        local f = assert(love.filesystem.openNativeFile(files[1], "w"))
        f:write(("Saved by LÖVE, save #%d\n"):format(saveCount))
        f:write(string.rep("0123456789", 10) .. "\n")
        f:close()
        say("DIALOG save wrote: %s", files[1])
      end
    end, { defaultname = "notes.txt", filters = { ["Text"] = "txt" } })
  end
end

function love.load()
  say("o: open file, m: open several, f: open folder, s: save, l: open file in 6s (no gesture)")
end

function love.keypressed(key)
  if key == "o" then open("open")
  elseif key == "m" then open("multi")
  elseif key == "f" then open("folder")
  elseif key == "s" then open("save")
  elseif key == "l" then pendingTimer = 6 -- seconds: past the browser's user activation window
  end
end

function love.update(dt)
  if pendingTimer then
    pendingTimer = pendingTimer - dt
    if pendingTimer <= 0 then pendingTimer = nil open("open") end
  end
end

function love.draw()
  for i, line in ipairs(log) do love.graphics.print(line, 10, 10 + (i - 1) * 18) end
end
