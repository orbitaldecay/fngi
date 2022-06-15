// Spor binary kernel.
// This C file implements spor, see spor.md for documentation.
//
// # Table of Contents
// Search for these headings to find information
//
// * 1: Environment and Test Setup
//   * 1.a: Panic Handling
//   * 1.b: Environment Setup
//
// * 2: Memory Managers and Data Structures
//   * 2.a: Stacks
//   * 2.b: BlockAllocator (BA)
//   * 2.c: BlockBumpArena (BBA)
//   * 2.d: Slc (slice) Data Structure
//   * 2.e: Dict Binary Search Tree
//
// * 3: Executing Instructions
//   * 3.a: Utilities
//   * 3.b: Functions
//   * 3.c: Giant Switch Statement
//
// * 4: Source Code
//   * 4.a: Reading and Mocking
//   * 4.b: Scan
//
// * 5: Compiler
//   * 5.a: Utilities
//   * 5.b: Spor Token Functions
//   * 5.c: Spor Compiler
//   * 5.d: Compile Constants
//
// * 6: Device Operations (DV)

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <setjmp.h>
#include <errno.h>

#include "../kernel/kernel.h"

#define memberSize(type, member) sizeof(((type *)0)->member)
#define eprint(str)                fprintf (stderr, str)
#define eprintf(format, args...)   fprintf (stderr, format, args)
#define usrLog(lvl, format, args...) if(g->logLvlUsr & lvl) { fprintf (stderr, format, args); }


#define R  return;
#define UNREACH   if(1) { eprint("!!! unreachable\n"); assert(false); }
#define ASSERT_EQ(E, CODE) if(1) { \
  U4 r = CODE; \
  if((E) != r) eprintf("!!! Assertion failed: 0x%X == 0x%X\n", E, r); \
  assert((E) == r); }

// ***********************
// * 1: Environment and Test Setup
// This sets up the environment and tests.


//   *******
//   * 1.a: Panic Handling
// In normal fngi code, errors should be returned. However, for critical
// failures code can and will "panic". In C, a panic causes a longjmp to the
// error handler. Panics should be considered extraordinary events, and although
// fngi code can handle them with D_panic, it is not recommended to do so for
// any but the most low-level code or for testing purposes.

jmp_buf* err_jmp = NULL;
bool expectingErr = false;
#define SET_ERR(E)  if(true) { assert(E); g->err = E; \
  if(!expectingErr) { eprintf("!! Hit Error, fn=%s [cline=%u]\n", __func__, __LINE__); } \
  longjmp(*err_jmp, 1); }
#define ASSERT_NO_ERR()    assert(!g->err)
#define ASM_ASSERT(C, E)   if(!(C)) { SET_ERR(E); }

//   *******
//   * 1.b: Environment Setup
// This provides helpful functions and macros for setting up the environment.
//
// The initial environment has a somewhat complicated memory layout. The basics
// are (1) everything is in 4k blocks, (2) everything except for the local stack
// data is in the first block. The layout of "everything" is below:
//
//   Kern | Th | ws data | cs data | csz data | bba.nodes | Globals ... room to grow
U1        *mem, *memEnd;  // start/end of spor memory
Ref       memSize;
Kern*     k;    // kernel owned data structures
Thread*   th;   // current thread
Globals*  g;    // global data structures (global base can move)
U4 line;

static inline void* bndsChk(U4 size, Ref r) { // bounds check
  ASM_ASSERT(r, E_null); ASM_ASSERT(r + size <= memSize, E_oob);
  return (void*) ((ssize_t)mem + r);
}

static inline void* bndsChkNull(U4 size, Ref r) {
  if(!r) return NULL; return bndsChk(size, r); }

#define asRef(PTR)          ((U1*)(PTR)  - mem)
#define asPtr(TY, REF)      ((TY*)bndsChk(sizeof(TY), REF))
#define asPtrNull(TY, REF)  ((TY*)bndsChkNull(sizeof(TY), REF))
#define asU1(REF)       asPtr(U1, REF)
#define WS              (th->ws)
#define LS              (th->ls)
#define CS              (th->cs)
#define CSZ             (th->csz)
#define LS_SP           (LS.ref + LS.sp)
#define CS_SP           (CS.ref + CS.sp)
#define SRC             (&g->src)
#define SRCM            asPtrNull(FileMethods, g->srcM)
#define Tplc    (g->src.plc)
#define Tlen    (g->src.b.len)
#define Tref    (g->src.b.ref)
#define Tslc    (Slc) {g->src.b.ref, g->src.plc}
#define Tdat    bndsChk(g->src.b.cap, Tref)

#define Stk_init(CAP, REF) (Stk) {.ref = REF, .sp = CAP, .cap = CAP}
void initEnv(U4 blocks) {
  memSize = BLOCK_SIZE * blocks;
  mem = malloc(memSize); assert(mem); memEnd = mem + memSize;
  k = (Kern*) mem;                    *k = (Kern)    {0};
  th = asPtr(Thread, sizeof(Kern));   *th = (Thread) {.ep = 1};
  g = asPtr(Globals,
            asRef(th) + sizeof(Thread)
            + (WS_DEPTH + CS_DEPTH) * RSIZE + CS_DEPTH // stacks
            + (2 * blocks)); // BA nodes
  *g = (Globals) {0};

  WS  = Stk_init(WS_DEPTH * RSIZE, asRef(th) + sizeof(Thread));
  CS  = Stk_init(CS_DEPTH * RSIZE, WS.ref + WS.cap);
  CSZ = Stk_init(CS_DEPTH        , CS.ref + CS.cap);
  LS  = Stk_init(BLOCK_SIZE      , BLOCK_SIZE);
  k->ba = (BA) {
    .blocks = BLOCK_SIZE * 2, .nodes = CSZ.ref + CSZ.cap,
    .rooti = BLOCK_END,       .cap = blocks - 2,
  };
  k->bba    = (BBA) { .ba = asRef(&k->ba), .rooti=BLOCK_END };
  k->bbaTmp = (BBA) { .ba = asRef(&k->ba), .rooti=BLOCK_END };

  g->gbuf = (Buf) {
    .ref = asRef(g), .len = sizeof(Globals), .cap = BLOCK_SIZE };
  g->src = (File) {
    .b = (Buf) { .ref = asRef(&g->buf0), .cap = TOKEN_SIZE },
    .code = F_error };
  g->curBBA = asRef(&k->bba);
}

#define NEW_ENV_BARE(BLOCKS) \
  jmp_buf local_err_jmp; \
  err_jmp = &local_err_jmp; \
  initEnv(BLOCKS);

#define ENV_CLEANUP()               \
    err_jmp = NULL;                 \
    free(mem)

