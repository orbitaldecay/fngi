\ The fngi kernel.
\
\ Included by kernel: constants.sp, errors.sp, globals.sp
\
\ This file bootstraps spor from a few constants and the native (i.e. C)
\ implementation into a more full-featured language with helpful macros.
\
\ # Table of Contents
\ Search for these headings to find information
\
\ [1] Bootstrap Macros: necessary functionality for the rest of the language.
\ [2] Core functions and macros: bread and butter of spor assembly
\ [3] ASM Flow Control (IF, LOOP, etc)
\ [4] xx, jmpl, Scanning and Alignment
\ [5] globals and locals
\ [6] Zoa strings and logging zoab
\ [7] Fngi compile loop


\ **********
\ * [1] Core functions and macros
\
\ Storage Locations: these allow ultra-minimalism for embedded systems
\   fn pub                          : store next name in public storage + dict
\   fn STORE_PUB / STORE_PRIV       : switch storage to public / private
\
\ Assertions: these panic with the supplied errCode if cond is not met.
\   fn assert [cond errCode]
\   fn assertNot [cond errCode]
\
\ Test Assertions: these panic with E_test if the cond is not met.
\   fn tAssert, tAssertNot, tAssertEq
\
\ Internal functions: these are used to create other compiler functions
\   fn bump [size align -> &mem]    : bump memory from storage
\   fn select [a b s -> a|b]        : a if s else b
\   fn h1 [U1] -> []                : push 1 byte to heap
\   fn h2 [U2] -> []                : push 2 byte to heap (big endian)
\   fn L0 [U1] -> []                : compile a small literal  [#0 - #3F]
\   fn L1 [U1] -> []                : compile a 1 byte literal [#0 - #FF]
\   fn L2 [U1] -> []                : compile a 2 byte literal [#0 - #FFFF]
\   fn scan     [<token>]           : scan the next token in src
\   fn $dictRef [<token> -> &DNode] : get node of token.
\   fn $dictAdd [<token> v m2]      : set dict@token
\
\ Inline functions:
\   fn xCatch [... &fn]       : execute a large function and catch error.
\   fn retz  [a]              : return immediately if not a  (     %RETZ)
\   fn retif [a]              : return immediately if a      (%NOT %RETZ)
\   fn reteq [a b]            : return immediately if a == b (%NEQ %RETZ)


@TY_FN@TY_FN_SYN^JN :pub %DRP \ make next name public
  .2%LIT@C_PUB_NAME, %FTGL@G_cstate, %JN  %SRGL@G_cstate, %RET

$pub @TY_FN@TY_FN_NOW^JN :STORE_PUB \ switch to using public storage.
  .2%LIT@C_PUB,      %FTGL@G_cstate, %JN  %SRGL@G_cstate, %RET
$pub @TY_FN@TY_FN_NOW^JN :STORE_PRIV \ switch to using private storage.
  .2%LIT@C_PUB^INV, %FTGL@G_cstate, %MSK %SRGL@G_cstate, %RET

$STORE_PRIV
@_FP@TY_FN_INLINE^JN :BBA_bump \ {size align &BBA -> &dat}: bump memory from BBA
  .1#3, @SLIT@BBAm_bump    ^JN, %DV@D_bba, %RET
@_FP@TY_FN_INLINE^JN :BBA_newBlock \ {&BBA}: create new block
  .1#3, @SLIT@BBAm_newBlock^JN, %DV@D_bba, %RET
@_FP@TY_FN_INLINE^JN :BBA_drop \ {&BBA}: drop BBA (return all blocks)
  .1#3, @SLIT@BBAm_drop    ^JN, %DV@D_bba, %RET

$STORE_PUB
$pub @TY_FN@TY_FN_NOW^JN :NEW_BLOCK_PUB \ start new public block
  .R%FTGL@G_bbaPub.2,  $BBA_newBlock %RET
$pub @TY_FN@TY_FN_NOW^JN :NEW_BLOCK_PRIV \ start new private block
  .R%FTGL@G_bbaPriv.2, $BBA_newBlock %RET

\ Using DV instr, we define a way to make assertions for tests.
$pub @_FP@TY_FN_INLINE^JN  :assertEq .1#2, %DV@D_assert, %RET \ {l r err}
$pub @_FP :assertNot %SLIT %SWP $assertEq %RET \ { l err} assert l == 0
$pub @_FP :assert %SWP %NOT %SWP %SLIT %SWP $assertEq %RET \ { l err} assert l != 0
$pub @_FP :assertNotNull .2%LIT@E_null, .2%JMPL@assert,
$pub @_FP :tAssertEq     .2%LIT@E_test, $assertEq %RET \ {chk} test assert
$pub @_FP :tAssertNot       %SLIT .2%JMPL@tAssertEq, \ tAssertEq(_, 0)
$pub @_FP :tAssert     %NOT %SLIT .2%JMPL@tAssertEq, \ tAssertEq(not _, 0)
#55 #54^INC $tAssertEq     #0    $tAssertNot    #1 $tAssert
#0 #1       $assertNot     #1 #1 $assert        #1 $assertNotNull

$STORE_PRIV \ Note: everything until STORE_PUB is in the private space.

@TY_FN :getCState .2%FTGL@G_cstate, %RET  \ for assertions
$getCState@C_PUB^MSK $tAssertNot

@TY_FN :wsLen      .1@SLIT@D_comp_wsLen^JN, %DV@D_comp, %RET
@TY_FN :assertNoWs .2%XSL@wsLen,  .2%JMPL@tAssertNot,
$assertNoWs

$STORE_PUB  $getCState@C_PUB^MSK $tAssert
$pub @_FP@TY_FN_INLINE^JN :bump \ {size align -> &dat} bump some memory
  .1#4, @SLIT, $BBA_bump %RET \ {&U1} Note: #3 is size of INLINE

@_FP :h1 \ {U1}: store 1 byte on heap (unchecked)
  .1@SLIT#1^JN, @SLIT,  \ {U1 size=1 aligned=false}
  $bump .1%SR %RET

@_FP :L0   \ {U1} compile a small literal (unchecked)
  .1%LIT#3F, %MSK   \ {U1} truncated to bottom 6 bits
  .1%LIT@SLIT, %JN  \ {jn(U1, SLIT) get full instr
  .2%JMPL@h1, \ made into SLIT instr and stored.

$STORE_PRIV
@_FP :answer #30$L0 #12$L0 %ADD %RET \ { -> 0x42}
$answer #42 $tAssertEq
$STORE_PUB   $assertNoWs

$pub@_FP :h2 #2$L0 #0$L0 $bump .2%SRBE %RET \ {U2} store on heap (BE)
$pub@_FP :h4 #4$L0 #0$L0 $bump .4%SRBE %RET \ {U4} store on heap (BE)

$pub@_FP :L1   \ {U1} compile a 1 byte literal (unchecked)
  .1%LIT @SZ1@LIT^JN$h1 .2%XSL@h1, \ {U1} compile .1%LIT instr
  .2%JMPL@h1, \ {} then compile the U1

$pub@_FP :L2   \ {U1} compile a 2 byte literal (unchecked)
  .1%LIT @SZ2@LIT^JN$h1 .2%XSL@h1, \ {U2} compile .2%LIT instr
  .2%JMPL@h2, \ {} then compile the U2

$STORE_PRIV
@_FP :answer2 #42$L1 #4242$L2 %RET
$answer2  #4242 $tAssertEq  #42 $tAssertEq
$STORE_PUB   $assertNoWs

@_FP@TY_FN_INLINE^JN :dnodeLast #3$h1 @D_comp_last$L0 %DV@D_comp$h1 %RET
@_FP@TY_FN_INLINE^JN :scan      #3$h1 @D_comp_scan$L0 %DV@D_comp$h1 %RET

\ {&root -> &DNode} root can be null
@_FP :dictRef   $scan @D_comp_dGet$L0 %DV@D_comp$h1 %RET
@_FP :dictAdd   $scan @D_comp_dAdd$L0 %DV@D_comp$h1 %RET \ {m v &root} root can be null

$STORE_PRIV       $NEW_BLOCK_PRIV
@_FP@TY_FN_INLINE^JN :d_mGet   #2$h1 .2%FTO@DN_m$h1 %RET \ {&DNode -> m}
@_FP@TY_FN_INLINE^JN :d_mSet   #2$h1 .2%SRO@DN_m$h1 %RET \ {m &DNode}
@_FP@TY_FN_INLINE^JN :d_vGet   #2$h1 .R%FTO@DN_v $h1 %RET \ {&DNode -> v}
@_FP@TY_FN_INLINE^JN :d_vSet   #2$h1 .R%SRO@DN_v $h1 %RET \ {v &DNode}

@_FP :tDictRef #0$L0 .2%XSL@dictRef,  %RET \ get dict ref for testing
@_FP :assertDictV \ {<token> v} assert the value of token
  #0$L0 .2%XSL@dictRef, $d_vGet .2%JMPL@tAssertEq,

#42 #0 $dictAdd answerV       #42 $assertDictV answerV

$STORE_PUB   $assertNoWs
@_FP@TY_FN_INLINE^JN :catch   #2$h1 %DV@D_catch$h1 %RET
@_FP@TY_FN_INLINE^JN :retz    #1$h1          %RETZ %RET
@_FP@TY_FN_INLINE^JN :reteq   #2$h1     %NEQ %RETZ %RET
@_FP@TY_FN_INLINE^JN :retif   #2$h1     %NOT %RETZ %RET

\ * [2] Spor syntax
\ These are core spor syntax helpers, many of which will also be used to
\ build equivalent fngi syntax.
\
\   fn FN <name>              : declare a function
\   fn PRE                    : make function "pre" (run after next token)
\   fn INLINE                 : make function "pre" (run after next token)
\   fn SYN                    : make function "syn" (syntax, always now)
\   fn NOW                    : require function to be "now" (use $)
\   fn LARGE                  : make function large (has locals)
\
\ There are also many helpers, like
\   fn isFnPre, isFnNormal

$STORE_PRIV

@_FP :_j2 .2%XSL@h1, .2%JMPL@h2, \ {ref instr} compile a context switch

@TY_FN :_xsl \ $_xsl <token> : compile unchecked xsl
  #0$L0 .2%XSL@dictRef, $d_vGet    .1%LIT@XSL2,   .2%JMPL@_j2,

@TY_FN :_jmp \ $_jmp <token> : compile unchecked jmpl
  #0$L0 .2%XSL@dictRef, $d_vGet    .1%LIT@JMPL2,  .2%JMPL@_j2,

@_FP@TY_FN_NOW^JN :c1 \ {instr} compile "compile instr" into function
  $_xsl L1
  @h1$L2 @XSL2$L2 $_jmp _j2

@TY_FN :callAnswer  $_xsl answer %RET     $callAnswer #42 $tAssertEq
@TY_FN :jmpAnswer   $_jmp answer          $jmpAnswer  #42 $tAssertEq

\ These take {&DNode} and return or assert information about it
$STORE_PUB      $assertNoWs
@_FP :isTyConst   $d_mGet  @META_TY_MASK$L1 %MSK  @TY_CONST$L1 %EQ %RET
@_FP :isTyFn      $d_mGet  @META_TY_MASK$L1 %MSK  @TY_FN$L1  %EQ %RET
@_FP :isFnLarge   $d_mGet  @TY_FN_LARGE$L1 %MSK %RET
@_FP :assertFn    $_xsl isTyFn @E_cNotFn$L2 $_jmp assert
@_FP :assertFnSmall %DUP $_xsl assertFn
  $_xsl isFnLarge  @E_cIsX$L2  $_jmp assertNot
@_FP :assertFnLarge %DUP $_xsl assertFn
  $_xsl isFnLarge  @E_cIsX$L2  $_jmp assert

@_FP :toMod #FFFF$L2 #10$L0 %SHL %MSK %RET

@_FP :isSameMod \ {ref ref} -> {sameMod}
  $_xsl toMod  %SWP  $_xsl toMod  %EQ %RET
@_FP :curMod   $dnodeLast  $d_vGet  $_jmp toMod    \ [] -> [mod]
@_FP :isCurMod $_xsl toMod  $_xsl curMod %EQ %RET  \ [ref] -> [isCurMod]
@_FP :assertCurMod  $_xsl isCurMod  @E_cMod$L2  $_jmp assert

      #42   $isCurMod $tAssert
#1234_5678  $isCurMod $tAssertNot

@_FP :_jSetup \ [&DNode] -> [ref]: checked jmp setup
  %DUP $_xsl assertFnSmall  $d_vGet %DUP $_jmp assertCurMod \ {ref}

\ TODO: rename d_mJn and swap args
@_FP :keyJnMeta \ {&DNode m -> &DNode} : apply meta to key
  %OVR $d_mGet %JN \ {&DNode mNew}
  %SWP $d_mSet %RET

$pub @_FP@TY_FN_INLINE^JN :heap #3$h1 @D_comp_heap$L0 %DV@D_comp$h1 %RET
$pub @_FP :assertNoNow  @E_cNoNow$L2  $_jmp assertNot  \ {now}
@TY_FN :_implFnTy \ {meta asNow}
  %SWP $_xsl assertNoNow \ {meta}
  .2%FTGL@G_metaNext$h2  %JN  .2%SRGL@G_metaNext$h2  %RET

$pub @TY_FN@TY_FN_SYN^JN :syn    @TY_FN_SYN   $L1 $_jmp _implFnTy
$pub @TY_FN@TY_FN_SYN^JN :now    @TY_FN_NOW   $L1 $_jmp _implFnTy
     @TY_FN@TY_FN_SYN^JN :pre    @TY_FN_PRE   $L1 $_jmp _implFnTy
$pub @TY_FN@TY_FN_SYN^JN :inline @TY_FN_INLINE$L1 $_jmp _implFnTy
     @TY_FN@TY_FN_SYN^JN :large  @TY_FN_LARGE $L1 $_jmp _implFnTy

\ example: $syn $large $FN <token>: declare a function with attributes.
@TY_FN@TY_FN_SYN^JN :FN   $_xsl assertNoNow \ {}
  #0$L0 @TY_FN$L1 .2%FTGL@G_metaNext$h2 %JN $_xsl dictAdd
  $heap $dnodeLast $d_vSet \ dnodeLast.v = heap
  #0$L0 .2%SRGL@G_metaNext$h2 \ clear metaNext
  $_jmp assertNoWs
$assertNoWs

\ These tell something about a &DNode. Type: [&DNode -> bool]
$pre $FN isFnPre     $d_mGet  @TY_FN_PRE$L1     %MSK %RET
$pre $FN isVarInput  $d_mGet  @TY_VAR_INPUT$L1  %MSK %RET
$pre $FN isFnNormal  $d_mGet  @TY_FN_TY_MASK$L1 %MSK  @TY_FN_NORMAL$L1 %EQ %RET
$pre $FN isFnNow     $d_mGet  @TY_FN_TY_MASK$L1 %MSK  @TY_FN_NOW$L1    %EQ %RET
$pre $FN isFnSyn     $d_mGet  @TY_FN_TY_MASK$L1 %MSK  @TY_FN_SYN$L1    %EQ %RET
$pre $FN isFnInline  $d_mGet  @TY_FN_TY_MASK$L1 %MSK  @TY_FN_INLINE$L1 %EQ %RET
$pre $FN isTyVar     $d_mGet  @META_TY_MASK$L1  %MSK  @TY_VAR$L1       %EQ %RET
$pre $FN assertTyVar $_xsl isTyVar  @E_cNotLocal$L2 $_jmp assert

$tDictRef assert  ^DUP $isFnPre $tAssert $isFnNormal $tAssert

$pub $pre $FN panic   #0$L0 $_jmp assert \ {err} panic immediately
$pub      $FN unreach @E_unreach$L2 $_jmp panic \ unreachable code
$pub $pre $FN unimplIfTrue @E_unimpl$L2 $_jmp assert \ {chk}: if true raise unimpl
$pre $FN assertSzI \ {szI}
  %DUP #CF$L1 %MSK @E_sz$L2 $_xsl assertNot \ non-sz bits empty
  #4$L0 %SHR #3$L1 %LT_U @E_sz$L2 $_jmp assert \ sz bits < 3

$now $pre $FN ftoN \ {offset szI} compile FTO szI w/offset
  %DUP $_xsl assertSzI  @FTO$L1 %ADD  $_xsl h1 $_jmp h1

\ **********
\ * [7] ASM (initial) Flow Control
\ Flow control either pushes the current heap on the WS or uses a local
\ constant. END/AGAIN uses this heap-val/local to do the right thing.
\
\   if/else: $IF ... $ELSE ... $END
\   loop:    $LOOP <l0> ... $BREAK0 <b0> ... $AGAIN <l0> $BREAK_END <b0>

\ @_FP :assertJmpL1  #80$L1 %LT_U  @E_cJmpL1$L2 $_jmp assert
$FN assertJmpL1 #80$L1 %LT_U @E_cJmpL1$L2 $_jmp assert \ {&jmpTo} assert valid

\ Switch to non/local storage
$FN storeNonLocal @C_LOCAL^INV$L2 .2%FTGL@G_cstate$h2 %MSK .2%SRGL@G_cstate$h2 %RET
$FN storeLocal    @C_LOCAL$L2     .2%FTGL@G_cstate$h2 %JN  .2%SRGL@G_cstate$h2 %RET
$FN ldictRef $_xsl storeLocal #0$L0 $_xsl dictRef $_jmp storeNonLocal \ see dictRef
$FN ldictAdd $_xsl storeLocal #0$L0 $_xsl dictAdd $_jmp storeNonLocal \ see dictAdd

$pub $pre $syn $FN IF $_xsl assertNoNow \ {} -> {&jmpTo} : start an if block
  @SZ1@JZL^JN $c1 \ compile .1%JZL instr
  $heap #0$L0 $_jmp h1  \ compile 0 (jump pad)

$FN _END \ {&jmpTo heapDiff} jmpTo is where to store (heap-heapDiff)
  $heap \ {&jmpTo heapDiff heap}
  %SWP %SUB   %DUP $_xsl assertJmpL1 \ {heapDiff (heap-heapDiff)}
  %SWP .1%SR %RET \ store at &jmpTo (1 byte literal)

$syn $FN END $_xsl assertNoNow %DUP $_jmp _END

$syn $FN ELSE $_xsl assertNoNow \ {&ifNotJmpTo} -> {&elseBlockJmpTo}
  @JMPL $c1       \ (end IF) compile unconditional jmp to end of ELSE
  $heap %SWP      \ {&elseBlockJmpTo &ifNotJmpTo}
  #0$L0 $_xsl h1  \ compile jmp lit for &elseBlockJmpTo
  %DUP $_jmp _END \ end of IF block (beginning of ELSE)

\ $LOOP l0 ... $BREAK0 b0 ... $AGAIN l0  $BREAK_END b0
     $syn $FN LOOP   $_xsl assertNoNow  $heap  #0$L0 $_jmp ldictAdd
$pre $syn $FN BREAK0 $_xsl IF                  #0$L0 $_jmp ldictAdd
$syn $FN END_BREAK  $_xsl assertNoNow #0$L0 $_xsl ldictRef $d_vGet %DUP $_jmp _END
$pre $syn $FN BREAK_IF  @NOT$c1  $_jmp BREAK0 \ break if true
$pre $syn $FN BREAK_EQ  @NEQ$c1  $_jmp BREAK0 \ break if equal
$pre $syn $FN BREAK_NEQ @EQ$c1   $_jmp BREAK0 \ break if not equal

$syn $FN AGAIN $_xsl assertNoNow
  @JMPL $c1  \ {} compile jmp
  $heap #0$L0 $_xsl ldictRef $d_vGet  \ {heap &loopTo}
  %SUB %DUP  $_xsl assertJmpL1  \ {heap-&loopTo}
  %NEG $_jmp h1  \ compile negative backwards jump to jmp offset

$STORE_PRIV
$FN testIf \ converts 1->4 else: 13
  #1$L0 %EQ  $IF   #4$L0  %RET
             $END  #13$L0 %RET
#1 $testIf      #4  $tAssertEq
#2 $testIf      #13 $tAssertEq

$FN testIfElse
  #1$L0 %EQ $IF #4$L0 $ELSE #13$L0 $END
  #2$L0 %ADD %RET
#1 $testIfElse  #6 $tAssertEq
#2 $testIfElse  #15 $tAssertEq

$STORE_PUB $assertNoWs

\ **********
\ * [8] xx, jmpl, Scanning and Alignment Utilities
\ Reading, peeking and szI alignment
\
\ fn xx:<token> [...]             : compile an execute to a token
\ fn jmp:<token> [...]            : compile an jmp to a token
\ fn align [aptr sz -> aptr]      : align aptr with sz bytes
\ fn align4 [aptr -> aptr]        : align aptr with 4 bytes
\ fn alignSzI [aptr szI -> aptr]  : align aptr with szI bytes
\ fn hN [U4 szI]                  : write a value of szI to heap (no align)
\ fn szToSzI [U4 -> SzI]          : convert number of bytes to SzI
\ fn szIToSz [SzI -> U1]          : convert szI to number of bytes
\
\ fn dictK [-> &key isFromLocal]  : any ref to current token.
\ fn read [ -> numRead]         : attempt to read bytes from src.
\ fn readNew [ -> numRead]      : clear token buf and read bytes.
\ fn scanNoEof []               : scan and assert not EOF.
\ fn peekChr [ -> c]            : peek at the next character.
\
\ fn assertSzI [szI]              : assert that szI is valid
\ fn isEof [ -> bool]           : return whether there was a token scanned
\ fn assertToken []             : assert there is a token
\ fn assertNoEof [numRead]      : assert that numRead > 0 (E_eof)

$pre $FN dnodeSzI $d_mGet @SZ_MASK$L1 %MSK %RET \ {&dnode -> szI}

$pub $pre $FN szIToSz \ {szI} -> {sz}
  %DUP @SZ1$L1 %EQ $IF  %DRP #1$L0 %RET  $END
  %DUP @SZ2$L1 %EQ $IF  %DRP #2$L0 %RET  $END
       @SZ4$L1 %EQ $IF       #4$L0 %RET  $END
  @E_sz$L2 $_jmp panic

$pub $pre $FN hN \ {value szI} write a value of szI to heap
  %DUP @SZ1$L1 %EQ $IF  %DRP $_jmp h1  $END
  %DUP @SZ2$L1 %EQ $IF  %DRP $_jmp h2  $END
  %DUP @SZ4$L1 %EQ $IF  %DRP $_jmp h4  $END
  @E_sz$L2 $_jmp panic

$pub $pre $FN szToSzI \ [sz] -> [SzI] convert sz to szI (instr)
  %DUP #1$L0 %EQ $IF  %DRP @SZ1 $L1 %RET  $END
  %DUP #2$L0 %EQ $IF  %DRP @SZ2 $L1 %RET  $END
       #4$L0 %EQ $IF  %DRP @SZ4 $L1 %RET  $END
  @E_sz$L2 $_jmp panic

$pub $pre $FN reqAlign \ {sz -> sz}: get required alignment
  %DUP @RSIZE^DEC$L0 %LT_U $retif  %DRP @RSIZE$L0 %RET

$pub $large $pre $FN align \ {aptr sz -> aptr}: align aptr with sz bytes
  @RSIZE$h1 \ locals [sz:U1]
  .1%SRLL#0$h1 \ cache sz
  %DUP \ {aptr aptr}
  .1%FTLL#0$h1 %MOD \ {aptr aptr%sz}
  %DUP $IF
    .1%FTLL#0$h1 %SWP %SUB \ {aptr (sz - aptr%sz)}
    %ADD %RET \ aptr + (sz - aptr%sz)
  $END
  %DRP %RET

$pub$pre $FN alignA  $_xsl reqAlign .2%XLL @align$h2 %RET  \ {aptr sz -> aptr}: align to SZA
$pub$pre $FN align4   #4$L0         .2%XLL @align$h2 %RET  \ {addr -> aligned4Addr}
$pub     $FN alignSzI $_xsl szIToSz  .2%XLL @align$h2 %RET \ {addr szI -> addrAlignedSzI}

$pre $FN ftSzI \ {&addr szI}
  %DUP @SZ1$L1 %EQ $IF %DRP .1%FT %RET $END
  %DUP @SZ2$L1 %EQ $IF %DRP .2%FT %RET $END
       @SZ4$L1 %EQ $IF      .4%FT %RET $END
  @E_sz$L2 $_xsl panic

$pre $FN srSzI \ {value &addr szI}
  %DUP @SZ1$L1 %EQ $IF %DRP .1%SR %RET $END
  %DUP @SZ2$L1 %EQ $IF %DRP .2%SR %RET $END
       @SZ4$L1 %EQ $IF      .4%SR %RET $END
  @E_sz$L2 $_xsl panic

$pre $FN xSzI \ {&DNode -> szI}: size requirement of calling DNode
  $d_vGet $_xsl isCurMod $IF  @SZ2$L1 %RET  $END  @SZA$L1 %RET

$STORE_PRIV
#0 $reqAlign  #0 $tAssertEq     #1 $reqAlign  #1 $tAssertEq
#2 $reqAlign  #2 $tAssertEq     #3 $reqAlign  #4 $tAssertEq
#4 $reqAlign  #4 $tAssertEq     #5 $reqAlign  #4 $tAssertEq

#20 #1 $alignA #20 $tAssertEq   #21 #1  $alignA #21 $tAssertEq
#20 #2 $alignA #20 $tAssertEq   #21 #2  $alignA #22 $tAssertEq
#21 #4 $alignA #24 $tAssertEq   #21 #11 $alignA #24 $tAssertEq

\ Inline helper functions for src and token
$inline $FN read #3$h1 @D_comp_read1$L0 %DV@D_comp$h1 %RET \ { -> numRead}
$inline $FN tokenPlc    #3$h1 .2%FTGL@G_src@Fs_plc^ADD$h2 %RET
$inline $FN tokenPlcSet #3$h1 .2%SRGL@G_src@Fs_plc^ADD$h2 %RET
$inline $FN tokenLen    #3$h1 .2%FTGL@G_src@Fs_buf^ADD@Buf_len^ADD$h2 %RET
$inline $FN tokenLenSet #3$h1 .2%SRGL@G_src@Fs_buf^ADD@Buf_len^ADD$h2 %RET
$inline $FN tokenDat #3$h1 .2%FTGL@G_src@Fs_buf^ADD@Buf_dat^ADD$h2 %RET
$STORE_PUB

     $FN isEof       $tokenPlc %NOT %RET
     $FN assertToken $tokenPlc @E_cNeedToken$L2 $_jmp assert
$pre $FN assertNoEof @E_eof$L2 $_jmp assert \ {numRead}

$FN scanNoEof  $scan  $_xsl isEof  @E_eof$L2 $_jmp assertNot

$FN peekChr \ {} -> {c} peek at a character
  $scan   $_xsl isEof $IF  #0$L0 %RET  $END
  $tokenDat .1%FT \ {c}
  #0$L0 $tokenPlc %RET \ reset scanner for next scan

$FN readNew \ { -> numRead} clear token buf and read bytes
  #0$L0 $tokenPlcSet  #0$L0 $tokenLenSet  $read  %RET

\ {&key szInstr szLit instr} compile a literal memory instr.
\   szLit the size of the literal to compile for the instr.
$pre $large $FN instrLitImpl
  @RSIZE$h1 \ 1 slot [szLit:U1 instr:U1]
  .1%SRLL #1$h1 \ var instr          {&key szInstr szLit}
  .1%SRLL #0$h1 \ var szLit          {&key szInstr}
  .1%FTLL #1$h1 %JN  $_xsl h1 \ compile (szInstr | instr) {&key}
  $d_vGet \ {oRef} offset or reference
  .1%FTLL #0$h1  $_jmp hN \ compile literal of proper instrSz

$pre $FN c_fn \ {&key}: compile a function of any type
  %DUP $_xsl assertFn \ {&key}
  %DUP $_xsl isFnInline $IF \ Inline compilation {&key}
    %DUP $_xsl isFnLarge @E_cInlineLarge$L2 $_xsl assertNot
    $d_vGet  $heap        \ {&heap &inlineFn}
    %DUP %INC %SWP .1%FT  \ {&heap &inlineFn+1 inlineLen}
    %DUP #0$L0 $bump %DRP \ {&heap &inlineFn+1 inlineLen} update heap
    %DV@D_memMove$h1 %RET \ {&inlineFn}
  $END
  %DUP $_xsl xSzI     \ {&key szLit}
  %OVR $_xsl isFnLarge  $IF @XLL$L1 $ELSE @XSL$L1 $END \ {&key instrSzI instr}
  %OVR %SWP \ {&key instrSzI litSzI instr} instr & lit are same sz
  .2%XLL @instrLitImpl$h2 %RET

$FN execute \ {&key} -> {...}: execute a dictionary key
  %DUP $_xsl isFnInline $IF \ if inline, assert not large and jmp to addr+1
    %DUP $_xsl isFnLarge @E_cInlineLarge$L2 $_xsl assertNot
    .R%FT %INC .R%JMPW
  $END
  %DUP $_xsl isFnLarge  $IF .R%FT .R%XLW %RET $END
  .R%FT .R%JMPW

$FN colon \ consume a colon token as syntactic surgar, i.e. xx:foo
  $scan $tokenPlc #1$L0  %EQ @E_cColon$L2 $_xsl assert \ assert len=1
  $tokenDat .1%FT #3A$L0 %EQ @E_cColon$L2 $_jmp assert \ assert ":"

$large $FN _xxPre
  @RSIZE$h1 $_xsl assertNoNow \ locals 0=&key
  $_xsl colon  #0$L0 $_xsl dictRef  %DUP .R%SRLL#0$h1 \ {&key}
  \ if fn is (PRE or SYN) and a compFn exists, compile next token.
  %DUP $_xsl isFnPre %SWP $_xsl isFnSyn %OR .R%FTGL@G_compFn$h2 %AND $IF
    .R%FTGL@G_compFn$h2 %XLW
  $END .R%FTLL #0$h1 %RET

\ $syn $FN xx .2%XLL@_xxPre$h2 $_jmp c_fn
\ $syn $FN jmp
\   $xx:_xxPre
\   %DUP
\   $xx:isFnLarge @E_cXHasL$L2
\   $xx:assertNot
\   .R%FT @JMPL2$L1 $_jmp _j2

$assertNoWs