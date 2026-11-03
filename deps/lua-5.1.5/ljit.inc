/*
** LÖVE web: a baseline compiler from Lua 5.1 bytecode to WebAssembly.
**
** Included at the end of lvm.c, so the helpers below can use the
** interpreter's static functions and macros and behave exactly like it.
**
** A hot function (see LJIT_THRESHOLD) is compiled into its own small wasm
** module that imports this module's memory and a few helper functions, and
** is added to the indirect function table. luaV_execute calls it at
** `reentry`. The compiled code
**  - keeps all Lua values in the Lua stack, exactly where the interpreter
**    keeps them, so it can be entered at any instruction (after calls,
**    coroutine resumes, and from a hot loop in the interpreter) and can hand
**    a frame back to the interpreter at any instruction;
**  - does moves, constants, booleans, nil, numeric arithmetic, numeric
**    comparisons, tests and numeric for loops inline;
**  - calls the helpers (copies of the interpreter's code) for everything
**    that can involve metatables, the GC, errors or calls;
**  - returns to luaV_execute to enter or leave Lua frames, like the
**    interpreter's own `goto reentry`.
** Frames with an active line or count hook (debug.sethook, love.thread's
** scheduler) are always interpreted.
*/

#include "ljit.h"

int ljit_enabled = LJIT_ENABLED;
int ljit_threshold = 50;
int ljit_depth = 0;
int ljit_pending = 0;

#if LJIT_ENABLED

#include <emscripten.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Implemented in web/native/library_ljit.js. */
extern int ljit_instantiate (const unsigned char *bytes, int size);
extern void ljit_release (int index);

/* Functions compiled / that failed to compile, bytes of wasm generated and
** time spent compiling (ours and the browser's), for tests and diagnostics. */
static int ljit_ncompiled = 0, ljit_nfailed = 0, ljit_ndeopts = 0;

/* The instruction whose speculation failed, stored by compiled code as it
** deopts (it may resume elsewhere: after a helper, at the next one). */
static int ljit_blame = 0;
static double ljit_nbytes = 0, ljit_ms = 0;

EMSCRIPTEN_KEEPALIVE double ljit_stats (int what) {
  switch (what) {
    case 0: return ljit_ncompiled;
    case 1: return ljit_nfailed;
    case 2: return ljit_nbytes;
    case 3: return ljit_ms;
    default: return ljit_ndeopts;
  }
}

/* Called from JS (player options, tests). */
EMSCRIPTEN_KEEPALIVE void ljit_configure (int enabled, int threshold) {
  ljit_enabled = enabled;
  if (threshold > 0) ljit_threshold = threshold;
}

/* ------------------------------------------------------------------------
** Helpers called by compiled code. Each takes the address of the
** instruction it implements.
** --------------------------------------------------------------------- */

#define LJ_DECODE \
  const Instruction i = *pc; \
  StkId base = L->base; \
  LClosure *cl = &clvalue(L->ci->func)->l; \
  TValue *k = cl->p->k; \
  StkId ra = RA(i); \
  (void)k; (void)cl; \
  L->savedpc = pc + 1;

#define lj_arith(op,tm) { \
        TValue *rb = RKB(i); \
        TValue *rc = RKC(i); \
        if (ttisnumber(rb) && ttisnumber(rc)) { \
          lua_Number nb = nvalue(rb), nc = nvalue(rc); \
          setnvalue(ra, op(nb, nc)); \
        } \
        else \
          Arith(L, ra, rb, rc, tm); \
        break; \
      }

/* Instructions that don't change control flow. */
EMSCRIPTEN_KEEPALIVE void ljit_h_exec (lua_State *L, const Instruction *pc) {
  LJ_DECODE
  switch (GET_OPCODE(i)) {
    case OP_GETUPVAL: {
      setobj2s(L, ra, cl->upvals[GETARG_B(i)]->v);
      break;
    }
    case OP_GETGLOBAL: {
      TValue g;
      sethvalue(L, &g, cl->env);
      luaV_gettable(L, &g, KBx(i), ra);
      break;
    }
    case OP_GETTABLE: {
      luaV_gettable(L, RB(i), RKC(i), ra);
      break;
    }
    case OP_SETGLOBAL: {
      TValue g;
      sethvalue(L, &g, cl->env);
      luaV_settable(L, &g, KBx(i), ra);
      break;
    }
    case OP_SETUPVAL: {
      UpVal *uv = cl->upvals[GETARG_B(i)];
      setobj(L, uv->v, ra);
      luaC_barrier(L, uv, ra);
      break;
    }
    case OP_SETTABLE: {
      luaV_settable(L, ra, RKB(i), RKC(i));
      break;
    }
    case OP_NEWTABLE: {
      int b = GETARG_B(i);
      int c = GETARG_C(i);
      sethvalue(L, ra, luaH_new(L, luaO_fb2int(b), luaO_fb2int(c)));
      luaC_checkGC(L);
      break;
    }
    case OP_SELF: {
      StkId rb = RB(i);
      setobjs2s(L, ra+1, rb);
      luaV_gettable(L, rb, RKC(i), ra);
      break;
    }
    case OP_ADD: lj_arith(luai_numadd, TM_ADD)
    case OP_SUB: lj_arith(luai_numsub, TM_SUB)
    case OP_MUL: lj_arith(luai_nummul, TM_MUL)
    case OP_DIV: lj_arith(luai_numdiv, TM_DIV)
    case OP_MOD: lj_arith(luai_nummod, TM_MOD)
    case OP_POW: lj_arith(luai_numpow, TM_POW)
    case OP_UNM: {
      TValue *rb = RB(i);
      if (ttisnumber(rb)) {
        lua_Number nb = nvalue(rb);
        setnvalue(ra, luai_numunm(nb));
      }
      else
        Arith(L, ra, rb, rb, TM_UNM);
      break;
    }
    case OP_LEN: {
      const TValue *rb = RB(i);
      switch (ttype(rb)) {
        case LUA_TTABLE: {
          setnvalue(ra, cast_num(luaH_getn(hvalue(rb))));
          break;
        }
        case LUA_TSTRING: {
          setnvalue(ra, cast_num(tsvalue(rb)->len));
          break;
        }
        default: {
          if (!call_binTM(L, rb, luaO_nilobject, ra, TM_LEN))
            luaG_typeerror(L, rb, "get length of");
        }
      }
      break;
    }
    case OP_CONCAT: {
      int b = GETARG_B(i);
      int c = GETARG_C(i);
      luaV_concat(L, c-b+1, c);
      luaC_checkGC(L);
      base = L->base;
      setobjs2s(L, RA(i), base+b);
      break;
    }
    case OP_CLOSE: {
      luaF_close(L, ra);
      break;
    }
    case OP_CLOSURE: {
      const Instruction *upc = pc + 1;
      Proto *p = cl->p->p[GETARG_Bx(i)];
      int nup = p->nups;
      int j;
      Closure *ncl = luaF_newLclosure(L, nup, cl->env);
      ncl->l.p = p;
      for (j = 0; j < nup; j++, upc++) {
        if (GET_OPCODE(*upc) == OP_GETUPVAL)
          ncl->l.upvals[j] = cl->upvals[GETARG_B(*upc)];
        else {
          lua_assert(GET_OPCODE(*upc) == OP_MOVE);
          ncl->l.upvals[j] = luaF_findupval(L, base + GETARG_B(*upc));
        }
      }
      setclvalue(L, ra, ncl);
      luaC_checkGC(L);
      break;
    }
    case OP_SETLIST: {
      int n = GETARG_B(i);
      int c = GETARG_C(i);
      int last;
      Table *h;
      if (n == 0) {
        n = cast_int(L->top - ra) - 1;
        L->top = L->ci->top;
      }
      if (c == 0) c = cast_int(*(pc + 1));
      if (!ttistable(ra)) break;
      h = hvalue(ra);
      last = ((c-1)*LFIELDS_PER_FLUSH) + n;
      if (last > h->sizearray)
        luaH_resizearray(L, h, last);
      for (; n > 0; n--) {
        TValue *val = ra+n;
        setobj2t(L, luaH_setnum(L, h, last--), val);
        luaC_barriert(L, h, val);
      }
      break;
    }
    case OP_VARARG: {
      int b = GETARG_B(i) - 1;
      int j;
      CallInfo *ci = L->ci;
      int n = cast_int(ci->base - ci->func) - cl->p->numparams - 1;
      if (b == LUA_MULTRET) {
        luaD_checkstack(L, n);
        ra = RA(i);  /* previous call may change the stack */
        b = n;
        L->top = ra + n;
      }
      for (j = 0; j < b; j++) {
        if (j < n) {
          setobjs2s(L, ra + j, ci->base - n + j);
        }
        else {
          setnilvalue(ra + j);
        }
      }
      break;
    }
    default:
      lua_assert(0);
  }
}

/* OP_EQ, OP_LT, OP_LE: returns the comparison result (before testing A). */
EMSCRIPTEN_KEEPALIVE int ljit_h_compare (lua_State *L, const Instruction *pc) {
  LJ_DECODE
  (void)ra;
  switch (GET_OPCODE(i)) {
    case OP_EQ: {
      TValue *rb = RKB(i);
      TValue *rc = RKC(i);
      return equalobj(L, rb, rc);
    }
    case OP_LT: return luaV_lessthan(L, RKB(i), RKC(i));
    default: return lessequal(L, RKB(i), RKC(i));
  }
}

/* After ljit_h_call set up a Lua call: runs the callee's compiled code
** directly (no trip through luaV_execute) when possible. Returns 0 when the
** callee has returned, else an LJIT_ code for the caller to return, having
** counted the callee's frame in ljit_pending. */
static int ljit_enter (lua_State *L) {
  int r;
  int replaced = 0;  /* the frame we entered was replaced by a tail call */
  ptrdiff_t level = L->ci - L->base_ci;  /* (L->ci may be reallocated) */
  if (!ljit_enabled || ljit_depth >= LJIT_MAX_DEPTH ||
      (L->hookmask & (LUA_MASKLINE | LUA_MASKCOUNT)))
    return LJIT_CALL;
  for (;;) {
    Proto *p = clvalue(L->ci->func)->l.p;
    if (p->jit == 0 && ++p->jitcount >= ljit_threshold)
      ljit_compile(L, p);
    if (p->jit <= 0) {
      if (!replaced) return LJIT_CALL;  /* luaV_execute runs the callee */
      ljit_pending++;  /* it takes over the frame that was replaced */
      return LJIT_TAILCALL;
    }
    ljit_depth++;
    r = ((ljit_Function)(size_t)p->jit)(L);
    ljit_depth--;
    /* LJIT_TAILCALL also comes up from a frame above the one we entered
    ** (a call it made, through ljit_enter, was replaced by a function
    ** that isn't compiled). Our frame is then still on the stack, below
    ** L->ci: hand both to luaV_execute (default case below). Only when
    ** L->ci is our frame's slot was our frame itself replaced. */
    if (r != LJIT_TAILCALL || L->ci - L->base_ci != level ||
        (L->hookmask & (LUA_MASKLINE | LUA_MASKCOUNT)))
      break;
    replaced = 1;  /* the callee was replaced by a tail call: run that one */
  }
  switch (r) {
    case LJIT_RET1:
      L->top = L->ci->top;
      return 0;
    case LJIT_RET0:
      return 0;
    default:
      /* The callee's frame stays on the Lua stack and luaV_execute takes
      ** it over, then handles r as if the callee had returned it (a call
      ** the callee made, a tail call, a yield or a hook). */
      ljit_pending++;
      return r;
  }
}

/* OP_CALL: 0 if the callee (a C function, or compiled Lua code entered
** directly) has returned and the caller continues, else an LJIT_ code for
** the caller to return to luaV_execute. */
/* luaD_precall's Lua case, for the common calls that don't need to grow
** the stack or CallInfo array, adjust varargs or call a hook. Returns 0 if
** the call needs luaD_precall. */
static int lean_precall (lua_State *L, StkId func, int nresults) {
  Proto *np;
  CallInfo *nci;
  StkId nbase = func + 1, st;
  if (!ttisfunction(func) || clvalue(func)->c.isC) return 0;
  np = clvalue(func)->l.p;
  if (np->is_vararg || (L->hookmask & LUA_MASKCALL) || L->ci == L->end_ci ||
      (char *)L->stack_last - (char *)L->top <= np->maxstacksize * (int)sizeof(TValue))
    return 0;
  L->ci->savedpc = L->savedpc;
  if (L->top > nbase + np->numparams)
    L->top = nbase + np->numparams;
  nci = ++L->ci;
  nci->func = func;
  L->base = nci->base = nbase;
  nci->top = nbase + np->maxstacksize;
  L->savedpc = np->code;
  nci->tailcalls = 0;
  nci->nresults = nresults;
  for (st = L->top; st < nci->top; st++)
    setnilvalue(st);
  L->top = nci->top;
  return 1;
}

