/*
** LÖVE web: compiles hot Lua functions to WebAssembly at run time.
** See ljit.inc.
*/

#ifndef ljit_h
#define ljit_h

#include "lobject.h"

/* Results of a compiled function, telling luaV_execute what to do next. */
#define LJIT_CALL      1  /* a Lua call was set up: nexeccalls++, reenter */
#define LJIT_TAILCALL  2  /* a Lua tail call replaced the frame: reenter */
#define LJIT_RET0      3  /* returned (luaD_poscall gave 0) */
#define LJIT_RET1      4  /* returned (luaD_poscall gave 1) */
#define LJIT_YIELD     5  /* a C function yielded */
#define LJIT_INTERP    6  /* a hook was set: interpret from L->savedpc */

/* Calls or loop iterations before a function is compiled. */
extern int ljit_threshold;

#ifdef __EMSCRIPTEN__
#define LJIT_ENABLED 1
#else
#define LJIT_ENABLED 0
#endif

extern int ljit_enabled;

/* Compiled functions call compiled Lua functions directly, up to this many
** nested frames on the wasm stack (deeper calls go through luaV_execute). */
#define LJIT_MAX_DEPTH 64
extern int ljit_depth;

/* Frames entered directly by compiled code that luaV_execute must take over
** (add to nexeccalls) when a compiled function returns to it. */
extern int ljit_pending;

typedef int (*ljit_Function) (lua_State *L);

/* Compiles p; returns 1 on success (p->jit set). */
int ljit_compile (lua_State *L, Proto *p);
void ljit_free (Proto *p);

#endif
