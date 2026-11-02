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
| `web/native/` | Web entry point (`web_main.cpp`) replacing LÖVE's blocking `main()` |
| `web/player/` | The player page (standalone and embeddable) |
| `web/editor/` | The editor (LÖVE Web Studio) |
| `web/examples/` | Example projects used by the editor |
| `build.py` | Build script (reads source lists from `love/CMakeLists.txt`) |
| `serve.py` | Local dev server |

## Compared to love.js

[love.js](https://github.com/Davidobot/love.js) is the established way to run
LÖVE on the web. Its latest release (11.4.1, 2022) ships LÖVE 11.4.

| | love.js 11.4.1 | This project |
| --- | --- | --- |
| LÖVE version | 11.4 | 12.0 (current development branch) |
| `love.thread` | Compatibility build: `Thread:start()` returns `false`. Threaded build: needs `SharedArrayBuffer`, i.e. COOP/COEP headers | Works on any host, no special headers (see below) |
| Audio | Compatibility build: "dodgy audio" (its readme) | Streaming refilled from the frame loop and a timer; keeps playing in background tabs |
| `bit` library (LuaJIT built-in) | Missing | Included (LuaBitOp), as a global and as `require "bit"` |
| Memory | Grows, but the initial size must cover the game's assets (`-m`) | Grows as needed; nothing to configure |
| Packaging | Node CLI builds a page and data package per game | Any `.love` file runs as is: by URL, drag and drop, or from the editor |
| Editor | None | Built-in editor with run, console and export |

The love.js entries come from its 11.4.1 npm package and readme. The threads
and `bit` rows were checked by running the same test game in both.

### Performance

Measured with the same `.love` (a benchmark that runs unchanged on 11.4 and
12) in the same Chrome instance, run back to back. Lower is better.

<!-- BENCHMARK-TABLE -->

Lua performance is at parity or better (both run PUC Lua 5.1; LuaJIT's JIT
isn't possible in WebAssembly). One fix was needed to get there: with
link-time optimization, Lua's interpreter loop was being inlined into the
`setjmp`-protected function behind `pcall`/`coroutine.resume`, which made
every call pay for WebAssembly exception handling. `luaV_execute` is now kept
out of line (`deps/lua-5.1.5/lvm.c`).

Download size is larger, because LÖVE 12 itself is: it adds the glslang
shader compiler, HarfBuzz text shaping and a Noto Sans default font.

| | love.js 11.4.1 | This project |
| --- | --- | --- |
| Engine (`.wasm`), gzip | 1.63 MB | 2.49 MB |
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

## Publishing a game

Copy `player.html`, `player.js`, `player.css`, `love.mjs` and `love.wasm` next
to your `game.love`, and add this to `player.html`'s `<head>`:

```html
<meta name="love-game" content="game.love">
```

The editor's **Export for web** does this for you and produces a zip you can
upload to itch.io as an HTML game. Serving `.wasm` as `application/wasm`
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
- Text input is enabled by default, as on desktop.

## Not supported

- `love.video` (no Theora port for Emscripten yet).
- Networking modules: `socket` (LuaSocket), `enet`, `https`.
- `love.window.showFileDialog`.
- LuaJIT-only features: the JIT itself and `ffi` (`bit` is provided).
- Compute shaders (WebGL 2 has none).

## How it works

`web/native/web_main.cpp` replaces LÖVE's `main()`. On desktop LÖVE resumes
its boot coroutine in a `while` loop. `boot.lua` yields once per iteration of
`love.run`'s main loop, so on the web that coroutine is resumed once per
`requestAnimationFrame` instead. Nothing in LÖVE's own Lua code changes.

The engine patches (`git diff dca79b4 -- love deps`) are kept small so they
can be rebased onto newer LÖVE commits:

- Add an `LOVE_EMSCRIPTEN` platform (`config.h`) and leave out video and
  networking.
- WebGL 2 differences: no texture swizzle (LA8 textures use
  `LUMINANCE_ALPHA`), no `GL_FRAMEBUFFER_SRGB` toggle.
- `love.timer` uses `emscripten_get_now`; sleeping doesn't block.
- Green threads for `love.thread` (above); audio streaming refills from the
  frame loop and a timer instead of a pool thread, and recovers from underruns.
- `boot.lua` skips modules the build doesn't include.
- LuaBitOp (`deps/luabitop`) is loaded as the global `bit`, as on LuaJIT.
- SDL3 text input is started when the window is created.

## Licenses

LÖVE is under the zlib license (`love/license.txt`); its bundled libraries
keep their own licenses. Lua 5.1 is MIT (`deps/lua-5.1.5/COPYRIGHT`), as is
LuaBitOp (`deps/luabitop/bit.c`).
Changes in this repository are marked in the source and follow the same
licenses.