EMSCRIPTEN_KEEPALIVE int ljit_h_call (lua_State *L, const Instruction *pc) {
  LJ_DECODE
  int b = GETARG_B(i);
  int nresults = GETARG_C(i) - 1;
  if (b != 0) L->top = ra+b;
  if (lean_precall(L, ra, nresults)) return ljit_enter(L);
  switch (luaD_precall(L, ra, nresults)) {
    case PCRLUA: return ljit_enter(L);
    case PCRC: {
      if (nresults >= 0) L->top = L->ci->top;
      return 0;
    }
    default: return LJIT_YIELD;
  }
}

EMSCRIPTEN_KEEPALIVE int ljit_h_tailcall (lua_State *L, const Instruction *pc) {
  LJ_DECODE
  int b = GETARG_B(i);
  if (b != 0) L->top = ra+b;
  if (!lean_precall(L, ra, LUA_MULTRET)) {
    switch (luaD_precall(L, ra, LUA_MULTRET)) {
      case PCRLUA: break;
      case PCRC: return 0;
      default: return LJIT_YIELD;
    }
  }
  {
    CallInfo *ci = L->ci - 1;
    int aux;
    StkId func = ci->func;
    StkId pfunc = (ci+1)->func;
    /* (luaF_close would do nothing unless an upvalue is open in the frame) */
    if (L->openupval && ngcotouv(L->openupval)->v >= ci->base)
      luaF_close(L, ci->base);
    L->base = ci->base = ci->func + ((ci+1)->base - pfunc);
    for (aux = 0; pfunc+aux < L->top; aux++)
      setobjs2s(L, func+aux, pfunc+aux);
    ci->top = L->top = func+aux;
    ci->savedpc = L->savedpc;
    ci->tailcalls++;
    L->ci--;
    return LJIT_TAILCALL;
  }
}

EMSCRIPTEN_KEEPALIVE int ljit_h_return (lua_State *L, const Instruction *pc) {
  LJ_DECODE
  int b = GETARG_B(i);
  if (b != 0) L->top = ra+b-1;
  /* (luaF_close would do nothing unless an upvalue is open in this frame) */
  if (L->openupval && ngcotouv(L->openupval)->v >= base) luaF_close(L, base);
  if (!(L->hookmask & LUA_MASKRET)) {
    /* luaD_poscall without the hook */
    CallInfo *ci = L->ci--;
    StkId res = ci->func, first = ra;
    int wanted = ci->nresults, n;
    L->base = (ci - 1)->base;
    L->savedpc = (ci - 1)->savedpc;
    for (n = wanted; n != 0 && first < L->top; n--)
      setobjs2s(L, res++, first++);
    while (n-- > 0)
      setnilvalue(res++);
    L->top = res;
    return wanted != LUA_MULTRET ? LJIT_RET1 : LJIT_RET0;
  }
  b = luaD_poscall(L, ra);
  return b ? LJIT_RET1 : LJIT_RET0;
}

EMSCRIPTEN_KEEPALIVE void ljit_h_forprep (lua_State *L, const Instruction *pc) {
  LJ_DECODE
  const TValue *init = ra;
  const TValue *plimit = ra+1;
  const TValue *pstep = ra+2;
  if (!tonumber(init, ra))
    luaG_runerror(L, LUA_QL("for") " initial value must be a number");
  else if (!tonumber(plimit, ra+1))
    luaG_runerror(L, LUA_QL("for") " limit must be a number");
  else if (!tonumber(pstep, ra+2))
    luaG_runerror(L, LUA_QL("for") " step must be a number");
  setnvalue(ra, luai_numsub(nvalue(ra), nvalue(pstep)));
}

/* OP_TFORLOOP: returns 1 if the loop continues. */
EMSCRIPTEN_KEEPALIVE int ljit_h_tforloop (lua_State *L, const Instruction *pc) {
  LJ_DECODE
  StkId cb = ra + 3;
  setobjs2s(L, cb+2, ra+2);
  setobjs2s(L, cb+1, ra+1);
  setobjs2s(L, cb, ra);
  L->top = cb+3;
  luaD_call(L, cb, GETARG_C(i));
  base = L->base;
  L->top = L->ci->top;
  cb = RA(i) + 3;
  if (!ttisnil(cb)) {
    setobjs2s(L, cb-1, cb);
    return 1;
  }
  return 0;
}

/* Import order; must match the helper list in library_ljit.js. */
enum {
  H_EXEC, H_COMPARE, H_CALL, H_TAILCALL, H_RETURN, H_FORPREP, H_TFORLOOP,
  NUM_HELPERS
};

/* ------------------------------------------------------------------------
** Wasm encoding
** --------------------------------------------------------------------- */

typedef struct Buf {
  unsigned char *p;
  size_t n, cap;
  int oom;
} Buf;

static void b_reserve (Buf *b, size_t extra) {
  if (b->n + extra > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 4096;
    unsigned char *np;
    while (cap < b->n + extra) cap *= 2;
    np = (unsigned char *)realloc(b->p, cap);
    if (np == NULL) { b->oom = 1; b->n = 0; return; }
    b->p = np;
    b->cap = cap;
  }
}

static void b_byte (Buf *b, int x) {
  b_reserve(b, 1);
  if (!b->oom) b->p[b->n++] = (unsigned char)x;
}

static void b_u32 (Buf *b, unsigned int x) {
  do {
    unsigned char c = x & 0x7f;
    x >>= 7;
    if (x) c |= 0x80;
    b_byte(b, c);
  } while (x);
}

static void b_s32 (Buf *b, int x) {
  int more = 1;
  while (more) {
    unsigned char c = x & 0x7f;
    x >>= 7;
    if ((x == 0 && !(c & 0x40)) || (x == -1 && (c & 0x40))) more = 0;
    else c |= 0x80;
    b_byte(b, c);
  }
}

static void b_bytes (Buf *b, const void *p, size_t n) {
  b_reserve(b, n);
  if (!b->oom) { memcpy(b->p + b->n, p, n); b->n += n; }
}

static void b_str (Buf *b, const char *s) {
  size_t n = strlen(s);
  b_u32(b, (unsigned int)n);
  b_bytes(b, s, n);
}

/* Appends `content` as a section with id `id`. */
static void b_section (Buf *b, int id, Buf *content) {
  b_byte(b, id);
  b_u32(b, (unsigned int)content->n);
  b_bytes(b, content->p, content->n);
}

enum {
  W_UNREACHABLE = 0x00, W_BLOCK = 0x02, W_LOOP = 0x03, W_IF = 0x04,
  W_ELSE = 0x05, W_END = 0x0b, W_BR = 0x0c, W_BR_IF = 0x0d,
  W_BR_TABLE = 0x0e, W_RETURN = 0x0f, W_CALL = 0x10,
  W_LOCAL_GET = 0x20, W_LOCAL_SET = 0x21, W_LOCAL_TEE = 0x22,
  W_I32_LOAD = 0x28, W_I64_LOAD = 0x29, W_F64_LOAD = 0x2b,
  W_I32_LOAD8_U = 0x2d, W_I32_STORE = 0x36, W_I64_STORE = 0x37,
  W_F64_STORE = 0x39, W_I32_CONST = 0x41, W_F64_CONST = 0x44,
  W_I32_EQZ = 0x45, W_I32_EQ = 0x46, W_I32_NE = 0x47,
  W_F64_EQ = 0x61, W_F64_LT = 0x63, W_F64_GT = 0x64, W_F64_LE = 0x65,
  W_I32_ADD = 0x6a, W_I32_SUB = 0x6b, W_I32_AND = 0x71, W_I32_OR = 0x72,
  W_I32_SHR_U = 0x76, W_F64_NEG = 0x9a, W_F64_FLOOR = 0x9c,
  W_I32_STORE8 = 0x3a, W_I32_LT_U = 0x49, W_I32_GE_U = 0x4f,
  W_I32_MUL = 0x6c, W_I32_SHL = 0x74, W_F64_CONVERT_I32_S = 0xb7,
  W_PREFIX_FC = 0xfc, W_FC_I32_TRUNC_SAT_F64_S = 0x02,
  W_F64_ADD = 0xa0, W_F64_SUB = 0xa1, W_F64_MUL = 0xa2, W_F64_DIV = 0xa3,
  W_VOID = 0x40, W_TYPE_I32 = 0x7f, W_TYPE_F64 = 0x7c
};

/* Locals of the compiled function. Param 0 is L. They are followed by one
** f64 local per Lua register, V_DREG(r), that holds the register's number
** while it is known to be one (see "Speculation" below). */
enum { V_L = 0, V_BASE, V_IDX, V_T, V_R, V_TAB, V_S, V_E, V_D1, V_D2 };
#define NUM_I32_LOCALS 7  /* V_BASE .. V_E */
#define V_DREG(r) (V_D2 + 1 + (r))

/* Offsets in TValue / lua_State used by the generated code. */
#define TV_SIZE   ((int)sizeof(TValue))
#define TV_VALUE  ((int)offsetof(TValue, value))
#define TV_TT     ((int)offsetof(TValue, tt))
#define L_BASE    ((int)offsetof(lua_State, base))
#define L_SAVEDPC ((int)offsetof(lua_State, savedpc))
#define L_HOOKMASK ((int)offsetof(lua_State, hookmask))
#define L_CI      ((int)offsetof(lua_State, ci))
#define CI_FUNC   ((int)offsetof(CallInfo, func))
#define CI_BASE   ((int)offsetof(CallInfo, base))
#define CI_SAVEDPC ((int)offsetof(CallInfo, savedpc))
#define CI_NRESULTS ((int)offsetof(CallInfo, nresults))
#define CI_SIZE   ((int)sizeof(CallInfo))
#define L_TOP     ((int)offsetof(lua_State, top))
#define L_OPENUPVAL ((int)offsetof(lua_State, openupval))
#define LCL_ENV   ((int)offsetof(LClosure, env))
#define T_FLAGS   ((int)offsetof(Table, flags))
#define T_MARKED  ((int)offsetof(Table, marked))
#define T_LSIZENODE ((int)offsetof(Table, lsizenode))
#define T_METATABLE ((int)offsetof(Table, metatable))
#define T_ARRAY   ((int)offsetof(Table, array))
#define T_NODE    ((int)offsetof(Table, node))
#define T_SIZEARRAY ((int)offsetof(Table, sizearray))
#define LCL_UPVALS ((int)offsetof(LClosure, upvals))
#define UV_V      ((int)offsetof(UpVal, v))
#define N_SIZE    ((int)sizeof(Node))
#define N_KEYVAL  ((int)offsetof(Node, i_key.nk.value))
#define N_KEYTT   ((int)offsetof(Node, i_key.nk.tt))
#define N_NEXT    ((int)offsetof(Node, i_key.nk.next))
/* Node values are found by their node address. */
typedef char ljit_check_node_layout[offsetof(Node, i_val) == 0 ? 1 : -1];

/* A set of Lua registers. */
#define LJIT_MAXREGS 256
typedef struct RegSet { unsigned int w[LJIT_MAXREGS / 32]; } RegSet;

static int rs_has (const RegSet *s, int r) {
  return r >= 0 && r < LJIT_MAXREGS && ((s->w[r >> 5] >> (r & 31)) & 1);
}
static void rs_add (RegSet *s, int r) {
  if (r >= 0 && r < LJIT_MAXREGS) s->w[r >> 5] |= 1u << (r & 31);
}
static void rs_del (RegSet *s, int r) {
  if (r >= 0 && r < LJIT_MAXREGS) s->w[r >> 5] &= ~(1u << (r & 31));
}
static void rs_delfrom (RegSet *s, int r) {
  for (; r < LJIT_MAXREGS; r++) rs_del(s, r);
}
static int rs_empty (const RegSet *s) {
  int j;
  for (j = 0; j < LJIT_MAXREGS / 32; j++) if (s->w[j]) return 0;
  return 1;
}
/* d &= s, d |= s: return whether d changed */
static int rs_and (RegSet *d, const RegSet *s) {
  int j, changed = 0;
  for (j = 0; j < LJIT_MAXREGS / 32; j++) {
    unsigned int w = d->w[j] & s->w[j];
    if (w != d->w[j]) { d->w[j] = w; changed = 1; }
  }
  return changed;
}
static int rs_or (RegSet *d, const RegSet *s) {
  int j, changed = 0;
  for (j = 0; j < LJIT_MAXREGS / 32; j++) {
    unsigned int w = d->w[j] | s->w[j];
    if (w != d->w[j]) { d->w[j] = w; changed = 1; }
  }
  return changed;
}

