"""Mac side of the VIOS host-file service (USB channel 4).

The board sends one request per VIOS handle operation and waits ~1 s for one
reply. Requests: O<r|w><path>, R<h><n>, W<h><bytes>, C<h>. Replies:
K<data> or E<message>, at most 240 bytes. Files live under one sandbox
directory; absolute paths, '..' and symlink escapes are refused.
"""
from pathlib import Path

MAX_REPLY = 240
MAX_OPEN = 4


class HostService:
    def __init__(self, root):
        self.root = Path(root).expanduser().resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.files = [None] * MAX_OPEN
        self.requests = 0

    def _path(self, name):
        if not name or name.startswith('/') or '\\' in name or any(part in ('', '.', '..') for part in name.split('/')):
            raise PermissionError('forbidden path')
        path = (self.root / name).resolve()
        if self.root != path and self.root not in path.parents:
            raise PermissionError('forbidden path')
        return path

    def handle(self, request):
        """Return the reply bytes for one request (never raises)."""
        self.requests += 1
        try:
            return self._handle(bytes(request))[:MAX_REPLY]
        except PermissionError as error:
            return b'E' + str(error).encode('ascii', 'replace')
        except FileNotFoundError:
            return b'Eno such host file'
        except OSError as error:
            return b'E' + (error.strerror or 'host I/O error').encode('ascii', 'replace')

    def _handle(self, r):
        if not r:
            return b'Eempty request'
        op = r[:1]
        if op == b'O':
            if len(r) < 3 or r[1:2] not in (b'r', b'w'):
                return b'Ebad open request'
            path = self._path(r[2:].decode('ascii'))
            slot = next((i for i, f in enumerate(self.files) if f is None), None)
            if slot is None:
                return b'Etoo many host files'
            if r[1:2] == b'w':
                path.parent.mkdir(parents=True, exist_ok=True)
            self.files[slot] = open(path, 'wb' if r[1:2] == b'w' else 'rb')
            return b'K' + bytes([slot])
        if len(r) < 2 or r[1] >= MAX_OPEN or self.files[r[1]] is None:
            return b'Ebad host handle'
        f = self.files[r[1]]
        if op == b'R':
            want = min(r[2] if len(r) > 2 else 0, MAX_REPLY - 1)
            return b'K' + f.read(want)
        if op == b'W':
            return b'K' + bytes([f.write(r[2:])])
        if op == b'C':
            f.close()
            self.files[r[1]] = None
            return b'K'
        return b'Eunknown request'

    def close_all(self):
        for i, f in enumerate(self.files):
            if f is not None:
                f.close()
                self.files[i] = None
