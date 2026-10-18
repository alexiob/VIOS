#!/usr/bin/env python3
"""Read the resident VDP transport keyboard state over ZDI (debug pause only).

Address/offsets come from the exact flashed ELF; pass --rx from its symbol map.
"""
import argparse
import json
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient


def read(c, rx):
    b = bytes.fromhex(c.tool('memory_read', {'address': rx + 548, 'count': 1211 - 548})['hex'])
    c.tool('resume')
    at = lambda o: b[o - 548]
    used = at(972)
    return {'ring_head': at(548) | at(549) << 8, 'ring_tail': at(550) | at(551) << 8,
            'dropped': at(832) | at(833) << 8 | at(834) << 16, 'key_count': at(964), 'key_route': at(965),
            'key_dropped': at(966) | at(967) << 8, 'line_mode': at(969), 'host_open': at(970),
            'local_pending': at(971), 'local_used': used, 'local': b[973 - 548:973 - 548 + used].decode('ascii', 'replace'),
            'local_dropped': at(1208) | at(1209) << 8}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rx', type=lambda v: int(v, 0), required=True)
    a = p.parse_args()
    with RPCClient(timeout=60) as c:
        c.initialize()
        print(json.dumps(read(c, a.rx)))
