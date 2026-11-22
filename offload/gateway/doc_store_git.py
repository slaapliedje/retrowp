# SPDX-License-Identifier: GPL-2.0-or-later
"""The document store kept in git (M21 T21.7): the same OFFLOAD_STORE,
OFFLOAD_LIST and OFFLOAD_FETCH answers as doc_store.py, but every copy a
machine sends is a commit, so nothing is ever thrown away.

    <store>/.git
    <store>/<machine>/<document>        the latest copy, a plain file

A document's versions are the commits that changed its file, numbered
from 1 for the first; version n is always the same bytes, since this
store never rewrites its history. A copy identical to the latest is not
a new version. LIST shows the newest `versions` of a document (the
machine's picker pages through them); FETCH can still name any.

The store is a working repository on purpose: clone it on another
computer to read the documents, see what changed between two saves
(git log -p), or keep a copy elsewhere (git remote add / git push --
the gateway never pushes by itself).

A copy is safe on the Pi before the answer goes back: the file is
written to a temporary name, flushed and renamed, then committed with
git's own flushing on (core.fsync), so a power cut after an OFFLOAD_OK
loses nothing. The gateway's commits carry a fixed author, "RetroWP
gateway", and use no global git configuration, hooks or signing.

Names are checked exactly as doc_store.py checks them (valid_name), so
".git" -- like any name starting with '.' -- can never be one.
"""

import os
import struct
import subprocess
import time

from wire_protocol import STATUS_OK, STATUS_ERR
from doc_store import _take_name, _fsync_dir, valid_name, DEFAULT_VERSIONS

AUTHOR = ('RetroWP gateway', 'gateway@retrowp.invalid')


class GitStoreError(Exception):
    pass


class GitDocStore:
    def __init__(self, root, versions=DEFAULT_VERSIONS, git='git'):
        self.root = os.path.abspath(root)
        self.versions = max(1, versions)
        self.git_bin = git
        os.makedirs(self.root, exist_ok=True)
        if not os.path.isdir(os.path.join(self.root, '.git')):
            if os.listdir(self.root):
                # Perhaps the numbered store's folder (doc_store.py):
                # refuse rather than mix the two layouts.
                raise GitStoreError('%s is not empty and not a git store' % self.root)
            self._git('init', '-q')
        # Each time, so a store made by hand or an older gateway gets them.
        for key, value in (('core.fsync', 'objects,reference'),
                           ('core.hooksPath', '/dev/null'),
                           ('commit.gpgSign', 'false'),
                           ('gc.auto', '0')):
            self._git('config', key, value)

    # -- git ------------------------------------------------------------

    def _git(self, *args, data=None):
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
               'HOME': self.root, 'LC_ALL': 'C',
               'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': os.devnull,
               'GIT_LITERAL_PATHSPECS': '1',      # a name may hold '*', '?' or '['
               'GIT_AUTHOR_NAME': AUTHOR[0], 'GIT_AUTHOR_EMAIL': AUTHOR[1],
               'GIT_COMMITTER_NAME': AUTHOR[0], 'GIT_COMMITTER_EMAIL': AUTHOR[1]}
        r = subprocess.run((self.git_bin,) + args, cwd=self.root, env=env, input=data,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if r.returncode != 0:
            raise GitStoreError(r.stderr.decode('utf-8', 'replace').strip())
        return r.stdout

    def _history(self, rel):
        """The commits that changed `rel`, oldest first: [(sha, time)]."""
        try:
            out = self._git('log', '--format=%H %ct', '--', rel)
        except GitStoreError:
            return []                  # no commits at all yet
        rows = [line.split() for line in out.decode('ascii').splitlines() if line]
        return [(sha, int(t)) for sha, t in reversed(rows)]

    def _size(self, sha, rel):
        return int(self._git('cat-file', '-s', '%s:%s' % (sha, rel)))

    @staticmethod
    def _when(t):
        return time.strftime('%Y-%m-%d %H:%M', time.localtime(t))

    @staticmethod
    def _rel(machine, name):
        return machine + '/' + name

    # -- the three ops ----------------------------------------------------

    def store(self, payload):
        """OFFLOAD_STORE: machine, document name, then the file's bytes."""
        machine, pos = _take_name(payload, 0)
        name, pos = _take_name(payload, pos)
        if machine is None or name is None:
            return STATUS_ERR, b'The Pi refused that name'
        body = payload[pos:]
        rel = self._rel(machine, name)
        mdir = os.path.join(self.root, machine)
        path = os.path.join(mdir, name)
        try:
            os.makedirs(mdir, exist_ok=True)
            if os.path.isdir(path):
                return STATUS_ERR, b'The Pi could not keep the copy'
            tmp = os.path.join(mdir, '.incoming')
            with open(tmp, 'wb') as f:
                f.write(body)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, path)
            _fsync_dir(mdir)
            self._git('add', '--', rel)
            changed = self._git('status', '--porcelain', '--', rel).strip() != b''
            if changed:
                self._git('commit', '-q', '-m', '%s: %s' % (machine, name), '--', rel)
        except (OSError, GitStoreError):
            return STATUS_ERR, b'The Pi could not keep the copy'
        count = len(self._history(rel))
        plural = '' if count == 1 else 's'
        if changed:
            line = 'Kept %s on the Pi (%d version%s)' % (name, count, plural)
        else:
            line = '%s is unchanged on the Pi (%d version%s)' % (name, count, plural)
        return STATUS_OK, line.encode('ascii')

    def list(self, payload):
        """OFFLOAD_LIST, in doc_store.py's shapes: a line a document
        (name, versions, bytes, time of its latest), newest first; or,
        given a name, a line a version (number, bytes, time), newest
        first and at most `versions` of them."""
        machine, pos = _take_name(payload, 0)
        if machine is None:
            return STATUS_ERR, b'The Pi refused that name'
        try:
            if pos < len(payload):
                name, pos = _take_name(payload, pos)
                if name is None:
                    return STATUS_ERR, b'The Pi refused that name'
                rel = self._rel(machine, name)
                hist = self._history(rel)
                out = []
                for number in range(len(hist), max(0, len(hist) - self.versions), -1):
                    sha, t = hist[number - 1]
                    out.append('%d\t%d\t%s\n' % (number, self._size(sha, rel), self._when(t)))
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
                rel = self._rel(machine, n)
                hist = self._history(rel)
                if not hist:
                    continue
                sha, t = hist[-1]
                docs.append((t, n, len(hist), self._size(sha, rel)))
        except GitStoreError:
            return STATUS_ERR, b''
        docs.sort(key=lambda d: (-d[0], d[1]))
        out = ''.join('%s\t%d\t%d\t%s\n' % (n, c, size, self._when(t)) for t, n, c, size in docs)
        return STATUS_OK, out.encode('ascii')

    def fetch(self, payload):
        """OFFLOAD_FETCH: machine, document name, u32 BE version (0 =
        the latest) -> that version's bytes."""
        machine, pos = _take_name(payload, 0)
        name, pos = _take_name(payload, pos)
        if machine is None or name is None or len(payload) != pos + 4:
            return STATUS_ERR, b''
        (version,) = struct.unpack('>I', payload[pos:pos + 4])
        rel = self._rel(machine, name)
        hist = self._history(rel)
        if version == 0:
            version = len(hist)
        if not 1 <= version <= len(hist):
            return STATUS_ERR, b''
        try:
            return STATUS_OK, self._git('cat-file', 'blob', '%s:%s' % (hist[version - 1][0], rel))
        except GitStoreError:
            return STATUS_ERR, b''
