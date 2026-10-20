#!/usr/bin/env python3
"""Capture untethered cold boots (U6) from the USB port, one JSON per boot.

docs/superpowers/specs/2026-10-19-vios-untethered-boot.md §16.10. Read-only:
never sends to the board unless --poll-b is given (then only the control
command `B`, which changes nothing), never writes flash or the SD card.

Power-on is seen from the host as the board's USB serial port coming back:

  vanish  (default) the board is powered through its USB port: wait for the
          port to disappear (power off) and come back (power on). t=0 is the
          moment the device node reappears; the port is opened right after,
          with DTR/RTS held low.
  open    the port stays present across the power cycle (separate supply):
          open it first, then t=0 is the first byte after >= 1 s of silence
          (the ESP ROM prints at 115200 when it powers up).

Every channel 3 event (BOOT ..., HEALTH ..., MOS: ... lines that MOS and the
stub print) and every channel 1 console line is kept with its time. A boot is
READY at `HEALTH flag 1` (the moment the ESP's `B` status turns to BOOT READY,
vios_bridge.h). After READY the capture keeps listening for --after seconds.

  vios_coldboot_capture.py OUTDIR [--runs N] [--first K] [--label NAME]
                           [--mode vanish|open] [--timeout S] [--after S]
                           [--poll-b S] [--port DEV]

Writes OUTDIR/NAME-NN.json per boot and OUTDIR/NAME-summary.json.
"""
import argparse
import json
import os
from pathlib import Path
import statistics
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vios_wire import MAX_FRAME, Packet, decode, encode  # noqa: E402

PORT = '/dev/cu.usbserial-224130'

# (key, channel, substring): the first event or console line containing it.
MILESTONES = (
    ('esp_banner', 3, 'BOOT VIOS VDP'),
    ('firmware', 3, 'FIRMWARE '),
    ('user_mode', 3, 'BOOT user mode'),
    ('dev_mode', 3, 'DISCOVER RAM'),
    ('mos_answered', 3, 'BOOT link: MOS answered'),
    ('mos_banner', 3, 'MOS Version'),
    ('stub_rate', 3, 'BOOT link: stub switched'),
    ('stub_loading', 3, 'BOOT stub loading'),
    ('stub_fallback', 3, 'VIOSBOOT: no ESP image'),
    ('stub_message', 3, 'VIOSBOOT:'),
    ('health0', 3, 'HEALTH flag 0'),
    ('ready', 3, 'HEALTH flag 1'),
    ('deadline', 3, 'deadline'),
    ('boot_confirmed', 1, 'boot confirmed'),
    ('forth_banner', 1, 'VIOS Forth'),
)


def open_port(port):
    import serial
    link = serial.Serial(port=None, baudrate=115200, timeout=0.05, exclusive=True)
    link.port, link.dtr, link.rts = port, False, False
    link.open()
    return link


class Run:
    def __init__(self, index, label, mode):
        self.record = {'run': index, 'label': label, 'mode': mode, 'wall_start': None,
                       't0': 'usb port reappeared' if mode == 'vanish' else 'first byte after >= 1 s silence',
                       'opened_at': None, 'first_byte': None, 'raw_bytes': 0, 'frame_errors': 0,
                       'milestones': {}, 'events': [], 'console': [], 'outcome': None}
        self.t0 = None
        self.buffer = bytearray()
        self.console = bytearray()

    def now(self):
        return round(time.monotonic() - self.t0, 3)

    def start(self, t0):
        self.t0 = t0
        self.record['wall_start'] = time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime())

    def mark(self, channel, text):
        for key, ch, needle in MILESTONES:
            if ch == channel and key not in self.record['milestones'] and needle in text:
                self.record['milestones'][key] = self.now()

    def feed(self, data):
        if not data:
            return
        if self.record['first_byte'] is None:
            self.record['first_byte'] = self.now()
        self.record['raw_bytes'] += len(data)
        for packet in self.packets(data):
            if packet.channel == 3:
                for line in packet.payload.decode('ascii', 'replace').splitlines():
                    if line.strip():
                        self.record['events'].append([self.now(), line.strip()])
                        self.mark(3, line)
                        print(f'  {self.now():7.3f}  {line.strip()}', flush=True)
            elif packet.channel == 1:
                self.console.extend(packet.payload)
                while b'\n' in self.console:
                    line, _, rest = self.console.partition(b'\n')
                    self.console = bytearray(rest)
                    text = line.decode('ascii', 'replace').strip()
                    if text:
                        self.record['console'].append([self.now(), text])
                        self.mark(1, text)
            elif packet.channel == 2:
                self.record['events'].append([self.now(), 'reply: ' + packet.payload.decode('ascii', 'replace').strip()])

    def packets(self, data):
        """Frames end with a zero byte only, so power-on noise (the ESP ROM's
        text) runs into the first frame: on a bad frame, try its suffixes."""
        out = []
        for value in data:
            if value:
                self.buffer.append(value)
                continue
            chunk, self.buffer = bytes(self.buffer[-MAX_FRAME:]), bytearray()
            for start in range(len(chunk)):
                try:
                    out.append(decode(chunk[start:]))
                    break
                except ValueError:
                    continue
            else:
                start = len(chunk)
            if start:  # bytes that were not part of a frame
                self.record['frame_errors'] += 1
        return out


def wait_power_cycle(port, run):
    """vanish mode: returns the t0 of power-on (the port node reappearing)."""
    if os.path.exists(port):
        print('  waiting for power OFF (unplug / switch off the board)', flush=True)
        while os.path.exists(port):
            time.sleep(0.02)
    print('  power is off; waiting for power ON', flush=True)
    while not os.path.exists(port):
        time.sleep(0.005)
    return time.monotonic()


