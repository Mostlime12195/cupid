#!/usr/bin/env python3
"""
Prints the download sizes of a build as a Markdown table.

    python3 tools/sizes.py dist
    python3 tools/sizes.py build/profiling --functions   # also code size by library

--functions needs a build that kept function names (build.py --profiling).
"""
import gzip
import re
import sys
import zlib
from collections import Counter
from pathlib import Path

try:
    import brotli
except ImportError:
    brotli = None

FILES = ["love.wasm", "love.mjs", "player.js", "app.js"]

# Patterns (matched at the start of the demangled function name) for each
# part of the engine; the first match wins.
GROUPS = [
    ("glslang", r"glslang::|spv::|yy|TPpContext|TParseContext|\(anonymous namespace\)::T|ShInitialize|ShFinalize|ShCompile"),
    ("love: graphics", r"love::graphics::|love::font::|love::window::"),
    ("love: image formats", r"love::image::|tinyexr::|stbi_|SaveEXR|LoadEXR|ParseEXR|WebP|VP8|ddsparse|dds::|miniz|mz_|tinfl_|tdefl_"),
    ("love: other", r"love::|luaopen_love|w_|loveweb|luax_|lua_https|EmscriptenClient|HTTPSClient|w__|utf8::|luaopen_luautf8|luaopen_https|Lutf8|utf8_|iter_aux|byteoffset|codepoint|utfchar|u_posrelat|utflen"),
    ("box2d", r"b2"),
    ("harfbuzz", r"hb_|_hb_|OT::|AAT::|CFF::|hb::|graph::|setup_syllables|_get_bounds|initial_reordering|final_reordering|reorder_|decompose_|compose_|collect_features|override_features|setup_masks|preprocess_text|postprocess_glyphs|record_|data_create|data_destroy|lookup_|arabic_|khmer|indic|myanmar|hangul|thai|use_|find_syllables|insert_dotted|_cmp|compare_|void hb|bool hb|hb|decompose|compose|output_|apply_|position_|zero_|fix_|propagate_|adjust_|buffer_verify|operator\(\)"),
    ("freetype", r"(FT_|ft_|TT_|tt_|cff_|af_|ps_|psh|cf2_|t1_|T1_|sfnt_|pcf_|bdf_|pfr_|cid_|PS_|BDF_|gray_|Load_|Ins_|T42_|t42_|woff|FNT_|fnt_|ftc_|FTC_|ps[a-z]+_|Compute_|Direct_Move|Project|Dual_Project|Round_|Read_CVT|Write_CVT|Move_CVT|Cur_PPEM|Interp|Normalize|IUP_|Free_Project|_bdf|pfr|Horizontal_|Vertical_|Render_|Draw_|Insert_|Sort|Bezier_|Line_|Conic_|Cubic_|Split_|Decompose|New_Profile|End_Profile|Finalize_Profile|Convert_Glyph|destroy_|parse_|compute_|reverse_|oid|t2_|cmap_|Get_|Set_|Find_|Init_|Done_|Update_|Move_Zp2|Compute_Funcs|Compute_Round|SetSuperRound|Current_|Check_|Modify_|skip_|scale_|open_face|woff2|reconstruct|Store|WriteTable|compute_bbox|read_|write_|store_|brotli|Brotli|Decode|Inverse|Safe|ProcessCommands|CopyUncompressed|ReadHuffman|ReadSymbol|ReadContext|ReadSimple|Huffman)"),
    ("sdl", r"SDL|Emscripten_|EMSCRIPTEN_|GLES2_|GLES_|Blit|RLE|Color16|Color32|BlitNto|Blit1to|BlitBto|BlitRGB|Blit4to|Blit_|SW_|Map1|MapNto|Convert_|YUV|yuv|STD_|RGB|ABGR|BGRA|ARGB|RGBA|BGR|XRGB|XBGR|Wait|audio_|mix_|U8_|S16|S32|F32|Mix|Cvt"),
    ("lua", r"(lua|ljit_|lean_|str_|tbl_|math_|os_|io_|db_|bit_|co_|g_|f_|luaB_|auxresume|resume|unroll|base_|ipairs|pairs|tconcat|sort|auxsort|ll_|loader_|gmatch|gfind|do_match|match|max_expand|push_|add_|addquoted|scanformat|read_|test_eof|pmain|traceback|subexpr|simpleexp|body|funcargs|statement|chunk|block|expr|primaryexp|constructor|llex|next|singlevar|close_func|open_func|forbody|exp1|cond|retstat|check_|enterlevel|adjust_assign|new_localvar|codestring|recfield|listfield|lastlistfield|repeatstat|whilestat|test_then_block|localfunc|localstat|exprstat|restassign|fieldsel|yindex|parlist|pushclosure|mark|propagate|sweep|singlestep|GCTM|reallymark|traverse|atomic|freeobj|callTM|call_|Arith|lessequal|l_str|get_compTM|numusearray|setnodevector|rehash|resize|computesizes|countint|getfreepos|hashnum|mainposition|newkey|arrayindex|findindex|unbound_search|index2adr|aux_|panic|l_alloc|pool_|getfunc|setn|getn|tinsert|tremove|unpack|select|foreach|maxn|instruction|compile_body|analyze|get_table|set_table|lookup|arith|compare|jump|guard|deopt|math_inline|math_call)"),
    ("theora/ogg/vorbis", r"(oc_|th_|theora_|ogg|vorbis|ov_|_ve_|_vp_|floor[01]_|res[012]|mapping0_|mdct_|drft_|_book_|_vorbis|oggpack|_ogg|dradf|dradb|drfti|_vds|_vd_|bark_noise|seed_|max_seeds|noise_normalize|_os_|_packetout|_fetch|_make_|_seek|_open|_get_|_initial|_bisect|_process|_decode|_ov_|render_line|_float32|inverse|accumulate_fit|fit_line|post_Y|icount|ilog|decode_|encode_)"),
    ("modplug", r"CSoundFile|ModPlug|CzWINDOWEDFIR|CzCUBICSPLINE|MMCMP|ITUnpack|PP20|(Fast)?(Filter)?(Mono|Stereo)|X86_|AMS|MDL|DMF|Ramp(Mono|Stereo)?Mix|abc_|ABC_|pat_|mmf|mid|MIDI|Midi|PAT|ABC"),
    ("physfs", r"PHYSFS|__PHYSFS|(ZIP_|zip_|DIR_|UNPK_|ISO9660|SZIP|sz_|Lzma|Bcj|x86_Convert|Xz|Sha256|Crc|SzAr|Ppmd|Bra|Delta|LookToRead|SecToRead|MtDec|doOpen|findBinaryPrefix|Sz|ReadNumber|ReadID|ReadByte|SkipData|WaitId|Buf_|GRP_|HOG_|MVL_|QPAK_|SLB_|VDF_|WAD_|qpak|hog|mvl|grp|wad|slb|vdf|iso)"),
    ("lodepng/zlib/lz4", r"lodepng|LodePNG|[Hh]uffman|inflate|deflate|adler32|crc32|zlib|LZ4|lz4|zc|_tr_|uncompress|compress|ucvector|uivector|readChunk|addChunk|color_tree|[uU]nfilter|Adam7|paeth|gz|build_tree|send_tree|scan_tree|fill_window|longest_match|pqdownheap|get(TreeInflate|PixelColor|ValueRequired)|rgba(8|16)To|postProcessScanlines|decodeGeneric|filter|encodeLZ77|searchCodeIndex|addPadding|removePadding|writeSignature"),
    ("audio decoders", r"wuff|drmp3|drflac|dr_|stb_|ma_"),
    ("libc++/libc", r"std::|operator new|operator delete|__cxa|__cxx|__dynamic_cast|__cxxabiv1|\(anonymous namespace\)::itanium_demangle|.*printf|.*scanf|__|_emscripten|emscripten_|em_|fmt_|pop_arg|wcs|mbs|isw|tow|qsort|sift|trinkle|cycle|dl|malloc|free|calloc|realloc|sbrk|exp|log|pow|sin|cos|tan|atan|asin|acos|scalbn|frexp|fmod|ceil|floor|round|trunc|sqrt|cbrt|hypot|ldexp|modf|nan|rint|lrint|str|mem|pthread|getenv|setenv|time|clock|locale|iconv|decfloat|hexfloat|scanexp|shgetc|shlim|vf|vs|sn|fputs|fwrite|fread|fopen|fclose|fflush|fseek|ftell|fget|fput|puts|tre_|regcomp|regexec|regfree|fnmatch|glob|bsearch|rand|srand|abort|exit|_Exit|atexit|void std|bool std|int std|char|wchar|unsigned|long|double|float"),
]


