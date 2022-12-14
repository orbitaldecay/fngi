#ifndef __FNGI_H
#define __FNGI_H

#include "civ/civ_unix.h"
#include "./gen/const.h"
#include "./gen/spor.h"

#define SZR         SZ4

#define WS_DEPTH    16
#define RS_DEPTH    128
#define TOKEN_SIZE  128
#define DICT_DEPTH  10
#define FN_ALLOC    256

#define SLIT_MAX    0x2F

// To use: function must accept a "Kernel* k" pointer
#define cfb             (k->fb)

#define WS              (&cfb->ws)
#define RS              (&cfb->rs)

#define WS_POP()          Stk_pop(WS)
#define WS_POP2(A, B)     Stk_pop2(WS, A, B)
#define WS_ADD(V)         Stk_add(WS, V)
#define WS_ADD2(A, B)     Stk_add2(WS, A, B)
#define WS_ADD3(A, B, C)  Stk_add3(WS, A, B, C)
#define RS_ADD(V)         Stk_add(RS, V)
#define RS_POP()          Stk_pop(RS)
#define INFO_ADD(V)       Stk_add(&cfb->info, V)
#define INFO_POP(V)       Stk_pop(&cfb->info)

#define TASSERT_WS(E)     TASSERT_STK(E, WS)
#define TASSERT_EMPTY()   TASSERT_EQ(0, Stk_len(WS))

#define TEST_FNGI(NAME, numBlocks)            \
  TEST_UNIX(NAME, numBlocks)                  \
    FnFiber fnFb = {0};                       \
    Fiber_init((Fiber*)&fnFb, &localErrJmp);  \
    assert(FnFiber_init(&fnFb));              \
    civ.fb = (Fiber*)&fnFb;                   \
    Kern _k = {0}; Kern* k = &_k;             \
    Kern_init(k, &fnFb);

#define END_TEST_FNGI   END_TEST_UNIX

// ################################
// # From gen/name.c
extern U1* unknownInstr;
Slc instrName(U1 instr);
U1* szName(U1 szI);

// ################################
// # Types

// Note: these are not cleanly laid out in memory.
typedef struct { U1 meta; CStr name; Slot node; } TyI;
typedef struct { U1 len; TyI dat[]; } TyDat;

typedef struct { CStr path; } FileInfo;

// A Ty can be a const, function, variable or dict. See TY_MASK in
// const.zty.
typedef struct _Ty {
  Bst          bst;  // symbol name and dict search.
  struct _Ty*  parent;
  U2           meta; // specifies node type
  U2           line; // src code line of definition.
  FileInfo*    file; // source file
  TyDat*       ty;   // type information (fields, etc)
  Slot         v;    // either a value or pointer (depends on node type/etc)
} Ty;

typedef struct { Ty d; U1 lSlots; U2 len; } TyFn;

#define TyFn_native(CNAME, META, NFN) {       \
  .d.bst.key = (CStr*) ( CNAME ),             \
  .d.meta = TY_FN | TY_FN_NATIVE | (META),    \
  .d.v = NFN \
}

#define TyFn_static(NAME, META, LSLOTS, DAT) \
  static TyFn NAME;               \
  NAME = (TyFn) {                 \
    .d.meta = TY_FN | META,       \
    .d.v = DAT,                   \
    .lSlots = LSLOTS,             \
  };


static inline TyFn litFn(U1* dat, U2 meta, U2 lSlots) {
  return (TyFn) {
    .d = (Ty) { .meta = TY_FN | meta, .v = (Slot)dat},
    .lSlots = lSlots,
  };
}

// Dict type nodes have an extra pointer which points to the TyDict that has
// them as a 'v' (dict parent). This is only used when finding the full name
// of a node for debugging.
typedef struct { Ty d; Ty* tyParent; } TyDict;

typedef struct {
  U2 glen; U2 gcap; // global data used and cap
  U2 metaNext; // meta of next fn
  U2 cstate;
  U1 fnState;
  U1 localOffset;
  U1 logLvlSys;
  U1 logLvlUsr;
  Ty* curTy;    // current type (fn, struct) being compiled
  TyFn* compFn; // current function that does compilation
  Slot dictBuf[DICT_DEPTH];
  Stk dictStk;
  Reader src; FileInfo* srcInfo; U2 srcLine;
  Buf token; U1 tokenDat[64]; U2 tokenLine;
  Buf code;
} Globals;