def capture(args, index):
    run = Run(index, args.label, args.mode)
    link = None
    if args.mode == 'vanish':
        run.start(wait_power_cycle(args.port, run))
        for _ in range(200):
            try:
                link = open_port(args.port)
                break
            except Exception:
                time.sleep(0.01)
        if link is None:
            run.record['outcome'] = 'could not open the port'
            run.record['class'] = classify(run.record)
            return run.record
        run.record['opened_at'] = run.now()
    else:
        link = open_port(args.port)
        print('  port open; power-cycle the board now', flush=True)
        quiet_since = time.monotonic()
        while True:
            data = link.read(512)
            if data and time.monotonic() - quiet_since >= 1.0:
                run.start(time.monotonic())
                run.record['opened_at'] = None
                run.feed(data)
                break
            if data:
                quiet_since = time.monotonic()
    ready_at, next_poll = None, (args.poll_b if args.poll_b else None)
    sequence = 0
    try:
        while True:
            t = run.now()
            if ready_at is None and 'ready' in run.record['milestones']:
                ready_at = t
            if ready_at is not None and t - ready_at >= args.after:
                run.record['outcome'] = 'ready'
                break
            if ready_at is None and t >= args.timeout:
                run.record['outcome'] = 'timeout'
                break
            if next_poll is not None and t >= next_poll:
                sequence = sequence % 250 + 1
                link.write(encode(Packet(2, sequence, b'B\n')))
                next_poll = t + args.poll_b
            run.feed(link.read(512))
    except Exception as error:  # the port vanished: power was cut mid-capture
        run.record['outcome'] = 'port lost: ' + type(error).__name__
    finally:
        try:
            link.close()
        except Exception:
            pass
    run.record['class'] = classify(run.record)
    return run.record


def classify(record):
    """ready: READY seen (the port may drop afterwards, at the next unplug).
    interrupted: the port dropped before the ESP's first event, so power was on
    too briefly for a boot (a bouncing connector); not counted as a boot.
    failed: anything else."""
    if 'ready' in record['milestones']:
        return 'ready'
    if 'esp_banner' not in record['milestones'] and not record['outcome'].startswith('timeout'):
        return 'interrupted'
    return 'failed'


def summarize(records):
    records = [r for r in records if r.setdefault('class', classify(r)) != 'interrupted'] + \
        [r for r in records if r['class'] == 'interrupted']
    boots = [r for r in records if r['class'] != 'interrupted']
    ready = [r['milestones']['ready'] for r in boots if r['class'] == 'ready']
    out = {'boots': len(boots), 'ready': len(ready), 'failed': [r['run'] for r in boots if r['class'] == 'failed'],
           'interrupted_power_ons': [r['run'] for r in records if r['class'] == 'interrupted']}
    records = boots
    if ready:
        out['ready_s'] = {'min': min(ready), 'median': round(statistics.median(ready), 3), 'max': max(ready),
                          'mean': round(statistics.mean(ready), 3)}
    keys = [k for k, _, _ in MILESTONES]
    out['milestone_median_s'] = {k: round(statistics.median(v), 3) for k in keys
                                 if (v := [r['milestones'][k] for r in records if k in r['milestones']])}
    out['per_run'] = [{'run': r['run'], 'class': r['class'], 'outcome': r['outcome'], 'ready': r['milestones'].get('ready'),
                       'firmware': next((e[1] for e in r['events'] if e[1].startswith('FIRMWARE')), None),
                       'fallback': 'stub_fallback' in r['milestones']} for r in records]
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('outdir', type=Path)
    parser.add_argument('--runs', type=int, default=1)
    parser.add_argument('--first', type=int, default=1, help='index of the first run (to resume a series)')
    parser.add_argument('--label', default='coldboot')
    parser.add_argument('--mode', choices=('vanish', 'open'), default='vanish')
    parser.add_argument('--timeout', type=float, default=60.0, help='seconds from t0 without READY before giving up')
    parser.add_argument('--after', type=float, default=3.0, help='seconds to keep listening after READY')
    parser.add_argument('--poll-b', type=float, default=0.0, help='send the read-only B status command every S seconds')
    parser.add_argument('--port', default=PORT)
    args = parser.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)
    index, counted = args.first, 0
    while counted < args.runs:
        print(f'[{args.label} run {index}, boot {counted + 1}/{args.runs}]', flush=True)
        record = capture(args, index)
        (args.outdir / f'{args.label}-{index:02d}.json').write_text(json.dumps(record, indent=1) + '\n')
        m = record['milestones']
        print(f'  -> {record["class"]} ({record["outcome"]}): ready {m.get("ready")} s, stub loading '
              f'{m.get("stub_loading")} s, fallback {"yes" if "stub_fallback" in m else "no"}', flush=True)
        counted += record['class'] != 'interrupted'
        index += 1
    # The summary covers the whole series in OUTDIR, earlier invocations included.
    records = [json.loads(path.read_text()) for path in sorted(args.outdir.glob(f'{args.label}-[0-9][0-9].json'))]
    summary = summarize(records)
    (args.outdir / f'{args.label}-summary.json').write_text(json.dumps(summary, indent=1) + '\n')
    print(json.dumps({k: v for k, v in summary.items() if k != 'per_run'}, indent=1))
    return 0 if summary['ready'] == summary['boots'] else 1


if __name__ == '__main__':
    sys.exit(main())