def leb(b, p):
    r = s = 0
    while True:
        x = b[p]
        p += 1
        r |= (x & 127) << s
        s += 7
        if not x & 128:
            return r, p


def sections(b):
    p = 8
    while p < len(b):
        sid = b[p]
        n, p = leb(b, p + 1)
        yield sid, p, n
        p += n


def function_sizes(b):
    """{function name: bytes of code}, from the code and name sections."""
    imports = 0
    bodies = []
    names = {}
    for sid, p, n in sections(b):
        if sid == 2:
            count, q = leb(b, p)
            for _ in range(count):
                for _ in range(2):
                    ln, q = leb(b, q)
                    q += ln
                kind = b[q]
                q += 1
                if kind == 0:
                    imports += 1
                    _, q = leb(b, q)
                elif kind == 1:
                    q += 1
                    flags, q = leb(b, q)
                    _, q = leb(b, q)
                    if flags & 1:
                        _, q = leb(b, q)
                elif kind == 2:
                    flags, q = leb(b, q)
                    _, q = leb(b, q)
                    if flags & 1:
                        _, q = leb(b, q)
                elif kind == 3:
                    q += 2
                elif kind == 4:
                    q += 1
                    _, q = leb(b, q)
        elif sid == 10:
            count, q = leb(b, p)
            for _ in range(count):
                size, q = leb(b, q)
                bodies.append(size)
                q += size
        elif sid == 0:
            ln, q = leb(b, p)
            if b[q:q + ln] == b"name":
                q += ln
                end = p + n
                while q < end:
                    sub = b[q]
                    size, q = leb(b, q + 1)
                    if sub == 1:
                        r = q
                        count, r = leb(b, r)
                        for _ in range(count):
                            idx, r = leb(b, r)
                            ln, r = leb(b, r)
                            names[idx] = b[r:r + ln].decode("utf-8", "replace")
                            r += ln
                    q += size
    return {names.get(i + imports, f"func{i + imports}"): size for i, size in enumerate(bodies)}