#define BARE_TEST(NAME, BLOCKS)  void NAME() {  \
  jmp_buf test_err_jmp; \
  eprintf("## %s\n", #NAME); \
  NEW_ENV_BARE(BLOCKS); \
  if(setjmp(local_err_jmp)) { \
    eprintf("!! Error #%X (line %u)\n", g->err, line); exit(1); } \
  else { /*Test code here */

#define TEST_END   } ENV_CLEANUP(); }

#define EXPECT_ERR(E, CALL) \
  err_jmp = &test_err_jmp; expectingErr = true; if(setjmp(test_err_jmp)) \
  { expectingErr = false; ASSERT_EQ(E, g->err); err_jmp = &local_err_jmp; } \
  else { CALL; UNREACH; }

BARE_TEST(testSetErr, 2)
  EXPECT_ERR(E_intern, SET_ERR(E_intern));
  EXPECT_ERR(E_intern, ASM_ASSERT(false, E_intern));
TEST_END

// ***********************
// * 2: Memory Managers and Data Structures
// Fngi has a few very simple data structures it uses for managing memory. All
// of these work successfully on low-level systems such as microcontrollers with
// zero overhead and no hardware support.

//   *******
//   * 2.a: Stacks
// Stacks are the core memory manager for operations (via the working stack
// (WS)) and for executing functions (via the call stack (CS)).
#define Stk_len(S)       (((S).cap - (S).sp) / RSIZE)
#define WS_POP()         Stk_pop(&WS)
#define WS_PUSH(V)       Stk_push(&WS, V)
#define WS_PUSH2(A, B)   if(1) { Stk_push(&WS, A); Stk_push(&WS, B); }
#define CS_PUSH(V)       Stk_push(&CS, V)

#define ASSERT_WS(E)     ASSERT_EQ(E, WS_POP())

U4 Stk_pop(Stk* stk) {
  ASM_ASSERT(stk->sp + RSIZE <= stk->cap, E_stkUnd);
  U4 out = *((U4*) (mem + stk->ref + stk->sp));
  stk->sp += RSIZE;
  return out;
}

void Stk_push(Stk* stk, U4 value) {
  ASM_ASSERT(stk->sp > 0, E_stkOvr);
  stk->sp -= RSIZE;
  *((U4*) (mem + stk->ref + stk->sp)) = value;
}

// Return value of ASCII hex char (or 0xFF if not a hex character)
/*fn*/ U1 charToHex(U1 c) {
  if ('0' <= c && c <= '9') return c - '0';
  if ('A' <= c && c <= 'F') return 10 + c - 'A';
  if ('a' <= c && c <= 'f') return 10 + c - 'a';
  return 0xFF;
}

BARE_TEST(testStk, 2)
  WS_PUSH(0xA);
  ASSERT_EQ(4, WS.cap - WS.sp); ASSERT_WS(0xA);
  WS_PUSH(0x10); WS_PUSH(0x11);
  assert(WS.sp == WS.cap - 8);
  ASSERT_WS(0x11); ASSERT_WS(0x10);
  ASSERT_EQ(0,   charToHex('0'));    ASSERT_EQ(9,   charToHex('9'));
  ASSERT_EQ(0xA, charToHex('A'));    ASSERT_EQ(0xF, charToHex('F'));
  ASSERT_EQ(0xA, charToHex('a'));    ASSERT_EQ(0xF, charToHex('f'));
  ASSERT_EQ(0xFF, charToHex('R'));
TEST_END

//   *******
//   * 2.b: BlockAllocator (BA)
// fngi utilizes 4KiB blocks of memory for many things including storing both
// the code and dictionary of it's module system. The kernel allocators are
// extremely lightweight but support dropable modules (and their code) without
// memory fragmentation.
//
// For more documentation see github.com/vitiral/spor_alloc

#define BA_block(BA, BI) ((BA).blocks + ((BI) << BLOCK_PO2))
#define BA_index(BA, BLOCK)   (((BLOCK) - (BA).blocks) >> BLOCK_PO2)
#define BBA_ba(bba) asPtr(BA, (bba).ba)

void BA_init(BA* ba) {
  if(ba->cap == 0) return; ASM_ASSERT(ba->cap < BLOCK_END, E_intern);
  BANode* nodes = asPtr(BANode, ba->nodes);
  ba->rooti = 0;
  U1 i, previ = BLOCK_END;
  for (i = 0; i < ba->cap; i += 1) {
    nodes[i].previ = previ; nodes[i].nexti = i + 1;
    previ = i;
  }
  nodes[i - 1].nexti = BLOCK_END;
}


// Allocate a block, updating BlockAllocator and client's root indexes.
//
// Go from:
//   baRoot     -> d -> e -> f
//   clientRoot -> c -> b -> a
// To (returning block 'd'):
//   baRoot     -> e -> f
//   clientRoot -> d -> c -> b -> a
Ref BA_alloc(BA* ba, U1* clientRooti) {
  uint8_t di = ba->rooti; // index of "d"
  if(di == BLOCK_END) return 0;

  BANode* nodes = asPtr(BANode, ba->nodes);
  BANode* d = &nodes[di]; // node "d"
  ba->rooti = d->nexti;  // baRoot -> e
  if (d->nexti != BLOCK_END) nodes[d->nexti].previ = BLOCK_END; // baRoot <- e

  ASM_ASSERT(d->previ == BLOCK_END, E_intern); // "d" is already root node
  d->nexti = *clientRooti; // d -> c
  if(*clientRooti != BLOCK_END) {
    nodes[*clientRooti].previ = di;  // d <- c
  }
  *clientRooti = di; // clientRooti -> d
  return BA_block(*ba, di); // return block 'd'
}

// Free a block, updating BlockAllocator and client's root indexes.
//
// Go from (freeing c):
//   clientRoot -> c -> b -> a
//   baRoot     -> d -> e -> f
// To:
//   clientRoot -> b -> a
//   baRoot     -> c -> d -> e -> f
void BA_free(BA* ba, uint8_t* clientRooti, Ref b) {
  // Assert block is within blocks memory region
  ASM_ASSERT(b >= ba->blocks, E_oob);
  uint8_t ci = BA_index(*ba, b);
  ASM_ASSERT(ci < ba->cap, E_oob);

  BANode* nodes = asPtr(BANode, ba->nodes);
  BANode* c = &nodes[ci]; // node 'c'
  if(ci == *clientRooti) {
    ASM_ASSERT(c->previ == BLOCK_END, E_intern);
    *clientRooti = c->nexti; // clientRoot -> b
    if(c->nexti != BLOCK_END) {
      nodes[c->nexti].previ = BLOCK_END; // clientRoot <- b
    }
  } else { // i.e. b -> c -> d  ===>  b -> d
    nodes[c->previ].nexti = c->nexti;
    nodes[c->nexti].previ = c->previ;
  }

  c->nexti               = ba->rooti; // c -> d
  nodes[ba->rooti].previ = ci;        // c <- d
  ba->rooti              = ci;        // baRoot -> c
  c->previ               = BLOCK_END; // baRoot <- c
}

// Free all blocks owned by the client.
//
// Go from:
//   clientRoot -> c -> b -> a
//   baRoot     -> d -> e -> f
// To:
//   clientRoot -> END
//   baRoot     -> a -> b -> c -> d -> e -> f
void BA_freeAll(BA* ba, uint8_t* clientRooti) {
  while(BLOCK_END != *clientRooti) {
    BA_free(ba, clientRooti, BA_block(*ba, *clientRooti));
  }
}

BARE_TEST(testBANew, 6)
  BA_init(&k->ba);
  BANode* nodes = asPtr(BANode, k->ba.nodes);

  ASSERT_EQ(4, k->ba.cap);
  // Check start node: root <-> a
  ASSERT_EQ(BLOCK_END, nodes[0].previ);
  ASSERT_EQ(1        , nodes[0].nexti);

  // Check end node
  ASSERT_EQ(2        , nodes[3].previ);
  ASSERT_EQ(BLOCK_END, nodes[3].nexti);
TEST_END

BARE_TEST(testAllocFree, 6)
  BA_init(&k->ba);
  BANode* nodes = asPtr(BANode, k->ba.nodes);
  uint8_t crooti = BLOCK_END; // clientRoot

  Ref a = BA_alloc(&k->ba, &crooti);
  ASSERT_EQ(k->ba.blocks, a); // first block

  // // clientroot -> a
  ASSERT_EQ(0         , crooti);
  ASSERT_EQ(BLOCK_END , nodes[0].previ);
  ASSERT_EQ(BLOCK_END , nodes[0].nexti);

  // // baRoot -> b -> c
  ASSERT_EQ(1         , k->ba.rooti);
  ASSERT_EQ(BLOCK_END , nodes[1].previ);
  ASSERT_EQ(2         , nodes[1].nexti);

  BA_free(&k->ba, &crooti, a);
  ASSERT_EQ(BLOCK_END , crooti);
  ASSERT_EQ(BLOCK_END , nodes[0].previ);
  ASSERT_EQ(1         , nodes[0].nexti);
  ASSERT_EQ(0         , nodes[1].previ);
TEST_END

BARE_TEST(testAlloc2FreeFirst, 6)
  BA_init(&k->ba);
  BANode* nodes = asPtr(BANode, k->ba.nodes);
  uint8_t crooti = BLOCK_END; // clientRoot

  Ref a = BA_alloc(&k->ba, &crooti);
  Ref b = BA_alloc(&k->ba, &crooti); // clientRoot -> b -> a;  baRoot -> c -> d
  ASSERT_EQ(a, k->ba.blocks);        // first block
  ASSERT_EQ(b, BA_block(k->ba, 1));  // second block
  BA_free(&k->ba, &crooti, a); // clientroot -> b -> END;  baRoot -> a -> c -> d

  // clientroot -> b -> END
  ASSERT_EQ(1         , crooti);
  ASSERT_EQ(BLOCK_END , nodes[1].previ);
  ASSERT_EQ(BLOCK_END , nodes[1].nexti);

  // baRoot -> a -> c ...
  ASSERT_EQ(0         , k->ba.rooti);
  ASSERT_EQ(BLOCK_END , nodes[0].previ);
  ASSERT_EQ(2         , nodes[0].nexti);
TEST_END


//   *******
//   * 2.c: BlockBumpArena (BBA)
// For storing code and dictionary entries which reference code, fngi uses a
// block bump arena. This "bumps" memory from the top (for aligned) or bottom of
// a 4k block, but does not allow freeing it. However, the entire arena can be
// dropped to recover all the memory without fragmentation.

bool _BBA_reserveIfSmall(BBA* bba, U2 size) {
  if((bba->cap) < (bba->len) + size) {
    if(0 == BA_alloc(BBA_ba(*bba), &bba->rooti)) return false;
    bba->len = 0;
    bba->cap = BLOCK_SIZE;
  }
  return true;
}

// Allocate "aligned" data from the top of the block.
//
// WARNING: It is the caller's job to ensure that size is suitably alligned to
// their system width.
Ref BBA_alloc(BBA* bba, U2 size) {
  if(!_BBA_reserveIfSmall(bba, size)) return 0;
  bba->cap -= size;
  Ref out = BA_block(*BBA_ba(*bba), bba->rooti) + bba->cap;
  return out;
}

// Allocate "unaligned" data from the bottom of the block.
Ref BBA_allocUnaligned(BBA* bba, uint16_t size) {
  if(!_BBA_reserveIfSmall(bba, size)) return 0;
  Ref out = BA_block(*BBA_ba(*bba), bba->rooti) + bba->len;
  bba->len += size;
  return out;
}

BARE_TEST(testBBA, 6)   BA_init(&k->ba);
  BANode* nodes = asPtr(BANode, k->ba.nodes);
  ASSERT_EQ(k->ba.blocks + BLOCK_SIZE - 12  , BBA_alloc(&k->bba, 12));
  ASSERT_EQ(BA_block(k->ba, 1)              , BBA_alloc(&k->bba, BLOCK_SIZE));
  ASSERT_EQ(BA_block(k->ba, 2)              , BBA_allocUnaligned(&k->bba, 13));
  ASSERT_EQ(BA_block(k->ba, 2) + 13         , BBA_allocUnaligned(&k->bba, 25));
  ASSERT_EQ(BA_block(k->ba, 3)              , BBA_allocUnaligned(&k->bba, BLOCK_SIZE - 20));
  ASSERT_EQ(0                               , BBA_alloc(&k->bba, BLOCK_SIZE));
TEST_END

//   *******
//   * 2.d: Slc (slice) Data Structure
// One of the core types in fngi is the Slc (slice) and it's child the Buf
// (buffer). A Slc is simply a reference and a U2 len (length). A Buf adds on a
// capacity, allowing for the data to grow. Note that a reference to a Buf is
// also a valid reference to a Slc.
//
// The other core type is cdata, often just represented as "c". This is counted
// data where the first byte has the count (length).
#define cAsSlc(CDATA)  asSlc(CDATA + 1, *asU1(CDATA))
#define sAsTmpSlc(S)   mvAndSlc(S, strlen(S))
#define bAsSlc(BUF)    (Slc) {.ref = (BUF).ref, .len = (BUF).len }

Slc asSlc(Ref ref, U2 len) {
  ASM_ASSERT(ref, E_null); ASM_ASSERT(ref + len < memSize, E_oob);
  return (Slc) {.ref = ref, .len = len};
}

Slc mvAndSlc(U1* buf, U2 len) {
  U1* gbuf = asU1(g->gbuf.ref + g->gbuf.len);
  memmove(gbuf, buf, len);
  return (Slc) { .ref = asRef(gbuf), .len = len };
}

I4 Slc_cmp(Slc l, Slc r) { // return -1 if l<r, 1 if l>r, 0 if eq
  U2 len; if(l.len < r.len) len = l.len;  else len = r.len;
  U1 *lp = mem + l.ref, *rp = mem + r.ref;
  for(U2 i = 0; i < len; i += 1) {
    if(*lp < *rp) return -1;
    if(*lp > *rp) return 1;
    lp += 1, rp += 1;
  }
  if(l.len < r.len) return -1;
  if(l.len > r.len) return 1;
  return 0;
}

#define TEST_SLICES \
  Ref c_a = BLOCK_SIZE; \
  Ref c_b = c_a + 4; \
  Ref c_c = c_b + 5; \
  memmove(asU1(c_a), "\x03" "aaa", 4); \
  memmove(asU1(c_b), "\x04" "abbd", 5); \
  memmove(asU1(c_c), "\x03" "abc", 4); \
  Slc a = cAsSlc(c_a); \
  Slc b = cAsSlc(c_b); \
  Slc c = cAsSlc(c_c);

BARE_TEST(testSlc, 5)
  TEST_SLICES
  ASSERT_EQ(3, c.len); assert(c_c + 1 == c.ref);
  ASSERT_EQ('a', *asU1(c.ref));  ASSERT_EQ('c', *asU1(c.ref + 2));


  ASSERT_EQ(0,  Slc_cmp(a, a));
  ASSERT_EQ(-1, Slc_cmp(a, b));
  ASSERT_EQ(-1, Slc_cmp(a, c));
  ASSERT_EQ(1,  Slc_cmp(b, a));
  ASSERT_EQ(1,  Slc_cmp(c, b));

  Slc z = sAsTmpSlc("abc");  ASSERT_EQ(0, Slc_cmp(c, z));
TEST_END


//   *******
//   * 2.e: Dict Binary Search Tree
// The dictionary is a basic unbalanced binary search tree with keys of cdata.
// It contains a U4 value and some metadata necessary for distinguishing between
// the kinds of values (see kernel/constants.sp).

// Find slice in BST, starting at node. Set result to node.
// returns 0 if node==NULL
// The return value is the result of `Slc_cmp(node.ckey, out.ckey)`
I4 Dict_find(DNode** node, Slc slc) {
  if(!*node) return 0;
  while(true) {
    I4 cmp = Slc_cmp(slc, cAsSlc((*node)->ckey));
    if(cmp == 0) return 0; // found exact match
    if(cmp < 0) {
      if((*node)->l)  *node = asPtr(DNode, (*node)->l); // search left
      else            return cmp; // not found
    } else /* cmp > 0 */ {
      if((*node)->r)  *node = asPtr(DNode, (*node)->r); // search right
      else            return cmp; // not found
    }
  }
}

DNode* Dict_get(DNode* root, Slc slc) {
  ASM_ASSERT(!Dict_find(&root, slc), E_cNoKey);
  return root;
}

// Add a node to the tree. WARNING: modifies *node.
void Dict_add(DNode** node, DNode* add) {
  if(!*node) {
    *node = add;
    return;
  }
  I4 cmp = Dict_find(node, cAsSlc(add->ckey));
  ASM_ASSERT(cmp, E_cKey);
  if(cmp < 0) (*node)->l = asRef(add);
  else        (*node)->r = asRef(add);
  add->l = 0, add->r = 0;
}

BARE_TEST(testDict, 2)
  TEST_SLICES

  DNode* n_a = asPtr(DNode, BLOCK_SIZE + 0x100);
  DNode* n_b = &n_a[1];
  DNode* n_c = &n_a[2];

  DNode* node = NULL;
  Dict_find(&node, b);
  assert(NULL == node);

  *n_a = (DNode) { .ckey = c_a };
  *n_b = (DNode) { .ckey = c_b };
  *n_c = (DNode) { .ckey = c_c };

  node = n_b; ASSERT_EQ( 0, Dict_find(&node, b));    assert(n_b == node); // b found
  node = n_b; ASSERT_EQ(-1, Dict_find(&node, a));    assert(n_b == node); // not found
  node = n_b; ASSERT_EQ( 1, Dict_find(&node, c));    assert(n_b == node); // not found

  node = NULL; Dict_add(&node, n_b); assert(node == n_b);
  node = n_b; Dict_add(&node, n_a);
  node = n_b; ASSERT_EQ( 0, Dict_find(&node, b));    assert(n_b == node); // b found
  node = n_b; ASSERT_EQ( 0, Dict_find(&node, a));    assert(n_a == node); // a found
  node = n_b; ASSERT_EQ( 1, Dict_find(&node, c));    assert(n_b == node); // not found

  node = n_b; Dict_add(&node, n_c);
  node = n_b; ASSERT_EQ( 0, Dict_find(&node, b));    assert(n_b == node); // b found
  node = n_b; ASSERT_EQ( 0, Dict_find(&node, a));    assert(n_a == node); // a found
  node = n_b; ASSERT_EQ( 0, Dict_find(&node, c));    assert(n_c == node); // c found

  ASSERT_EQ(n_b->l, asRef(n_a));
  ASSERT_EQ(n_b->r, asRef(n_c));
TEST_END

// ***********************
// * 3: Executing Instructions
// Fngi's assembly is defined in kernel/constants.sp. These constants are
// auto-generated into constants.h, which are imported here.
//
// The VM executes instruction bytecode in the fngi memory space, utilizing
// the fngi globals like CS and WS.
#define sectorRef(R)  (/*join with ep's sector*/ (0xFFFF0000 & th->ep) | r)

//   *******
//   * 3.a: Utilities
// There are a few utility functions necessary for executing instructions.

U4 ftBE(Ref ref, U2 size) { // fetch Big Endian
  U1* p = bndsChk(size, ref);
  switch(size) {
    case 1: return *p;                  case 2: return (*p<<8) + *(p + 1);
    case 4: return (*p << 24) + (*(p + 1)<<16) + (*(p + 2)<<8) + *(p + 3);
    default: SET_ERR(E_sz);
  }
}

void srBE(Ref ref, U2 size, U4 value) { // store Big Endian
  U1* p = bndsChk(size, ref);
  switch(size) {
    case 1: *p = value; break;
    case 2: *p = value>>8; *(p+1) = value; break;
    case 4: *p = value>>24; *(p+1) = value>>16; *(p+2) = value>>8; *(p+3) = value;
            break;
    default: SET_ERR(E_sz);
  }
}

U4 popLit(U1 size) { U4 out = ftBE(th->ep, size); th->ep += size; return out; }
U4 min(U4 a, U4 b) { if(a < b) return a; return b; }
U4 max(U4 a, U4 b) { if(a < b) return b; return a; }

static inline void _memmove(Ref dst, Ref src, U2 len) {
  void* d = bndsChk(len, dst); void* s = bndsChk(len, src);
  memmove(d, s, len);
}

// "Clear" the place buffer by moving existing data after plc to the beginning.
void clearPlcBuf(PlcBuf* p) {
  U1* ref = mem + p->ref;   p->len -= p->plc; // the new length
  memmove(ref, ref + p->plc, p->len); p->plc = 0;
}

BARE_TEST(testUtilities, 2)
  srBE(BLOCK_SIZE,      1, 0x01);
  srBE(BLOCK_SIZE + 1,  2, 0x2345);
  srBE(BLOCK_SIZE + 3,  4, 0x6789ABCD);
  ASSERT_EQ(0x01,         ftBE(BLOCK_SIZE, 1));
  ASSERT_EQ(0x2345,       ftBE(BLOCK_SIZE + 1, 2));
  ASSERT_EQ(0x6789ABCD,   ftBE(BLOCK_SIZE + 3, 4));
  th->ep = BLOCK_SIZE;
  ASSERT_EQ(0x01,         popLit(1));
  ASSERT_EQ(0x2345,       popLit(2));

  memmove(asU1(SRC->b.ref), "Hi there?!", 10);
  Tplc = 4; Tlen = 10; clearPlcBuf(F_plcBuf(*SRC));
  ASSERT_EQ(0, Tplc); ASSERT_EQ(6, Tlen);
  assert(!memcmp("here?!", asU1(SRC->b.ref), 6));
TEST_END

//   *******
//   * 3.b: Functions
// Functions can be either "small" (no locals) or "large" (has locals).

void xImpl(U1 growSz, Ref fn) { // base impl for XS and XL.
  CS_PUSH(th->ep);
  CSZ.sp -= 1; *(mem + CSZ.ref + CSZ.sp) = growSz; // push growSz onto csz
  th->ep = fn;
}

void xlImpl(Ref fn) { // impl for XL*
  // get amount to grow, must be multipled by APtr size .
  U1 growSz = *asU1(fn);
  ASM_ASSERT(growSz % RSIZE, E_align4);
  ASM_ASSERT(LS.sp >= growSz, E_stkOvr);
  LS.sp -= growSz; // grow locals stack
  xImpl(growSz, fn + 1);
}

//   *******
//   * 3.c: Giant Switch Statement
//
// Instructions (which are really just a single byte) are executed inside a
// giant switch statement. Most instructions modify the working stack and read
// literal values from the execution pointer. Some can also affect the execution
// pointerand the call stack.

static inline void executeDV(U1 dv);

inline static void executeInstr(Instr instr) {
  U4 l, r;
  switch ((U1)instr) {
    // Operation Cases
    case NOP: return;
    case RETZ: if(WS_POP()) return; // intentional fallthrough
    case RET:
      r = Stk_pop(&CS);
      l = *(mem + CSZ.ref + CSZ.sp); // size to shrink locals
      CSZ.sp += 1;
      ASM_ASSERT(LS.sp + l <= LS.cap, E_stkUnd);
      LS.sp += l;
      th->ep = r;
      return;
    case SWP: r = WS_POP(); l = WS_POP(); WS_PUSH(r); WS_PUSH(l); R
    case DRP : WS_POP(); R
    case OVR : r = WS_POP(); l = WS_POP(); WS_PUSH(l); WS_PUSH(r); WS_PUSH(l); R
    case DUP : r = WS_POP(); WS_PUSH(r); WS_PUSH(r);      R
    case DUPN: r = WS_POP(); WS_PUSH(r); WS_PUSH(0 == r); R
    case DV: executeDV(popLit(1)); R
    case RG:
      r = popLit(1);
      if (R_LP & r) {
        return WS_PUSH(LS_SP + (0x7F & r)); // local stack pointer + offset
      } else {
        switch (r) {
          case R_EP: WS_PUSH(th->ep); R
          case R_GB: WS_PUSH(g->gbuf.ref); R
          default: SET_ERR(E_cReg);
        }
      }

    case INC : WS_PUSH(WS_POP() + 1); return;
    case INC2: WS_PUSH(WS_POP() + 2); return;
    case INC4: WS_PUSH(WS_POP() + 4); return;
    case DEC : WS_PUSH(WS_POP() - 1); return;
    case INV : WS_PUSH(~WS_POP()); return;
    case NEG : WS_PUSH(-WS_POP()); return;
    case NOT : WS_PUSH(0 == WS_POP()); return;
    case CI1 : WS_PUSH((I4) ((I1) WS_POP())); return;
    case CI2 : WS_PUSH((I4) ((I2) WS_POP())); return;

    case ADD : r = WS_POP(); WS_PUSH(WS_POP() + r); return;
    case SUB : r = WS_POP(); WS_PUSH(WS_POP() - r); return;
    case MOD : r = WS_POP(); WS_PUSH(WS_POP() % r); return;
    case SHL : r = WS_POP(); WS_PUSH(WS_POP() << r); return;
    case SHR : r = WS_POP(); WS_PUSH(WS_POP() >> r); return;
    case MSK : r = WS_POP(); WS_PUSH(WS_POP() & r); return;
    case JN  : r = WS_POP(); WS_PUSH(WS_POP() | r); return;
    case XOR : r = WS_POP(); WS_PUSH(WS_POP() ^ r); return;
    case AND : r = WS_POP(); WS_PUSH(WS_POP() && r); return;
    case OR  : r = WS_POP(); WS_PUSH(WS_POP() || r); return;
    case EQ  : r = WS_POP(); WS_PUSH(WS_POP() == r); return;
    case NEQ : r = WS_POP(); WS_PUSH(WS_POP() != r); return;
    case GE_U: r = WS_POP(); WS_PUSH(WS_POP() >= r); return;
    case LT_U: r = WS_POP(); WS_PUSH(WS_POP() < r); return;
    case GE_S: r = WS_POP(); WS_PUSH((I4)WS_POP() >= (I4) r); return;
    case LT_S: r = WS_POP(); WS_PUSH((I4)WS_POP() < (I4) r); return;
    case MUL  :r = WS_POP(); WS_PUSH(WS_POP() * r); return;
    case DIV_U:r = WS_POP(); WS_PUSH(WS_POP() / r); return;
    case DIV_S:
      r = WS_POP();
      ASM_ASSERT(r, E_divZero);
      WS_PUSH((I4) WS_POP() / (I4) r);
      return;

    // Mem Cases
    case SZ1 + FT: WS_PUSH(*asU1(WS_POP())); R
    case SZ2 + FT: WS_PUSH(*asPtr(U2, WS_POP())); R
    case SZ4 + FT: WS_PUSH(*asPtr(U4, WS_POP())); R

    case SZ1 + FTO: WS_PUSH(*asU1(WS_POP() + popLit(1))); R
    case SZ2 + FTO: WS_PUSH(*asPtr(U2, WS_POP() + popLit(1))); R
    case SZ4 + FTO: WS_PUSH(*asPtr(U4, WS_POP() + popLit(1))); R

    case SZ1 + FTLL: WS_PUSH(*asU1(LS_SP + popLit(1))); R
    case SZ2 + FTLL: WS_PUSH(*asPtr(U2, LS_SP + popLit(1))); R
    case SZ4 + FTLL: WS_PUSH(*asPtr(U4, LS_SP + popLit(1))); R

    case SZ1 + FTGL: WS_PUSH(*asU1(g->gbuf.ref + popLit(2))); R
    case SZ2 + FTGL: WS_PUSH(*asPtr(U2, g->gbuf.ref + popLit(2))); R
    case SZ4 + FTGL: WS_PUSH(*asPtr(U4, g->gbuf.ref + popLit(2))); R

    case SZ1 + SR: r = WS_POP(); *asU1(r) = WS_POP(); R
    case SZ2 + SR: r = WS_POP(); *asPtr(U2, r) = WS_POP(); R
    case SZ4 + SR: r = WS_POP(); *asPtr(U4, r) = WS_POP(); R

    case SZ1 + SRO: r = WS_POP(); *asU1(r + popLit(1)) = WS_POP(); R
    case SZ2 + SRO: r = WS_POP(); *asPtr(U2, r + popLit(1)) = WS_POP(); R
    case SZ4 + SRO: r = WS_POP(); *asPtr(U4, r + popLit(1)) = WS_POP(); R

    case SZ1 + SRLL: *asU1(LS_SP + popLit(1)) = WS_POP(); R
    case SZ2 + SRLL: *asPtr(U2, LS_SP + popLit(1)) = WS_POP(); R
    case SZ4 + SRLL: *asPtr(U4, LS_SP + popLit(1)) = WS_POP(); R

    case SZ1 + SRGL: *asU1(g->gbuf.ref + popLit(2)) = WS_POP(); R
    case SZ2 + SRGL: *asPtr(U2, g->gbuf.ref + popLit(2)) = WS_POP(); R
    case SZ4 + SRGL: *asPtr(U4, g->gbuf.ref + popLit(2)) = WS_POP(); R

    case SZ1 + LIT: WS_PUSH(popLit(1)); R
    case SZ2 + LIT: WS_PUSH(popLit(2)); R
    case SZ4 + LIT: WS_PUSH(popLit(4)); R

    // Jmp Cases
    case SZ1 + JMPL: r = popLit(1); th->ep = th->ep + (I1)r; R
    case SZ2 + JMPL: r = popLit(2); th->ep = sectorRef(r); R
    case SZ4 + JMPL: r = popLit(4); th->ep = r; R

    case SZ1 + JMPW:
    case SZ2 + JMPW:
    case SZ4 + JMPW: th->ep = WS_POP();

    case SZ1 + JZL: r = popLit(1); if(!WS_POP()) { th->ep = th->ep + (I1)r; } R
    case SZ2 + JZL: r = popLit(2); if(!WS_POP()) { th->ep = sectorRef(r); } R
    case SZ4 + JZL: r = popLit(4); if(!WS_POP()) { th->ep = r; } R

    case SZ1 + JTBL:
    case SZ2 + JTBL:
    case SZ4 + JTBL: assert(false); // TODO: not impl

    case SZ1 + XLL: xlImpl(th->ep + (I1)popLit(1)); R;
    case SZ2 + XLL: xlImpl(sectorRef(popLit(2))); R;
    case SZ4 + XLL: xlImpl(popLit(4)); R;

    case SZ1 + XLW:
    case SZ2 + XLW:
    case SZ4 + XLW: xlImpl(WS_POP()); R;

    case SZ1 + XSL: xImpl(0, th->ep + (I1)popLit(1)); R;
    case SZ2 + XSL: xImpl(0, sectorRef(popLit(2))); R;
    case SZ4 + XSL: xImpl(0, popLit(4)); R;

    case SZ1 + XSW:
    case SZ2 + XSW:
    case SZ4 + XSW: xImpl(0, WS_POP()); R;

    default: if(instr >= SLIT) return WS_PUSH(0x3F & instr);
             SET_ERR(E_cInstr);
  }
}

void execute(U1 instr) {
  U2 startingLen = Stk_len(CS);
  while(true) {
    executeInstr(instr);
    if(Stk_len(CS) == startingLen) return;
    instr = popLit(1);
  }
}

// ***********************
// * 4: Files and Source Code

//   *******
//   * 4.a: Files
// Fngi uses a File object for all file operations, including scanning tokens.
// The design differs from linux in several ways:
//
//  1. File is both a struct and a role, meaning there are FileMethods. This
//     allows using user-space filesystems and mocks while still calling
//     the device op.
//  2. There is only a single device op which uses the index to the
//     FileMethods.  Unlike unix, all operations happen on the same small File
//     struct instead of introducing a whole range of complex structures to
//     manage operation.
//  3. Always non-blocking. Fngi only supports cooperative multi-tasking, so
//     blocking can never be okay.
//  4. EOF code. Fngi has distinct codes for done (buffer full) and EOF
//     conditions. Linux's design here is convoluted, especially when you
//     want to select() a file at the EOF.
//
// > Note: After mostly flushing out the design of fngi's File API I came across
// > the more modern `aio.h`, which shares many similarities.
#include <fcntl.h>
#define handleFMethods(name, m, f) \
  if(m) { WS_PUSH(asRef(f)); WS_PUSH(m->name); return execute(SZ4 + XLW); }

int F_handleErr(File* f, int res) {
  if(errno == EWOULDBLOCK) return res;
  if(res < 0) {
    f->code = F_Eio; g->syserr = errno; errno = 0;
  }
  return res;
}

File* F_new(U2 bufCap) {
  File* f = asPtr(File, BBA_alloc(&k->bba, sizeof(File)));
  *f = (File) {
    .b = (Buf) { .ref = BBA_allocUnaligned(&k->bba, bufCap), .cap = bufCap },
    .code = F_done,
  }; return f;
}

void F_open(FileMethods* m, File* f) {
  handleFMethods(open, m, f);
  U1 pathname[256];
  ASM_ASSERT(f->b.len < 255, E_io);
  memcpy(pathname, bndsChk(f->b.len, f->b.ref), f->b.len);
  pathname[f->b.len] = 0;
  int fd = F_handleErr(f, open(pathname, O_NONBLOCK, O_RDWR));
  if(fd < 0) { f->code = F_error; g->syserr = errno; errno = 0; }
  else { f->pos = 0; f->fid = F_INDEX | fd; f->code = F_done; f->plc = 0; }
  f->plc = 0; f->b.len = 0; f->code = F_done;
}

void F_close(FileMethods* m, File* f) {
  handleFMethods(close, m, f);
  if(m) { WS_PUSH(asRef(f)); WS_PUSH(m->close); return execute(SZ4 + XLW); }
  if(!close(F_FD(*f))) { f->code = F_error; g->syserr = errno; }
  else { f->code = F_done; }
}

void F_read(FileMethods* m, File* f) {
  handleFMethods(read, m, f);
  ASM_ASSERT(f->code == F_reading || f->code >= F_done, E_io);
  int len;
  if(!(F_INDEX & f->fid)) { // mocked file. TODO: add some randomness
    PlcBuf* p = asPtr(PlcBuf, f->fid);
    len = min(p->len - p->plc, f->b.cap - f->b.len);
    _memmove(f->b.ref, p->ref + p->plc, len); p->plc += len;
  } else {
    f->code = F_reading;
    len = F_handleErr(f, read(F_FD(*f), asU1(f->b.ref + f->b.len), f->b.cap - f->b.len));
    assert(len >= 0);
  }
  f->b.len += len; f->pos += len;
  if(f->b.len == f->b.cap) f->code = F_done;
  else if (0 == len) f->code = F_eof;
}

// Read file blocking. Any errors result in a panic.
void readAtLeast(FileMethods* m, File* f, U2 atLeast) {
  ASM_ASSERT(f->b.cap - f->b.len >= atLeast, E_intern);
  U2 startLen = f->b.len;
  while(1) {
    F_read(m, f);
    if(f->code == F_eof || f->b.len - startLen >= atLeast) break;
    ASM_ASSERT(f->code < F_error, E_io);
  }
}

void openMock(File* f, U1* contents) { // Used for tests
  PlcBuf* p = asPtr(PlcBuf, BBA_alloc(&k->bba, sizeof(PlcBuf)));
  U2 len = strlen(contents);
  U1* s = asU1(BBA_allocUnaligned(&k->bba, len));
  memmove(s, contents, len);
  *p = (PlcBuf) { .ref = asRef(s), .len = len, .cap = len };
  f->fid = asRef(p); f->pos = 0;
  f->plc = 0; f->b.len = 0; f->code = F_done;
}

void openUnix(File* f, U1* path) { // Used for tests
  f->b.len = strlen(path); assert(f->b.cap >= f->b.len);
  memcpy(bndsChk(f->b.len, f->b.ref), path, f->b.len);
  F_open(NULL, f); ASSERT_EQ(F_done, f->code);
}

U1* TEST_expectedTxt = "Hi there Bob\nThis is Jane.\n";
BARE_TEST(testReadMock, 4)  BA_init(&k->ba);
  File* f = F_new(10); ASSERT_EQ(10, f->b.cap);
  openMock(f, TEST_expectedTxt);
  F_read(NULL, f); ASSERT_EQ(F_done, f->code);
  ASSERT_EQ(10, f->b.len);
  ASSERT_EQ(0, memcmp("Hi there B", asU1(f->b.ref), 10));
  f->b.len = 0; F_read(NULL, f); assert(!memcmp("ob\nThis is", asU1(f->b.ref), 10));
  f->b.len = 0; F_read(NULL, f); assert(!memcmp(" Jane.", asU1(f->b.ref), 6));
  ASSERT_EQ(F_done, f->code); F_read(NULL, f); ASSERT_EQ(F_eof, f->code);
TEST_END

BARE_TEST(testReadUnix, 4)  BA_init(&k->ba);
  File* f = F_new(128); ASSERT_EQ(128, f->b.cap);
  openUnix(f, "./tests/testData.txt"); f->b.len = 0;
  readAtLeast(NULL, f, 128); ASSERT_EQ(F_eof, f->code);
  ASSERT_EQ(strlen(TEST_expectedTxt), f->b.len);
  assert(!memcmp(TEST_expectedTxt, asU1(f->b.ref), f->b.len));
TEST_END

//   *******
//   * 4.b: Scan
// The fngi scanner is used for both spor and fngi syntax. The entire compiler
// works by:
//  1. scanning a single tokens.
//  2. executing or compiling the current token.
//     2.a. peek at the next token
//     2.b. reading raw characters
//
// The basic architecture is to use a small buffer to buffer input from the
// operating system or hardware. We keep track of the currently scanned token.
// When it is not used, we shift bytes to the left to delete the current token.
//
// The advantage to this is simplicity. We do pay some cost for the performance
// of shifting bytes left after every token, but that cost is small compared to
// serial IO.
// The scanner reads the next token, storing at the beginning of Tbuf with
// length Tplc.

U1 toTokenGroup(U1 c) {
  if(c <= ' ')             return T_WHITE;
  if('0' <= c && c <= '9') return T_NUM;
  if('a' <= c && c <= 'f') return T_HEX;
  if('A' <= c && c <= 'F') return T_HEX;
  if(c == '_')             return T_HEX;
  if('g' <= c && c <= 'z') return T_ALPHA;
  if('G' <= c && c <= 'Z') return T_ALPHA;
  if(c == '%' || c == '\\' || c == '$' || c == '|' ||
     c == '.' || c ==  '(' || c == ')') {
    return T_SINGLE;
  }
  return T_SYMBOL;
}

void readNewAtLeast(FileMethods* m, File* f, U2 num) {
  f->plc = 0; f->b.len = 0; readAtLeast(m, f, num);
}

void _scan(FileMethods* m, File* f) {
  U1 firstTg; PlcBuf* p = F_plcBuf(*f); U1* dat = bndsChk(p->cap, p->ref);
  while(true) { // Skip whitespace
    if(p->plc >= p->len) { readNewAtLeast(m, f, 1); } // buffer full of white, get new
    if(p->len == 0) return; // TODO: eof check
    firstTg = toTokenGroup(dat[p->plc]); if(firstTg != T_WHITE) break;
    if(dat[p->plc] == '\n') line += 1;
    p->plc += 1;
  }
  clearPlcBuf(F_plcBuf(*f));
  if(!p->len) { readAtLeast(m, f, 1); }
  assert(p->len); U1 c = dat[p->plc];
  if(firstTg == T_SINGLE) { p->plc += 1; R }
  // Parse token until the group changes.
  while (true) {
    if (p->plc >= p->len) readAtLeast(m, f, 1);
    if (p->plc >= p->len) break;
    ASM_ASSERT(p->plc < p->cap, E_cTLen);
    c = dat[p->plc];
    U1 tg = toTokenGroup(c);
    if (tg == firstTg) {}
    else if ((tg <= T_ALPHA) && (firstTg <= T_ALPHA)) {}
    else break;
    p->plc += 1;
  }
}

void scan(FileMethods* m, File* f) { _scan(m, f); }

#define ASSERT_TOKEN(S)  if(1) { scan(NULL, SRC); if(!Teq(S)) { \
  eprintf("! Token: %s == %.*s\n", S, Tplc, asU1(Tref)); \
  assert(false); } }