typedef struct {
  Fiber fb;
  U1* ep;             // execution pointer
  Stk ws; Stk rs;     // working and return stack
  Stk info;           // info stack
} FnFiber;

typedef struct {
  U4 _null;
  BBA bbaCode;
  BBA bbaDict;
  BBA bbaRepl;
  Globals g;     // kernel globals
  Ty*   dict;    // kernel dictionary (root)
  FnFiber* fb;   // current fiber.
} Kern;

// ################################
// # Init
void Kern_init(Kern* k, FnFiber* fb);

// Initialze FnFiber (beyond Fiber init).
bool FnFiber_init(FnFiber* fb);

static inline Slot kFn(void(*native)(Kern*)) { return (Slot) native; }

// ################################
// # Execute

void executeFn(Kern* k, TyFn* fn);

// #################################
// # Scan
// scan fills g.token with a token. If one already exists it is a noop.
// scan does NOT affect g.src's ring buffer, except to increment tail with
// characters.
// When the token is used, tokenDrop should be called. This will update
// src's ring buffer as well so the next token can be scanned.
void scan(Kern* k);
void tokenDrop(Kern* k); // clear token and update src.ring
U1 cToU1(U1 c); // return 0-15 or 0xFF if not a hex integer.
typedef struct { bool isNum; U4 v; } ParsedNumber;
ParsedNumber parseU4(Slc t);

// #################################
// # Compiler
static inline bool isTyLocal(Ty* fn)  { return C_LOCAL & fn->meta; }

#define IS_TY(M)   { return (M) == (TY_MASK & fn->meta); }
static inline bool isTyConst(Ty* fn)  IS_TY(TY_CONST)
static inline bool isTyFn(Ty* fn)     IS_TY(TY_FN)
static inline bool isTyVar(Ty* fn)    IS_TY(TY_VAR)
static inline bool isTyDict(Ty* fn)   IS_TY(TY_DICT)
#undef IS_TY
#define IS_FN(M)   { return (M) & fn->d.meta; }
static inline bool isFnPre(TyFn* fn)       IS_FN(TY_FN_PRE)
static inline bool isFnNative(TyFn* fn)    IS_FN(TY_FN_NATIVE)
#undef IS_FN
#define IS_FN(M)   { return (TY_FN_TY_MASK & fn->d.meta) == (M); }
static inline bool isFnNormal(TyFn* fn)    IS_FN(TY_FN_NORMAL)
static inline bool isFnNow(TyFn* fn)       IS_FN(TY_FN_NOW)
static inline bool isFnSyn(TyFn* fn)       IS_FN(TY_FN_SYN)
static inline bool isFnInline(TyFn* fn)    IS_FN(TY_FN_INLINE)
static inline bool isFnComment(TyFn* fn)   IS_FN(TY_FN_COMMENT)
#undef IS_FN

static inline TyFn* tyFn(void* p) {
  ASSERT(isTyFn((Ty*)p), "invalid TyFn");
  return (TyFn*)p;
}

Ty* Kern_findTy(Kern* k, Slc t);
void Kern_addTy(Kern* k, Ty* ty);

void Kern_fns(Kern* k);
void single(Kern* k, bool asNow);
void compileSrc(Kern* k);

// Compile a stream to bbaRepl, returning the start.
//
// Any functions/etc will still be compiled to the normal locations.
U1*  compileRepl(Kern* k, bool withRet);

// #################################
// # Test Helpers

#define _COMPILE_VARS(CODE) \
  BufFile_var(LINED(bf), 64, CODE); \
  Reader LINED(f) = File_asReader(BufFile_asFile(&LINED(bf))); \
  k->g.src = LINED(f); \
  eprintf("!! Compiling: %.*s\n", Dat_fmt(LINED(bf).b))

#define COMPILE_NAMED(NAME, CODE, withRet) \
  _COMPILE_VARS(CODE); U1* NAME = compileRepl(k, withRet)
#define COMPILE(CODE, withRet) \
  _COMPILE_VARS(CODE); compileRepl(k, withRet)
#define COMPILE_EXEC(CODE)    \
  COMPILE_NAMED(LINED(code), CODE, true); executeInstrs(k, LINED(code));

void executeInstrs(Kern* k, U1* instrs);

void simpleRepl(Kern* k);

#endif // __FNGI_H