def group_of(name):
    # "void foo<...>(...)" and "bool ns::f()": skip a leading return type.
    name = re.sub(r"^(?:(?:void|bool|int|float|double|char|unsigned|long|auto|decltype\(auto\))[ *&]+)+(?=[A-Za-z_(])", "", name)
    for group, pattern in GROUPS:
        if re.match(pattern, name):
            return group
    return "other"


def main():
    root = Path(sys.argv[1])
    print(f"### Sizes of `{root}`\n")
    print("| File | Raw | gzip -9 | brotli 11 |")
    print("| --- | ---: | ---: | ---: |")
    for name in FILES:
        path = root / name
        if not path.exists():
            continue
        data = path.read_bytes()
        br = f"{len(brotli.compress(data, quality=11, lgwin=24)):,}" if brotli else "n/a"
        print(f"| `{name}` | {len(data):,} | {len(gzip.compress(data, 9)):,} | {br} |")

    wasm = (root / "love.wasm").read_bytes()
    parts = {sid: n for sid, _, n in sections(wasm) if sid in (10, 11)}
    print(f"\nlove.wasm: {parts.get(10, 0):,} bytes of code, {parts.get(11, 0):,} bytes of data.")

    if "--functions" in sys.argv:
        sizes = function_sizes(wasm)
        groups = Counter()
        for name, size in sizes.items():
            groups[group_of(name)] += size
        print("\n| Code | Bytes |\n| --- | ---: |")
        for group, size in groups.most_common():
            print(f"| {group} | {size:,} |")
        print("\nLargest functions:\n\n```")
        for name, size in sorted(sizes.items(), key=lambda kv: -kv[1])[:40]:
            print(f"{size:8,}  {name[:110]}")
        print("```\n\nLargest unclassified functions:\n\n```")
        other = [(n, s) for n, s in sizes.items() if group_of(n) == "other"]
        for name, size in sorted(other, key=lambda kv: -kv[1])[:40]:
            print(f"{size:8,}  {name[:110]}")
        print("```")


if __name__ == "__main__":
    main()
