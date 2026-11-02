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
import os
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DIST = ROOT / "dist"
PLAYER_FILES = ["player.js", "player.css", "love.mjs", "love.wasm"]


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

    for f in PLAYER_FILES:
        shutil.copyfile(DIST / f, args.out / f)

    title = args.title or args.game.stem
    html = (DIST / "player.html").read_text()
    meta = (f'<meta name="love-game" content="game.love">\n'
            f'<meta name="love-scale" content="{args.scale}">\n')
    html = html.replace("<head>\n", "<head>\n" + meta, 1)
    html = html.replace("<title>Smitten Player</title>", f"<title>{title}</title>")
    (args.out / "index.html").write_text(html)

    size = sum(f.stat().st_size for f in args.out.iterdir() if f.is_file())
    print(f"Wrote {args.out}/ ({size / 1e6:.1f} MB)")

    if args.zip:
        archive = shutil.make_archive(str(args.out), "zip", args.out)
        print(f"Wrote {os.path.relpath(archive)}")


if __name__ == "__main__":
    main()
