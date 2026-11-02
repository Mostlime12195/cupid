-- string escapes (LuaJIT / Lua 5.2 / 5.3)
local function bytes(s) return table.concat({ s:byte(1, -1) }, " ") end
print(bytes("\x41\x62\x7a\xff\x00"))
print("a\z
      b", "c\z  d")
print(bytes("\u{48}\u{e9}\u{20AC}\u{1F600}\u{7FFFFFFF}"))
assert("\u{20AC}" == "\226\130\172")
assert("\x41" == "A")
local line = debug.getinfo(1, "l").currentline
local s = "x\z

  y"
assert(s == "xy" and debug.getinfo(1, "l").currentline == line + 4)
for _, src in ipairs({ [["\xZZ"]], [["\x4"]], [["\u{110000000}"]], [["\u48"]], [["\u{48"]] }) do
  local f, e = loadstring("return " .. src)
  print(f == nil, (tostring(e):gsub("^[^:]*:%d+: ", "")))
end
print(select(2, loadstring("return 1LL")))
print(select(2, loadstring("return 3i")))
