local results = {}
local function check(name, fn)
  local ok, err = pcall(fn)
  results[#results + 1] = { name = name, ok = ok and err ~= false, msg = ok and (type(err) == "string" and err or "") or tostring(err) }
  print((ok and err ~= false and "PASS " or "FAIL ") .. name .. " " .. tostring(results[#results].msg))
end

local shader, canvas, world, bodies, img, batch, mesh, psys, music, beep, runs

function love.load()
  check("filesystem save persists", function()
    local n = tonumber(love.filesystem.read("runs.txt") or "0") + 1
    assert(love.filesystem.write("runs.txt", tostring(n)))
    runs = n
    return "run #" .. n .. " (reload to see it increase) save dir: " .. love.filesystem.getSaveDirectory()
  end)
  check("shader", function()
    shader = love.graphics.newShader[[
      uniform float time;
      vec4 effect(vec4 color, Image tex, vec2 tc, vec2 sc) {
        return vec4(0.5 + 0.5 * sin(time + sc.x / 40.0), 0.5 + 0.5 * cos(time + sc.y / 30.0), 0.8, 1.0) * color;
      }
    ]]
    shader:send("time", 1)
  end)
  check("canvas", function()
    canvas = love.graphics.newCanvas(128, 128)
    love.graphics.setCanvas(canvas)
    love.graphics.clear(0, 0, 0, 0)
    love.graphics.setColor(0.2, 0.8, 0.3)
    love.graphics.circle("fill", 64, 64, 50)
    love.graphics.setCanvas()
    love.graphics.setColor(1, 1, 1)
    local d = love.graphics.readbackTexture(canvas)
    local r, g = d:getPixel(64, 64)
    assert(g > 0.7 and r < 0.3, ("readback pixel %.2f %.2f"):format(r, g))
  end)
  check("imagedata + image", function()
    local d = love.image.newImageData(32, 32)
    d:mapPixel(function(x, y) return x / 31, y / 31, 1, 1 end)
    img = love.graphics.newImage(d)
    img:setFilter("nearest", "nearest")
  end)
  check("spritebatch", function()
    batch = love.graphics.newSpriteBatch(img, 100)
    for i = 0, 9 do batch:add(i * 18, 0, 0, 0.5, 0.5) end
  end)
  check("mesh", function()
    mesh = love.graphics.newMesh({ { 0, 0, 0, 0, 1, 0, 0 }, { 60, 0, 1, 0, 0, 1, 0 }, { 30, 50, 0.5, 1, 0, 0, 1 } }, "triangles")
  end)
  check("particles", function()
    psys = love.graphics.newParticleSystem(img, 200)
    psys:setParticleLifetime(1, 2); psys:setEmissionRate(80)
    psys:setSpeed(40, 120); psys:setSpread(math.pi * 2); psys:setSizes(0.3, 0)
  end)
  check("physics", function()
    world = love.physics.newWorld(0, 400)
    local ground = love.physics.newBody(world, 450, 600, "static")
    love.physics.newRectangleShape(ground, 800, 20)
    bodies = {}
    for i = 1, 8 do
      local b = love.physics.newBody(world, 300 + i * 40, 100 - i * 30, "dynamic")
      love.physics.newRectangleShape(b, 30, 30)
      b:setAngularVelocity(i)
      bodies[i] = b
    end
    world:update(1 / 60)
  end)
  check("audio static ogg", function() beep = love.audio.newSource("beep.ogg", "static"); beep:play(); return ("%.2fs"):format(beep:getDuration()) end)
  check("audio wav", function() local s = love.audio.newSource("blip.wav", "static"); return ("%.2fs"):format(s:getDuration()) end)
  check("audio flac decoder", function() local d = love.sound.newDecoder("song.flac"); return ("%.2fs %dHz"):format(d:getDuration(), d:getSampleRate()) end)
  check("audio stream mp3", function() music = love.audio.newSource("music.mp3", "stream"); music:setVolume(0.2); music:play(); music:setLooping(true) end)
  check("sounddata synth", function()
    local sd = love.sound.newSoundData(4410, 44100, 16, 1)
    for i = 0, 4409 do sd:setSample(i, math.sin(i / 44100 * 880 * 2 * math.pi) * 0.3) end
    love.audio.newSource(sd):play()
  end)
  check("data compress/hash", function()
    local c = love.data.compress("string", "zlib", string.rep("love ", 100))
    assert(#love.data.decompress("string", "zlib", c) == 500)
    return love.data.encode("string", "hex", love.data.hash("string", "md5", "love"))
  end)
  check("math noise/random/transform", function()
    local rng = love.math.newRandomGenerator(42)
    local t = love.math.newTransform(10, 20, 0, 2, 2)
    local x, y = t:transformPoint(1, 1)
    assert(x == 12 and y == 22)
    return ("noise=%.3f rnd=%d"):format(love.math.perlinNoise(0.5, 0.25), rng:random(1, 100))
  end)
  check("font + utf8", function()
    local f = love.graphics.newFont(20)
    assert(f:getWidth("héllo wörld ❤") > 0)
    return "utf8.len=" .. require("utf8").len("héllo ❤")
  end)
  check("system", function() return love.system.getOS() .. " / " .. table.concat({ love.graphics.getRendererInfo() }, " | ") end)
  check("window", function() local w, h, f = love.window.getMode(); return w .. "x" .. h .. " dpi=" .. love.window.getDPIScale() end)
  check("love.thread", function() local c = love.thread.newChannel(); love.thread.newThread("local c = ...\nc:push(6 * 7)"):start(c); return "answer " .. c:demand(1) end)
  check("joystick module", function() return "joysticks: " .. love.joystick.getJoystickCount() end)
end

local t = 0
function love.update(dt)
  t = t + dt
  if world then world:update(dt) end
  if psys then psys:update(dt) end
  if shader then shader:send("time", t) end
  if math.floor(t) ~= math.floor(t - dt) and music then
    print(("t=%.1f music playing=%s tell=%.2f active=%d"):format(t, tostring(music:isPlaying()), music:tell(), love.audio.getActiveSourceCount()))
  end
end

function love.draw()
  love.graphics.setShader(shader)
  love.graphics.rectangle("fill", 520, 20, 360, 140)
  love.graphics.setShader()
  love.graphics.draw(canvas, 520, 170)
  love.graphics.draw(img, 660, 170, 0, 2, 2)
  love.graphics.draw(batch, 730, 170)
  love.graphics.draw(mesh, 730, 200)
  love.graphics.draw(psys, 820, 260)
  love.graphics.setColor(0.9, 0.5, 0.2)
  for _, b in ipairs(bodies) do love.graphics.polygon("fill", b:getWorldPoints(b:getShapes()[1]:getPoints())) end
  love.graphics.setColor(1, 1, 1)
  love.graphics.line(50, 590, 850, 590)
  for i, r in ipairs(results) do
    love.graphics.setColor(r.ok and { 0.5, 1, 0.5 } or { 1, 0.4, 0.4 })
    love.graphics.print((r.ok and "PASS  " or "FAIL  ") .. r.name .. "  " .. r.msg, 10, 4 + i * 16)
  end
  love.graphics.setColor(1, 1, 1)
  love.graphics.print(("FPS %d   music playing=%s tell=%.1f"):format(love.timer.getFPS(), tostring(music and music:isPlaying()), music and music:tell() or -1), 10, 615)
end
