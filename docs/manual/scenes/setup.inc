# A freshly booted board with two saved modules (shared by the scenes).
module DEMO \\ Squares and cubes\n: SQUARE ( n -- n*n ) DUP * ;\n: CUBE ( n -- n^3 ) DUP SQUARE * ;\n
module GREET \\ Greetings\n: HELLO ( -- ) ." Hello from VIOS" CR ;\n: STARS ( n -- ) 0 ?DO 42 EMIT LOOP ;\n
conraw boot confirmed A\n\nVIOS Forth 24 / Agon Light 2\n
