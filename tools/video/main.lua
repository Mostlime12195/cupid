-- love.video test: plays test.ogv (320x240, 30 fps, 6 s, Theora + Vorbis)
-- twice, once synced to its audio Source and once on its own clock, and runs
-- a scripted play / pause / seek sequence on both, printing PASS/FAIL lines.
--
-- The first pass plays straight through and remembers which frame (by hash)
-- is shown at which time, so the seeks afterwards can check that they show
-- the right frame, not just report the right time.
--
-- Space: pause/resume, Left/Right: seek -/+1 s, T or click: rerun the script
-- (browsers may keep audio, and so the audio-synced video, paused until a click).
--
-- test.ogv is synthetic (ffmpeg's testsrc pattern and a 440 Hz beep every
-- second), encoded with libtheora 1.2.0's examples/encoder_example:
--   ffmpeg -f lavfi -i testsrc=size=320x240:rate=30:duration=6 -pix_fmt yuv420p clip.y4m
--   ffmpeg -f lavfi -i sine=frequency=440:beep_factor=4:sample_rate=44100:duration=6 -ac 1 \
--     -c:a pcm_s16le -bitexact -map_metadata -1 -fflags +bitexact -flags:a +bitexact clip.wav
--   encoder_example -v 5 -a 0 -k 30 -o test.ogv clip.wav clip.y4m

local TOL = 0.2 -- seconds; frame timing on the web is per animation frame
local players = {}
local log = {}

