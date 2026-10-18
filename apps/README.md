# VIOS example applications

`hello-autorun.fth` demonstrates booting into an application from SD:

1. Stage and activate it as a module, e.g. with the harness `develop` tool
   (`name` HELLO, `version` 1, `source` the file contents).
2. `boot_trial` writes all active modules to the inactive source slot; a
   reboot tests it and `boot_confirm` keeps it. `boot_revert` goes back.
3. On boot, `AUTORUN` starts as a console task (`SPAWN-CON`).

Steps 2-3 change the SD boot slot and are only done with the owner's approval.