typedef struct Compiler {
  Buf code;
  Proto *p;
  int n;      /* number of instructions */
  int pcidx;  /* instruction being compiled */
  int depth;  /* blocks opened inside the current instruction */
  TString *indexname;  /* "__index" */
  /* Speculation (see below) */
  int spec;        /* speculate on types (else `known` is all empty) */
  RegSet *known;   /* per instruction: registers known to be numbers */
  RegSet *uses;    /* per instruction: registers used as numbers ahead */
  int *work;       /* the analysis's worklist */
  int *stub;       /* per instruction: entry stub number, or -1 */
  char *seen, *queued;
  /* Innermost loops are wasm loops (see compile_body) */
  int *head;       /* per instruction inside a loop, past its head: the head */
  int *loopend;    /* per loop head: the loop's last instruction */
  int *topcount;   /* per instruction: blocks of the dispatcher before it */
  RegSet cur;      /* the same, as the current instruction's code goes */
  int exitlabel;   /* label of the deopt exit, at depth 0 */
} Compiler;

#define C (&cc->code)

static void op (Compiler *cc, int o) { b_byte(C, o); }
static void lget (Compiler *cc, int v) { b_byte(C, W_LOCAL_GET); b_u32(C, v); }
static void lset (Compiler *cc, int v) { b_byte(C, W_LOCAL_SET); b_u32(C, v); }
static void ltee (Compiler *cc, int v) { b_byte(C, W_LOCAL_TEE); b_u32(C, v); }
static void i32c (Compiler *cc, int x) { b_byte(C, W_I32_CONST); b_s32(C, x); }
static void f64c (Compiler *cc, double x) { b_byte(C, W_F64_CONST); b_bytes(C, &x, 8); }

static void mem (Compiler *cc, int o, int align, unsigned int offset) {
  b_byte(C, o);
  b_u32(C, align);
  b_u32(C, offset);
}

static void block (Compiler *cc, int kind, int type) {
  b_byte(C, kind);
  b_byte(C, type);
  cc->depth++;
}

static void end (Compiler *cc) {
  b_byte(C, W_END);
  cc->depth--;
}

static unsigned int pcaddr (Compiler *cc, int idx) {
  return (unsigned int)(size_t)(cc->p->code + idx);
}

/* Offset of register r from base. */
static unsigned int reg (int r) { return (unsigned int)(r * TV_SIZE); }

static int isk_number (Compiler *cc, int rk) {
  return ISK(rk) && ttisnumber(&cc->p->k[INDEXK(rk)]);
}

/* Is RK operand rk a register known to hold a number (in its V_DREG)? */
static int known (Compiler *cc, int rk) {
  return !ISK(rk) && rs_has(&cc->cur, rk);
}

/* Pushes the f64 value of RK operand rk (must be a register or a numeric
** constant). */
static void load_num (Compiler *cc, int rk) {
  if (ISK(rk))
    f64c(cc, nvalue(&cc->p->k[INDEXK(rk)]));
  else if (known(cc, rk))
    lget(cc, V_DREG(rk));
  else {
    lget(cc, V_BASE);
    mem(cc, W_F64_LOAD, 3, reg(rk) + TV_VALUE);
  }
}

/* Pushes 1 if register r is not a number. */
static void not_number (Compiler *cc, int r) {
  lget(cc, V_BASE);
  mem(cc, W_I32_LOAD, 2, reg(r) + TV_TT);
  i32c(cc, LUA_TNUMBER);
  op(cc, W_I32_NE);
}

/* Pushes 1 if any of the RK operands isn't a number (constants and known
** registers are numbers). Returns 0 if nothing was pushed. */
static int push_not_numbers (Compiler *cc, int b, int c) {
  int pushed = 0;
  if (!ISK(b) && !known(cc, b)) { not_number(cc, b); pushed++; }
  if (!ISK(c) && !known(cc, c)) {
    not_number(cc, c);
    if (pushed) op(cc, W_I32_OR);
    pushed++;
  }
  return pushed;
}

static void store_num (Compiler *cc, int r, int vlocal) {
  lget(cc, V_BASE);
  lget(cc, vlocal);
  mem(cc, W_F64_STORE, 3, reg(r) + TV_VALUE);
  lget(cc, V_BASE);
  i32c(cc, LUA_TNUMBER);
  mem(cc, W_I32_STORE, 2, reg(r) + TV_TT);
}

static void copy_tvalue (Compiler *cc, int dst, int src_is_abs, unsigned int src) {
  int half;
  for (half = 0; half < 2; half++) {
    lget(cc, V_BASE);
    if (src_is_abs) i32c(cc, 0); else lget(cc, V_BASE);
    mem(cc, W_I64_LOAD, 3, src + half * 8);
    mem(cc, W_I64_STORE, 3, reg(dst) + half * 8);
  }
}

static void reload_base (Compiler *cc) {
  lget(cc, V_L);
  mem(cc, W_I32_LOAD, 2, L_BASE);
  lset(cc, V_BASE);
}

/* call helper(L, &code[pcidx]) */
static void call_helper (Compiler *cc, int h) {
  lget(cc, V_L);
  i32c(cc, (int)pcaddr(cc, cc->pcidx));
  b_byte(C, W_CALL);
  b_u32(C, h);
}

/* Leaves the compiled code to interpret from instruction `idx`
** (LJIT_DEOPT): a speculation failed. The Lua stack is always up to date,
** so nothing else needs to be written back. */
static void deopt (Compiler *cc, int idx) {
  i32c(cc, (int)(size_t)&ljit_blame);
  i32c(cc, cc->pcidx);
  mem(cc, W_I32_STORE, 2, 0);
  i32c(cc, idx);
  lset(cc, V_IDX);
  b_byte(C, W_BR);
  b_u32(C, (unsigned int)(cc->depth + cc->exitlabel));
}

/* Pops a number into register r: into its f64 local and the Lua stack,
** whose type tag is already right if r was known to be a number. */
static void def_num (Compiler *cc, int r) {
  lset(cc, V_DREG(r));
  lget(cc, V_BASE);
  lget(cc, V_DREG(r));
  mem(cc, W_F64_STORE, 3, reg(r) + TV_VALUE);
  if (!rs_has(&cc->cur, r)) {
    lget(cc, V_BASE);
    i32c(cc, LUA_TNUMBER);
    mem(cc, W_I32_STORE, 2, reg(r) + TV_TT);
  }
  if (cc->spec) rs_add(&cc->cur, r);
}

/* Makes register r known to be a number, leaving the compiled code at
** instruction `idx` if it isn't one. */
static void guard (Compiler *cc, int r, int idx) {
  if (rs_has(&cc->cur, r)) return;
  not_number(cc, r);
  block(cc, W_IF, W_VOID);
  deopt(cc, idx);
  end(cc);
  lget(cc, V_BASE);
  mem(cc, W_F64_LOAD, 3, reg(r) + TV_VALUE);
  lset(cc, V_DREG(r));
  rs_add(&cc->cur, r);
}

/* Loads every register in `set` into its local, leaving the compiled code
** at instruction `idx` if any isn't a number (one branch for all: this is
** emitted after every helper call). The current set is left as it was. */
static void guard_all (Compiler *cc, const RegSet *set, int idx) {
  int r, n = 0;
  for (r = 0; r < LJIT_MAXREGS; r++) {
    if (!rs_has(set, r)) continue;
    not_number(cc, r);
    if (n++) op(cc, W_I32_OR);
  }
  if (n == 0) return;
  block(cc, W_IF, W_VOID);
  deopt(cc, idx);
  end(cc);
  for (r = 0; r < LJIT_MAXREGS; r++) {
    if (!rs_has(set, r)) continue;
    lget(cc, V_BASE);
    mem(cc, W_F64_LOAD, 3, reg(r) + TV_VALUE);
    lset(cc, V_DREG(r));
  }
}

/* After a helper call, which may have run any Lua code (a metamethod, a
** hook, a function that changes our locals with debug.setlocal), before
** going on to instruction s: reloads the registers s expects to be known
** numbers, checking that they still are. */
static void reestablish (Compiler *cc, int s) {
  if (s < 0 || s >= cc->n || rs_empty(&cc->known[s])) return;
  guard_all(cc, &cc->known[s], s);
}

/* Instruction k's single successor (for helpers that don't jump). */
static int next_insn (Compiler *cc, int k);

/* The generic helper for the current instruction. */
static void helper_exec (Compiler *cc) {
  call_helper(cc, H_EXEC);
  reload_base(cc);
  reestablish(cc, next_insn(cc, cc->pcidx));
}

static void hook_check (Compiler *cc, int t);

/* The innermost loop instruction k is in (its head, if k is the head of
** one), or -1. */
static int loop_of (Compiler *cc, int k) {
  if (cc->head[k] >= 0) return cc->head[k];
  return cc->loopend[k] >= 0 ? k : -1;
}

/* Label, at depth 0 of the current instruction, of the dispatcher's block
** for instruction m (outside any loop, past the current one), or with
** m = n, of the dispatcher's loop. */
static int top_label (Compiler *cc, int m) {
  int k = cc->pcidx, h = loop_of(cc, k), from = k, base = 0;
  if (h >= 0) {
    from = cc->loopend[h];
    base = from - k + 1;  /* the loop's blocks past k, and the loop */
  }
  return base + cc->topcount[m] - cc->topcount[from + 1];
}

static void br (Compiler *cc, int label) {
  b_byte(C, W_BR);
  b_u32(C, (unsigned int)(cc->depth + label));
}

/* Continues execution at instruction t. */
static void jump (Compiler *cc, int t) {
  int k = cc->pcidx, h = loop_of(cc, k);
  if (h >= 0 && t == h) {  /* the loop's back edge */
    hook_check(cc, t);
    br(cc, cc->loopend[h] - k);
  }
  else if (t > k) {
    if (h >= 0 && t <= cc->loopend[h])  /* forward in the loop */
      br(cc, t - k - 1);
    else if (cc->head[t] < 0)
      br(cc, top_label(cc, t));
    else {  /* into a loop: its head's selector goes on to t */
      i32c(cc, t + 1);
      lset(cc, V_E);
      br(cc, top_label(cc, cc->head[t]));
    }
  }
  else {
    /* Backward jump, not an innermost loop's: through the dispatcher.
    ** Hand over to the interpreter if a hook was set (it may have been set
    ** by a function called in this loop). */
    lget(cc, V_L);
    mem(cc, W_I32_LOAD8_U, 0, L_HOOKMASK);
    i32c(cc, LUA_MASKLINE | LUA_MASKCOUNT);
    op(cc, W_I32_AND);
    b_byte(C, W_IF); b_byte(C, W_VOID);
    lget(cc, V_L);
    i32c(cc, (int)pcaddr(cc, t));
    mem(cc, W_I32_STORE, 2, L_SAVEDPC);
    i32c(cc, LJIT_INTERP);
    op(cc, W_RETURN);
    b_byte(C, W_END);
    /* Registers known at t are known here too: enter t past the stub that
    ** would check and load them (see compile_body). */
    i32c(cc, rs_empty(&cc->known[t]) ? t : cc->n + t);
    lset(cc, V_IDX);
    br(cc, top_label(cc, cc->n));
  }
}

/* For a loop's back edge: hand over to the interpreter if a hook was set
** (by a function the loop called). */
static void hook_check (Compiler *cc, int t) {
  lget(cc, V_L);
  mem(cc, W_I32_LOAD8_U, 0, L_HOOKMASK);
  i32c(cc, LUA_MASKLINE | LUA_MASKCOUNT);
  op(cc, W_I32_AND);
  b_byte(C, W_IF); b_byte(C, W_VOID);
  lget(cc, V_L);
  i32c(cc, (int)pcaddr(cc, t));
  mem(cc, W_I32_STORE, 2, L_SAVEDPC);
  i32c(cc, LJIT_INTERP);
  op(cc, W_RETURN);
  b_byte(C, W_END);
}

