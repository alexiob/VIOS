/* Core library words written in Forth, evaluated once per runtime start.
 * Replaceable like any definition; keeping them out of forth_init lets
 * small ports build forth/system.c with tiny dictionaries. */
#include "vios/runtime.h"
static const char prelude[]=
 ": > SWAP < ; : 0< 0 < ; : 0> 0 SWAP < ; : <> = 0= ; : U> SWAP U< ; "
 ": ROT >R SWAP R> SWAP ; : -ROT ROT ROT ; : NIP SWAP DROP ; : TUCK SWAP OVER ; "
 ": ?DUP DUP IF DUP THEN ; : ABS DUP 0< IF NEGATE THEN ; "
 ": MAX 2DUP < IF SWAP THEN DROP ; : MIN 2DUP SWAP < IF SWAP THEN DROP ; "
 ": 2SWAP ROT >R ROT R> ; : 2OVER >R >R 2DUP R> R> 2SWAP ; "
 ": +! DUP @ ROT + SWAP ! ; : 2* 1 LSHIFT ; : BL 32 ; : SPACE BL EMIT ; "
 ": SPACES BEGIN DUP 0> WHILE SPACE 1- REPEAT DROP ; "
 ": COUNT DUP 1+ SWAP C@ ; : WITHIN OVER - >R - R> U< ; "
 ": ? @ . ; : ERASE 0 FILL ; : /MOD 2DUP MOD -ROT / ; : */ >R * R> / ; : BOUNDS OVER + SWAP ; "
 ": .HEX2 DUP 16 < IF 48 EMIT THEN . ; "
 ": DUMP BASE @ >R HEX BOUNDS ?DO I 15 AND 0= IF CR I . SPACE THEN I C@ .HEX2 LOOP CR R> BASE ! ; ";
int vios_prelude_init(void){return forth_eval(prelude);}