U1 Teq(U1* s) { U2 len = strlen(s);  if(len != Tplc) return false;
                return 0 == memcmp(s, asU1(Tref), len); }

BARE_TEST(testScan, 3)  BA_init(&k->ba);
  openMock(SRC, "hi there$==");
  ASSERT_TOKEN("hi"); ASSERT_TOKEN("there"); ASSERT_TOKEN("$"); ASSERT_TOKEN("==");

  openMock(SRC, "\\         lots     \n\n\n\n    of \n\n  empty            ");
  ASSERT_TOKEN("\\"); ASSERT_TOKEN("lots"); ASSERT_TOKEN("of"); ASSERT_TOKEN("empty");

  openMock(SRC, "\\ comment\n#00 #0=bob");
  ASSERT_TOKEN("\\"); ASSERT_TOKEN("comment"); ASSERT_TOKEN("#"); ASSERT_TOKEN("00");
  ASSERT_TOKEN("#");  ASSERT_TOKEN("0"); ASSERT_TOKEN("="); ASSERT_TOKEN("bob");

  openUnix(SRC, "kernel/constants.sp");
  ASSERT_TOKEN("\\"); ASSERT_TOKEN("Kernel"); ASSERT_TOKEN("Constants");
  ASSERT_TOKEN("\\");
  ASSERT_TOKEN("\\"); ASSERT_TOKEN("Note"); ASSERT_TOKEN(":"); ASSERT_TOKEN("this");
  assert(!close(F_FD(g->src)));
