-- require("https") checks. Needs tools/test_server.py (its /test/ routes):
--   python3 tools/test_server.py 8772
--   http://localhost:8772/player.html?game=tools/https.love
local https = require("https")

local lines = {}
local passed, failed = 0, 0

local function check(name, fn)
  local ok, err = pcall(fn)
  if ok and err ~= false then passed = passed + 1 else failed = failed + 1 end
  local line = ((ok and err ~= false) and "PASS " or "FAIL ") .. name .. ((type(err) == "string" and err ~= "") and (" " .. err) or "")
  lines[#lines + 1] = line
  print(line)
end

local function expected_bytes(n)
  local t = {}
  for i = 0, n - 1 do t[#t + 1] = string.char(i % 256) end
  return table.concat(t)
end

function love.load()
  check("simple GET returns code and body only", function()
    local code, body, headers = https.request("/test/echo?x=1")
    assert(code == 200, tostring(code))
    assert(body == "", ("%q"):format(body))
    assert(headers == nil, "simple form returns no headers")
    assert(select("#", https.request("/test/echo")) == 2, "returns 2 values")
  end)

  local origin
  check("absolute URL", function()
    local code, body = https.request("/test/origin")
    assert(code == 200, tostring(code))
    origin = body
    local code2 = https.request(origin .. "/test/status/200")
    assert(code2 == 200, tostring(code2))
    return origin
  end)

  check("404", function()
    local code, body = https.request("/does-not-exist.txt")
    assert(code == 404, tostring(code))
  end)

  check("other status (418)", function()
    local code, body = https.request("/test/status/418")
    assert(code == 418, tostring(code))
    assert(body == "status 418", body)
  end)

  check("POST data: default method and Content-Type", function()
    local code, body, headers = https.request("/test/echo", { data = "a=1&b=2" })
    assert(code == 200, tostring(code))
    assert(body == "a=1&b=2", body)
    assert(headers["x-echo-method"] == "POST", tostring(headers["x-echo-method"]))
    assert(headers["x-echo-content-type"] == "application/x-www-form-urlencoded", tostring(headers["x-echo-content-type"]))
  end)

  check("PUT with custom headers", function()
    local code, body, headers = https.request("/test/echo", {
      method = "put", data = "{}", headers = { ["Content-Type"] = "application/json", ["X-Test"] = "abc" },
    })
    assert(code == 200, tostring(code))
    assert(headers["x-echo-method"] == "PUT")
    assert(headers["x-echo-x-test"] == "abc", tostring(headers["x-echo-x-test"]))
    assert(headers["x-echo-content-type"] == "application/json", tostring(headers["x-echo-content-type"]))
    assert(body == "{}")
  end)

  check("DELETE and PATCH", function()
    for _, m in ipairs({ "delete", "PATCH" }) do
      local code, _, headers = https.request("/test/echo", { method = m })
      assert(code == 200 and headers["x-echo-method"] == m:upper(), m)
    end
  end)

  check("HEAD has no body", function()
    local code, body, headers = https.request("/test/bytes?n=100", { method = "head" })
    assert(code == 200, tostring(code))
    assert(body == "", ("%d bytes"):format(#body))
    assert(headers["content-length"] == "100", tostring(headers["content-length"]))
  end)

  check("redirect is followed", function()
    local code, _, headers = https.request("/test/redirect", { method = "get" })
    assert(code == 200, tostring(code))
    assert(headers["x-echo-method"] == "GET")
  end)

  check("binary GET (all byte values)", function()
    local n = 300000
    local code, body = https.request("/test/bytes?n=" .. n)
    assert(code == 200, tostring(code))
    assert(#body == n, ("got %d bytes"):format(#body))
    assert(body == expected_bytes(n), "bytes differ")
    return ("%d bytes"):format(#body)
  end)

  check("binary POST round trip", function()
    local data = expected_bytes(70000) .. "\0\255\0"
    local code, body = https.request("/test/echo", { data = data, headers = { ["Content-Type"] = "application/octet-stream" } })
    assert(code == 200, tostring(code))
    assert(body == data, ("got %d bytes, sent %d"):format(#body, #data))
  end)

  check("static binary file hash (love.wasm)", function()
    local code, body = https.request("love.wasm")
    assert(code == 200, tostring(code))
    local _, want = https.request("/test/sha256?path=love.wasm")
    local got = love.data.encode("string", "hex", love.data.hash("string", "sha256", body))
    assert(got == want, got .. " ~= " .. tostring(want))
    return ("%d bytes, sha256 %s"):format(#body, got:sub(1, 16))
  end)

  check("unknown scheme: nil, error", function()
    local code, err = https.request("ftp://example.com/x")
    assert(code == nil, tostring(code))
    assert(err == "Unknown url schema", tostring(err))
    return err
  end)

  check("invalid method raises an error", function()
    local ok, err = pcall(https.request, "/test/echo", { method = "brew" })
    assert(not ok and err:find("expected one of"), tostring(err))
  end)

  check("cross-origin without CORS: code 0 and a message", function()
    local _, other = https.request("/test/origin?other=1")
    local code, body = https.request(other .. "/test/echo")
    assert(code == 0, tostring(code))
    assert(#body > 0, "no message")
    return body
  end)

  check("cross-origin with CORS", function()
    local _, other = https.request("/test/origin?other=1")
    local code, body = https.request(other .. "/test/cors")
    assert(code == 200, tostring(code))
    assert(body == "cors ok", body)
  end)

  check("connection refused: code 0", function()
    local code, body = https.request("http://localhost:1/")
    assert(code == 0, tostring(code))
    return body
  end)

  local summary = ("https: %d passed, %d failed"):format(passed, failed)
  lines[#lines + 1] = summary
  print(summary)
end

function love.draw()
  love.graphics.setColor(1, 1, 1)
  for i, line in ipairs(lines) do
    if line:sub(1, 4) == "FAIL" then love.graphics.setColor(1, 0.4, 0.4) else love.graphics.setColor(0.8, 1, 0.8) end
    love.graphics.print(line, 10, 10 + (i - 1) * 18)
  end
end