/* Pushes l_isfalse(register r). */
static void is_false (Compiler *cc, int r) {
  lget(cc, V_BASE);
  mem(cc, W_I32_LOAD, 2, reg(r) + TV_TT);
  ltee(cc, V_T);
  op(cc, W_I32_EQZ);
  lget(cc, V_T);
  i32c(cc, LUA_TBOOLEAN);
  op(cc, W_I32_EQ);
  lget(cc, V_BASE);
  mem(cc, W_I32_LOAD, 2, reg(r) + TV_VALUE);
  op(cc, W_I32_EQZ);
  op(cc, W_I32_AND);
  op(cc, W_I32_OR);
}

/* If the i32 on the stack is nonzero, return it from the function. */
static void return_if_nonzero (Compiler *cc) {
  ltee(cc, V_R);
  b_byte(C, W_IF); b_byte(C, W_VOID);
  lget(cc, V_R);
  op(cc, W_RETURN);
  b_byte(C, W_END);
}

static int speculative (Compiler *cc, int k);

/* Pushes the result of numeric operation wop on RK operands b and c. */
static void num_op (Compiler *cc, int wop, int b, int c) {
  if (wop == W_F64_FLOOR) {
    /* luai_nummod: a - floor(a/b)*b */
    load_num(cc, b); lset(cc, V_D1);
    load_num(cc, c); lset(cc, V_D2);
    lget(cc, V_D1);
    lget(cc, V_D1); lget(cc, V_D2); op(cc, W_F64_DIV);
    op(cc, W_F64_FLOOR);
    lget(cc, V_D2); op(cc, W_F64_MUL);
    op(cc, W_F64_SUB);
  }
  else {
    load_num(cc, b);
    load_num(cc, c);
    op(cc, wop);
  }
}

static void arith (Compiler *cc, Instruction i, int wop) {
  int a = GETARG_A(i), b = GETARG_B(i), c = GETARG_C(i);
  if ((ISK(b) && !isk_number(cc, b)) || (ISK(c) && !isk_number(cc, c))) {
    helper_exec(cc);
    return;
  }
  if (speculative(cc, cc->pcidx)) {
    /* operands that aren't numbers leave the compiled code */
    if (!ISK(b)) guard(cc, b, cc->pcidx);
    if (!ISK(c)) guard(cc, c, cc->pcidx);
    num_op(cc, wop, b, c);
    def_num(cc, a);
    return;
  }
  block(cc, W_BLOCK, W_VOID);  /* done */
  block(cc, W_BLOCK, W_VOID);  /* slow */
  if (push_not_numbers(cc, b, c)) {
    b_byte(C, W_BR_IF); b_u32(C, 0);
  }
  num_op(cc, wop, b, c);
  lset(cc, V_D1);
  store_num(cc, a, V_D1);
  rs_del(&cc->cur, a);
  b_byte(C, W_BR); b_u32(C, 1);
  end(cc);
  helper_exec(cc);
  end(cc);
}

/* For `register == constant` with a nil, boolean or string constant, pushes
** the result (no metamethod can apply). Returns 0 if not applicable. */
static int compare_const (Compiler *cc, int b, int c) {
  int r, kidx;
  TValue *kv;
  if (ISK(b) && !ISK(c)) { r = c; kidx = INDEXK(b); }
  else if (ISK(c) && !ISK(b)) { r = b; kidx = INDEXK(c); }
  else return 0;
  kv = &cc->p->k[kidx];
  switch (ttype(kv)) {
    case LUA_TNIL:
      lget(cc, V_BASE); mem(cc, W_I32_LOAD, 2, reg(r) + TV_TT);
      op(cc, W_I32_EQZ);
      return 1;
    case LUA_TBOOLEAN:
    case LUA_TSTRING:
      lget(cc, V_BASE); mem(cc, W_I32_LOAD, 2, reg(r) + TV_TT);
      i32c(cc, ttype(kv)); op(cc, W_I32_EQ);
      lget(cc, V_BASE); mem(cc, W_I32_LOAD, 2, reg(r) + TV_VALUE);
      /* strings are interned, so equal strings are the same object */
      i32c(cc, ttisstring(kv) ? (int)(size_t)rawtsvalue(kv) : bvalue(kv));
      op(cc, W_I32_EQ);
      op(cc, W_I32_AND);
      return 1;
    default:
      return 0;
  }
}

static void compare (Compiler *cc, Instruction i, int o) {
  int a = GETARG_A(i), b = GETARG_B(i), c = GETARG_C(i);
  int next = cc->pcidx + 1;
  int target = next + 1 + GETARG_sBx(cc->p->code[next]);
  int fast = !(ISK(b) && !isk_number(cc, b)) && !(ISK(c) && !isk_number(cc, c));
  int wop = o == OP_EQ ? W_F64_EQ : o == OP_LT ? W_F64_LT : W_F64_LE;
  if (o == OP_EQ && compare_const(cc, b, c)) {
    i32c(cc, a);
    op(cc, W_I32_EQ);
    block(cc, W_IF, W_VOID);
    jump(cc, target);
    end(cc);
    jump(cc, cc->pcidx + 2);
    return;
  }
  if (fast && ((o != OP_EQ && speculative(cc, cc->pcidx)) ||
               ((ISK(b) || known(cc, b)) && (ISK(c) || known(cc, c))))) {
    /* numbers (known, or else leave the compiled code) */
    if (!ISK(b)) guard(cc, b, cc->pcidx);
    if (!ISK(c)) guard(cc, c, cc->pcidx);
    load_num(cc, b);
    load_num(cc, c);
    op(cc, wop);
    i32c(cc, a);
    op(cc, W_I32_EQ);
    block(cc, W_IF, W_VOID);
    jump(cc, target);
    end(cc);
    jump(cc, cc->pcidx + 2);
    return;
  }
  if (fast) {
    block(cc, W_BLOCK, W_VOID);  /* done */
    block(cc, W_BLOCK, W_VOID);  /* slow */
    if (push_not_numbers(cc, b, c)) {
      b_byte(C, W_BR_IF); b_u32(C, 0);
    }
    load_num(cc, b);
    load_num(cc, c);
    op(cc, wop);
    lset(cc, V_R);
    b_byte(C, W_BR); b_u32(C, 1);
    end(cc);
  }
  call_helper(cc, H_COMPARE);
  lset(cc, V_R);
  reload_base(cc);
  if (!rs_empty(&cc->known[target]) || !rs_empty(&cc->known[cc->pcidx + 2])) {
    /* (the helper may have run a metamethod) */
    lget(cc, V_R);
    i32c(cc, a);
    op(cc, W_I32_EQ);
    block(cc, W_IF, W_VOID);
    reestablish(cc, target);
    jump(cc, target);
    end(cc);
    reestablish(cc, cc->pcidx + 2);
    jump(cc, cc->pcidx + 2);
  }
  if (fast) end(cc);
  lget(cc, V_R);
  i32c(cc, a);
  op(cc, W_I32_EQ);
  block(cc, W_IF, W_VOID);
  jump(cc, target);
  end(cc);
  jump(cc, cc->pcidx + 2);
}

/* ------------------------------------------------------------------------
** Inline table access. These handle the common case (a table with the key
** present, or an array slot) and branch to the helper for everything else,
** which then does exactly what the interpreter does.
** --------------------------------------------------------------------- */

/* br_if to the `slow` block, `levels` blocks out. */
static void br_if_out (Compiler *cc, int levels) {
  b_byte(C, W_BR_IF);
  b_u32(C, (unsigned int)levels);
}

/* V_TAB = table in register r; branches out `slow` levels if not a table. */
static void load_table_reg (Compiler *cc, int r, int slow) {
  lget(cc, V_BASE);
  mem(cc, W_I32_LOAD, 2, reg(r) + TV_TT);
  i32c(cc, LUA_TTABLE);
  op(cc, W_I32_NE);
  br_if_out(cc, slow);
  lget(cc, V_BASE);
  mem(cc, W_I32_LOAD, 2, reg(r) + TV_VALUE);
  lset(cc, V_TAB);
}

/* V_TAB = the current closure's environment (always a table). */
static void load_env (Compiler *cc) {
  lget(cc, V_L);
  mem(cc, W_I32_LOAD, 2, L_CI);
  mem(cc, W_I32_LOAD, 2, CI_FUNC);
  mem(cc, W_I32_LOAD, 2, TV_VALUE);
  mem(cc, W_I32_LOAD, 2, LCL_ENV);
  lset(cc, V_TAB);
}

/* V_S = address of the value for string key ks in V_TAB's hash part, or 0.
** The key's hash is known when compiling. */
static void lookup_str (Compiler *cc, TString *ks) {
  lget(cc, V_TAB);
  mem(cc, W_I32_LOAD, 2, T_NODE);
  i32c(cc, 1);
  lget(cc, V_TAB);
  mem(cc, W_I32_LOAD8_U, 0, T_LSIZENODE);
  op(cc, W_I32_SHL);
  i32c(cc, 1);
  op(cc, W_I32_SUB);
  i32c(cc, (int)ks->tsv.hash);
  op(cc, W_I32_AND);
  i32c(cc, N_SIZE);
  op(cc, W_I32_MUL);
  op(cc, W_I32_ADD);
  lset(cc, V_S);
  block(cc, W_BLOCK, W_VOID);
  block(cc, W_LOOP, W_VOID);
  lget(cc, V_S); mem(cc, W_I32_LOAD, 2, N_KEYTT);
  i32c(cc, LUA_TSTRING);
  op(cc, W_I32_EQ);
  lget(cc, V_S); mem(cc, W_I32_LOAD, 2, N_KEYVAL);
  i32c(cc, (int)(size_t)ks);
  op(cc, W_I32_EQ);
  op(cc, W_I32_AND);
  br_if_out(cc, 1);  /* found */
  lget(cc, V_S); mem(cc, W_I32_LOAD, 2, N_NEXT);
  ltee(cc, V_S);
  br_if_out(cc, 0);  /* next in chain */
  end(cc);  /* end of chain: V_S is 0 */
  end(cc);
}

/* V_S = address of V_TAB's array slot for the integer key whose f64 value is
** on the stack, or 0 if it isn't an integer in the array part. */
static void lookup_num (Compiler *cc) {
  ltee(cc, V_D2);
  b_byte(C, W_PREFIX_FC); b_u32(C, W_FC_I32_TRUNC_SAT_F64_S);
  ltee(cc, V_T);
  op(cc, W_F64_CONVERT_I32_S);
  lget(cc, V_D2);
  op(cc, W_F64_EQ);
  lget(cc, V_T); i32c(cc, 1); op(cc, W_I32_SUB);
  lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_SIZEARRAY);
  op(cc, W_I32_LT_U);
  op(cc, W_I32_AND);
  block(cc, W_IF, W_TYPE_I32);
  lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_ARRAY);
  lget(cc, V_T); i32c(cc, 1); op(cc, W_I32_SUB);
  i32c(cc, TV_SIZE); op(cc, W_I32_MUL);
  op(cc, W_I32_ADD);
  b_byte(C, W_ELSE);
  i32c(cc, 0);
  end(cc);
  lset(cc, V_S);
}

/* Key kinds the inline paths handle. */
enum { KEY_NONE, KEY_STR, KEY_NUM };

static int key_kind (Compiler *cc, int rk) {
  if (ISK(rk)) {
    TValue *kv = &cc->p->k[INDEXK(rk)];
    if (ttisstring(kv)) return KEY_STR;
    if (ttisnumber(kv)) return KEY_NUM;
    return KEY_NONE;
  }
  return KEY_NUM;  /* registers: numbers inline, anything else via helper */
}

/* Emits the lookup of key rk in V_TAB into V_S, branching out `slow`
** levels if it can't be handled inline. */
static void lookup (Compiler *cc, int rk, int slow) {
  if (key_kind(cc, rk) == KEY_STR)
    lookup_str(cc, rawtsvalue(&cc->p->k[INDEXK(rk)]));
  else {
    if (!ISK(rk) && !known(cc, rk)) {
      not_number(cc, rk);
      br_if_out(cc, slow);
    }
    load_num(cc, rk);
    lookup_num(cc);
  }
  lget(cc, V_S);
  op(cc, W_I32_EQZ);
  br_if_out(cc, slow);
}

/* Levels of __index tables followed inline (an object, its class and the
** class's base class). */
#define LJIT_INDEX_LEVELS 3

/* ra = nil */
static void set_nil (Compiler *cc, int a) {
  lget(cc, V_BASE); i32c(cc, LUA_TNIL);
  mem(cc, W_I32_STORE, 2, reg(a) + TV_TT);
}

