#!/bin/sh
# Builds the standalone Lua 5.1 interpreter (with the JIT) for Node.js, to
# test the JIT against the interpreter: see tools/jit/run-tests.mjs.
set -e
cd "$(dirname "$0")/../.."
# OUT and CFLAGS make a second build to compare with (e.g.
# OUT=build/jit-nopool CFLAGS=-DLUAI_POOL=0), run with LUA_BUILD=build/jit-nopool/lua.mjs.
OUT=${OUT:-build/jit-node}
mkdir -p "$OUT"
SRC=$(ls deps/lua-5.1.5/*.c | grep -v -e luac.c -e print.c)
emcc $SRC deps/luabitop/bit.c \
  -O3 -flto $CFLAGS -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -Ideps/lua-5.1.5 \
  -sENVIRONMENT=node -sNODERAWFS=1 -sALLOW_TABLE_GROWTH=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2MB \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 \
  -sEXPORTED_FUNCTIONS=_main,_ljit_configure,_ljit_stats -sEXPORTED_RUNTIME_METHODS=callMain \
  --js-library web/native/library_ljit.js \
  -o "$OUT/lua.mjs"
echo "built $OUT/lua.mjs"