TEST_END

// ***********************
// * 5: Compiler
// We can finally begin writing the compiler. There are only a few characters
// that the native spor assembler can handle (see `compile()` function).
//
// We must first implement how to handle each case and then we can start
// compiling!

typedef struct { U1 sz; U1 instr; } Compiler;
Compiler compiler;

//   *******
//   * 5.a: Utilities

U1 charToSz(U1 c) {
  switch (c) { case '1': return 1; case '2': return 2; case '4': return 4;
               case 'R': return RSIZE; default: SET_ERR(E_sz); } }

Ref bump(BBA* bba, U1 aligned, U4 size) {
  Ref ref;  U1 starti = bba->rooti;
  if(aligned) ref = BBA_alloc(bba, size);
  else        ref = BBA_allocUnaligned(bba, size);
  ASM_ASSERT(starti == bba->rooti, E_newBlock); ASM_ASSERT(ref, E_oom);
  return ref;
}

Ref kbump(U1 aligned, U4 size) { return bump(asPtr(BBA, g->curBBA), aligned, size); }

U1 scanInstr(FileMethods* m, File* f) { // scan a single instruction, combine with sz
  scan(m, f); DNode* node = Dict_get(asPtr(DNode, k->dict), Tslc);
  U1 instr = node->v;
  if((0xC0 & instr == I_MEM) || (0xC0 & instr == I_JMP)) {
    switch (compiler.sz) {
      case 1: instr |= SZ1; break; case 2: instr |= SZ2; break;
      case 4: instr |= SZ4; break; default: SET_ERR(E_intern);
    }
  }
  return instr;
}

