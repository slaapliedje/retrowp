#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Set the Finder's hasBundle flag on a Retro68 application's outputs.

Retro68's Rez writes the type and creator but no Finder flags, and the
Finder reads an application's 'BNDL' (its icons, the documents it owns)
only when hasBundle (0x2000 in fdFlags) is set. Run in the build
directory with the application's name; each output is changed in place,
and running it again changes nothing:

  NAME.bin             MacBinary: byte 73 (fdFlags' high byte), then its CRC
  %NAME.ad             AppleDouble: the Finder Info entry (id 9)
  .finf/NAME.APPL      BasiliskII's ExtFS Finder info (what the gates boot)
  NAME.dsk             the HFS floppy: its copy replaced from NAME.bin by
                       hfsutils (hcopy -m keeps the Finder info), where
                       installed -- hattrib cannot set this flag

The Finder also caches bundles in its Desktop file, so a copy already
seen without the flag may need a desktop rebuild (Cmd+Option at boot).
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HAS_BUNDLE_HI = 0x20   # fdFlags 0x2000, its high byte


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def macbinary(path):
    with open(path, 'r+b') as f:
        head = bytearray(f.read(128))
        if len(head) < 128 or head[0] != 0 or head[74] != 0:
            raise SystemExit('%s: not MacBinary' % path)
        head[73] |= HAS_BUNDLE_HI
        struct.pack_into('>H', head, 124, crc16_xmodem(head[:124]))
        f.seek(0)
        f.write(head)


def appledouble(path):
    with open(path, 'r+b') as f:
        head = f.read(26)
        magic, = struct.unpack('>I', head[:4])
        if magic != 0x00051607:
            raise SystemExit('%s: not AppleDouble' % path)
        count, = struct.unpack('>H', head[24:26])
        for i in range(count):
            f.seek(26 + i * 12)
            eid, off, ln = struct.unpack('>III', f.read(12))
            if eid == 9 and ln >= 10:
                f.seek(off + 8)
                b = f.read(1)[0] | HAS_BUNDLE_HI
                f.seek(off + 8)
                f.write(bytes([b]))
                return
    raise SystemExit('%s: no Finder Info entry' % path)


def finf(path):
    with open(path, 'r+b') as f:
        info = bytearray(f.read(32))
        if len(info) < 10:
            raise SystemExit('%s: too short' % path)
        info[8] |= HAS_BUNDLE_HI
        f.seek(0)
        f.write(info)


def dsk(path, name, macbin):
    if shutil.which('hmount') is None:
        print('set_bundle_bit: no hfsutils, %s left without hasBundle' % path)
        return
    home = tempfile.mkdtemp()   # hfsutils keeps its mount state in $HOME
    env = dict(os.environ, HOME=home)
    try:
        subprocess.run(['hmount', path], env=env, check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['hdel', ':' + name], env=env, check=True)
        subprocess.run(['hcopy', '-m', macbin, ':' + name], env=env, check=True)
    finally:
        subprocess.run(['humount'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(home, ignore_errors=True)


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: set_bundle_bit.py NAME')
    name = sys.argv[1]
    macbinary(name + '.bin')
    appledouble('%' + name + '.ad')
    if os.path.exists(os.path.join('.finf', name + '.APPL')):
        finf(os.path.join('.finf', name + '.APPL'))
    dsk(name + '.dsk', name, name + '.bin')


main()