/* ra = the TValue at V_S */
static void copy_from_slot (Compiler *cc, int a) {
  int half;
  for (half = 0; half < 2; half++) {
    lget(cc, V_BASE);
    lget(cc, V_S);
    mem(cc, W_I64_LOAD, 3, half * 8);
    mem(cc, W_I64_STORE, 3, reg(a) + half * 8);
  }
}

/* ra = V_TAB[key]; `table` is a register, or -1 for the environment.
** Like luaV_gettable, a string key that is absent (or nil) is looked up in
** the metatable's __index table, up to LJIT_INDEX_LEVELS tables, and is nil
** without a metatable or __index; anything else (an __index function, a
** deeper chain) goes to the helper. */
static void get_table (Compiler *cc, int a, int table, int key, int self) {
  int strkey = key_kind(cc, key) == KEY_STR;
  int spec;
  block(cc, W_BLOCK, W_VOID);  /* done */
  block(cc, W_BLOCK, W_VOID);  /* slow */
  if (table < 0) load_env(cc); else load_table_reg(cc, table, 0);
  /* (the helper repeats this copy, so it can come first) */
  if (self) copy_tvalue(cc, a + 1, 0, reg(table));
  if (!strkey) {
    lookup(cc, key, 0);
    /* an array slot: nil without a metatable means nil */
    lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_TT);
    op(cc, W_I32_EQZ);
    block(cc, W_IF, W_VOID);
    lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_METATABLE);
    br_if_out(cc, 1);  /* slow: may have __index */
    set_nil(cc, a);
    b_byte(C, W_BR); b_u32(C, 2);  /* done */
    end(cc);
    copy_from_slot(cc, a);
    b_byte(C, W_BR); b_u32(C, 1);
    end(cc);
    helper_exec(cc);
    end(cc);
    return;
  }
  /* A loop (rather than unrolled, which makes much bigger code) over the
  ** chain of tables, V_R counting the levels left. When what follows uses
  ** the result as a number, it is checked to be one (else the interpreter
  ** reads it again: no metamethod was involved) and kept in a local. */
  spec = !self && speculative(cc, cc->pcidx);
  i32c(cc, LJIT_INDEX_LEVELS);
  lset(cc, V_R);
  block(cc, W_LOOP, W_VOID);  /* labels: loop 0, slow 1, done 2 */
  block(cc, W_BLOCK, W_VOID);  /* miss */
  lookup_str(cc, rawtsvalue(&cc->p->k[INDEXK(key)]));
  lget(cc, V_S); op(cc, W_I32_EQZ);
  br_if_out(cc, 0);  /* absent */
  lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_TT);
  op(cc, W_I32_EQZ);
  br_if_out(cc, 0);  /* nil */
  if (spec) {
    lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_TT);
    i32c(cc, LUA_TNUMBER); op(cc, W_I32_NE);
    block(cc, W_IF, W_VOID);
    deopt(cc, cc->pcidx);
    end(cc);
    lget(cc, V_S); mem(cc, W_F64_LOAD, 3, TV_VALUE);
    def_num(cc, a);
  }
  else copy_from_slot(cc, a);
  b_byte(C, W_BR); b_u32(C, 3);  /* done */
  end(cc);
  /* absent: V_TAB = metatable, or the result is nil */
  lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_METATABLE);
  ltee(cc, V_TAB);
  op(cc, W_I32_EQZ);
  block(cc, W_IF, W_VOID);
  if (spec) deopt(cc, cc->pcidx);
  set_nil(cc, a);
  b_byte(C, W_BR); b_u32(C, 3);  /* done */
  end(cc);
  /* V_S = metatable.__index (the metatable's flags only cache absence) */
  lookup_str(cc, cc->indexname);
  lget(cc, V_S); op(cc, W_I32_EQZ);
  block(cc, W_IF, W_TYPE_I32);
  i32c(cc, 1);
  b_byte(C, W_ELSE);
  lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_TT);
  op(cc, W_I32_EQZ);
  end(cc);
  block(cc, W_IF, W_VOID);
  if (spec) deopt(cc, cc->pcidx);
  set_nil(cc, a);  /* no __index */
  b_byte(C, W_BR); b_u32(C, 3);  /* done */
  end(cc);
  /* an __index table is looked in next; anything else is the helper's */
  lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_TT);
  i32c(cc, LUA_TTABLE); op(cc, W_I32_NE);
  br_if_out(cc, 1);  /* slow */
  lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_VALUE);
  lset(cc, V_TAB);
  lget(cc, V_R); i32c(cc, 1); op(cc, W_I32_SUB);
  ltee(cc, V_R);
  br_if_out(cc, 0);  /* next level */
  end(cc);  /* loop */
  b_byte(C, W_BR); b_u32(C, 0);  /* slow: a longer chain */
  end(cc);
  helper_exec(cc);
  end(cc);
}

/* V_TAB[key] = value (RK); `table` is a register, or -1 for the environment. */
static void set_table (Compiler *cc, int table, int key, int value) {
  block(cc, W_BLOCK, W_VOID);  /* done */
  block(cc, W_BLOCK, W_VOID);  /* slow */
  if (table < 0) load_env(cc); else load_table_reg(cc, table, 0);
  if (key_kind(cc, key) == KEY_STR) {
    TString *ks = rawtsvalue(&cc->p->k[INDEXK(key)]);
    block(cc, W_BLOCK, W_VOID);  /* write: labels write 0, slow 1, done 2 */
    lookup_str(cc, ks);
    lget(cc, V_S);
    block(cc, W_IF, W_VOID);
    /* present: a nil value may mean __newindex, unless there is no
    ** metatable */
    lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_TT);
    op(cc, W_I32_EQZ);
    lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_METATABLE);
    i32c(cc, 0); op(cc, W_I32_NE);
    op(cc, W_I32_AND);
    br_if_out(cc, 2);  /* slow */
    b_byte(C, W_BR); b_u32(C, 1);  /* write */
    end(cc);
    /* Absent: like luaH_set's newkey when the key's main position is free
    ** (a table built by a constructor), in a table without a metatable
    ** (else __newindex may apply) that isn't black (else the key needs a
    ** GC barrier). Collisions, a full or empty hash part: the helper. */
    lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_METATABLE);
    br_if_out(cc, 1);
    lget(cc, V_TAB); mem(cc, W_I32_LOAD8_U, 0, T_MARKED);
    i32c(cc, bitmask(BLACKBIT)); op(cc, W_I32_AND);
    br_if_out(cc, 1);
    lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_NODE);
    i32c(cc, (int)(size_t)luaH_dummynode);
    op(cc, W_I32_EQ);
    br_if_out(cc, 1);
    lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_NODE);
    i32c(cc, 1);
    lget(cc, V_TAB); mem(cc, W_I32_LOAD8_U, 0, T_LSIZENODE);
    op(cc, W_I32_SHL);
    i32c(cc, 1); op(cc, W_I32_SUB);
    i32c(cc, (int)ks->tsv.hash); op(cc, W_I32_AND);
    i32c(cc, N_SIZE); op(cc, W_I32_MUL);
    op(cc, W_I32_ADD);
    ltee(cc, V_S);
    mem(cc, W_I32_LOAD, 2, TV_TT);
    br_if_out(cc, 1);  /* taken */
    lget(cc, V_S); i32c(cc, (int)(size_t)ks); mem(cc, W_I32_STORE, 2, N_KEYVAL);
    lget(cc, V_S); i32c(cc, LUA_TSTRING); mem(cc, W_I32_STORE, 2, N_KEYTT);
    end(cc);  /* write (to V_S; the value's barrier is still checked) */
  }
  else {
    lookup(cc, key, 0);
    /* The slot must hold a value already (else __newindex may apply),
    ** except array slots of tables without a metatable. */
    lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_TT);
    op(cc, W_I32_EQZ);
    lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_METATABLE);
    i32c(cc, 0); op(cc, W_I32_NE);
    op(cc, W_I32_AND);
    br_if_out(cc, 0);
  }
  /* Storing a collectable value into a black table needs a GC barrier. */
  if (ISK(value)) {
    if (iscollectable(&cc->p->k[INDEXK(value)])) {
      lget(cc, V_TAB); mem(cc, W_I32_LOAD8_U, 0, T_MARKED);
      i32c(cc, bitmask(BLACKBIT)); op(cc, W_I32_AND);
      br_if_out(cc, 0);
    }
  }
  else {
    lget(cc, V_BASE); mem(cc, W_I32_LOAD, 2, reg(value) + TV_TT);
    i32c(cc, LUA_TSTRING); op(cc, W_I32_GE_U);
    lget(cc, V_TAB); mem(cc, W_I32_LOAD8_U, 0, T_MARKED);
    i32c(cc, bitmask(BLACKBIT)); op(cc, W_I32_AND);
    i32c(cc, 0); op(cc, W_I32_NE);  /* to 0/1 before combining */
    op(cc, W_I32_AND);
    br_if_out(cc, 0);
  }
  {
    int half;
    unsigned int src = ISK(value) ? (unsigned int)(size_t)(cc->p->k + INDEXK(value))
                                  : reg(value);
    for (half = 0; half < 2; half++) {
      lget(cc, V_S);
      if (ISK(value)) i32c(cc, 0); else lget(cc, V_BASE);
      mem(cc, W_I64_LOAD, 3, src + half * 8);
      mem(cc, W_I64_STORE, 3, half * 8);
    }
  }
  /* like luaH_set: the table's metamethod cache is reset */
  lget(cc, V_TAB); i32c(cc, 0); mem(cc, W_I32_STORE8, 0, T_FLAGS);
  b_byte(C, W_BR); b_u32(C, 1);
  end(cc);
  helper_exec(cc);
  end(cc);
}

static int inline_key (Compiler *cc, int rk) {
  return key_kind(cc, rk) != KEY_NONE;
}

/* ------------------------------------------------------------------------
** Speculation
**
** Like V8's optimizing tiers, compiled code assumes what the interpreter has
** seen so far: arithmetic and comparisons whose operands have always been
** numbers (no feedback from the interpreter, see ljit_note, or from an
** earlier deopt says otherwise) check their register operands and, if one
** isn't a number, leave the compiled code at that instruction (LJIT_DEOPT)
** for the interpreter, which runs it the slow way, instead of calling a
** helper. Past the check, and past their results, those registers are known
** numbers: their values are kept in f64 locals (V_DREG) and used from there
** with no more loads or type checks, across instructions and loop
** iterations.
**
** `known[k]` is the set of registers known to be numbers when instruction k
** starts. The Lua stack is always written too (so exits, errors, hooks and
** the GC see the right values, and leaving the compiled code needs nothing
** written back), which keeps the invariant simple: a register in known[k]
** holds a number on the Lua stack and the same number in its local. Every
** way into k must make that true:
**  - instructions inline keep the locals of the registers they write;
**  - after a helper call (which may run any Lua code: a metamethod, a hook
**    using debug.setlocal) the registers known at the next instruction are
**    loaded again and checked (`reestablish`);
**  - entering the function at k (after a call returned through the
**    interpreter, a coroutine resumed, the interpreter's loop got hot) goes
**    through a stub that loads and checks known[k] (compile_body).
** Any choice of known sets is therefore correct; the analysis below picks
** registers that are numbers on every path and are about to be used as
** numbers. A failed check deopts: the interpreter takes over, the site is
** noted, and the function is compiled again later with what was learned
** (ljit_deopt).
** --------------------------------------------------------------------- */

/* A deopt discards the code; after this many, a function is compiled
** without speculation. */
#define LJIT_MAX_DEOPTS 5

/* Notes from ljit_note, in a fixed table rather than allocated per
** function so they don't change the addresses the interpreter's own objects
** get (tostring of tables, the order of pairs over tables with table
** keys): the same whether the JIT is on or not. Losing a note costs a
** deopt at worst. */
#define LJIT_NOTES 4096
#define LJIT_NOTE_PROBES 8
static struct { const Proto *p; int k; } ljit_notes[LJIT_NOTES];

static unsigned int ljit_notehash (const Proto *p, int k) {
  return (((unsigned int)(size_t)p >> 3) * 2654435761u + (unsigned int)k * 40503u) &
         (LJIT_NOTES - 1);
}

static int ljit_noted (const Proto *p, int k) {
  unsigned int h = ljit_notehash(p, k);
  int j;
  for (j = 0; j < LJIT_NOTE_PROBES; j++) {
    unsigned int slot = (h + j) & (LJIT_NOTES - 1);
    if (ljit_notes[slot].p == p && ljit_notes[slot].k == k) return 1;
    if (ljit_notes[slot].p == NULL) return 0;
  }
  return 0;
}