//   *******
//   * 5.b: Spor Token Functions
// Each of these handle a single "token" (really single character) case in the
// spore compiler.

#define SPOR_TEST(NAME, BLOCKS) \
  BARE_TEST(NAME, BLOCKS) \
  initSpor();

void initSpor() {
  compiler = (Compiler) { .sz = RSIZE, .instr = NOP };
  BA_init(&k->ba);
}

Ref newBlock() { // start a new block
  BBA* bba = asPtr(BBA, g->curBBA);
  Ref r = BA_alloc(asPtr(BA, bba->ba), &bba->rooti);
  ASM_ASSERT(r, E_oom); bba->len = 0; bba->cap = BLOCK_SIZE;
  return r;
}

void cDot() { // `.`, aka "set size"
  if(Tplc >= Tlen) readAtLeast(SRCM, SRC, 1);
  ASM_ASSERT(Tlen, E_eof);
  compiler.sz = charToSz(*asU1(Tref + Tplc));
  Tplc += 1;
}

void cForwardSlash(FileMethods* m, File* f) { // `\`, aka line comment
  U1* dat = Tdat;
  while(true) {
    if (Tplc >= Tlen) readNewAtLeast(m, f, 1);
    if (Tlen == 0) break;
    if (dat[Tplc] == '\n') break;
    Tplc += 1;
  }
}

