#!/usr/bin/env python3
"""
Build LÖVE 12 for the web with Emscripten.

Source lists come straight from love/CMakeLists.txt so upstream updates are
picked up automatically. Objects are cached in build/obj and rebuilt only when
a source or one of its headers changes.

    python3 build.py            # release build -> dist/
    python3 build.py --debug    # assertions, no optimisation, source maps
    python3 build.py -j 8
"""
import argparse
import concurrent.futures as cf
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
LOVE = ROOT / "love"
LUA = ROOT / "deps" / "lua-5.1.5"
THEORA = ROOT / "deps" / "theora"
WEB = ROOT / "web"
BUILD = ROOT / "build"
DIST = ROOT / "dist"

# CMake targets from love/CMakeLists.txt that make up the web build.
LOVE_TARGETS = [
    "love_common",
    "love_audio_root", "love_audio_null", "love_audio_openal",
    "love_data",
    "love_event_root", "love_event_sdl",
    "love_filesystem_root", "love_filesystem_physfs",
    "love_font_root", "love_font_freetype",
    "love_graphics_root", "love_graphics_opengl",
    "love_image_root", "love_image_magpie",
    "love_joystick_root", "love_joystick_sdl",
    "love_keyboard_root", "love_keyboard_sdl",
    "love_math",
    "love_mouse_root", "love_mouse_sdl",
    "love_physics_root", "love_physics_box2d",
    "love_sensor_root", "love_sensor_sdl",
    "love_sound_root", "love_sound_lullaby",
    "love_system_root", "love_system_sdl",
    "love_thread_root", "love_thread_sdl",
    "love_timer",
    "love_touch_root", "love_touch_sdl",
    "love_video_root", "love_video_theora",
    "love_window_root", "love_window_sdl",
    "liblove",
    "love_3p_box2d", "love_3p_ddsparse", "love_3p_glad", "love_3p_glslang",
    "love_3p_lodepng", "love_3p_lua53", "love_3p_lz4", "love_3p_noise1234",
    "love_3p_physfs", "love_3p_wuff", "love_3p_xxhash",
]

# Files listed by CMake that must not be built for the web.
EXCLUDE = {
    "src/libraries/glslang/glslang/OSDependent/Windows/ossource.cpp",
}

# Third-party code is optimised for size; engine code for speed.
SIZE_OPT_PREFIXES = ("src/libraries/glslang/",)

PORTS = ["-sUSE_SDL=3", "-sUSE_FREETYPE=1", "-sUSE_HARFBUZZ=1",
         "-sUSE_OGG=1", "-sUSE_VORBIS=1", "-sUSE_ZLIB=1", "-sUSE_MODPLUG=1"]

COMMON_FLAGS = [
    "-fwasm-exceptions", "-sSUPPORT_LONGJMP=wasm",
    "-msimd128",
    "-DLOVE_WEB=1",
    "-I" + str(LOVE / "src"),
    "-I" + str(LOVE / "src" / "modules"),
    "-I" + str(LOVE / "src" / "libraries"),
    "-I" + str(LOVE / "src" / "libraries" / "physfs"),
    "-I" + str(LOVE / "src" / "libraries" / "box2d" / "include"),
    "-I" + str(LOVE / "src" / "libraries" / "glslang"),
    "-I" + str(LUA),
    "-I" + str(THEORA / "include"),
    "-w",
] + PORTS


def emscripten_includes():
    """Emscripten's OpenAL headers live in sysroot/include/AL; LÖVE includes them bare."""
    cache = subprocess.run(["em-config", "CACHE"], capture_output=True, text=True).stdout.strip()
    return ["-I" + os.path.join(cache, "sysroot", "include", "AL")]


def cmake_sources():
    """Collect .c/.cpp files for LOVE_TARGETS from add_library/target_sources."""
    text = (LOVE / "CMakeLists.txt").read_text()
    wanted = set(LOVE_TARGETS)
    files = []
    block_re = re.compile(r"(add_library|target_sources)\(\s*([A-Za-z0-9_]+)(.*?)\)", re.S)
    for m in block_re.finditer(text):
        target = m.group(2)
        if target not in wanted:
            continue
        # Skip conditionally-added platform sources (Windows/Apple .mm etc).
        for tok in m.group(3).split():
            if tok.startswith("src/") and tok.endswith((".c", ".cpp")) and tok not in EXCLUDE:
                files.append(tok)
    seen, out = set(), []
    for f in files:
        if f not in seen and (LOVE / f).exists():
            seen.add(f)
            out.append(LOVE / f)
    return out


# Individual files needed from targets that are otherwise left out.
EXTRA_SOURCES = [
    "src/modules/thread/web/GreenThreads.cpp",
]


def lua_sources():
    skip = {"lua.c", "luac.c", "print.c"}
    lua = sorted(p for p in LUA.glob("*.c") if p.name not in skip)
    return lua + [ROOT / "deps" / "luabitop" / "bit.c"]


def theora_sources():
    """libtheora's decoder (lib/Makefile.am decoder_sources, minus the legacy API wrappers)."""
    return sorted((THEORA / "lib").glob("*.c"))


def web_sources():
    return sorted((WEB / "native").glob("*.cpp")) + sorted((WEB / "native").glob("*.c"))


def obj_path(src: Path) -> Path:
    rel = src.relative_to(ROOT)
    return BUILD / "obj" / rel.with_suffix(rel.suffix + ".o")


