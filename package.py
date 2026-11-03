#!/usr/bin/env python3
"""
Package a LÖVE game for the web.

    python3 package.py path/to/game-folder out/        # or a .love file
    python3 package.py game.love out/ --zip            # also write out.zip (for itch.io)

The output folder contains index.html (the player), the engine and
game.love, and can be uploaded to any static host. Requires a built dist/
(python3 build.py).
"""
import argparse
import json
import os
import re
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DIST = ROOT / "dist"
PLAYER_FILES = ["player.js", "player.css", "love.mjs", "love.wasm"]
COMPAT_RULES = ROOT / "web" / "editor" / "compat-rules.json"


def strip_lua(text: str):
    """Lua text with comments blanked ('source'), and with comments and string
    contents blanked ('code'); newlines are kept. Mirrors compat.js."""
    def blank(s): return re.sub(r"[^\n]", " ", s)
    def long_end(level, start):
        end = text.find("]" + "=" * level + "]", start)
        return len(text) if end < 0 else end + level + 2
    special = re.compile(r"--|[\"'\[]")
    long_open = re.compile(r"\[(=*)\[")
    source, code, i = [], [], 0
    while True:
        m = special.search(text, i)
        if not m:
            break
        source.append(text[i:m.start()])
        code.append(text[i:m.start()])
        tok = m.group()
        if tok == "--":
            lm = long_open.match(text, m.start() + 2)
            if lm:
                j = long_end(len(lm.group(1)), lm.end())
            else:
                j = text.find("\n", m.start())
                j = len(text) if j < 0 else j
            source.append(blank(text[m.start():j]))
            code.append(blank(text[m.start():j]))
        elif tok == "[":
            lm = long_open.match(text, m.start())
            if not lm:
                source.append("[")
                code.append("[")
                i = m.start() + 1
                continue
            j = long_end(len(lm.group(1)), lm.end())
            source.append(text[m.start():j])
            code.append(blank(text[m.start():j]))
        else:  # quoted string
            j = m.start() + 1
            while j < len(text) and text[j] != tok and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, len(text))
            source.append(text[m.start():j])
            code.append(blank(text[m.start():j]))
        i = j
    source.append(text[i:])
    code.append(text[i:])
    return {"source": "".join(source), "code": "".join(code)}


def check_compat(love_file: Path):
    """Prints web-compatibility issues in the game's Lua files; returns the
    number of errors."""
    rules = json.loads(COMPAT_RULES.read_text())["rules"]
    seen, issues = set(), []
    with zipfile.ZipFile(love_file) as z:
        for name in sorted(n for n in z.namelist() if n.endswith(".lua")):
            stripped = strip_lua(z.read(name).decode("utf-8", "replace"))
            for rule in rules:
                if rule.get("once") and rule["id"] in seen:
                    continue
                if rule.get("unlessFile") and re.search(rule["unlessFile"], stripped["code"]):
                    continue
                text = stripped.get(rule["scope"], stripped["code"])
                for m in re.finditer(rule["pattern"], text):
                    if stripped["code"][m.start()] != text[m.start()]:
                        continue  # starts inside a string
                    start = text.rfind("\n", 0, m.start()) + 1
                    end = text.find("\n", m.start())
                    line_text = stripped["source"][start:end if end >= 0 else None]
                    if rule.get("unless") and re.search(rule["unless"], line_text):
                        continue
                    line = text.count("\n", 0, m.start()) + 1
                    msg = rule["message"].replace("{1}", m.group(1) if m.groups() and m.group(1) else "")
                    issues.append((name, line, rule["severity"], msg))
                    seen.add(rule["id"])
                    if rule.get("once"):
                        break
    for name, line, severity, msg in sorted(issues):
        print(f"{severity}: {name}:{line}: {msg}", file=sys.stderr)
    return sum(1 for i in issues if i[2] == "error")


def make_love(folder: Path, dest: Path):
    if not (folder / "main.lua").exists():
        sys.exit(f"{folder} has no main.lua")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(folder.rglob("*")):
            rel = path.relative_to(folder)
            if path.is_file() and not any(p.startswith(".") for p in rel.parts):
                z.write(path, rel.as_posix())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", type=Path, help="game folder (with main.lua) or .love file")
    ap.add_argument("out", type=Path, help="output folder")
    ap.add_argument("--title", help="page title (default: game name)")
    ap.add_argument("--scale", choices=["fit", "integer", "none"], default="fit",
                    help="how fixed-size windows are scaled to the page")
    ap.add_argument("--zip", action="store_true", help="also write <out>.zip")
    args = ap.parse_args()

    missing = [f for f in PLAYER_FILES + ["player.html"] if not (DIST / f).exists()]
    if missing:
        sys.exit(f"dist/ is missing {', '.join(missing)}: run python3 build.py first")

    args.out.mkdir(parents=True, exist_ok=True)
    if args.game.is_dir():
        make_love(args.game, args.out / "game.love")
    elif args.game.suffix in (".love", ".zip"):
        shutil.copyfile(args.game, args.out / "game.love")
    else:
        sys.exit(f"{args.game} is neither a folder nor a .love file")

    errors = check_compat(args.out / "game.love")
    if errors:
        print(f"{errors} part(s) of this game won't work on the web (see above).", file=sys.stderr)

    for f in PLAYER_FILES:
        shutil.copyfile(DIST / f, args.out / f)

    title = args.title or args.game.stem
    html = (DIST / "player.html").read_text()
    meta = (f'<meta name="love-game" content="game.love">\n'
            f'<meta name="love-scale" content="{args.scale}">\n')
    html = html.replace("<head>\n", "<head>\n" + meta, 1)
    html = html.replace("<title>Cupid Player</title>", f"<title>{title}</title>")
    (args.out / "index.html").write_text(html)

    size = sum(f.stat().st_size for f in args.out.iterdir() if f.is_file())
    print(f"Wrote {args.out}/ ({size / 1e6:.1f} MB)")

    if args.zip:
        archive = shutil.make_archive(str(args.out), "zip", args.out)
        print(f"Wrote {os.path.relpath(archive)}")


if __name__ == "__main__":
    main()