void cHash() { // `#`, aka hex literal
  U1* dat = Tdat; U4 v = 0; scan(SRCM, SRC);
  for(U1 i = 0; i < Tplc; i += 1) {
    U1 c = dat[i];
    if (c == '_') continue;
    ASM_ASSERT(toTokenGroup(c) <= T_HEX, E_cHex);
    v = (v << 4) + charToHex(c);
  }
  WS_PUSH(v); clearPlcBuf(F_plcBuf(*SRC));
}

void cEqual() { // `=`, aka dict set
  U1 meta = WS_POP(); U4 value = WS_POP(); scan(SRCM, SRC);
  Ref ckey = kbump(false, Tplc + 1);
  *(mem + ckey) = Tplc; // Note: unsafe write, memory already checked.
  _memmove(ckey + 1, Tref, Tplc);

  DNode* add = (DNode*) (mem + kbump(true, sizeof(DNode)));
  *add = (DNode) {.ckey = ckey, .v = value, .m1 = meta};

  DNode* root = NULL; if(k->dict) root = asPtr(DNode, k->dict);
  Dict_add(&root, add);
  if(!k->dict) k->dict = asRef(root);
}

void cAt() { // `@`, aka dict get
  scan(SRCM, SRC);
  DNode* node = Dict_get(asPtr(DNode, k->dict), Tslc);
  WS_PUSH(node->v);
}

