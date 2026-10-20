#!/usr/bin/env python3
"""On-board acceptance for the GUI development tools (kernel/vios_tools.c).

Runs through the harness (127.0.0.1:8765) on a board booted into the GUI. All
keys are injected at the ESP window manager (esp_gui), as a keyboard would
press them; the CPU only sees semantic events after the display server's nudge.
  browser    Ctrl+Shift+B opens it; Down selects a module; its source arrives in
             the code pane (read back with UI-TEXT@); key-to-window latency
  workspace  Ctrl+Shift+W; typed text, Ctrl+P prints the result into the text,
             Ctrl+[ / Ctrl+] indent and de-indent
  inspector  Ctrl+Shift+I; tabs by Ctrl+Tab; memory pager; tasks report
  manager    Ctrl+Shift+M; boot report; an action asks first and Esc cancels
             (no boot slot is changed)
  speed      TextView read-back of 2000 bytes (UI-TEXT@): seconds and bytes/s
  health     no protocol errors, frame budgets
Uses only the tool windows 20-24; closes them at the end.
"""
import argparse
import json
import re
from pathlib import Path
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient

CTRL, SHIFT = 1, 2
VK_A, VK_TAB, VK_RETURN, VK_ESCAPE, VK_DOWN, VK_UP, VK_HOME, VK_END = 22, 142, 143, 125, 152, 150, 133, 135
B_MODULES, B_CODE, W_TEXT, I_TABS, I_TABLE, I_DETAIL, M_DETAIL = 42, 45, 48, 50, 51, 52, 56


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    r = {'status': 'running', 'checks': {}}

    def check(name, ok, detail=None):
        r['checks'][name] = {'ok': bool(ok), 'detail': detail}
        if not ok:
            raise AssertionError(f'{name}: {detail}')
    try:
        with RPCClient(timeout=120) as c:
            c.initialize()
            c.tool('connect', {})

            def ev(source):
                out = c.tool('evaluate', {'source': source, 'timeout': 60})
                if not out['ok']:
                    raise AssertionError(f'{source!r}: {out["output"][-200:]}')
                return out['output']

            def result(source):
                # Background tasks (an AUTORUN heartbeat) may print between the echo and the answer.
                lines = [x for x in ev(source).split('\r\n') if 'heartbeat' not in x]
                for i, line in enumerate(lines):
                    if line.strip() == source.strip() and i + 1 < len(lines):
                        return lines[i + 1]
                raise AssertionError(f'no echo of {source!r}: {lines}')

            def press(ascii=0, mods=0, vk=0, settle=0.25):
                used = c.tool('esp_gui', {'vk': vk, 'ascii': ascii, 'mods': mods})['used']
                time.sleep(settle)          # the CPU acts from its idle loop after the nudge
                ev('')                      # one REPL round trip: idle has run
                return used

            def letter(ch, mods):
                return press(ord(ch) - ord('a') + 1 if mods & CTRL else ord(ch), mods, VK_A + ord(ch) - ord('a'))

            def text(view, limit=2000):
                return result(f'{view} GT-BUF {limit} UI-TEXT@ GT-BUF SWAP TYPE')

            def status():
                return c.tool('esp_gui')['status']

            sections, worst = {}, {'vblank_start_max': 0, 'frame_work_max': 0}

            def section(name):
                """Frame stats since the previous section (reset each time), so a spike names its step."""
                gui = c.tool('esp_gui')['status']   # before the stats reset below (which clears frame_wait); resets its own maxima
                st = c.tool('esp_stats', {'reset': True})
                m = re.search(r'frame_wait (\d+) by (\S+)(?: at ([0-9a-f]+))?', gui)
                if m:
                    st['frame_wait'], st['frame_wait_by'], st['frame_wait_at'] = int(m.group(1)), m.group(2), m.group(3)
                for key in ('batch_max_us', 'ops_us', 'after_us', 'style_us', 'hold_us', 'wait_us', 'key_us'):
                    m = re.search(rf'\b{key} (\d+)', gui)
                    if m:
                        st[key] = int(m.group(1))
                sections[name] = {k: st.get(k) for k in ('vblank_start_max', 'frame_work_max', 'frame_lock_max', 'frame_gui_max',
                                                         'present_frames', 'present_work_max', 'keyboard_max', 'parser_max',
                                                         # newer firmware: the worst vblank start's cause, the longest display-lock hold
                                                         'late_us', 'late_cause', 'late_prev_kind', 'late_prev_us', 'feed_max', 'key_max',
                                                         'batch_max_us', 'ops_us', 'after_us', 'style_us', 'hold_us', 'wait_us', 'key_us', 'frame_wait', 'frame_wait_by', 'frame_wait_at')}
                for k in worst:
                    worst[k] = max(worst[k], st.get(k) or 0)

            def close_all():                  # -1 TOOL (newer firmware); older: nothing to do on a clean boot
                out = c.tool('evaluate', {'source': '-1 TOOL', 'timeout': 30})
                return out['ok']
            r['close_all_word'] = close_all()  # a clean start: no tool window open
            time.sleep(0.5)
            # 2 KB of data space (of 8): a failed CREATE ... ALLOT would leave a GT-BUF header
            # pointing past the data space (Forth core), so never risk it.
            if not c.tool('evaluate', {'source': "' GT-BUF DROP", 'timeout': 20})['ok']:   # a second run reuses it
                ev('CREATE GT-BUF 2048 ALLOT')
            check('gui_on', status().startswith('GUI on'), status())
            c.tool('esp_stats', {'reset': True})

            # Browser.
            spot = [[40, 40]]
            desk = c.tool('esp_gui', {'pixels': spot})['pixels']
            t = time.perf_counter()
            check('browser_key', letter('b', CTRL | SHIFT))
            r['browser_open_s'] = round(time.perf_counter() - t, 3)
            time.sleep(0.3)
            drawn = c.tool('esp_gui', {'pixels': spot})['pixels']
            check('browser_window_drawn', drawn != desk, (desk, drawn))
            modules = result('MODULES')
            r['modules'] = modules
            press(vk=VK_DOWN); press(vk=VK_DOWN)
            sel = result(f'{B_MODULES} GT-BUF UI-WGET DROP .').split()
            check('module_selected', sel and sel[0] == '2', sel)
            code = text(B_CODE)
            r['browser_code_head'] = code[:80]
            check('module_source_shown', len(code) > 0 or 'none' in modules.lower(), (code[:60], modules[:60]))

            section('browser')
            # Workspace: print it, indentation.
            check('workspace_key', letter('w', CTRL | SHIFT))
            press(vk=VK_END, mods=CTRL)
            for ch in '6 7 *':
                press(ord(ch), settle=0.02)
            letter('p', CTRL)
            ws = text(W_TEXT)
            r['workspace_text'] = ws
            check('print_it', '6 7 * 42' in ws, ws)
            press(vk=VK_HOME)
            press(27, CTRL, 0)
            check('indent', '  6 7 * 42' in text(W_TEXT), text(W_TEXT))
            press(29, CTRL, 0)
            check('deindent', '\n6 7 * 42' in text(W_TEXT) or text(W_TEXT).startswith('6 7 *'), text(W_TEXT))
            ev('DROP')

            section('workspace')
            # Inspector: memory pager, tasks.
            check('inspector_key', letter('i', CTRL | SHIFT))
            press(9, CTRL, VK_TAB)                      # Memory
            press(vk=VK_UP)                             # past the top: a page event
            tabs = result(f'{I_TABS} GT-BUF UI-WGET DROP .').split()
            check('memory_tab', tabs and tabs[0] == '1', tabs)
            press(9, CTRL, VK_TAB)                      # Tasks
            tasks = text(I_DETAIL)
            r['tasks_report'] = tasks[:200]
            check('tasks_report', 'task' in tasks.lower(), tasks[:120])
            press(9, CTRL, VK_TAB); press(9, CTRL, VK_TAB)   # System
            system = text(I_DETAIL)
            r['system_report'] = system
            check('system_report', 'display epoch' in system, system[:200])

            section('inspector')
            # Manager: report and a cancelled action.
            check('manager_key', letter('m', CTRL | SHIFT))
            boot = text(M_DETAIL)
            r['boot_report'] = boot[:300]
            check('boot_report', 'Enter: run' in boot, boot[:200])
            press(9, 0, VK_TAB)                         # tabs -> action list
            press(vk=VK_RETURN)                         # asks first
            st = status()
            r['dialog_status'] = st
            press(vk=VK_ESCAPE)                         # cancels: nothing changes

            section('manager')
            # TextView read-back speed (open decision 2).
            ev(f'GT-BUF 2000 CHAR x FILL  GT-BUF 2000 {W_TEXT} UI-TEXT')
            t = time.perf_counter()
            n = result(f'{W_TEXT} GT-BUF 2000 UI-TEXT@ .').split()
            dt = time.perf_counter() - t
            r['readback'] = {'bytes': int(n[0]) if n else 0, 'seconds': round(dt, 3), 'bytes_per_s': round(2000 / dt) if dt else None}
            check('readback_complete', n and n[0] == '2000', n)

            section('readback')
            # Close a tool with Ctrl+W, then the rest.
            letter('w', CTRL)
            if not close_all():
                for _ in range(4):
                    letter('w', CTRL)
            sync = result('UI-SYNC . . .').split()
            r['sync'] = sync
            check('no_protocol_errors', sync and sync[0] == '0', sync)
            section('close')
            r['sections'] = sections
            s = dict(worst)
            r['frames'] = worst
            r['gui'] = status()
            r['resources'] = result('RESOURCES')
            check('frame_budget', s['frame_work_max'] <= 6000 and s['vblank_start_max'] <= 1000, r['frames'])
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps({k: r.get(k) for k in ('status', 'error', 'readback', 'browser_open_s')}))


if __name__ == '__main__':
    main()