local function out(s)
  print(s)
  log[#log + 1] = s
  if #log > 12 then table.remove(log, 1) end
end

local function check(p, name, ok, detail)
  out(("%s %s/%s %s"):format(ok and "PASS" or "FAIL", p.name, name, detail or ""))
  if not ok then p.failed = p.failed + 1 end
end

local function near(a, b, tol) return a and b and math.abs(a - b) <= (tol or TOL) end

-- Renders the current frame 1:1 to a canvas and hashes it, to show that
-- frames really change (and are not black).
local canvas
local function frameInfo(video)
  love.graphics.push("all")
  love.graphics.setCanvas(canvas)
  love.graphics.clear(0, 0, 0, 1)
  love.graphics.origin()
  love.graphics.draw(video)
  love.graphics.pop()
  local data = love.graphics.readbackTexture(canvas)
  local r, g, b = data:getPixel(100, 60)
  return love.data.encode("string", "hex", love.data.hash("string", "md5", data)):sub(1, 8), r + g + b
end

-- Checks that the frame on screen is the one first seen at about `t`.
local function checkFrame(p, name, t)
  local seenAt = p.seen[p.hash]
  check(p, name, near(seenAt, t, 0.1),
    ("tell=%.2f, frame %s was shown at %s"):format(t, p.hash, seenAt and ("%.2f"):format(seenAt) or "never"))
end

-- Each step runs `delay` seconds after the previous one (or, with `wait`,
-- once that returns true). Expectations use measured times, so a slow frame
-- doesn't fail the test.
local function script(p)
  local v = p.video
  local paused, tPlay, tResume, tSeek, seekTo
  local function seek(to)
    v:seek(to)
    tSeek, seekTo = love.timer.getTime(), to
  end
  -- A seek lands on the packet at the target time, so frames until the next
  -- keyframe (one per second in test.ogv) can differ from straight playback.
  -- The frame is compared after at least one keyframe.
  local function checkSeek(name)
    local t = v:tell()
    local expected = seekTo + love.timer.getTime() - tSeek
    check(p, name, near(t, expected), ("tell=%.2f expected %.2f"):format(t, expected))
    checkFrame(p, name .. " shows frame", t)
  end
  return {
    { delay = 0.0, function() v:rewind(); v:play(); tPlay = love.timer.getTime() end },
    { delay = 1.0, function()
      if p.source then
        check(p, "source playing", p.source:isPlaying(), ("source:tell()=%.2f"):format(p.source:tell()))
      end
    end },
    { delay = 1.0, function()
      local t, expected = v:tell(), love.timer.getTime() - tPlay
      check(p, "speed", near(t, expected), ("tell=%.2f after %.2f s"):format(t, expected))
      check(p, "isPlaying", v:isPlaying())
      check(p, "frames change", #p.hashes >= 20, ("%d distinct frames in %.2f s"):format(#p.hashes, expected))
      v:pause()
      paused = v:tell()
      p.pausedHash = nil
    end },
    { delay = 0.6, function()
      local t = v:tell()
      check(p, "pause holds", math.abs(t - paused) < 0.001 and not v:isPlaying(), ("tell=%.3f paused at %.3f"):format(t, paused))
      check(p, "frame holds while paused", p.pausedHash == p.hash, ("%s / %s"):format(tostring(p.pausedHash), p.hash))
      v:play()
      tResume = love.timer.getTime()
    end },
    { delay = 1.0, function()
      local t, expected = v:tell(), paused + love.timer.getTime() - tResume
      check(p, "resume", near(t, expected), ("tell=%.2f expected %.2f"):format(t, expected))
    end },
    { wait = function() return not v:isPlaying() or love.timer.getTime() - tResume > 8 end, function()
      local now, expected = love.timer.getTime(), tResume + 6.0 - paused
      check(p, "ends", not v:isPlaying() and near(now, expected, 0.3),
        ("stopped %.2f s after the expected end"):format(now - expected))
      p.linear = false
      v:rewind()
      tSeek, seekTo = love.timer.getTime(), 0
      v:play()
    end },
    { delay = 1.2, function() checkSeek("rewind after end"); seek(4.5) end },
    { delay = 1.0, function() checkSeek("seek forward"); seek(2.0) end },
    { delay = 1.2, function()
      checkSeek("seek back")
      v:pause()
      seek(1.0)
      v:play()
    end },
    { delay = 1.2, function()
      checkSeek("seek while paused")
      out(("%s: %s (%d failed)"):format(p.name, p.failed == 0 and "ALL PASSED" or "FAILURES", p.failed))
      p.done = true
    end },
  }
end

local function start(p)
  p.t0 = love.timer.getTime()
  p.tStep = p.t0
  p.step = 1
  p.failed = 0
  p.done = false
  p.linear = true
  p.hashes = {}
  p.seen = {}
  p.nextReport = 0
  p.checkedPixels = false
  p.steps = script(p)
end

function love.load()
  canvas = love.graphics.newCanvas(320, 240)

  local synced = love.graphics.newVideo("test.ogv", { audio = true })
  local free = love.graphics.newVideo("test.ogv", { audio = false })
  players[1] = { name = "audio-synced", video = synced, source = synced:getSource(), x = 20 }
  players[2] = { name = "own-clock", video = free, x = 380 }

  out(("video %dx%d, source %s"):format(synced:getWidth(), synced:getHeight(), tostring(synced:getSource())))
  check(players[1], "has source", synced:getSource() ~= nil)
  check(players[2], "no source", free:getSource() == nil)

  for _, p in ipairs(players) do start(p) end
end

function love.update(dt)
  for _, p in ipairs(players) do
    local now = love.timer.getTime()
    local e = now - p.t0
    local s = p.steps[p.step]
    if s and ((s.wait and s.wait()) or (s.delay and now - p.tStep >= s.delay)) then
      s[1]()
      p.step = p.step + 1
      p.tStep = now
    end
    if not p.done and e >= p.nextReport then
      print(("%s elapsed=%.2f tell=%.2f playing=%s frame=%s"):format(p.name, e, p.video:tell(), tostring(p.video:isPlaying()), tostring(p.hash)))
      p.nextReport = p.nextReport + 0.5
    end
    -- Loop once the script is done.
    if p.done and not p.video:isPlaying() then
      p.video:rewind()
      p.video:play()
    end
  end
end

function love.draw()
  for _, p in ipairs(players) do
    local hash, sum = frameInfo(p.video)
    if hash ~= p.hash then
      p.hashes[#p.hashes + 1] = hash
      if p.linear and not p.seen[hash] then p.seen[hash] = p.video:tell() end
    end
    p.hash = hash
    if not p.video:isPlaying() and not p.pausedHash then p.pausedHash = hash end
    if love.timer.getTime() - p.t0 >= 1.0 and not p.checkedPixels then
      p.checkedPixels = true
      check(p, "frame not black", sum > 0.3, ("pixel sum %.2f"):format(sum))
    end

    love.graphics.draw(p.video, p.x, 40)
    love.graphics.print(("%s  tell %.2f  %s"):format(p.name, p.video:tell(), p.video:isPlaying() and "playing" or "paused"), p.x, 20)
  end
  for i, line in ipairs(log) do
    love.graphics.print(line, 20, 290 + (i - 1) * 9, 0, 0.7)
  end
end

function love.keypressed(key)
  if key == "t" then
    for _, p in ipairs(players) do start(p) end
    return
  end
  for _, p in ipairs(players) do
    local v = p.video
    if key == "space" then
      if v:isPlaying() then v:pause() else v:play() end
    elseif key == "left" then
      v:seek(math.max(0, v:tell() - 1))
    elseif key == "right" then
      v:seek(v:tell() + 1)
    end
  end
end

function love.mousepressed()
  for _, p in ipairs(players) do start(p) end
end