/* Does instruction k speculate that its operands are numbers? */
static int speculative (Compiler *cc, int k) {
  Proto *p = cc->p;
  Instruction i = p->code[k];
  int b = GETARG_B(i), c = GETARG_C(i);
  if (!cc->spec || (p->jitnotes && ljit_noted(p, k))) return 0;
  switch (GET_OPCODE(i)) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_LT: case OP_LE:
      return !(ISK(b) && !isk_number(cc, b)) && !(ISK(c) && !isk_number(cc, c));
    case OP_UNM:
      return 1;
    case OP_GETTABLE: case OP_GETGLOBAL:
      /* a field read, as a number, by what follows */
      if (GET_OPCODE(i) == OP_GETGLOBAL ? GETARG_Bx(i) > MAXINDEXRK
                                        : key_kind(cc, c) != KEY_STR)
        return 0;
      return k + 1 < cc->n && rs_has(&cc->uses[k + 1], GETARG_A(i));
    default:
      return 0;
  }
}

/* Successors of instruction k in the compiled code (not counting the
** dispatcher); for OP_FORLOOP the loop's exit comes first. */
static int successors (Compiler *cc, int k, int *t) {
  Proto *p = cc->p;
  Instruction i = p->code[k];
  int m;
  switch (GET_OPCODE(i)) {
    case OP_JMP: t[0] = k + 1 + GETARG_sBx(i); m = 1; break;
    case OP_EQ: case OP_LT: case OP_LE: case OP_TEST: case OP_TESTSET:
    case OP_TFORLOOP:
      t[0] = k + 2; t[1] = k + 2 + GETARG_sBx(p->code[k + 1]); m = 2; break;
    case OP_LOADBOOL: t[0] = GETARG_C(i) ? k + 2 : k + 1; m = 1; break;
    case OP_FORLOOP: t[0] = k + 1; t[1] = k + 1 + GETARG_sBx(i); m = 2; break;
    case OP_FORPREP: t[0] = k + 1 + GETARG_sBx(i); m = 1; break;
    case OP_RETURN: m = 0; break;
    case OP_CLOSURE: t[0] = k + 1 + p->p[GETARG_Bx(i)]->nups; m = 1; break;
    case OP_SETLIST: t[0] = GETARG_C(i) == 0 ? k + 2 : k + 1; m = 1; break;
    default: t[0] = k + 1; m = 1; break;
  }
  {
    int j, valid = 0;
    for (j = 0; j < m; j++) if (t[j] >= 0 && t[j] < cc->n) t[valid++] = t[j];
    return valid;
  }
}

static int next_insn (Compiler *cc, int k) {
  int t[2];
  return successors(cc, k, t) == 1 ? t[0] : -1;
}

/* Registers used as numbers ahead (backward: from the uses after k to
** the uses at k): s is the set after k on entry, before k on return. */
static void uses_transfer (Compiler *cc, int k, RegSet *s) {
  Instruction i = cc->p->code[k];
  int a = GETARG_A(i), b = GETARG_B(i), c = GETARG_C(i);
  switch (GET_OPCODE(i)) {
    case OP_MOVE:  /* a number moved to a register used as one */
      if (rs_has(s, a)) { rs_del(s, a); rs_add(s, b); }
      break;
    case OP_LOADK: case OP_LOADBOOL: case OP_GETUPVAL: case OP_GETGLOBAL:
    case OP_GETTABLE: case OP_NEWTABLE: case OP_NOT: case OP_LEN:
    case OP_CLOSURE:
      rs_del(s, a);
      break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_POW:
      rs_del(s, a);
      if (!ISK(b)) rs_add(s, b);
      if (!ISK(c)) rs_add(s, c);
      break;
    case OP_UNM:
      rs_del(s, a);
      rs_add(s, b);
      break;
    case OP_EQ: case OP_LT: case OP_LE:
      if (!ISK(b)) rs_add(s, b);
      if (!ISK(c)) rs_add(s, c);
      break;
    case OP_FORLOOP:
      rs_add(s, a); rs_add(s, a + 1); rs_add(s, a + 2);
      break;
    default:
      break;
  }
}

/* Registers known to be numbers (forward): s is the set before k on entry,
** after it (along the edge to successor number `edge`) on return. Writes
** that may not be numbers remove registers; numeric results add them. */
static void known_transfer (Compiler *cc, int k, RegSet *s, int edge,
                            const RegSet *uses) {
  Instruction i = cc->p->code[k];
  int a = GETARG_A(i), b = GETARG_B(i), c = GETARG_C(i);
  int r;
  switch (GET_OPCODE(i)) {
    case OP_MOVE:
      if (rs_has(s, b) && rs_has(uses, b)) rs_add(s, a);
      else rs_del(s, a);
      break;
    case OP_LOADK:
      if (ttisnumber(cc->p->k + GETARG_Bx(i))) rs_add(s, a);
      else rs_del(s, a);
      break;
    case OP_LOADNIL:
      for (r = a; r <= b; r++) rs_del(s, r);
      break;
    case OP_CONCAT:
      for (r = b; r <= c; r++) rs_del(s, r);
      rs_del(s, a);
      break;
    case OP_SELF:
      rs_del(s, a); rs_del(s, a + 1);
      break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_UNM:
      if (speculative(cc, k)) {
        if (!ISK(b)) rs_add(s, b);
        if (GET_OPCODE(i) != OP_UNM && !ISK(c)) rs_add(s, c);
        rs_add(s, a);
      }
      else rs_del(s, a);
      break;
    case OP_LT: case OP_LE:
      if (speculative(cc, k)) {
        if (!ISK(b)) rs_add(s, b);
        if (!ISK(c)) rs_add(s, c);
      }
      break;
    case OP_GETTABLE: case OP_GETGLOBAL:
      if (speculative(cc, k)) rs_add(s, a);
      else rs_del(s, a);
      break;
    case OP_CALL: case OP_TAILCALL: case OP_VARARG: case OP_TFORLOOP:
      rs_delfrom(s, a);
      break;
    case OP_FORPREP:  /* forprep made them numbers (or raised an error) */
      rs_add(s, a); rs_add(s, a + 1); rs_add(s, a + 2);
      break;
    case OP_FORLOOP:  /* written only when the loop goes on */
      if (edge == 1) { rs_add(s, a); rs_add(s, a + 3); }
      break;
    case OP_SETTABLE: case OP_SETGLOBAL: case OP_SETUPVAL: case OP_SETLIST:
    case OP_JMP: case OP_TEST: case OP_EQ: case OP_CLOSE: case OP_RETURN:
      break;
    default:  /* everything else writes register A, at most */
      rs_del(s, a);
      break;
  }
}

/* Registers kept in locals, at most: every helper call is followed by
** reloading the known ones, so more would make big code for functions with
** many numeric locals. */
#define LJIT_MAX_KNOWN 16

/* The LJIT_MAX_KNOWN registers used most as numbers (uses in loops count
** more), the only ones the analysis tracks. */
static void choose_registers (Compiler *cc, RegSet *allowed) {
  int n = cc->n, k, r, m;
  int weight[LJIT_MAXREGS];
  int *inloop = cc->work;  /* (free until the analysis below) */
  memset(weight, 0, sizeof(weight));
  memset(allowed, 0, sizeof(*allowed));
  memset(inloop, 0, (size_t)n * sizeof(int));
  for (k = 0; k < n; k++) {  /* inloop[k]: loops instruction k is in */
    Instruction i = cc->p->code[k];
    if (GET_OPCODE(i) == OP_FORLOOP || (GET_OPCODE(i) == OP_JMP && GETARG_sBx(i) < 0)) {
      int h = k + 1 + GETARG_sBx(i), j;
      for (j = h < 0 ? 0 : h; j <= k; j++) inloop[j]++;
    }
  }
  for (k = 0; k < n; k++) {
    RegSet s;
    int w = 1 + 4 * inloop[k];
    memset(&s, 0, sizeof(s));
    uses_transfer(cc, k, &s);  /* the registers k reads as numbers */
    for (r = 0; r < LJIT_MAXREGS; r++)
      if (rs_has(&s, r)) weight[r] += w;
  }
  for (m = 0; m < LJIT_MAX_KNOWN; m++) {
    int best = -1;
    for (r = 0; r < LJIT_MAXREGS; r++)
      if (weight[r] > 0 && (best < 0 || weight[r] > weight[best])) best = r;
    if (best < 0) break;
    rs_add(allowed, best);
    weight[best] = 0;
  }
}

/* Fills cc->known. */
static void analyze (Compiler *cc) {
  int n = cc->n, k, j, changed;
  int t[2], ns;
  RegSet *uses = cc->uses;
  char *seen = cc->seen, *queued = cc->queued;
  int *work = cc->work;
  int nwork = 0;
  if (cc->spec) {
    RegSet allowed;
    choose_registers(cc, &allowed);
    /* registers used as numbers ahead, to the fixpoint */
    do {
      changed = 0;
      for (k = n - 1; k >= 0; k--) {
        RegSet s;
        memset(&s, 0, sizeof(s));
        ns = successors(cc, k, t);
        for (j = 0; j < ns; j++) rs_or(&s, &uses[t[j]]);
        uses_transfer(cc, k, &s);
        rs_and(&s, &allowed);
        if (rs_or(&uses[k], &s)) changed = 1;
      }
    } while (changed);
    /* registers known to be numbers: nothing at the start, the
    ** intersection over every way in, to the fixpoint */
    seen[0] = 1;
    work[nwork++] = 0;
    queued[0] = 1;
    while (nwork > 0) {
      k = work[--nwork];
      queued[k] = 0;
      ns = successors(cc, k, t);
      for (j = 0; j < ns; j++) {
        RegSet s = cc->known[k];
        int changed_t;
        known_transfer(cc, k, &s, ns == 2 && GET_OPCODE(cc->p->code[k]) == OP_FORLOOP ? j : 1, &uses[k]);
        if (!seen[t[j]]) { seen[t[j]] = 1; cc->known[t[j]] = s; changed_t = 1; }
        else changed_t = rs_and(&cc->known[t[j]], &s);
        if (changed_t && !queued[t[j]]) { queued[t[j]] = 1; work[nwork++] = t[j]; }
      }
    }
    /* keep the ones about to be used as numbers */
    for (k = 0; k < n; k++) {
      if (!seen[k]) memset(&cc->known[k], 0, sizeof(RegSet));
      else rs_and(&cc->known[k], &uses[k]);
    }
  }
}