void cComma() { // `,`, aka write heap
  U4 value = WS_POP();
  srBE(kbump(/*aligned=*/ false, compiler.sz), compiler.sz, value);
}

void cPercent() { // `%`, aka compile instr
  srBE(kbump(/*aligned=*/ false, 1), 1, scanInstr(SRCM, SRC));
}

void cCarrot() { // `^`, aka execute instr
  U1 instr = scanInstr(SRCM, SRC);
  th->ep += 1;
  executeInstr(instr);
}

void cDollar() { // `$`, aka execute token
  scan(SRCM, SRC); DNode* n = Dict_get(asPtr(DNode, k->dict), Tslc);

  if(TY_FN_INLINE == (TY_FN_TY_MASK & n->m1)) {
    U1 len = *asU1(n->v);
    memmove(asU1(kbump(false, len)), asU1(n->v + 1), len);
    return;
  }
  if(TY_FN_SYN == (TY_FN_TY_MASK & n->m1)) WS_PUSH(false); // pass asNow=false
  WS_PUSH(n->v);
  if(TY_FN_LARGE & n->m1) execute(SZ4 + XLW);
  else                    execute(SZ4 + XSW);
}

#define ASSERT_VALUE(K, V) ASSERT_EQ( \
  V, Dict_get(asPtr(DNode, k->dict), sAsTmpSlc(K))->v)

