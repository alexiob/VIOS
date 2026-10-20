"""Pause the board's autorun application around an on-board acceptance run.

Some checks need no live background task (ENGINE switches are refused while
tasks live; task counts must start at zero), but the board may run an
autorun app as a console task (HELLO's heartbeat). pause_autorun() stops a
live AUTORUN task and reports whether it did; resume_autorun() starts it
again (' AUTORUN SPAWN-CON) on a fresh connection, so it also works from a
finally block after the run's own connection is gone.
"""
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient


def pause_autorun(c):
    c.tool('connect', {})   # idempotent: 'already_connected' when the run connected first
    live = [t for t in c.tool('tasks')['tasks'] if t['name'] == 'AUTORUN' and t['state'] in ('ready', 'sleeping')]
    for t in live:
        c.tool('evaluate', {'source': f"{t['id']} TASK-KILL", 'timeout': 20})
    return bool(live)


def resume_autorun(paused):
    if not paused:
        return
    with RPCClient(timeout=60) as c:
        c.initialize()
        c.tool('connect', {})
        c.tool('evaluate', {'source': "' AUTORUN SPAWN-CON DROP", 'timeout': 20})
