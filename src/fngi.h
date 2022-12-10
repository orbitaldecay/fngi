#ifndef __FNGI_H
#define __FNGI_H

#include "civ/civ_unix.h"
#include "./gen/const.h"
#include "./gen/spor.h"

#define WS_DEPTH    16
#define RS_DEPTH    128
#define TOKEN_SIZE  128
#define DICT_DEPTH  10

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

#define FNGI_TEST(NAME, numBlocks)            \
  TEST_UNIX(NAME, numBlocks)                  \
    FnFiber fnFb = {0};                       \
    Fiber_init((Fiber*)&fnFb, &localErrJmp);  \
    assert(FnFiber_init(&fnFb));          \
    Civ_init((Fiber*)&fnFb);                  \
    Kern _k = {0}; Kern* k = &_k;             \
    Kern_init(k, &fnFb);

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

// A Ty can be a const, function, variable or dict. See META_TY_MASK in
// const.zty.
typedef struct _Ty {
  Bst          bst;  // symbol name and dict search.
  struct _Ty*  parent;
  U2           meta; // specifies node type
  U2           line; // src code line of definition.
  FileInfo*    info; // source file
  TyDat*       ty;   // type information (fields, etc)
  Slot         v;    // either a value or pointer (depends on node type/etc)
} Ty;

typedef struct {Ty d; U1 lSlots; } TyFn;

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
  BBA* bbaCode; // where code is stored
  BBA* bbaDict; // where dictionary/etc is stored
  Ty* dictBuf[DICT_DEPTH];
  Stk dictStk;
  Reader src; FileInfo* srcInfo; U2 line;
  Buf token;
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
  Globals g;     // kernel globals
  Ty*   dict;    // kernel dictionary (root)
  FnFiber* fb;   // current fiber.
} Kern;

// ################################
// # Init
void Kern_init(Kern* k, FnFiber* fb);

// Initialze FnFiber (beyond Fiber init).
bool FnFiber_init(FnFiber* fb);

// ################################
// # Execute
void executeFn(Kern* k, TyFn* fn);
void executeInstrs(Kern* k, Slc instrs);

// #################################
// # Scan
void scan(Reader f, Buf* b);
void scanNext(Reader f, Buf* b);

// #################################
// # Compiler
static inline bool isTyLocal(Ty* fn)  { return C_LOCAL & fn->meta; }

#define IS_TY(M)   { return (M) == (META_TY_MASK & fn->meta); }
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




#endif // __FNGI_H