def needs_rebuild(src: Path, obj: Path) -> bool:
    if not obj.exists():
        return True
    t = obj.stat().st_mtime
    dep = obj.with_suffix(".d")
    if not dep.exists():
        return True
    # Make-style depfile: escaped newlines, and spaces in paths escaped as "\ ".
    text = dep.read_text().replace("\\\n", " ").split(": ", 1)[-1]
    deps = [d.replace("\\ ", " ") for d in re.split(r"(?<!\\)\s+", text) if d]
    for d in deps:
        try:
            if os.stat(d).st_mtime > t:
                return True
        except FileNotFoundError:
            return True
    return False


def compile_one(src: Path, opt: list):
    obj = obj_path(src)
    obj.parent.mkdir(parents=True, exist_ok=True)
    is_cpp = src.suffix == ".cpp"
    cc = "em++" if is_cpp else "emcc"
    flags = list(COMMON_FLAGS) + list(opt)
    rel = str(src.relative_to(LOVE)) if LOVE in src.parents else ""
    if rel.startswith(SIZE_OPT_PREFIXES) and "-O0" not in opt:
        flags = [f for f in flags if not f.startswith("-O")] + ["-Oz"]
    if is_cpp:
        flags.append("-std=c++20")

    cmd = [cc, *flags, "-MMD", "-MF", str(obj.with_suffix(".d")), "-c", str(src), "-o", str(obj)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return src, r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--only-compile", action="store_true")
    ap.add_argument("--web-only", action="store_true", help="only copy player/editor files")
    ap.add_argument("--profiling", action="store_true",
                    help="keep function names (for size analysis); output to build/profiling")
    args = ap.parse_args()

    if args.web_only:
        copy_web()
        return
    if not shutil.which("emcc"):
        sys.exit("emcc not found: source emsdk_env.sh first")

    COMMON_FLAGS.extend(emscripten_includes())
    opt = ["-O0", "-g", "-sASSERTIONS=1"] if args.debug else ["-O3", "-flto"]
    if args.debug:
        BUILD.joinpath("debug").mkdir(parents=True, exist_ok=True)

    sources = cmake_sources() + [LOVE / f for f in EXTRA_SOURCES] + lua_sources() + theora_sources() + web_sources()
    todo = [s for s in sources if needs_rebuild(s, obj_path(s))]
    print(f"{len(sources)} sources, {len(todo)} to compile")

    failed = []
    with cf.ThreadPoolExecutor(max_workers=args.j) as ex:
        for i, (src, rc, out) in enumerate(ex.map(lambda s: compile_one(s, opt), todo), 1):
            name = src.relative_to(ROOT)
            if rc != 0:
                failed.append(name)
                print(f"[{i}/{len(todo)}] FAILED {name}\n{out}", flush=True)
            elif out.strip():
                print(f"[{i}/{len(todo)}] {name}\n{out}", flush=True)
    if failed:
        sys.exit(f"{len(failed)} file(s) failed to compile")
    if args.only_compile:
        return

    out_dir = BUILD / "profiling" if args.profiling else DIST
    out_dir.mkdir(parents=True, exist_ok=True)
    objs = [str(obj_path(s)) for s in sources]
    link = [
        "em++", *objs, *COMMON_FLAGS, *opt,
        "-lopenal", "-lidbfs.js",
        "--js-library", str(WEB / "native" / "library_love.js"),
        "--js-library", str(WEB / "native" / "library_ljit.js"),
        "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=createLove",
        "-sENVIRONMENT=web",
        "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=64MB", "-sSTACK_SIZE=2MB",
        "-sALLOW_TABLE_GROWTH=1",  # Lua JIT adds compiled functions
        "-sMAX_WEBGL_VERSION=2", "-sMIN_WEBGL_VERSION=2", "-sFULL_ES3=1",
        "-sGL_ENABLE_GET_PROC_ADDRESS=1",
        "-sFORCE_FILESYSTEM=1",
        "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,callMain,ccall,cwrap,HEAPU8",
        "-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_loveweb_quit,_loveweb_set_paused,_loveweb_fit_window,_ljit_configure,_ljit_stats",
        "-sINVOKE_RUN=0", "-sEXIT_RUNTIME=0",
        "-o", str(out_dir / "love.mjs"),
    ] + (["--profiling-funcs"] if args.profiling else [])
    print("linking...")
    r = subprocess.run(link)
    if r.returncode != 0:
        sys.exit("link failed")
    for f in ("love.mjs", "love.wasm"):
        p = out_dir / f
        print(f"  {f}: {p.stat().st_size / 1e6:.2f} MB")
    if not args.profiling:
        copy_web()


def build_id():
    h = hashlib.sha1()
    for f in ("love.mjs", "love.wasm"):
        p = DIST / f
        if p.exists():
            h.update(p.read_bytes())
    return h.hexdigest()[:12]


def copy_web():
    """Copy the player, editor and examples next to the engine."""
    for sub in ("player", "editor", "dev"):
        src = WEB / sub
        if src.exists():
            shutil.copytree(src, DIST, dirs_exist_ok=True)
    player = DIST / "player.js"
    if player.exists():
        player.write_text(player.read_text().replace("__LOVE_BUILD__", build_id()))
    if (WEB / "examples").exists():
        shutil.copytree(WEB / "examples", DIST / "examples", dirs_exist_ok=True)


if __name__ == "__main__":
    main()
