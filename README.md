# Cupid

Cupid is LÖVE 12 for the web. Cupid compiles the real [LÖVE](https://love2d.org) 12 engine to WebAssembly. Cupid
also has a player and a browser-based editor. Use the player to publish games. Use the editor to make games. To try
the editor, go to <https://mostlime12195.github.io/cupid/>.

Cupid is an independent project. Cupid has no affiliation with the LÖVE team.

Cupid is not a new implementation of the LÖVE API. Cupid uses the upstream C++ engine (`love2d/love` main branch,
12.0 development). Cupid builds this engine with Emscripten and adds a small set of patches for the browser. Games
behave the same as on desktop. This includes the LÖVE error screen, shaders, Box2D physics, audio decoding and save
files.

## What is in this repository

| Path | What it is |
| --- | --- |
| `dist/` | Build output, ready to host: `index.html` (editor), `player.html`, `love.mjs` + `love.wasm` (engine), `examples/` |
| `love/` | LÖVE source (upstream commit `ef388d4`) with web patches |
| `deps/lua-5.1.5/` | PUC Lua 5.1.5 (LuaJIT does not support WebAssembly) |
| `deps/theora/` | libtheora 1.2.0 decoder, for `love.video` |
| `web/native/` | Web entry point (`web_main.cpp`). It replaces the blocking `main()` of LÖVE. |
| `web/player/` | The player page. You can use it standalone or embed it. |
| `web/editor/` | The Cupid editor |
| `web/examples/` | Example projects for the editor |
| `build.py` | Build script. It reads the source lists from `love/CMakeLists.txt`. |
| `serve.py` | Local development server |
| `package.py` | Command-line web export for a game folder or a `.love` file |
| `tools/` | Test games: `bench` (benchmark), `smoke` (feature checks), `threads`, `video`, `https`, `filedialog`, `textinput`. `test_server.py` serves them. |

## Compared to love.js

[love.js](https://github.com/Davidobot/love.js) is the usual way to run LÖVE on the web. Its latest release (11.4.1,
2022) has LÖVE 11.4.

| | love.js 11.4.1 | Cupid |
| --- | --- | --- |
| LÖVE version | 11.4 | 12.0 (current development branch) |
| Lua speed | PUC Lua 5.1 interpreter | The same interpreter and a Lua-to-WebAssembly JIT. Lua-heavy code runs 1.7–5.6× faster (measurement in V8). |
| `love.thread` | Compatibility build: `Thread:start()` returns `false`. Threaded build: needs `SharedArrayBuffer`, which needs COOP/COEP headers. | Works on all hosts. Needs no special headers (see below). |
| Audio | Compatibility build: "dodgy audio" (from its readme) | The frame loop and a timer refill the audio stream. Audio continues to play in background tabs. |
| `bit` library (LuaJIT built-in) | Missing | Included (LuaBitOp), as a global and as `require "bit"` |
| LuaJIT language extensions (`goto`, `\x` escapes, yield inside `pcall`, ...) | Missing | Included (see "Lua compatibility") |
| Memory | Grows, but the initial size must be large enough for the game assets (`-m`) | Grows when necessary. You do not need to configure it. |
| Packaging | A Node CLI builds a page and a data package for each game | All `.love` files run without changes: from a URL, by drag and drop, or from the editor |
| Editor | None | Built-in editor with run, console and export |

The love.js entries come from its 11.4.1 npm package and its readme. The threads and `bit` rows come from a test that
ran the same test game in both projects.

### Performance

**Lua-heavy code runs 1.7–5.6× faster than love.js.** Code that mostly calls C library functions gains 10–20%. The
speed comes from a JIT that compiles frequently used Lua functions to WebAssembly (see [The Lua JIT](#the-lua-jit)).

LuaJIT cannot run in a browser. No other machine-code JIT can run in a browser. Thus, love.js interprets every Lua
instruction. Cupid with `?jit=0` also interprets every Lua instruction.

The next table shows a pure-Lua benchmark (`tools/jit/bench.lua`). The values are the median of 3 runs in V8 (Node 26,
the same engine as Chrome). The benchmark uses the same build flags as the web build. "JIT off" is the plain PUC Lua
5.1 interpreter. Its speed is approximately the same as the love.js interpreter (see the love.js table below).

| Test | JIT off | JIT on | Speedup |
| --- | --- | --- | --- |
| Mandelbrot 200×200 (float math loops) | 58.6 ms | 10.5 ms | 5.6× |
| Update 10k particles (table fields) | 27.0 ms | 5.9 ms | 4.6× |
| Sieve of Eratosthenes, 1M (array reads/writes) | 46.0 ms | 12.2 ms | 3.8× |
| Recursive `fib(27)` (Lua calls) | 26.6 ms | 15.5 ms | 1.7× |
| OOP with metatables, 300k objects | 115.0 ms | 97.2 ms | 1.2× |
| String formatting and patterns | 29.5 ms | 25.5 ms | 1.2× |
| Fill and `table.sort` 200k numbers | 39.8 ms | 36.1 ms | 1.1× |

The last three tests use most of their time in C library functions (`setmetatable`, `string.format`, `table.sort`)
and in allocation. The JIT does not change these parts.

The next table compares love.js and Cupid. Both ran the same `.love` file (`tools/bench`). This file runs without
changes on 11.4 and 12. Both ran in the same Chrome instance, one immediately after the other. These measurements are
from before the JIT existed.

| Test (`tools/bench`) | love.js 11.4.1 | Cupid, interpreter only |
| --- | --- | --- |
| Lua: recursive `fib(27)` | 11.9 ms | 11.7 ms |
| Lua: fill and sort 200k-element table | 34.1 ms | 28.9 ms |
| Lua: string formatting and pattern matching | 20.4 ms | 20.2 ms |
| Lua: OOP with metatables (300k objects) | 69.3 ms | 71.4 ms |
| `ImageData:mapPixel` on 512×512 | 9.6 ms | 9.0 ms |
| CPU time to submit 20k sprites + 5k rectangles | 5.6 ms | 6.1 ms |
| Page load to `love.load` (local server, cached) | 90 ms | 112 ms |

The data is from one back-to-back run on an Apple Silicon Mac. While other work ran, the results changed by 10–20%.
Thus, treat differences under ~15% as noise.

To get the same speed as love.js, one fix was necessary. Link-time optimization inlined the Lua interpreter loop into
the `setjmp`-protected function behind `pcall`/`coroutine.resume`. Thus, every call had the cost of WebAssembly
exception handling. Now, `luaV_execute` stays out of line (`deps/lua-5.1.5/lvm.c`).

### Compared to desktop LÖVE

The next table shows the same benchmark in the official macOS build of LÖVE 12. This build is from the same upstream
commit (`love2d/love` CI run for `ef388d4`). This build uses LuaJIT with its JIT on. The values are the median
of 3 runs.

| Test | Desktop LÖVE 12 (LuaJIT) | Cupid (web, JIT on) |
| --- | --- | --- |
| Recursive `fib(27)` | 0.9 ms | 15.5 ms |
| Fill and sort 200k numbers | 32.0 ms | 36.1 ms |
| String formatting and patterns | 5.3 ms | 25.5 ms |
| OOP with metatables, 300k objects | 10.9 ms | 97.2 ms |
| `ImageData:mapPixel` on 512×512 | 0.3 ms | 9.0 ms (browser, no JIT) |
| CPU time to submit 20k sprites + 5k rectangles | 14.1 ms (Metal) | 5.6–6.1 ms (WebGL 2) |

For pure Lua, the LuaJIT trace compiler is much faster. It is 10–17× faster on call-heavy and allocation-heavy code.
On the web, CPU-heavy Lua code needs noticeably more CPU time.

Engine work (drawing, physics, audio decoding and image decoding) uses the same C++ code on both. In this test, drawing
used less CPU time on the web. Thus, games that use most of their frame time in the LÖVE C++ code, not in Lua, lose only
a small amount of speed. The web column for the Lua rows comes from the Node/V8 runs above. It does not come
from a browser.

### Compared to p5.js

`tools/p5bench` does the same workload in the usual p5.js style (`tint`, `push`/`translate`/`rotate`/`image`/`pop`,
`rect`, `pixels[]`). Both tests ran in the same Chromium on an Apple Silicon Mac. The canvas was 800×600 at pixel
density 1. "Frame period" is the measured time between frames. Thus, it includes GPU work.

| Drawing 20k tinted, rotated sprites + 5k rectangles | Frame period |
| --- | --- |
| Cupid (WebGL 2) | 12 ms |
| p5.js 1.11.3, `WEBGL` | 400 ms |
| p5.js 2.0.3, `WEBGL` | 617 ms |
| p5.js 1.11.3, default canvas | about 3,800 ms |
| p5.js 2.0.3, default canvas | about 3,100 ms |
| p5.js 2.0.3, default canvas, sprites not tinted | 50 ms |

LÖVE combines consecutive draws of the same texture into one GPU draw call. p5.js sends each shape separately. For
each tinted draw, the default canvas renderer of p5.js draws the image again into an offscreen canvas.

For pure language speed, the result is the opposite. V8 compiles JavaScript directly to machine code. The p5.js values
are the median of 6 page loads. The Cupid values come from 2 page loads.

| Test | p5.js (JavaScript) | Cupid (Lua, JIT on) |
| --- | --- | --- |
| Recursive `fib(27)` | 1.0 ms | 12.5–13.9 ms |
| Fill and sort 200k numbers | 22 ms | 33–38 ms |
| String formatting and patterns | 3.4 ms | 25–30 ms |
| 300k small objects | 1.4 ms | 109–117 ms |
| Fill a 512×512 image | 0.6 ms (`pixels[]`) | 14–15 ms (`mapPixel`) |

Thus, a sketch that draws a lot runs much faster in Cupid. A sketch that mostly does its own calculations runs faster
in p5.js.

### Download size

The download of Cupid is larger than love.js because LÖVE 12 itself is larger. LÖVE 12 adds the glslang shader
compiler, HarfBuzz text shaping and a Noto Sans default font. Cupid also adds Theora video.

| | love.js 11.4.1 | Cupid |
| --- | --- | --- |
| Engine (`.wasm`), gzip | 1.63 MB | 2.52 MB |
| Glue JS, gzip | 0.07 MB | 0.06 MB |

## Threads without SharedArrayBuffer

Browsers cannot block the main thread. Real threads need cross-origin isolation headers, and many hosts do not send
these headers. Instead, each `love.thread` Thread has its own Lua state. Each Thread runs as a coroutine on the main
thread (`love/src/modules/thread/web/GreenThreads.cpp`):

- An instruction-count hook preempts the threads (Lua 5.1 can yield from hooks). Thus, even `while true do ... end`
  worker loops do not freeze the page.
- `Channel:demand`, `Channel:supply`, `Thread:wait` and `love.timer.sleep` yield the thread until they can complete.
- When the main thread blocks (for example `channel:demand()` in `love.load`), other threads run until the main thread
  can continue. A real deadlock causes an error and does not freeze the tab.
- Threads get the time that remains in each 60 Hz frame after the game update and draw (2 to 12 ms).

There is a disadvantage. Threads share one CPU core with the game. Thus, blocking-style code works, but threads do not
add parallel speed.

## Lua compatibility

Desktop LÖVE runs LuaJIT, which is Lua 5.1 with extensions. Cupid runs PUC Lua 5.1.5. Cupid adds back the LuaJIT 2.1
extensions that games and libraries commonly use (`deps/lua-5.1.5`). Thus, code for desktop LÖVE loads and behaves
the same in Cupid:

| Extension | Notes |
| --- | --- |
| `goto` and `::labels::` | Lua 5.2 rules. A `goto` cannot jump into the scope of a local. Labels are visible in nested blocks. `goto continue` to a label at the end of a loop works. |
| `\xXX`, `\z` and `\u{XXXX}` string escapes | |
| `coroutine.yield` inside `pcall`/`xpcall` | Inside a coroutine, the function runs in a child coroutine that passes yields through. `coroutine.running`/`status` report the parent. `xpcall` handlers run at the point of the error. Thus, `debug.traceback` shows the code that failed. Outside coroutines, `pcall` is the plain C function, at full speed. |
| `load(string [, name [, mode [, env]]])`, `loadfile(name, mode, env)` | |
| `xpcall(f, handler, ...)` | Passes the extra arguments to `f` |
| `math.log(x, base)`, `string.rep(s, n, sep)` | |
| `string.format`: `%s` uses `__tostring` and accepts any value; `%a` | |
| `table.move`, `require("table.new")`, `require("table.clear")` | |
| `coroutine.isyieldable`, `package.searchpath`, `os.exit(true/false)` | |
| `bit` | LuaBitOp, as on LuaJIT |

Some features cannot work in a browser. These features fail with a message that gives the reason, not with a generic
error:

- `require("ffi")`, `require("jit")`, `jit.*` modules and `string.buffer`: "isn't available on the web: it's part of
  LuaJIT ...". The `jit` global is nil. Thus, libraries that check `if jit then` use their plain-Lua path.
- `require("socket")`, `"enet"` and the other LuaSocket modules: browsers have no raw sockets. Use `require("https")`
  instead.
- 64-bit literals (`1LL`, `1ULL`) and imaginary literals (`2i`): a syntax error that names the LuaJIT FFI.
- A yield inside a metamethod, a `for ... in` iterator or a C function: the error says that a yield works inside
  `pcall`, but not there. LuaJIT allows the first two. The comparator of `table.sort` is an example of a C function.

### Compatibility check

Each time the editor runs or exports a project, it checks the Lua files of the project. `package.py` does the same
check when it packages a game. The console (or stderr) shows each problem with the file and line. The check finds
these problems:

- LuaJIT-only modules and networking modules that the code requires without `pcall`
- 64-bit literals
- Compute shaders
- `io.popen` and `os.execute`
- Unguarded `jit.*` calls

The check also gives notes about these items:

- `love.system.getOS()` returns `"Web"`.
- Threads share one core.

The rules are in `web/editor/compat-rules.json`.

## The Lua JIT

`deps/lua-5.1.5/ljit.inc` compiles a Lua function to a small WebAssembly module after 50 calls or loop iterations. The
browser then compiles that module to machine code, as with all other wasm.

- Lua values stay in the Lua stack, at the same location as in the interpreter. Thus, execution can enter and leave
  compiled code at any instruction. This is possible after calls, when a coroutine resumes, and in the middle of a
  frequently used loop. Also, when code sets a debug hook, the JIT can give a frame back to the interpreter.
- These operations run inline: arithmetic, comparisons, numeric `for` loops, tests, moves, constants, and reads and
  writes of table fields and array slots. For table fields, the keys are constant strings, and the JIT hashes them at
  compile time.
- Operations that can involve metatables, the garbage collector (GC), errors or calls go to helper functions. These
  helper functions are copies of the interpreter code. Thus, the behavior is identical, including the error messages.
- Compiled functions call other compiled functions directly, up to a depth of 64.
- Some math calls become single WebAssembly instructions: `math.floor`, `ceil`, `sqrt` and `abs` with one number, and
  `math.min` and `max` with two numbers. First, the JIT checks that the function in the call is that same function.
  This also works through `local floor = math.floor`.
- The interpreter runs frames that have a line hook or a count hook. Thus, `debug.sethook` and the `love.thread`
  scheduler work without changes.
- To turn off the JIT, add `?jit=0` to the player URL.

`tools/jit/` builds the standalone interpreter for Node and runs a differential test suite. Each test must print
exactly the same output in these three modes:

- The JIT is off.
- The JIT compiles everything immediately.
- The code uses a mix of compiled and interpreted frames.

To build the interpreter, run this command:

```bash
sh tools/jit/build-node.sh
```

To run the test suite, run this command:

```bash
bash tools/jit/test.sh
```

## Building

You need the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) and Python 3.10+. The tested
Emscripten SDK version is 6.0.13.

First, load the Emscripten environment:

```bash
source /path/to/emsdk/emsdk_env.sh
```

Then, run the build script:

```bash
python3 build.py
```

Builds are incremental. The first build compiles about 400 source files. The build script has these other options:

- `python3 build.py --debug`: build with assertions and no optimization.
- `python3 build.py --web-only`: copy only the player, the editor and the examples into `dist/`.
- `python3 build.py --profiling`: keep function names for size analysis. The output goes to `build/profiling/`.

Then, serve `dist/`:

```bash
python3 serve.py
```

Open http://localhost:8765/ for the editor. Open http://localhost:8765/player.html?game=examples/hello.love for the
player.

To run the test games in `tools/`, use `python3 tools/test_server.py 8772`. This server makes a zip of each game when
the browser requests it. The server also adds the routes that the `https` test needs. For example, open
http://localhost:8772/player.html?game=tools/https.love (or `smoke`, `filedialog`, `textinput`, ...).

## The editor

The editor is `dist/index.html`. The editor stores all data in the browser (IndexedDB). The editor does not upload
anything.

- Projects with a file tree. To import files, folders, `.love` files and `.zip` files, drag and drop them.
- CodeMirror 6 editor with Lua highlighting and LÖVE 12 autocompletion. `web/editor/tools/introspect.html` generates
  the API list (`web/editor/love-api.json`) from the real engine.
- Run (F5 or Ctrl+Enter), Stop, Restart, Pause, run after save, and a pop-out window.
- Command palette (Ctrl+Shift+P or F1), go to file (Ctrl+P) and go to function (Ctrl+Shift+O). The status bar has a
  go-to-line function.
- Settings (Ctrl+,) for these items:
  - Theme (system, light, dark)
  - Font size (Ctrl +/−)
  - Tab size, and spaces or tabs
  - Word wrap (Alt+Z)
  - Line numbers, whitespace and bracket closing
  - Suggestions and auto save
  - Trim whitespace on save
  - Game scaling (fit or pixel-perfect)
  - The Lua JIT
  - Console timestamps and wrapping
  - A list of every keyboard shortcut
- Console. Click a `file:line` error to go to that line. You can filter the output and copy it.
- Web compatibility check on each run and export. The status bar shows the number of problems. Click the number to
  open the list of problems.
- Image, audio and font previews.
- Export to `.love`, export to a web build (zip) that is ready to upload, and share links for small projects.
- Examples: platformer, particles, shaders, physics, text input, canvas and blend modes, synth.
- The editor works on narrow screens.

The editor has a bundled copy of CodeMirror (`web/editor/vendor/codemirror.js`). Thus, the editor loads from its own
host. To rebuild this bundle, use `web/editor/tools/bundle-codemirror.sh`.

## Publishing a game

Copy `player.html`, `player.js`, `player.css`, `love.mjs` and `love.wasm` next to your `game.love`. Then, add
this line to the `<head>` of `player.html`:

```html
<meta name="love-game" content="game.love">
```

The **Export for web** command in the editor does these steps for you. The command line can also do these steps:

```bash
python3 package.py path/to/game out/ --zip
```

Both methods make a zip file. You can upload this zip file to itch.io as an HTML game.

Serve `.wasm` files as `application/wasm`. Then browsers can compile the engine while it downloads. Most hosts already
do this.

While the game loads, the player shows a splash screen (the Cupid heart). Then the splash screen fades into the game
automatically. To show your own splash screen, put a `splash.png` file at the top level of the `.love` file. To change
the other settings, put a `splash.json` file next to it:

```json
{ "background": "#1a1c2c", "duration": 2, "madeWith": false, "pixel": true }
```

- `duration` is the minimum time that the splash screen shows, in seconds.
- `madeWith: false` hides "Runs on Cupid".
- `pixel` keeps a pixel-art picture sharp.

You can also show a start page before the download starts. The start page shows a picture, the name and a Play button.
To show the start page, link to `play.html?game=game.love&title=My%20game&by=Me&poster=cover.png` instead of
`player.html`.

Player URL options:

| Option | Effect |
| --- | --- |
| `?game=url` | Load a `.love` from a URL |
| `?autoplay=1` | Skip the Play button (audio can stay muted until the first click) |
| `?scale=fit` (default) / `integer` / `none` | How the player scales a fixed-size window to the page |
| `?pixelated=1` | Nearest-neighbour upscaling (`scale=integer` also sets this option) |
| `?embed=1` | Embedded mode. Use `postMessage` to control it (see `web/player/player.js`). |

## Web-specific behavior

- `love.system.getOS()` returns `"Web"`.
- Save files are in IndexedDB. Cupid writes them when the game saves a file, when the tab becomes hidden, and when the
  game quits.
- `love.timer.sleep` on the main thread does not block. It lets threads run instead.
- Text input is on by default, as on desktop. On touch devices (no hover, coarse pointer), text input starts off, as on
  Android and iOS.
- When text input is on, a hidden text field has the keyboard focus, not the canvas. Thus, IME composition (Chinese,
  Japanese, Korean), emoji pickers, dictation and on-screen keyboards send their text to `love.textinput`. Cupid sends
  the composition in progress to `love.textedited`.
- `love.keypressed` works as before. Keys that are part of an IME composition do not make key presses.
- The hidden text field ignores paste operations, as on desktop. To get the clipboard text, use
  `love.system.getClipboardText`.
- On touch devices, tap the game while text input is on. This shows the on-screen keyboard. Browsers show the
  on-screen keyboard only after a tap. `love.keyboard.setTextInput(true, x, y, w, h)` moves the hidden field over that
  rectangle. IME candidate windows appear at that location.
- `require("https")` (lua-https) works. It uses a synchronous `XMLHttpRequest`, so it blocks as on desktop.
  - Browser rules apply. Other sites must allow cross-origin requests (CORS). An `https` page cannot request `http`
    URLs. The browser sets some request headers (`User-Agent`, `Content-Length`, ...).
  - Network errors return status `0` with an error message as the body (like lua-https on macOS). This includes
    refused CORS requests.
  - Response header names are lower case.
  - Relative URLs resolve against the page. Thus, a game can request files that are next to it on the host.
- `love.window.showFileDialog` uses the file picker of the browser.
  - `openfile` and `openfolder` copy the files or folders that the user picks into the in-memory file system
    (`/picked/<n>/...`). Then they return those paths. To read the files, use `love.filesystem.openNativeFile`,
    `mountFullPath` or `io.open`, as on desktop. Filters become the `accept` list of the picker. Browsers cannot show a
    title or button labels.
  - `savefile` returns `/picked/<n>/<defaultname>`. When the game writes and closes that file, the browser downloads
    it with that name. The browser downloads it again each time that the game saves to it.
  - If the user cancels, the result is an empty list, as on desktop.
  - Browsers open pickers only after a click or a key press. If the game calls `showFileDialog` from `love.keypressed`
    or `love.mousepressed`, the picker opens directly. This works in browsers that report `navigator.userActivation`.
    In other cases, and always for `savefile`, the player shows a small bar. The bar has a "Choose file…" or "Save"
    button and a Cancel button. When the user clicks the button, the picker opens.
- Games with `t.window.resizable = true` fill the page and get `love.resize`.
- `love.window.setFullscreen` resizes the game immediately. The browser goes into real fullscreen on the next click or
  key press. Browsers allow fullscreen only after a user gesture.

## Not supported

- Networking modules: `socket` (LuaSocket) and `enet`. Browsers have no raw sockets. `https` works (see above).
- WOFF2 fonts. The FreeType build does not include Brotli. TTF and OTF fonts work.
- LuaJIT and its `ffi` library. Lua runs on PUC Lua 5.1 with the LuaJIT language extensions and the Cupid JIT (see
  "Lua compatibility").
- Compute shaders. WebGL 2 does not have compute shaders.

## How it works

`web/native/web_main.cpp` replaces the `main()` of LÖVE. On desktop, LÖVE resumes its boot coroutine in a `while`
loop. `boot.lua` yields once for each iteration of the main loop of `love.run`. Thus, on the web, the coroutine
resumes once for each `requestAnimationFrame` instead. The Lua code of LÖVE does not change.

The engine patches (`git diff dca79b4 -- love deps`) are small. Thus, you can rebase them onto newer LÖVE commits. The
patches are:

- A new `LOVE_EMSCRIPTEN` platform (`config.h`). The build does not include LuaSocket and ENet.
- A browser backend for lua-https (`love/src/libraries/luahttps/src/emscripten`). `web/native/library_love.js` makes
  the requests.
- WebGL 2 differences: no texture swizzle (LA8 textures use `LUMINANCE_ALPHA`), no `GL_FRAMEBUFFER_SRGB` toggle.
- `love.timer` uses `emscripten_get_now`. Sleep does not block.
- A Lua-to-WebAssembly JIT (see above) in `deps/lua-5.1.5/ljit.inc`. It has small hooks in `lvm.c`, `ldo.c` and
  `lfunc.c`.
- Green threads for `love.thread` (see above).
- Audio streaming refills from the frame loop and a timer, not from a pool thread. Audio streaming also recovers from
  underruns.
- `love.video` also decodes Theora from the frame loop, not from its worker thread. The build compiles the libtheora
  decoder from source (`deps/theora`), without its x86/ARM assembly. libogg is the Emscripten port.
- If the game seeks a paused or stopped streaming Source, the Source no longer skips ahead when it plays again. This
  problem caused the audio in a video to jump ahead.
- `boot.lua` skips the modules that the build does not include.
- LuaBitOp (`deps/luabitop`) loads as the global `bit`, as on LuaJIT.
- SDL3 text input starts when the window opens (not on touch devices). `web/native/web_main.cpp` sends the text from
  the hidden text field of the page as SDL text events.
- `love.window.showFileDialog` sends the dialog to `library_love.js`, not to SDL
  (`love/src/modules/window/sdl/Window.cpp`).

## Licenses

LÖVE uses the zlib license (`love/license.txt`). The libraries that LÖVE bundles keep their own licenses. Lua 5.1 uses
the MIT license (`deps/lua-5.1.5/COPYRIGHT`). LuaBitOp also uses the MIT license (`deps/luabitop/bit.c`). libtheora
uses a BSD-style license (`deps/theora/COPYING`). The source marks the changes in this repository, and the changes
follow the same licenses.
