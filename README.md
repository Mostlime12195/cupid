# LÖVE 12 for the Web

The real [LÖVE](https://love2d.org) 12 engine compiled to WebAssembly, with a
player for publishing games and a browser-based editor for making them.

This isn't a reimplementation of the LÖVE API. It's the upstream C++ engine
(`love2d/love` main branch, 12.0 development) built with Emscripten, plus a
small set of patches for the browser. Games behave the way they do on
desktop, including LÖVE's own error screen, shaders, Box2D physics, audio
decoding, and save files.

## What's here

| Path | What it is |
| --- | --- |
| `dist/` | Ready-to-host build output: `index.html` (editor), `player.html`, `love.mjs` + `love.wasm` (engine), `examples/` |
| `love/` | LÖVE source (upstream commit `ef388d4`) with web patches |
| `deps/lua-5.1.5/` | PUC Lua 5.1.5 (LuaJIT can't target WebAssembly) |
| `deps/theora/` | libtheora 1.2.0 decoder, for `love.video` |
| `web/native/` | Web entry point (`web_main.cpp`) replacing LÖVE's blocking `main()` |
| `web/player/` | The player page (standalone and embeddable) |
| `web/editor/` | The editor (LÖVE Web Studio) |
| `web/examples/` | Example projects used by the editor |
| `build.py` | Build script (reads source lists from `love/CMakeLists.txt`) |
| `serve.py` | Local dev server |
| `package.py` | Command-line web export for a game folder or `.love` |
| `tools/` | Test games: `bench` (benchmark), `smoke` (feature checks), `threads`, `video`, `https`, `filedialog`, `textinput`; `test_server.py` serves them |

## Compared to love.js

[love.js](https://github.com/Davidobot/love.js) is the established way to run
LÖVE on the web. Its latest release (11.4.1, 2022) ships LÖVE 11.4.

| | love.js 11.4.1 | This project |
| --- | --- | --- |
| LÖVE version | 11.4 | 12.0 (current development branch) |
| Lua speed | PUC Lua 5.1 interpreter | Same interpreter plus a Lua-to-WebAssembly JIT: 1.7–5.6× faster on Lua-heavy code (measured in V8) |
| `love.thread` | Compatibility build: `Thread:start()` returns `false`. Threaded build: needs `SharedArrayBuffer`, i.e. COOP/COEP headers | Works on any host, no special headers (see below) |
| Audio | Compatibility build: "dodgy audio" (its readme) | Streaming refilled from the frame loop and a timer; keeps playing in background tabs |
| `bit` library (LuaJIT built-in) | Missing | Included (LuaBitOp), as a global and as `require "bit"` |
| Memory | Grows, but the initial size must cover the game's assets (`-m`) | Grows as needed; nothing to configure |
| Packaging | Node CLI builds a page and data package per game | Any `.love` file runs as is: by URL, drag and drop, or from the editor |
| Editor | None | Built-in editor with run, console and export |

The love.js entries come from its 11.4.1 npm package and readme. The threads
and `bit` rows were checked by running the same test game in both.

### Performance

**Lua-heavy code runs 1.7–5.6× faster than love.js** (code that mostly calls
C library functions gains 10–20%), thanks to a JIT that compiles hot Lua
functions to WebAssembly (see [The Lua JIT](#the-lua-jit)). Neither
LuaJIT nor any other machine-code JIT can run in a browser, so love.js (and
this project with `?jit=0`) interprets every Lua instruction.

Pure-Lua benchmark (`tools/jit/bench.lua`), median of 3 runs in V8 (Node
26, same engine as Chrome), same build flags as the web build. "JIT off" is
the plain PUC Lua 5.1 interpreter, which performs like love.js's (see the
next table):

| Test | JIT off | JIT on | Speedup |
| --- | --- | --- | --- |
| Mandelbrot 200×200 (float math loops) | 58.6 ms | 10.5 ms | 5.6× |
| Update 10k particles (table fields) | 27.0 ms | 5.9 ms | 4.6× |
| Sieve of Eratosthenes, 1M (array reads/writes) | 46.0 ms | 12.2 ms | 3.8× |
| Recursive `fib(27)` (Lua calls) | 26.6 ms | 15.5 ms | 1.7× |
| OOP with metatables, 300k objects | 115.0 ms | 97.2 ms | 1.2× |
| String formatting and patterns | 29.5 ms | 25.5 ms | 1.2× |
| Fill and `table.sort` 200k numbers | 39.8 ms | 36.1 ms | 1.1× |

The last three spend most of their time in C library functions
(`setmetatable`, `string.format`, `table.sort`) and allocation, which the
JIT doesn't change.

Against love.js, measured with the same `.love` (`tools/bench`, which runs
unchanged on 11.4 and 12) in the same Chrome instance, back to back, before
the JIT existed:

| Test (`tools/bench`) | love.js 11.4.1 | This project, interpreter only |
| --- | --- | --- |
| Lua: recursive `fib(27)` | 11.9 ms | 11.7 ms |
| Lua: fill and sort 200k-element table | 34.1 ms | 28.9 ms |
| Lua: string formatting and pattern matching | 20.4 ms | 20.2 ms |
| Lua: OOP with metatables (300k objects) | 69.3 ms | 71.4 ms |
| `ImageData:mapPixel` on 512×512 | 9.6 ms | 9.0 ms |
| CPU time to submit 20k sprites + 5k rectangles | 5.6 ms | 6.1 ms |
| Page load to `love.load` (local server, cached) | 90 ms | 112 ms |

One back-to-back run on an Apple Silicon Mac; runs varied by 10–20% while
other work was running, so treat differences under ~15% as noise. Reaching
parity needed one fix: with link-time optimization, Lua's interpreter loop
was inlined into the `setjmp`-protected function behind
`pcall`/`coroutine.resume`, so every call paid for WebAssembly exception
handling. `luaV_execute` is now kept out of line (`deps/lua-5.1.5/lvm.c`).

### Compared to desktop LÖVE

The same benchmark in the official macOS build of LÖVE 12 from the same
upstream commit (`love2d/love` CI run for `ef388d4`), which uses LuaJIT with
its JIT on, median of 3 runs:

| Test | Desktop LÖVE 12 (LuaJIT) | This project (web, JIT on) |
| --- | --- | --- |
| Recursive `fib(27)` | 0.9 ms | 15.5 ms |
| Fill and sort 200k numbers | 32.0 ms | 36.1 ms |
| String formatting and patterns | 5.3 ms | 25.5 ms |
| OOP with metatables, 300k objects | 10.9 ms | 97.2 ms |
| `ImageData:mapPixel` on 512×512 | 0.3 ms | 9.0 ms (browser, no JIT) |
| CPU time to submit 20k sprites + 5k rectangles | 14.1 ms (Metal) | 5.6–6.1 ms (WebGL 2) |

LuaJIT's trace compiler is in a different league for pure Lua (10–17×
faster on call- and allocation-heavy code); expect CPU-heavy Lua to need
noticeably more headroom on the web. Engine-side work (drawing, physics,
audio and image decoding) is the same C++ code on both, and drawing was
actually cheaper on the web in this test, so games that spend their frame
in LÖVE's C++ code rather than in Lua lose little. The web column for the
Lua rows comes from the Node/V8 runs above, not from inside a browser.

### Compared to p5.js

`tools/p5bench` is the same workload written the idiomatic p5.js way
(`tint`, `push`/`translate`/`rotate`/`image`/`pop`, `rect`, `pixels[]`).
Both ran in the same Chromium on an Apple Silicon Mac, 800×600 canvas at
pixel density 1. "Frame period" is the measured time between frames, so it
includes GPU work.

| Drawing 20k tinted, rotated sprites + 5k rectangles | Frame period |
| --- | --- |
| This project (WebGL 2) | 12 ms |
| p5.js 1.11.3, `WEBGL` | 400 ms |
| p5.js 2.0.3, `WEBGL` | 617 ms |
| p5.js 1.11.3, default canvas | about 3,800 ms |
| p5.js 2.0.3, default canvas | about 3,100 ms |
| p5.js 2.0.3, default canvas, sprites not tinted | 50 ms |

LÖVE batches consecutive draws of the same texture into one GPU draw call;
p5.js issues each shape separately, and its default canvas renderer redraws
the image into an offscreen canvas for every tinted draw.

Pure-language speed goes the other way, since V8 compiles JavaScript to
machine code directly (p5.js: median of 6 page loads; this project: 2 page
loads):

| Test | p5.js (JavaScript) | This project (Lua, JIT on) |
| --- | --- | --- |
| Recursive `fib(27)` | 1.0 ms | 12.5–13.9 ms |
| Fill and sort 200k numbers | 22 ms | 33–38 ms |
| String formatting and patterns | 3.4 ms | 25–30 ms |
| 300k small objects | 1.4 ms | 109–117 ms |
| Fill a 512×512 image | 0.6 ms (`pixels[]`) | 14–15 ms (`mapPixel`) |

So a sketch that draws a lot runs far faster here, while one dominated by
its own number crunching runs faster in p5.js.

Download size is larger than love.js because LÖVE 12 itself is: it adds the
glslang shader compiler, HarfBuzz text shaping, a Noto Sans default font,
and here also Theora video.

| | love.js 11.4.1 | This project |
| --- | --- | --- |
| Engine (`.wasm`), gzip | 1.63 MB | 2.52 MB |
| Glue JS, gzip | 0.07 MB | 0.06 MB |

## Threads without SharedArrayBuffer

Browsers can't block the main thread, and real threads need cross-origin
isolation headers that many hosts don't send. Instead, each `love.thread`
Thread gets its own Lua state and runs as a coroutine on the main thread
(`love/src/modules/thread/web/GreenThreads.cpp`):

- Threads are preempted by an instruction-count hook (Lua 5.1 can yield from
  hooks), so even `while true do ... end` worker loops don't freeze the page.
- `Channel:demand`, `Channel:supply`, `Thread:wait` and `love.timer.sleep`
  yield the thread until they can complete.
- When the main thread blocks (for example `channel:demand()` in
  `love.load`), other threads run until it can continue. A real deadlock
  raises an error instead of hanging the tab.
- Threads get whatever time is left in each 60 Hz frame after the game's
  update and draw (2 to 12 ms).

The trade-off: threads share one CPU core with the game, so they make
blocking-style code work but don't add parallel speed.

## The Lua JIT

`deps/lua-5.1.5/ljit.inc` compiles a Lua function to a small WebAssembly
module once it has been called, or has looped, 50 times. The browser then
compiles that to machine code like any other wasm.

- Lua values stay in the Lua stack, exactly where the interpreter keeps
  them, so compiled code can be entered and left at any instruction: after
  calls, when a coroutine resumes, from the middle of a hot loop, or to hand
  a frame back to the interpreter when a debug hook is set.
- Arithmetic, comparisons, numeric `for` loops, tests, moves, constants, and
  reads and writes of table fields (constant string keys, hashed at compile
  time) and array slots run inline. Everything that can involve metatables,
  the GC, errors or calls goes to helpers that are copies of the
  interpreter's own code, so behavior (including error messages) is
  identical.
- Compiled functions call other compiled functions directly, up to 64 deep.
- Frames with a line or count hook are interpreted, so `debug.sethook` and
  `love.thread`'s scheduler work unchanged.
- `?jit=0` on the player turns it off.

`tools/jit/` builds the standalone interpreter for Node and runs a
differential test suite: every test must print exactly the same with the
JIT off, compiling everything immediately, and with a mix of compiled and
interpreted frames.

```bash
sh tools/jit/build-node.sh
```

```bash
bash tools/jit/test.sh
```

## Building

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
(tested with 6.0.13) and Python 3.10+.

```bash
source /path/to/emsdk/emsdk_env.sh
```

```bash
python3 build.py
```

Builds are incremental (about 400 source files the first time). Other options:

- `python3 build.py --debug` for assertions and no optimization
- `python3 build.py --web-only` to copy only the player, editor and examples into `dist/`
- `python3 build.py --profiling` to keep function names for size analysis (writes to `build/profiling/`)

Then serve `dist/`:

```bash
python3 serve.py
```

and open http://localhost:8765/ for the editor or
http://localhost:8765/player.html?game=examples/hello.love for the player.

The test games in `tools/` run with `python3 tools/test_server.py 8772`,
which zips them on request and adds the routes the `https` test needs:
http://localhost:8772/player.html?game=tools/https.love (or `smoke`,
`filedialog`, `textinput`, ...).

## The editor (LÖVE Web Studio)

`dist/index.html`. Everything is stored in the browser (IndexedDB); nothing is
uploaded anywhere.

- Projects with a file tree; import files, folders, `.love` and `.zip` by drag and drop
- CodeMirror 6 editor with Lua highlighting and LÖVE 12 autocompletion. The
  API list (`web/editor/love-api.json`) is generated by introspecting the
  real engine (`web/editor/tools/introspect.html`)
- Run (F5 or Ctrl+Enter), Stop, Restart, Pause, auto-run on save, pop-out window
- Console with clickable `file:line` errors that jump to the line
- Image, audio and font previews
- Export `.love`, export a ready-to-upload web build (zip), share links for small projects
- Examples: platformer, particles, shaders, physics, text input, canvas and blend modes, synth
- Dark and light themes; works on narrow screens

The editor loads CodeMirror from esm.sh, so the first load needs an internet
connection (it falls back to a plain text area if that fails).

## Publishing a game

Copy `player.html`, `player.js`, `player.css`, `love.mjs` and `love.wasm` next
to your `game.love`, and add this to `player.html`'s `<head>`:

```html
<meta name="love-game" content="game.love">
```

The editor's **Export for web** does this for you, as does the command line:

```bash
python3 package.py path/to/game out/ --zip
```

Either produces a zip you can upload to itch.io as an HTML game. Serving `.wasm` as `application/wasm`
lets browsers compile the engine while it downloads; most hosts already do.

Player URL options:

| Option | Effect |
| --- | --- |
| `?game=url` | Load a `.love` from a URL |
| `?autoplay=1` | Skip the Play button (audio may stay muted until the first click) |
| `?scale=fit` (default) / `integer` / `none` | How a fixed-size window is scaled to the page |
| `?pixelated=1` | Nearest-neighbour upscaling (also implied by `scale=integer`) |
| `?embed=1` | Embedded mode, controlled with `postMessage` (see `web/player/player.js`) |

## Web-specific behavior

- `love.system.getOS()` returns `"Web"`.
- Save files live in IndexedDB and are flushed every 2 seconds, when the tab
  is hidden, and when the game quits.
- `love.timer.sleep` on the main thread doesn't block (it lets threads run instead).
- Text input is enabled by default, as on desktop, except on touch devices
  (no hover, coarse pointer), where it starts off as on Android and iOS.
  While it's on, a hidden text field has keyboard focus instead of the
  canvas, so IME composition (Chinese, Japanese, Korean), emoji pickers,
  dictation and on-screen keyboards reach `love.textinput` (composition in
  progress is reported to `love.textedited`). `love.keypressed` works as
  before; keys that belong to an IME composition don't produce key presses.
  Pasting into the field is ignored, as on desktop: use
  `love.system.getClipboardText`.
- On touch devices, tap the game while text input is on to bring up the
  on-screen keyboard (browsers only show it in response to a tap).
  `love.keyboard.setTextInput(true, x, y, w, h)` moves the hidden field over
  that rectangle, which is where IME candidate windows appear.
- `require("https")` (lua-https) works, using a synchronous `XMLHttpRequest`:
  it blocks like on desktop. Browser rules apply: other sites must allow
  cross-origin requests (CORS), an `https` page can't request `http` URLs,
  and some request headers (`User-Agent`, `Content-Length`, ...) are set by
  the browser. Network errors, including refused CORS requests, return
  status `0` with an error message as the body (like lua-https on macOS).
  Response header names are lower case. Relative URLs are resolved against
  the page, so a game can request files hosted next to it.
- `love.window.showFileDialog` uses the browser's file picker.
  - `openfile` and `openfolder` copy what was picked into the in-memory file
    system (`/picked/<n>/...`) and return those paths; read them as on
    desktop with `love.filesystem.openNativeFile`, `mountFullPath` or
    `io.open`. Filters become the picker's `accept` list; browsers can't
    show a title or button labels.
  - `savefile` returns `/picked/<n>/<defaultname>`. When the game has
    written and closed that file, the browser downloads it under that name
    (again on every later save to it).
  - Cancelling returns an empty list, as on desktop.
  - Browsers only open pickers in response to a click or key press. If the
    game calls `showFileDialog` from `love.keypressed` or
    `love.mousepressed`, the picker opens directly (in browsers that report
    `navigator.userActivation`). Otherwise, and always for `savefile`, the
    player shows a small bar with a "Choose file…" / "Save" button and
    Cancel; the picker opens when it's clicked.
- Games with `t.window.resizable = true` fill the page and get `love.resize`.
- `love.window.setFullscreen` resizes the game immediately; the browser
  enters real fullscreen on the next click or key press (browsers only allow
  fullscreen from a user gesture).

## Not supported

- Networking modules: `socket` (LuaSocket) and `enet` (browsers have no raw
  sockets). `https` works, see above.
- WOFF2 fonts (FreeType is built without Brotli); TTF and OTF work.
- LuaJIT itself and its `ffi` library (Lua runs on PUC Lua 5.1 with this project's own JIT; `bit` is provided).
- Compute shaders (WebGL 2 has none).

## How it works

`web/native/web_main.cpp` replaces LÖVE's `main()`. On desktop LÖVE resumes
its boot coroutine in a `while` loop. `boot.lua` yields once per iteration of
`love.run`'s main loop, so on the web that coroutine is resumed once per
`requestAnimationFrame` instead. Nothing in LÖVE's own Lua code changes.

The engine patches (`git diff dca79b4 -- love deps`) are kept small so they
can be rebased onto newer LÖVE commits:

- Add an `LOVE_EMSCRIPTEN` platform (`config.h`) and leave out LuaSocket
  and ENet.
- lua-https gets a browser backend
  (`love/src/libraries/luahttps/src/emscripten`, requests made in
  `web/native/library_love.js`).
- WebGL 2 differences: no texture swizzle (LA8 textures use
  `LUMINANCE_ALPHA`), no `GL_FRAMEBUFFER_SRGB` toggle.
- `love.timer` uses `emscripten_get_now`; sleeping doesn't block.
- A Lua-to-WebAssembly JIT (above), in `deps/lua-5.1.5/ljit.inc` with small hooks in `lvm.c`, `ldo.c` and `lfunc.c`.
- Green threads for `love.thread` (above); audio streaming refills from the
  frame loop and a timer instead of a pool thread, and recovers from underruns.
- `love.video` decodes Theora from the frame loop too, instead of its worker
  thread. libtheora's decoder is built from source (`deps/theora`, without
  its x86/ARM assembly); libogg is Emscripten's port.
- A streaming Source seeked while paused or stopped no longer skips ahead
  when played again (this showed up as a video with audio jumping ahead).
- `boot.lua` skips modules the build doesn't include.
- LuaBitOp (`deps/luabitop`) is loaded as the global `bit`, as on LuaJIT.
- SDL3 text input is started when the window is created (not on touch
  devices). Text from the page's hidden text field is pushed as SDL text
  events by `web/native/web_main.cpp`.
- `love.window.showFileDialog` hands the dialog to `library_love.js`
  instead of SDL (`love/src/modules/window/sdl/Window.cpp`).

## Licenses

LÖVE is under the zlib license (`love/license.txt`); its bundled libraries
keep their own licenses. Lua 5.1 is MIT (`deps/lua-5.1.5/COPYRIGHT`), as is
LuaBitOp (`deps/luabitop/bit.c`). libtheora is BSD-style
(`deps/theora/COPYING`).
Changes in this repository are marked in the source and follow the same
licenses.
