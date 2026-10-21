#!/usr/bin/env python3
"""On-board acceptance for /dev/win and /dev/ui/tree (kernel/vios_win.c over
the display server's files, firmware lib/vios-vds/src/vds_files.c).

Runs through the harness (127.0.0.1:8765) on a board booted into the GUI.
  make       /dev/win/new makes a window, a button, a list and a label; the
             read-back gives the window's id
  list       /dev/win lists them (id kind parent name rect), also when read by
             a background task (requests inside a task slice)
  ctl        commands (rect, text, show, focus) change the server's state and
             reading ctl shows it
  text       list rows and label text written and read back
  events     a task blocked in READ-WAIT on /dev/win/N/event wakes with
             "activate" when Enter is pressed on the button through the ESP
             window manager (esp_gui), and gets "select 1" from Down in the
             list; key-to-task latency
  tree       /dev/ui/tree (UI-SNAPSHOT) has the source of the window
  health     the window closes through ctl; no handles leak; no new protocol
             errors; no console drops
Defines temporary RAM words; uses the lowest free window and surface ids, and
closes its window at the end. Touches no files, boot slots or tool windows.
"""
import argparse
import json
from pathlib import Path
import re
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient

VK_RETURN, VK_DOWN = 143, 152


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    r = {'status': 'running', 'checks': {}, 'ms': {}}
    win_open = None

    def check(name, ok, detail=None):
        r['checks'][name] = {'ok': bool(ok), 'detail': detail}
        if not ok:
            raise AssertionError(f'{name}: {detail}')
    try:
        with RPCClient(timeout=150) as c:
            c.initialize()
            c.tool('connect', {})

            def ev(source, timeout=60):
                out = c.tool('evaluate', {'source': source, 'timeout': timeout})
                if not out['ok']:
                    raise AssertionError(f'{source!r}: {out["output"][-300:]}')
                return out['output']

            def result(source):
                # Background output (an AUTORUN heartbeat) may come between the echo and the answer.
                lines = [x for x in ev(source).split('\r\n') if 'heartbeat' not in x]
                for i, line in enumerate(lines):
                    if line.strip() == source.strip() and i + 1 < len(lines):
                        return '\r\n'.join(lines[i + 1:])
                raise AssertionError(f'no echo of {source!r}: {lines}')

            def val(source):
                return int(next(t for t in result(source + ' .').split() if re.fullmatch(r'-?\d+', t)))

            def signed(v):
                v &= 0xFFFFFF
                return v - (1 << 24) if v & 0x800000 else v

            def typed(count_source):
                # WB's first n bytes as the console shows them, newlines as \n.
                got = result(f'WB {val(count_source)} TYPE 126 EMIT')
                return got[:got.rindex('~')].replace('\r\n', '\n')

            def slurp(path):
                # WB holds the file; ~ marks the end, as the console may add ok and line ends.
                text = result(f'S" {path}" WSHOW')
                return text[:text.rindex('~')].replace('\r\n', '\n')

            def spit(path, text):
                assert '"' not in text and len(text) < 200
                ev(f'S" {text}" WB SWAP MOVE')
                return signed(val(f'WB {len(text)} S" {path}" WSPIT'))

            def spit_lines(path, lines):
                # Newlines are put into WB one line at a time.
                at = 0
                for line in lines:
                    ev(f'S" {line}" WB {at} + SWAP MOVE  10 WB {at + len(line)} + C!')
                    at += len(line) + 1
                return signed(val(f'WB {at} S" {path}" WSPIT'))

            drops = lambda: int(re.search(r'console-dropped (\d+)', ev('VDP-INFO'))[1])
            r['dropped_before'] = drops()
            r['errors_before'] = val('UI-SYNC SWAP DROP SWAP DROP')
            ev('CREATE WB 1024 ALLOT  VARIABLE WT  VARIABLE WN  VARIABLE WL')
            ev(': WSLURP ( pa pu -- n ) 1 OPEN DUP 0 < IF EXIT THEN >R WB 1024 R@ READ R> CLOSE DROP ;')
            ev(': WSPIT ( a u pa pu -- r ) 2 OPEN DUP 0 < IF >R 2DROP R> EXIT THEN >R R@ WRITE R> CLOSE DROP ;')
            ev(': WSHOW ( pa pu -- ) WSLURP DUP 0 < IF . ELSE WB SWAP TYPE THEN 126 EMIT ;')

            # make
            t = time.perf_counter()
            check('make_window', spit('/dev/win/new', 'window - Win accept') == 19)
            r['ms']['make_window'] = round((time.perf_counter() - t) * 1000)
            win = slurp('/dev/win/new').strip()
            check('new_reads_id', re.fullmatch(r'w\d+', win), win)
            win_open = win
            for line in ('button %s ok' % win, 'list %s rows' % win, 'label %s msg' % win):
                check('make_' + line.split()[0], spit('/dev/win/new', line) > 0, line)
                r[line.split()[0]] = int(slurp('/dev/win/new').strip())
            button, lst, label = r['button'], r['list'], r['label']
            check('refused_kind', spit('/dev/win/new', 'gizmo %s x' % win) < 0)

            # list
            t = time.perf_counter()
            listing = slurp('/dev/win')
            r['ms']['list'] = round((time.perf_counter() - t) * 1000)
            r['listing'] = listing
            check('list_window', f'{win} window - Win_accept 64 64 32 12' in listing, listing)
            check('list_widgets', f'{button} button {win} ok ' in listing and f'{lst} list {win} rows ' in listing, listing)
            ev("0 WT !  : WLIST S\" /dev/win\" 1 OPEN >R WB 512 + 400 R@ READ WT ! R> CLOSE DROP ;  ' WLIST SPAWN DROP")
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline and not val('WT @'):
                time.sleep(0.2)
            n = val('WT @')
            from_task = result(f'WB 512 + {n} TYPE 126 EMIT')
            from_task = from_task[:from_task.rindex('~')].replace('\r\n', '\n')
            check('list_from_task', n > 0 and listing.startswith(from_task), (n, from_task))

            # ctl
            check('ctl_write', spit_lines(f'/dev/win/{win}/ctl', ['rect 96 80 36 14', 'text Win accept 2']) > 0)
            check('ctl_widget', spit_lines(f'/dev/win/{button}/ctl', ['rect 1 1 10 1', 'text Say &hello']) > 0
                  and spit_lines(f'/dev/win/{lst}/ctl', ['rect 1 3 20 5']) > 0 and spit(f'/dev/win/{label}/ctl', 'rect 1 9 30 1') > 0)
            ctl = slurp(f'/dev/win/{button}/ctl')
            check('ctl_read', 'rect 1 1 10 1\n' in ctl and 'text Say &hello\n' in ctl and f'parent {win}\n' in ctl, ctl)
            wctl = slurp(f'/dev/win/{win}/ctl')
            check('ctl_window', 'rect 96 80 36 14\n' in wctl and 'name Win accept 2\n' in wctl, wctl)
            check('ctl_hide_show', spit(f'/dev/win/{label}/ctl', 'show 0') > 0 and 'show 0\n' in slurp(f'/dev/win/{label}/ctl')
                  and spit(f'/dev/win/{label}/ctl', 'show 1') > 0)
            check('ctl_refuses', spit(f'/dev/win/{button}/ctl', 'bogus 1') < 0)

            # text
            check('text_rows', spit_lines(f'/dev/win/{lst}/text', ['alpha', 'beta', 'gamma']) > 0)
            check('text_rows_back', slurp(f'/dev/win/{lst}/text') == 'alpha\nbeta\ngamma\n', slurp(f'/dev/win/{lst}/text'))
            check('text_label', spit(f'/dev/win/{label}/text', 'Hello from /dev/win') > 0
                  and slurp(f'/dev/win/{label}/text') == 'Hello from /dev/win\n')

            # events: a task waits on the button's stream; Enter through the ESP window manager wakes it.
            ev(f'0 WN ! 0 WL !  : WWAIT S" /dev/win/{button}/event" 1 OPEN >R 1 WL ! WB 64 R@ READ-WAIT WN ! R> CLOSE DROP ;')
            check('focus', spit_lines(f'/dev/win/{win}/ctl', ['focus']) > 0 and spit(f'/dev/win/{button}/ctl', 'focus') > 0)
            ev("' WWAIT SPAWN DROP")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not val('WL @'):
                time.sleep(0.1)
            t = time.perf_counter()
            used = c.tool('esp_gui', {'vk': VK_RETURN, 'ascii': 13, 'mods': 0})['used']
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not val('WN @'):
                time.sleep(0.05)
            r['ms']['enter_to_task'] = round((time.perf_counter() - t) * 1000)
            got = typed('WN @')
            check('event_activate', used and got == 'activate\n', (used, got))
            ev(f'0 WN ! 0 WL !  : WSEL S" /dev/win/{lst}/event" 1 OPEN >R 1 WL ! WB 64 R@ READ-WAIT WN ! R> CLOSE DROP ;  ' "' WSEL SPAWN DROP")
            check('focus_list', spit(f'/dev/win/{lst}/ctl', 'focus') > 0)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not val('WL @'):
                time.sleep(0.1)
            c.tool('esp_gui', {'vk': VK_DOWN, 'ascii': 0, 'mods': 0})
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not val('WN @'):
                time.sleep(0.05)
            got = typed('WN @')
            check('event_select', got == 'select 1\n', got)

            # tree
            tree = result('UI-SNAPSHOT 126 EMIT')
            tree = tree[:tree.rindex('~')].replace('\r\n', '\n')
            check('tree', f'UI-WINDOW S" Win accept 2" {win[1:]} UI-TITLE' in tree and f'S" alpha" {lst} 0 UI-ITEM' in tree, tree[-600:])

            # health
            win_open = None
            check('close', spit(f'/dev/win/{win}/ctl', 'close') > 0 and f'{win} window' not in slurp('/dev/win'))
            listed = c.tool('handles')
            check('no_leaked_handles', listed['handles'] == [], listed['handles'])
            r['errors_after'] = val('UI-SYNC SWAP DROP SWAP DROP')
            check('no_protocol_errors', r['errors_after'] == r['errors_before'], (r['errors_before'], r['errors_after']))
            r['dropped_after'] = drops()
            check('no_console_drops', r['dropped_after'] == r['dropped_before'], (r['dropped_before'], r['dropped_after']))
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        try:                                    # leave no test window behind
            if win_open:
                with RPCClient(timeout=60) as c:
                    c.initialize()
                    c.tool('evaluate', {'source': f'S" close" WB SWAP MOVE  WB 5 S" /dev/win/{win_open}/ctl" WSPIT DROP', 'timeout': 20})
        except Exception:
            pass
        raise
    finally:
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps({k: r.get(k) for k in ('status', 'ms', 'error')}, indent=1)[:2000])


if __name__ == '__main__':
    main()
