\ HELLO: example application module for the VIOS AUTORUN boot convention.
\ Installed as a module and written to a boot trial, it starts as a
\ console-attached background task after reboot; the REPL stays usable and
\ TASK-KILL stops it. Prints a greeting, then a heartbeat every 10 seconds.
VARIABLE HELLO-BEATS
: HELLO-BEAT  1 HELLO-BEATS +!  ." hello: heartbeat " HELLO-BEATS @ . CR ;
: AUTORUN  0 HELLO-BEATS !  ." hello: VIOS application started (TASKS / TASK-KILL)" CR
  BEGIN 10000 MS HELLO-BEAT AGAIN ;
