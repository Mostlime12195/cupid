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
static int ljit_ncompiled = 0, ljit_nfailed = 0;
static double ljit_nbytes = 0, ljit_ms = 0;

EMSCRIPTEN_KEEPALIVE double ljit_stats (int what) {
  switch (what) {
    case 0: return ljit_ncompiled;
    case 1: return ljit_nfailed;
    case 2: return ljit_nbytes;
    default: return ljit_ms;
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
EMSCRIPTEN_KEEPALIVE int ljit_h_call (lua_State *L, const Instruction *pc) {
  LJ_DECODE
  int b = GETARG_B(i);
  int nresults = GETARG_C(i) - 1;
  if (b != 0) L->top = ra+b;
  if (ttisfunction(ra) && !clvalue(ra)->c.isC) {
    /* luaD_precall's Lua case, for the common calls that don't need to
    ** grow the stack or CallInfo array, adjust varargs or call a hook. */
    Proto *np = clvalue(ra)->l.p;
    CallInfo *nci;
    StkId nbase = ra + 1, st;
    if (!np->is_vararg && !(L->hookmask & LUA_MASKCALL) &&
        L->ci != L->end_ci &&
        (char *)L->stack_last - (char *)L->top > np->maxstacksize * (int)sizeof(TValue)) {
      L->ci->savedpc = L->savedpc;
      if (L->top > nbase + np->numparams)
        L->top = nbase + np->numparams;
      nci = ++L->ci;
      nci->func = ra;
      L->base = nci->base = nbase;
      nci->top = nbase + np->maxstacksize;
      L->savedpc = np->code;
      nci->tailcalls = 0;
      nci->nresults = nresults;
      for (st = L->top; st < nci->top; st++)
        setnilvalue(st);
      L->top = nci->top;
      return ljit_enter(L);
    }
  }
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
  switch (luaD_precall(L, ra, LUA_MULTRET)) {
    case PCRLUA: {
      CallInfo *ci = L->ci - 1;
      int aux;
      StkId func = ci->func;
      StkId pfunc = (ci+1)->func;
      if (L->openupval) luaF_close(L, ci->base);
      L->base = ci->base = ci->func + ((ci+1)->base - pfunc);
      for (aux = 0; pfunc+aux < L->top; aux++)
        setobjs2s(L, func+aux, pfunc+aux);
      ci->top = L->top = func+aux;
      ci->savedpc = L->savedpc;
      ci->tailcalls++;
      L->ci--;
      return LJIT_TAILCALL;
    }
    case PCRC: return 0;
    default: return LJIT_YIELD;
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

/* Locals of the compiled function. Param 0 is L. */
enum { V_L = 0, V_BASE, V_IDX, V_T, V_R, V_TAB, V_S, V_D1, V_D2 };
#define NUM_I32_LOCALS 6  /* V_BASE .. V_S */

/* Offsets in TValue / lua_State used by the generated code. */
#define TV_SIZE   ((int)sizeof(TValue))
#define TV_VALUE  ((int)offsetof(TValue, value))
#define TV_TT     ((int)offsetof(TValue, tt))
#define L_BASE    ((int)offsetof(lua_State, base))
#define L_SAVEDPC ((int)offsetof(lua_State, savedpc))
#define L_HOOKMASK ((int)offsetof(lua_State, hookmask))
#define L_CI      ((int)offsetof(lua_State, ci))
#define CI_FUNC   ((int)offsetof(CallInfo, func))
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

typedef struct Compiler {
  Buf code;
  Proto *p;
  int n;      /* number of instructions */
  int pcidx;  /* instruction being compiled */
  int depth;  /* blocks opened inside the current instruction */
  TString *indexname;  /* "__index" */
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

/* Pushes the f64 value of RK operand rk (must be a register or a numeric
** constant). */
static void load_num (Compiler *cc, int rk) {
  if (ISK(rk))
    f64c(cc, nvalue(&cc->p->k[INDEXK(rk)]));
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

/* Pushes 1 if any of the RK operands isn't a number (constants are known). */
static int push_not_numbers (Compiler *cc, int b, int c) {
  int pushed = 0;
  if (!ISK(b)) { not_number(cc, b); pushed++; }
  if (!ISK(c)) {
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

/* Continues execution at instruction t. */
static void jump (Compiler *cc, int t) {
  int k = cc->pcidx;
  if (t > k) {
    b_byte(C, W_BR);
    b_u32(C, (unsigned int)(cc->depth + (t - (k + 1))));
  }
  else {
    /* Backward jump: hand over to the interpreter if a hook was set (it
    ** may have been set by a function called in this loop), else
    ** dispatch. */
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
    i32c(cc, t);
    lset(cc, V_IDX);
    b_byte(C, W_BR);
    b_u32(C, (unsigned int)(cc->depth + (cc->n - 1 - k)));
  }
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

static void arith (Compiler *cc, Instruction i, int wop) {
  int a = GETARG_A(i), b = GETARG_B(i), c = GETARG_C(i);
  if ((ISK(b) && !isk_number(cc, b)) || (ISK(c) && !isk_number(cc, c))) {
    call_helper(cc, H_EXEC);
    reload_base(cc);
    return;
  }
  block(cc, W_BLOCK, W_VOID);  /* done */
  block(cc, W_BLOCK, W_VOID);  /* slow */
  if (push_not_numbers(cc, b, c)) {
    b_byte(C, W_BR_IF); b_u32(C, 0);
  }
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
  lset(cc, V_D1);
  store_num(cc, a, V_D1);
  b_byte(C, W_BR); b_u32(C, 1);
  end(cc);
  call_helper(cc, H_EXEC);
  reload_base(cc);
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
  if (o == OP_EQ && compare_const(cc, b, c)) {
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
    op(cc, o == OP_EQ ? W_F64_EQ : o == OP_LT ? W_F64_LT : W_F64_LE);
    lset(cc, V_R);
    b_byte(C, W_BR); b_u32(C, 1);
    end(cc);
  }
  call_helper(cc, H_COMPARE);
  lset(cc, V_R);
  reload_base(cc);
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
    if (!ISK(rk)) {
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
  int level;
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
    call_helper(cc, H_EXEC);
    reload_base(cc);
    end(cc);
    return;
  }
  for (level = 0; level < LJIT_INDEX_LEVELS; level++) {
    /* V_TAB: the table to look in; depth: done is 2 blocks out of miss */
    block(cc, W_BLOCK, W_VOID);  /* miss */
    lookup_str(cc, rawtsvalue(&cc->p->k[INDEXK(key)]));
    lget(cc, V_S); op(cc, W_I32_EQZ);
    br_if_out(cc, 0);  /* absent */
    lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_TT);
    op(cc, W_I32_EQZ);
    br_if_out(cc, 0);  /* nil */
    copy_from_slot(cc, a);
    b_byte(C, W_BR); b_u32(C, 2);  /* done */
    end(cc);
    /* absent: V_TAB = metatable, or the result is nil */
    lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_METATABLE);
    ltee(cc, V_TAB);
    op(cc, W_I32_EQZ);
    block(cc, W_IF, W_VOID);
    set_nil(cc, a);
    b_byte(C, W_BR); b_u32(C, 2);  /* done */
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
    set_nil(cc, a);  /* no __index */
    b_byte(C, W_BR); b_u32(C, 2);  /* done */
    end(cc);
    /* an __index table is looked in next; anything else is the helper's */
    lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_TT);
    i32c(cc, LUA_TTABLE); op(cc, W_I32_NE);
    br_if_out(cc, 0);  /* slow */
    lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_VALUE);
    lset(cc, V_TAB);
  }
  b_byte(C, W_BR); b_u32(C, 0);  /* slow: a longer chain */
  end(cc);
  call_helper(cc, H_EXEC);
  reload_base(cc);
  end(cc);
}

/* V_TAB[key] = value (RK); `table` is a register, or -1 for the environment. */
static void set_table (Compiler *cc, int table, int key, int value) {
  int numkey = key_kind(cc, key) == KEY_NUM;
  block(cc, W_BLOCK, W_VOID);  /* done */
  block(cc, W_BLOCK, W_VOID);  /* slow */
  if (table < 0) load_env(cc); else load_table_reg(cc, table, 0);
  lookup(cc, key, 0);
  /* The slot must hold a value already (else __newindex may apply), except
  ** array slots of tables without a metatable. */
  lget(cc, V_S); mem(cc, W_I32_LOAD, 2, TV_TT);
  op(cc, W_I32_EQZ);
  if (numkey) {
    lget(cc, V_TAB); mem(cc, W_I32_LOAD, 2, T_METATABLE);
    i32c(cc, 0); op(cc, W_I32_NE);
    op(cc, W_I32_AND);
  }
  br_if_out(cc, 0);
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
  call_helper(cc, H_EXEC);
  reload_base(cc);
  end(cc);
}

static int inline_key (Compiler *cc, int rk) {
  return key_kind(cc, rk) != KEY_NONE;
}

static void instruction (Compiler *cc) {
  Proto *p = cc->p;
  int pcidx = cc->pcidx;
  Instruction i = p->code[pcidx];
  int a = GETARG_A(i);
  switch (GET_OPCODE(i)) {
    case OP_MOVE:
      copy_tvalue(cc, a, 0, reg(GETARG_B(i)));
      break;
    case OP_LOADK:
      copy_tvalue(cc, a, 1, (unsigned int)(size_t)(p->k + GETARG_Bx(i)));
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
      block(cc, W_BLOCK, W_VOID);
      block(cc, W_BLOCK, W_VOID);
      not_number(cc, b);
      b_byte(C, W_BR_IF); b_u32(C, 0);
      load_num(cc, b);
      op(cc, W_F64_NEG);
      lset(cc, V_D1);
      store_num(cc, a, V_D1);
      b_byte(C, W_BR); b_u32(C, 1);
      end(cc);
      call_helper(cc, H_EXEC);
      reload_base(cc);
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
        call_helper(cc, H_EXEC);
        reload_base(cc);
      }
      break;
    case OP_SETGLOBAL:
      if (GETARG_Bx(i) <= MAXINDEXRK)
        set_table(cc, -1, RKASK(GETARG_Bx(i)), a);
      else {
        call_helper(cc, H_EXEC);
        reload_base(cc);
      }
      break;
    case OP_GETTABLE:
      if (inline_key(cc, GETARG_C(i)))
        get_table(cc, a, GETARG_B(i), GETARG_C(i), 0);
      else {
        call_helper(cc, H_EXEC);
        reload_base(cc);
      }
      break;
    case OP_SELF:
      if (inline_key(cc, GETARG_C(i)))
        get_table(cc, a, GETARG_B(i), GETARG_C(i), 1);
      else {
        call_helper(cc, H_EXEC);
        reload_base(cc);
      }
      break;
    case OP_SETTABLE:
      if (inline_key(cc, GETARG_B(i)))
        set_table(cc, a, GETARG_B(i), GETARG_C(i));
      else {
        call_helper(cc, H_EXEC);
        reload_base(cc);
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
      break;
    case OP_TAILCALL:
      call_helper(cc, H_TAILCALL);
      return_if_nonzero(cc);
      reload_base(cc);
      break;
    case OP_RETURN:
      call_helper(cc, H_RETURN);
      op(cc, W_RETURN);
      break;
    case OP_FORLOOP: {
      /* idx = ra + step; continue if (0 < step ? idx <= limit : limit <= idx) */
      lget(cc, V_BASE); mem(cc, W_F64_LOAD, 3, reg(a) + TV_VALUE);
      lget(cc, V_BASE); mem(cc, W_F64_LOAD, 3, reg(a + 2) + TV_VALUE);
      ltee(cc, V_D2);
      op(cc, W_F64_ADD);
      lset(cc, V_D1);
      f64c(cc, 0);
      lget(cc, V_D2);
      op(cc, W_F64_LT);
      block(cc, W_IF, W_TYPE_I32);
      lget(cc, V_D1);
      lget(cc, V_BASE); mem(cc, W_F64_LOAD, 3, reg(a + 1) + TV_VALUE);
      op(cc, W_F64_LE);
      b_byte(C, W_ELSE);
      lget(cc, V_BASE); mem(cc, W_F64_LOAD, 3, reg(a + 1) + TV_VALUE);
      lget(cc, V_D1);
      op(cc, W_F64_LE);
      end(cc);
      block(cc, W_IF, W_VOID);
      lget(cc, V_BASE); lget(cc, V_D1);
      mem(cc, W_F64_STORE, 3, reg(a) + TV_VALUE);
      store_num(cc, a + 3, V_D1);
      jump(cc, pcidx + 1 + GETARG_sBx(i));
      end(cc);
      break;
    }
    case OP_FORPREP:
      call_helper(cc, H_FORPREP);
      reload_base(cc);
      jump(cc, pcidx + 1 + GETARG_sBx(i));
      break;
    case OP_TFORLOOP: {
      int next = pcidx + 1;
      call_helper(cc, H_TFORLOOP);
      lset(cc, V_R);
      reload_base(cc);
      lget(cc, V_R);
      block(cc, W_IF, W_VOID);
      jump(cc, next + 1 + GETARG_sBx(p->code[next]));
      end(cc);
      jump(cc, pcidx + 2);
      break;
    }
    case OP_CLOSURE: {
      int nup = p->p[GETARG_Bx(i)]->nups;
      call_helper(cc, H_EXEC);
      reload_base(cc);
      if (nup > 0) jump(cc, pcidx + 1 + nup);
      break;
    }
    case OP_SETLIST:
      call_helper(cc, H_EXEC);
      reload_base(cc);
      if (GETARG_C(i) == 0) jump(cc, pcidx + 2);
      break;
    default:  /* everything else goes through the generic helper */
      call_helper(cc, H_EXEC);
      reload_base(cc);
      break;
  }
}

/* Largest function compiled: each instruction is a nested block. */
#define LJIT_MAX_CODE 3000

static int compile_body (Compiler *cc) {
  int k;
  int n = cc->n;
  /* locals: base, idx, t, r, tab, s (i32); d1, d2 (f64) */
  b_u32(C, 2);
  b_u32(C, NUM_I32_LOCALS); b_byte(C, W_TYPE_I32);
  b_u32(C, 2); b_byte(C, W_TYPE_F64);

  reload_base(cc);
  /* idx = (L->savedpc - code) / sizeof(Instruction) */
  lget(cc, V_L);
  mem(cc, W_I32_LOAD, 2, L_SAVEDPC);
  i32c(cc, (int)pcaddr(cc, 0));
  op(cc, W_I32_SUB);
  i32c(cc, 2);
  op(cc, W_I32_SHR_U);
  lset(cc, V_IDX);

  b_byte(C, W_LOOP); b_byte(C, W_VOID);
  for (k = 0; k < n; k++) {
    b_byte(C, W_BLOCK); b_byte(C, W_VOID);
  }
  lget(cc, V_IDX);
  b_byte(C, W_BR_TABLE);
  b_u32(C, n);
  for (k = 0; k < n; k++) b_u32(C, k);
  b_u32(C, 0);
  for (k = 0; k < n; k++) {
    b_byte(C, W_END);  /* code for instruction k follows its block */
    cc->pcidx = k;
    cc->depth = 0;
    instruction(cc);
  }
  b_byte(C, W_END);  /* loop */
  op(cc, W_UNREACHABLE);
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
  free(cc.code.p);
  free(m.p);
  free(s.p);
  ljit_ms += emscripten_get_now() - t0;
  return p->jit > 0;
}

void ljit_free (Proto *p) {
  if (p->jit > 0) ljit_release(p->jit);
  p->jit = 0;
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

#endif