static void instruction (Compiler *cc) {
  Proto *p = cc->p;
  int pcidx = cc->pcidx;
  Instruction i = p->code[pcidx];
  int a = GETARG_A(i);
  switch (GET_OPCODE(i)) {
    case OP_MOVE: {
      int b = GETARG_B(i);
      copy_tvalue(cc, a, 0, reg(b));
      if (known(cc, b)) {
        lget(cc, V_DREG(b)); lset(cc, V_DREG(a));
        rs_add(&cc->cur, a);
      }
      break;
    }
    case OP_LOADK:
      copy_tvalue(cc, a, 1, (unsigned int)(size_t)(p->k + GETARG_Bx(i)));
      if (cc->spec && ttisnumber(p->k + GETARG_Bx(i))) {
        f64c(cc, nvalue(p->k + GETARG_Bx(i))); lset(cc, V_DREG(a));
        rs_add(&cc->cur, a);
      }
      break;
    case OP_LOADBOOL:
      lget(cc, V_BASE); i32c(cc, GETARG_B(i) ? 1 : 0);
      mem(cc, W_I32_STORE, 2, reg(a) + TV_VALUE);
      lget(cc, V_BASE); i32c(cc, LUA_TBOOLEAN);
      mem(cc, W_I32_STORE, 2, reg(a) + TV_TT);
      if (GETARG_C(i)) jump(cc, pcidx + 2);
      break;
    case OP_LOADNIL: {
      int r;
      for (r = a; r <= GETARG_B(i); r++) {
        lget(cc, V_BASE); i32c(cc, LUA_TNIL);
        mem(cc, W_I32_STORE, 2, reg(r) + TV_TT);
      }
      break;
    }
    case OP_ADD: arith(cc, i, W_F64_ADD); break;
    case OP_SUB: arith(cc, i, W_F64_SUB); break;
    case OP_MUL: arith(cc, i, W_F64_MUL); break;
    case OP_DIV: arith(cc, i, W_F64_DIV); break;
    case OP_MOD: arith(cc, i, W_F64_FLOOR); break;
    case OP_UNM: {
      int b = GETARG_B(i);
      if (speculative(cc, pcidx)) {
        guard(cc, b, pcidx);
        load_num(cc, b);
        op(cc, W_F64_NEG);
        def_num(cc, a);
        break;
      }
      block(cc, W_BLOCK, W_VOID);
      block(cc, W_BLOCK, W_VOID);
      if (!known(cc, b)) {
        not_number(cc, b);
        b_byte(C, W_BR_IF); b_u32(C, 0);
      }
      load_num(cc, b);
      op(cc, W_F64_NEG);
      lset(cc, V_D1);
      store_num(cc, a, V_D1);
      rs_del(&cc->cur, a);
      b_byte(C, W_BR); b_u32(C, 1);
      end(cc);
      helper_exec(cc);
      end(cc);
      break;
    }
    case OP_GETUPVAL: {
      /* ra = cl->upvals[B]->v */
      int half;
      lget(cc, V_L);
      mem(cc, W_I32_LOAD, 2, L_CI);
      mem(cc, W_I32_LOAD, 2, CI_FUNC);
      mem(cc, W_I32_LOAD, 2, TV_VALUE);
      mem(cc, W_I32_LOAD, 2, LCL_UPVALS + GETARG_B(i) * (int)sizeof(UpVal *));
      mem(cc, W_I32_LOAD, 2, UV_V);
      lset(cc, V_S);
      for (half = 0; half < 2; half++) {
        lget(cc, V_BASE);
        lget(cc, V_S);
        mem(cc, W_I64_LOAD, 3, half * 8);
        mem(cc, W_I64_STORE, 3, reg(a) + half * 8);
      }
      break;
    }
    /* The inline paths take keys as RK operands, which can only refer to
    ** the first MAXINDEXRK+1 constants. */
    case OP_GETGLOBAL:
      if (GETARG_Bx(i) <= MAXINDEXRK)
        get_table(cc, a, -1, RKASK(GETARG_Bx(i)), 0);
      else {
        helper_exec(cc);
      }
      break;
    case OP_SETGLOBAL:
      if (GETARG_Bx(i) <= MAXINDEXRK)
        set_table(cc, -1, RKASK(GETARG_Bx(i)), a);
      else {
        helper_exec(cc);
      }
      break;
    case OP_GETTABLE:
      if (inline_key(cc, GETARG_C(i)))
        get_table(cc, a, GETARG_B(i), GETARG_C(i), 0);
      else {
        helper_exec(cc);
      }
      break;
    case OP_SELF:
      if (inline_key(cc, GETARG_C(i)))
        get_table(cc, a, GETARG_B(i), GETARG_C(i), 1);
      else {
        helper_exec(cc);
      }
      break;
    case OP_SETTABLE:
      if (inline_key(cc, GETARG_B(i)))
        set_table(cc, a, GETARG_B(i), GETARG_C(i));
      else {
        helper_exec(cc);
      }
      break;
    case OP_NOT:
      is_false(cc, GETARG_B(i));
      lset(cc, V_R);
      lget(cc, V_BASE); lget(cc, V_R);
      mem(cc, W_I32_STORE, 2, reg(a) + TV_VALUE);
      lget(cc, V_BASE); i32c(cc, LUA_TBOOLEAN);
      mem(cc, W_I32_STORE, 2, reg(a) + TV_TT);
      break;
    case OP_JMP:
      jump(cc, pcidx + 1 + GETARG_sBx(i));
      break;
    case OP_EQ: case OP_LT: case OP_LE:
      compare(cc, i, GET_OPCODE(i));
      break;
    case OP_TEST: {
      int next = pcidx + 1;
      is_false(cc, a);
      i32c(cc, GETARG_C(i));
      op(cc, W_I32_NE);
      block(cc, W_IF, W_VOID);
      jump(cc, next + 1 + GETARG_sBx(p->code[next]));
      end(cc);
      jump(cc, pcidx + 2);
      break;
    }
    case OP_TESTSET: {
      int next = pcidx + 1;
      int b = GETARG_B(i);
      is_false(cc, b);
      i32c(cc, GETARG_C(i));
      op(cc, W_I32_NE);
      block(cc, W_IF, W_VOID);
      copy_tvalue(cc, a, 0, reg(b));
      jump(cc, next + 1 + GETARG_sBx(p->code[next]));
      end(cc);
      jump(cc, pcidx + 2);
      break;
    }
    case OP_CALL:
      call_helper(cc, H_CALL);
      return_if_nonzero(cc);
      reload_base(cc);
      reestablish(cc, pcidx + 1);
      break;
    case OP_TAILCALL:
      call_helper(cc, H_TAILCALL);
      return_if_nonzero(cc);
      reload_base(cc);
      reestablish(cc, pcidx + 1);
      break;
    case OP_RETURN: {
      int nres = GETARG_B(i) - 1;
      if (nres >= 0 && nres <= 8) {
        /* ljit_h_return inline for its usual case: no return hook, no
        ** upvalue open in this frame, and the caller wants nres results. */
        int j, half;
        block(cc, W_BLOCK, W_VOID);  /* slow */
        lget(cc, V_L); mem(cc, W_I32_LOAD8_U, 0, L_HOOKMASK);
        i32c(cc, LUA_MASKRET); op(cc, W_I32_AND);
        br_if_out(cc, 0);
        lget(cc, V_L); mem(cc, W_I32_LOAD, 2, L_OPENUPVAL);
        ltee(cc, V_S);
        block(cc, W_IF, W_VOID);
        lget(cc, V_S); mem(cc, W_I32_LOAD, 2, UV_V);
        lget(cc, V_BASE);
        op(cc, W_I32_GE_U);
        br_if_out(cc, 1);
        end(cc);
        lget(cc, V_L); mem(cc, W_I32_LOAD, 2, L_CI);
        ltee(cc, V_T);
        mem(cc, W_I32_LOAD, 2, CI_NRESULTS);
        i32c(cc, nres); op(cc, W_I32_NE);
        br_if_out(cc, 0);
        /* results to ci->func and up (below them: copying up is safe) */
        lget(cc, V_T); mem(cc, W_I32_LOAD, 2, CI_FUNC);
        lset(cc, V_S);
        for (j = 0; j < nres; j++)
          for (half = 0; half < 2; half++) {
            lget(cc, V_S);
            lget(cc, V_BASE);
            mem(cc, W_I64_LOAD, 3, reg(a + j) + half * 8);
            mem(cc, W_I64_STORE, 3, (unsigned int)(j * TV_SIZE + half * 8));
          }
        lget(cc, V_L);
        lget(cc, V_S); i32c(cc, nres * TV_SIZE); op(cc, W_I32_ADD);
        mem(cc, W_I32_STORE, 2, L_TOP);
        lget(cc, V_L);
        lget(cc, V_T); i32c(cc, CI_SIZE); op(cc, W_I32_SUB);
        ltee(cc, V_T);
        mem(cc, W_I32_STORE, 2, L_CI);
        lget(cc, V_L);
        lget(cc, V_T); mem(cc, W_I32_LOAD, 2, CI_BASE);
        mem(cc, W_I32_STORE, 2, L_BASE);
        lget(cc, V_L);
        lget(cc, V_T); mem(cc, W_I32_LOAD, 2, CI_SAVEDPC);
        mem(cc, W_I32_STORE, 2, L_SAVEDPC);
        i32c(cc, LJIT_RET1);
        op(cc, W_RETURN);
        end(cc);
      }
      call_helper(cc, H_RETURN);
      op(cc, W_RETURN);
      break;
    }
    case OP_FORLOOP: {
      /* idx = ra + step; continue if (0 < step ? idx <= limit : limit <= idx)
      ** (like the interpreter, without checking types: forprep did) */
      RegSet saved;
      load_num(cc, a);
      load_num(cc, a + 2);
      ltee(cc, V_D2);
      op(cc, W_F64_ADD);
      lset(cc, V_D1);
      f64c(cc, 0);
      lget(cc, V_D2);
      op(cc, W_F64_LT);
      block(cc, W_IF, W_TYPE_I32);
      lget(cc, V_D1);
      load_num(cc, a + 1);
      op(cc, W_F64_LE);
      b_byte(C, W_ELSE);
      load_num(cc, a + 1);
      lget(cc, V_D1);
      op(cc, W_F64_LE);
      end(cc);
      block(cc, W_IF, W_VOID);
      saved = cc->cur;
      lget(cc, V_D1);
      def_num(cc, a);
      lget(cc, V_D1);
      def_num(cc, a + 3);
      jump(cc, pcidx + 1 + GETARG_sBx(i));
      cc->cur = saved;
      end(cc);
      break;
    }
    case OP_FORPREP: {
      int target = pcidx + 1 + GETARG_sBx(i);
      call_helper(cc, H_FORPREP);
      reload_base(cc);
      reestablish(cc, target);
      jump(cc, target);
      break;
    }
    case OP_TFORLOOP: {
      int next = pcidx + 1;
      int target = next + 1 + GETARG_sBx(p->code[next]);
      call_helper(cc, H_TFORLOOP);
      lset(cc, V_R);
      reload_base(cc);
      lget(cc, V_R);
      block(cc, W_IF, W_VOID);
      reestablish(cc, target);
      jump(cc, target);
      end(cc);
      reestablish(cc, pcidx + 2);
      jump(cc, pcidx + 2);
      break;
    }
    case OP_CLOSURE: {
      int nup = p->p[GETARG_Bx(i)]->nups;
      helper_exec(cc);
      if (nup > 0) jump(cc, pcidx + 1 + nup);
      break;
    }
    case OP_SETLIST:
      helper_exec(cc);
      if (GETARG_C(i) == 0) jump(cc, pcidx + 2);
      break;
    default:  /* everything else goes through the generic helper */
      helper_exec(cc);
      break;
  }
}

/* Largest function compiled: each instruction is a nested block. */
#define LJIT_MAX_CODE 3000

/* Marks in cc->queued the instructions where the function is often entered
** from outside: 0, after a call (that returned through luaV_execute, or a
** coroutine resumed), and loop heads (the interpreter's loop got hot). */
static void entry_points (Compiler *cc) {
  int k;
  char *entry = cc->queued;
  memset(entry, 0, (size_t)cc->n);
  entry[0] = 1;
  for (k = 0; k < cc->n; k++) {
    Instruction i = cc->p->code[k];
    if (GET_OPCODE(i) == OP_CALL && k + 1 < cc->n) entry[k + 1] = 1;
    if (GET_OPCODE(i) == OP_JMP || GET_OPCODE(i) == OP_FORLOOP) {
      int t = k + 1 + GETARG_sBx(i);
      if (t >= 0 && t <= k) entry[t] = 1;
    }
  }
}

/* Finds the innermost loops that can be wasm loops: a backward jump from j
** to h, with no other backward jump in between (but to h), from which
** nothing jumps back into (h, j] from after j. Jumps in from before h go
** through the loop's selector. */
static void find_loops (Compiler *cc) {
  int n = cc->n, j, k, s, ns, t[2];
  for (k = 0; k < n; k++) cc->head[k] = cc->loopend[k] = -1;
  for (j = 0; j < n; j++) {
    Instruction i = cc->p->code[j];
    int h, ok = 1;
    if (GET_OPCODE(i) != OP_FORLOOP && !(GET_OPCODE(i) == OP_JMP && GETARG_sBx(i) < 0))
      continue;
    h = j + 1 + GETARG_sBx(i);
    if (h < 0 || h > j) continue;
    for (k = h; k <= j && ok; k++) {
      if (cc->head[k] >= 0 || cc->loopend[k] >= 0) ok = 0;
      ns = successors(cc, k, t);
      for (s = 0; s < ns; s++)
        if (t[s] < h || (t[s] <= k && t[s] != h)) ok = 0;
    }
    for (k = j + 1; k < n && ok; k++) {
      ns = successors(cc, k, t);
      for (s = 0; s < ns; s++)
        if (t[s] > h && t[s] <= j) ok = 0;
    }
    if (!ok) continue;
    cc->loopend[h] = j;
    for (k = h + 1; k <= j; k++) cc->head[k] = h;
  }
  cc->topcount[0] = 0;
  for (k = 0; k < n; k++)
    cc->topcount[k + 1] = cc->topcount[k] + (cc->head[k] < 0);
}

