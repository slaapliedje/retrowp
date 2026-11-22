# SPDX-License-Identifier: GPL-2.0-or-later
"""The gateway's document store (M21 T21.6): OFFLOAD_STORE, OFFLOAD_LIST
and OFFLOAD_FETCH.

Each machine's documents are kept apart, by the machine name its
requests carry, and each document keeps its last N versions:

    <store>/<machine>/<document>/000001
                                 000002 ...

A version is written to a temporary file, flushed to the disk (fsync,
then the directory's too) and only then renamed into place, so an
OFFLOAD_OK for a store means the copy is safe on the Pi: a power cut
after the answer loses nothing. The oldest versions past N are removed
after the new one is in.

Names are checked before they touch the file system: a machine or
document name is printable ASCII, at most 63 bytes, with no '/', '\\'
or ':', not starting with '.' or a space -- so neither "..", nor an
absolute path, nor a hidden file can be named, and every path built
from them stays inside the store. The payload shapes are in
docs/formats/offload-protocol.md.
"""

import os
import struct
import time

from wire_protocol import STATUS_OK, STATUS_ERR

NAME_MAX = 63
DEFAULT_VERSIONS = 5


def valid_name(raw):
    """The name as a str, or None when it may not be used as one."""
    if not 1 <= len(raw) <= NAME_MAX:
        return None
    if raw[0] in b'. ':
        return None
    for c in raw:
        if c < 0x20 or c > 0x7E or c in b'/\\:':
            return None
    return raw.decode('ascii')


def _take_name(payload, pos):
    """A u8-length-prefixed name at pos: (name or None, next pos)."""
    if pos >= len(payload):
        return None, pos
    n = payload[pos]
    raw = payload[pos + 1:pos + 1 + n]
    if len(raw) != n:
        return None, pos
    return valid_name(raw), pos + 1 + n


def _versions(doc_dir):
    """The version numbers kept for a document, oldest first."""
    try:
        names = os.listdir(doc_dir)
    except OSError:
        return []
    return sorted(int(n) for n in names if len(n) == 6 and n.isdigit())


def _when(path):
    return time.strftime('%Y-%m-%d %H:%M', time.localtime(os.stat(path).st_mtime))


def _fsync_dir(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


class DocStore:
    def __init__(self, root, versions=DEFAULT_VERSIONS):
        self.root = os.path.abspath(root)
        self.versions = max(1, versions)

    def _doc_dir(self, machine, name):
        return os.path.join(self.root, machine, name)

    def store(self, payload):
        """OFFLOAD_STORE: machine, document name, then the file's bytes."""
        machine, pos = _take_name(payload, 0)
        name, pos = _take_name(payload, pos)
        if machine is None or name is None:
            return STATUS_ERR, b'The Pi refused that name'
        body = payload[pos:]
        doc_dir = self._doc_dir(machine, name)
        try:
            os.makedirs(doc_dir, exist_ok=True)
            have = _versions(doc_dir)
            number = (have[-1] + 1) if have else 1
            final = os.path.join(doc_dir, '%06d' % number)
            tmp = os.path.join(doc_dir, '.incoming')
            with open(tmp, 'wb') as f:
                f.write(body)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, final)
            _fsync_dir(doc_dir)
            have.append(number)
            for old in have[:-self.versions]:
                os.remove(os.path.join(doc_dir, '%06d' % old))
        except OSError:
            return STATUS_ERR, b'The Pi could not keep the copy'
        kept = min(len(have), self.versions)
        line = 'Kept %s on the Pi (%d version%s)' % (name, kept, '' if kept == 1 else 's')
        return STATUS_OK, line.encode('ascii')

    def list(self, payload):
        """OFFLOAD_LIST: machine, and optionally a document name.

        Without a name, one line a document, newest first:
            name<TAB>versions<TAB>bytes<TAB>YYYY-MM-DD HH:MM<LF>
        (the size and time are its latest version's). With one, a line
        a version of it, newest first:
            version<TAB>bytes<TAB>YYYY-MM-DD HH:MM<LF>"""
        machine, pos = _take_name(payload, 0)
        if machine is None:
            return STATUS_ERR, b'The Pi refused that name'
        if pos < len(payload):
            name, pos = _take_name(payload, pos)
            if name is None:
                return STATUS_ERR, b'The Pi refused that name'
            doc_dir = self._doc_dir(machine, name)
            out = []
            for v in reversed(_versions(doc_dir)):
                p = os.path.join(doc_dir, '%06d' % v)
                out.append('%d\t%d\t%s\n' % (v, os.path.getsize(p), _when(p)))
            return STATUS_OK, ''.join(out).encode('ascii')
        mdir = os.path.join(self.root, machine)
        try:
            names = os.listdir(mdir)
        except OSError:
            names = []
        docs = []
        for n in names:
            if valid_name(n.encode('ascii', 'replace')) != n:
                continue
            have = _versions(os.path.join(mdir, n))
            if not have:
                continue
            p = os.path.join(mdir, n, '%06d' % have[-1])
            docs.append((os.stat(p).st_mtime, n, len(have), os.path.getsize(p), _when(p)))
        docs.sort(key=lambda d: (-d[0], d[1]))
        out = ''.join('%s\t%d\t%d\t%s\n' % (n, c, size, when) for _, n, c, size, when in docs)
        return STATUS_OK, out.encode('ascii')

    def fetch(self, payload):
        """OFFLOAD_FETCH: machine, document name, u32 BE version (0 =
        the latest) -> the file's bytes."""
        machine, pos = _take_name(payload, 0)
        name, pos = _take_name(payload, pos)
        if machine is None or name is None or len(payload) != pos + 4:
            return STATUS_ERR, b''
        (version,) = struct.unpack('>I', payload[pos:pos + 4])
        doc_dir = self._doc_dir(machine, name)
        have = _versions(doc_dir)
        if version == 0 and have:
            version = have[-1]
        if version not in have:
            return STATUS_ERR, b''
        try:
            with open(os.path.join(doc_dir, '%06d' % version), 'rb') as f:
                return STATUS_OK, f.read()
        except OSError:
            return STATUS_ERR, b''
