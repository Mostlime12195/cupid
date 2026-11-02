-- LuaJIT extensions and web-specific errors, inside the real engine.
local results = {}
local function check(name, ok, detail)
  results[#results + 1] = ("%s %s%s"):format(ok and "PASS" or "FAIL", name, detail and (": " .. detail) or "")
  print("COMPAT " .. results[#results])
end

function love.load()
  local n = 0
  for i = 1, 10 do if i % 2 == 0 then goto continue end n = n + i ::continue:: end
  check("goto", n == 25)
  check("escapes", "\x41\u{20AC}" == "A\226\130\172")
  local co = coroutine.wrap(function()
    local ok, v = pcall(function() return coroutine.yield(1) + 1 end)
    return ok and v
  end)
  co()
  check("yield in pcall", co(41) == 42)
  check("table.new", type(require("table.new")) == "function")
  check("load with env", load("return x", "c", "t", { x = 5 })() == 5)
  local ok, err = pcall(require, "ffi")
  check("ffi message", not ok and err:find("isn't available on the web") ~= nil, err)
  ok, err = pcall(require, "socket")
  check("socket message", not ok and err:find("raw network sockets") ~= nil, err)
  check("jit global", jit == nil)
  local f, lerr = loadstring("return 1LL")
  check("LL message", f == nil and lerr:find("FFI") ~= nil, lerr)
  print("COMPAT done")
end

function love.draw()
  for i, r in ipairs(results) do love.graphics.print(r, 10, i * 18) end
end