static int compile_body (Compiler *cc) {
  int k, j;
  int n = cc->n;
  int nstubs = 0, ntop;
  int *stub = cc->stub;
  find_loops(cc);
  entry_points(cc);
  ntop = cc->topcount[n];
  for (k = 0; k < n; k++) {
    stub[k] = -1;
    /* stubs check known registers, and go into a loop through its head */
    if (cc->queued[k] && (!rs_empty(&cc->known[k]) || cc->head[k] >= 0))
      stub[k] = nstubs++;
  }
  /* locals: base, idx, t, r, tab, s, e (i32); d1, d2 and one per register (f64) */
  b_u32(C, 2);
  b_u32(C, NUM_I32_LOCALS); b_byte(C, W_TYPE_I32);
  b_u32(C, 2 + cc->p->maxstacksize); b_byte(C, W_TYPE_F64);

  reload_base(cc);
  /* idx = (L->savedpc - code) / sizeof(Instruction) */
  lget(cc, V_L);
  mem(cc, W_I32_LOAD, 2, L_SAVEDPC);
  i32c(cc, (int)pcaddr(cc, 0));
  op(cc, W_I32_SUB);
  i32c(cc, 2);
  op(cc, W_I32_SHR_U);
  lset(cc, V_IDX);

  /* Layout:
  **   block $exit
  **     loop $dispatch
  **       block (one per instruction outside loops, and per loop head:
  **              the block of k ends where k's code starts)
  **         block (nstubs: stub s's code follows its block)
  **           block $deopt_entry
  **             br_table on idx
  **           end: entering where registers are known, with no stub
  **         stubs: load and check known[k], then on to k's code
  **       code of each instruction; for an innermost loop h..j:
  **         loop $l
  **           block (one per instruction h+1..j)
  **             block $selector: br_table on V_E if set
  **             code of h
  **           code of h+1 .. j (j's back edge: br $l)
  **   deopt: L->savedpc = code + idx, return LJIT_DEOPT
  ** idx 0..n-1 enters k through its stub if it has one; n+k enters k
  ** directly (a backward jump, from where known[k] is known). Going into a
  ** loop past its head sets V_E to the instruction (+1) and goes to the
  ** head's block, whose selector goes on to it. */
  b_byte(C, W_BLOCK); b_byte(C, W_VOID);
  b_byte(C, W_LOOP); b_byte(C, W_VOID);
  for (k = 0; k < ntop + nstubs + 1; k++) {
    b_byte(C, W_BLOCK); b_byte(C, W_VOID);
  }
  lget(cc, V_IDX);
  b_byte(C, W_BR_TABLE);
  b_u32(C, 2 * n);
  for (k = 0; k < n; k++) {
    if (stub[k] >= 0) b_u32(C, 1 + stub[k]);
    else if (rs_empty(&cc->known[k]) && cc->head[k] < 0)
      b_u32(C, 1 + nstubs + cc->topcount[k]);
    else b_u32(C, 0);
  }
  for (k = 0; k < n; k++)
    b_u32(C, cc->head[k] < 0 ? 1 + nstubs + cc->topcount[k] : 0);
  b_u32(C, 0);
  b_byte(C, W_END);  /* $deopt_entry: leave at idx */
  b_byte(C, W_BR); b_u32(C, (unsigned int)(nstubs + ntop + 1));
  for (j = 0; j < nstubs; j++) {
    int h;
    b_byte(C, W_END);
    for (k = 0; k < n && stub[k] != j; k++) ;
    cc->pcidx = k;
    cc->depth = 0;
    cc->exitlabel = (nstubs - 1 - j) + ntop + 1;
    memset(&cc->cur, 0, sizeof(cc->cur));
    guard_all(cc, &cc->known[k], k);
    h = cc->head[k];
    if (h >= 0) {
      i32c(cc, k + 1);
      lset(cc, V_E);
    }
    else h = k;
    b_byte(C, W_BR); b_u32(C, (unsigned int)((nstubs - 1 - j) + cc->topcount[h]));
  }
  for (k = 0; k < n; k++) {
    b_byte(C, W_END);  /* code for instruction k follows its block */
    if (cc->loopend[k] >= 0) {
      int last = cc->loopend[k], m;
      b_byte(C, W_LOOP); b_byte(C, W_VOID);
      for (m = last; m > k; m--) {
        b_byte(C, W_BLOCK); b_byte(C, W_VOID);
      }
      if (last > k) {
        /* the selector: if V_E is set, V_E = 0 and on to instruction V_E-1 */
        b_byte(C, W_BLOCK); b_byte(C, W_VOID);
        lget(cc, V_E);
        op(cc, W_I32_EQZ);
        b_byte(C, W_BR_IF); b_u32(C, 0);
        lget(cc, V_E);
        i32c(cc, k + 1);
        op(cc, W_I32_SUB);
        i32c(cc, 0);
        lset(cc, V_E);
        b_byte(C, W_BR_TABLE);
        b_u32(C, (unsigned int)(last - k + 1));
        for (m = 0; m <= last - k; m++) b_u32(C, (unsigned int)m);
        b_u32(C, 0);
        b_byte(C, W_END);
      }
    }
    cc->pcidx = k;
    cc->depth = 0;
    cc->exitlabel = top_label(cc, n) + 1;
    cc->cur = cc->known[k];
    instruction(cc);
    if ((cc->head[k] >= 0 && cc->loopend[cc->head[k]] == k) || cc->loopend[k] == k)
      b_byte(C, W_END);  /* the loop */
  }
  b_byte(C, W_END);  /* $dispatch */
  op(cc, W_UNREACHABLE);
  b_byte(C, W_END);  /* $exit */
  lget(cc, V_L);
  lget(cc, V_IDX);
  i32c(cc, 2);
  op(cc, W_I32_SHL);
  i32c(cc, (int)pcaddr(cc, 0));
  op(cc, W_I32_ADD);
  mem(cc, W_I32_STORE, 2, L_SAVEDPC);
  i32c(cc, LJIT_DEOPT);
  b_byte(C, W_END);  /* function */
  return !C->oom;
}

static void module_header (Buf *m) {
  static const unsigned char magic[] = { 0, 'a', 's', 'm', 1, 0, 0, 0 };
  Buf s = { 0 };
  int h;
  b_bytes(m, magic, sizeof(magic));

  /* types: 0 = (i32) -> i32, 1 = (i32 i32) -> (), 2 = (i32 i32) -> i32 */
  b_u32(&s, 3);
  b_byte(&s, 0x60); b_u32(&s, 1); b_byte(&s, W_TYPE_I32); b_u32(&s, 1); b_byte(&s, W_TYPE_I32);
  b_byte(&s, 0x60); b_u32(&s, 2); b_byte(&s, W_TYPE_I32); b_byte(&s, W_TYPE_I32); b_u32(&s, 0);
  b_byte(&s, 0x60); b_u32(&s, 2); b_byte(&s, W_TYPE_I32); b_byte(&s, W_TYPE_I32);
  b_u32(&s, 1); b_byte(&s, W_TYPE_I32);
  b_section(m, 1, &s);

  /* imports: helpers (as functions h0..), then memory */
  s.n = 0;
  b_u32(&s, NUM_HELPERS + 1);
  for (h = 0; h < NUM_HELPERS; h++) {
    char name[8];
    int type = (h == H_EXEC || h == H_FORPREP) ? 1 : 2;
    snprintf(name, sizeof(name), "h%d", h);
    b_str(&s, "e");
    b_str(&s, name);
    b_byte(&s, 0x00);
    b_u32(&s, type);
  }
  b_str(&s, "e");
  b_str(&s, "m");
  b_byte(&s, 0x02);
  b_byte(&s, 0x00);  /* limits: min only */
  b_u32(&s, 1);
  b_section(m, 2, &s);

  /* one function of type 0, exported as "f" */
  s.n = 0;
  b_u32(&s, 1);
  b_u32(&s, 0);
  b_section(m, 3, &s);

  s.n = 0;
  b_u32(&s, 1);
  b_str(&s, "f");
  b_byte(&s, 0x00);
  b_u32(&s, NUM_HELPERS);
  b_section(m, 7, &s);

  free(s.p);
}

int ljit_compile (lua_State *L, Proto *p) {
  Compiler cc;
  Buf m = { 0 }, s = { 0 };
  int index = 0;
  char *scratch = NULL;
  double t0 = emscripten_get_now();
  if (p->jit != 0) return p->jit > 0;
  p->jit = -1;  /* unless we succeed */
  if (p->sizecode > LJIT_MAX_CODE || p->sizecode == 0) {
    ljit_nfailed++;
    return 0;
  }

  memset(&cc, 0, sizeof(cc));
  cc.p = p;
  cc.indexname = G(L)->tmname[TM_INDEX];
  cc.n = p->sizecode;
  cc.spec = p->jitdeopts < LJIT_MAX_DEOPTS && p->maxstacksize <= LJIT_MAXREGS;
  /* The analysis's memory, in one block. Compiling should leave the heap
  ** as it found it, so the interpreter's objects get the same addresses
  ** (tostring of a table, the order of pairs over table keys) with the JIT
  ** on or off: like the code buffers, it is big enough for malloc to take
  ** it from the top of the heap, not from the free lists of small blocks
  ** the interpreter's objects come from, and the top is whole again once
  ** everything is freed. */
  {
    size_t size = (size_t)cc.n * (2 * sizeof(RegSet) + 5 * sizeof(int) + 2) + sizeof(int);
    scratch = (char *)calloc(size < 4096 ? 4096 : size, 1);
  }
  if (scratch == NULL) goto done;
  cc.known = (RegSet *)scratch;
  cc.uses = cc.known + cc.n;
  cc.work = (int *)(cc.uses + cc.n);
  cc.stub = cc.work + cc.n;
  cc.head = cc.stub + cc.n;
  cc.loopend = cc.head + cc.n;
  cc.topcount = cc.loopend + cc.n;
  cc.seen = (char *)(cc.topcount + cc.n + 1);
  cc.queued = cc.seen + cc.n;
  analyze(&cc);
  if (!compile_body(&cc)) goto done;

  module_header(&m);
  b_u32(&s, 1);
  b_u32(&s, (unsigned int)cc.code.n);
  b_bytes(&s, cc.code.p, cc.code.n);
  b_section(&m, 10, &s);
  if (m.oom || s.oom) goto done;

  index = ljit_instantiate(m.p, (int)m.n);
  if (index > 0) p->jit = index;
  ljit_nbytes += m.n;

done:
  if (p->jit > 0) ljit_ncompiled++;
  else ljit_nfailed++;
  free(s.p);
  free(m.p);
  free(cc.code.p);
  free(scratch);
  ljit_ms += emscripten_get_now() - t0;
  return p->jit > 0;
}

void ljit_free (Proto *p) {
  if (p->jit > 0) ljit_release(p->jit);
  p->jit = 0;
  if (p->jitnotes) {
    int j;
    for (j = 0; j < LJIT_NOTES; j++)
      if (ljit_notes[j].p == p) ljit_notes[j].p = NULL;
    p->jitnotes = 0;
  }
}

/* The interpreter or a deopt saw instruction k go the slow way (operands
** that aren't numbers): it won't be compiled speculatively again. */
void ljit_note (Proto *p, int k) {
  unsigned int h = ljit_notehash(p, k);
  int j;
  if (!ljit_enabled) return;
  p->jitnotes = 1;
  for (j = 0; j < LJIT_NOTE_PROBES; j++) {
    unsigned int slot = (h + j) & (LJIT_NOTES - 1);
    if (ljit_notes[slot].p == NULL || (ljit_notes[slot].p == p && ljit_notes[slot].k == k)) {
      ljit_notes[slot].p = p;
      ljit_notes[slot].k = k;
      return;
    }
  }
  ljit_notes[h].p = p;  /* full: forget an older note */
  ljit_notes[h].k = k;
}

/* Compiled code returned LJIT_DEOPT for the frame L->ci, at L->savedpc:
** notes the instruction, and throws the code away (none of it is running:
** a deopt returns all the way to luaV_execute) for the interpreter to run
** the function, and learn more, until it is hot again. */
void ljit_deopt (lua_State *L) {
  Proto *p = clvalue(L->ci->func)->l.p;
  ljit_ndeopts++;
  ljit_note(p, ljit_blame);
  p->jitdeopts++;
  if (p->jit > 0) {
    ljit_release(p->jit);
    p->jit = 0;
    p->jitcount = 0;
  }
}

#else  /* !LJIT_ENABLED */

int ljit_compile (lua_State *L, Proto *p) {
  (void)L;
  p->jit = -1;
  return 0;
}

void ljit_free (Proto *p) {
  p->jit = 0;
}

void ljit_note (Proto *p, int k) {
  (void)p; (void)k;
}

void ljit_deopt (lua_State *L) {
  (void)L;
}

#endif