SPOR_TEST(testSporBasics, 4)  newBlock();
  openMock(SRC, " 12 ");  cHash();   ASSERT_EQ(0x12, WS_POP());
  WS_PUSH2(0x42, 0); openMock(SRC, "mid");    cEqual(); ASSERT_VALUE("mid", 0x42);
  WS_PUSH2(0x44, 0); openMock(SRC, "aLeft");  cEqual(); ASSERT_VALUE("aLeft", 0x44);
  WS_PUSH2(0x88, 0); openMock(SRC, "zRight"); cEqual(); ASSERT_VALUE("zRight", 0x88);
TEST_END

//   *******
//   * 5.c: Spor Compiler

void compile() {
  Tplc = 1; U1 c = *asU1(Tref); // spor compiler only uses first character
  switch (c) {
    case '.': cDot(); R             case '\\': cForwardSlash(SRCM, SRC); R
    case '#': cHash(); R            case '=': cEqual(); R
    case '@': cAt(); R              case ',': cComma(); R
    case '%': cPercent(); R         case '^': cCarrot(); R
    case '$': cDollar(); R
    default: eprintf("!! Invalid ASM token: %.*s\n", Tplc, asU1(Tref));
             SET_ERR(E_cToken);
  }
}

void compileLoop() {
  while(true) {
    scan(SRCM, SRC); if(Tplc == 0) return; else compile();
  }
}

//   *******
//   * 5.d: Compile Constants

void compileFile(char* s) {
  line = 1;
  openUnix(SRC, s); compileLoop(); ASSERT_NO_ERR();
}

void compileConstants() {
  WS_PUSH(newBlock());
  WS_PUSH(RSIZE); WS_PUSH(SZR);
  compileFile("kernel/constants.sp");
  newBlock(); compileFile("kernel/errors.sp");
}

SPOR_TEST(testConstants, 4)
  compileConstants();
  ASSERT_VALUE("JMPL", 0x80);    ASSERT_VALUE("XLW", 0x85);
  ASSERT_VALUE("E_io", 0xE010);  ASSERT_VALUE("E_unreach", 0xE003);
TEST_END

// ***********************
// * 6: Cooperative Multitasking
// Fngi supports cooperative multitasking. If you are familiar with threading,
// this has several differences:
// * All operations that would normally take a long time (such as file IO and
//   sleeping), are always non-blocking.
// * Threads yield control using the `CEDE` instruction. This stops execution on
//   the current thread until other threads have been checked.
//
// Threads can be created with D_thread. On the fngi side, this must be passed
// memory for a new WS, LS, CS, CSZ, VM, and (optionally) globals. In the C
// implementation, this also creates a new "thread context" using makecontext,
// which is added to the current threads to be executed as the top priority
// (immediately after CEDE is called).
#include <signal.h>
#include <ucontext.h>

typedef struct {

} CThread;

//      if ((sigstk.ss_sp = malloc(SIGSTKSZ)) == NULL)
//          /* error return */
//        sigstk.ss_size = SIGSTKSZ;
//            sigstk.ss_flags = 0;

// ***********************
// * 7: Device Operations (DV)
// Besides instructions for the most basic actions, Device Operations (DV) are
// the primary mechanism that spor code communicates with hardware. The kernel
// defines some extremely basic device operations which are sufficient to both:
//   (1) bootstrap fngi on running spor assembly
//   (2) enable a usable computer operating system or general purpose
//       programming language.

static inline void executeDV(U1 dv) {
  switch (dv) {
    case D_assert: { U4 chk = WS_POP(); ASM_ASSERT(chk, WS_POP()); R }
    case D_catch: {
      U4 ep = th->ep, cs_sp = CS.sp, ls_sp = LS.sp;
      jmp_buf* prev_err_jmp = err_jmp; // cache prev jmp location
      jmp_buf local_err_jmp; err_jmp = &local_err_jmp;
      if(setjmp(local_err_jmp)) { /* got error, handled below */ }
      else execute(SZ4 + XLW);
      // ALWAYS Reset ep, call, and local stack
      th->ep = ep, CS.sp = cs_sp, CSZ.sp = cs_sp / 4, LS.sp = ls_sp;
      WS.sp = WS.cap;              // clear WS
      err_jmp = prev_err_jmp;      // restore prev jmp location
      WS_PUSH(g->err); g->err = 0; // push and clear error
      return;
    } case D_memset: {
        U2 len = WS_POP(); U1 value = WS_POP(); void* dst = bndsChk(len, WS_POP());
        memset(dst, value, len); return;
    } case D_memcmp: {
        U2 len = WS_POP();
        void* r = bndsChk(len, WS_POP()); void* l = bndsChk(len, WS_POP());
        return WS_PUSH(memcmp(l, r, len));
    } case D_memmove: {
        U2 len = WS_POP(); Ref src = WS_POP(); return _memmove(WS_POP(), src, len);
    } case D_bump: {
      BBA* bba = asPtr(BBA, WS_POP()); U4 aligned = WS_POP();
      return WS_PUSH(bump(bba, aligned, WS_POP()));
    } case D_log: {
      U1 lvl = WS_POP(); U2 len = WS_POP();
      if(g->logLvlUsr & lvl) {
        eprintf("D_log [%X]", lvl);
        for(U2 i = 0; i < len; i++) eprintf(" %.4X", WS_POP());
      } else {
        for(U2 i = 0; i < len; i++) WS_POP(); // drop len ws items
      }
      return;
    } case D_file: { // method &File &FileMethods
      FileMethods* m = asPtrNull(FileMethods, WS_POP());
      File* f = asPtr(File, WS_POP());
      switch (WS_POP()) {
        case (offsetof(FileMethods, open)  / RSIZE): F_open(m, f);  R
        case (offsetof(FileMethods, close) / RSIZE): F_close(m, f); R
        case (offsetof(FileMethods, read)  / RSIZE): F_read(m, f);  R
        default: SET_ERR(E_dv);
      }
      return;
    } case D_scan: { // method &File &FileMethods
      FileMethods* m = asPtrNull(FileMethods, WS_POP());
      File* f = asPtr(File, WS_POP());
      switch (WS_POP()) {
        case 0: readAtLeast(m, f, 1); R
        case 1: cForwardSlash(m, f);  R
        case 2: scan(m, f);           R
        default: SET_ERR(E_dv);
      }
    }
    default: SET_ERR(E_dv);
  }
}

int main() {
  eprint("# main\n");
  testSetErr();
  testStk();
  testBANew();
  testAllocFree();
  testAlloc2FreeFirst();
  testBBA();
  testSlc();
  testDict();
  testUtilities();
  testReadMock();
  testReadUnix();
  testScan();
  testSporBasics();
  testConstants();

  return 0;
}
