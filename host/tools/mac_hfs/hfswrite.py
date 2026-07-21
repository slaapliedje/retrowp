#!/usr/bin/env python3
"""Write-capable counterpart to hfsread.py (M12 Phase 0).

Not a general HFS writer -- purpose-built for `inject_startup_item.py`'s
one job: splice a new file record into the catalog B-tree (via correct
cascading node splits, standard bottom-up B+tree insertion) and hand out
real Volume Bitmap space for that file's forks. See docs/mac-platform.md
for the three real bugs found getting this right, including one that
caused a 100%-reproducible boot-time crash.
"""
import struct
import sys

SIG_HFS = 0x4244


def cat_key_bytes(parent_id, name):
    name_b = name.encode('mac-roman')
    key_len = 6 + len(name_b)
    key = bytes([key_len, 0]) + struct.pack('>I', parent_id) + bytes([len(name_b)]) + name_b
    if len(key) % 2 == 1:
        key += b'\x00'
    return key


def cat_key_cmp(a, b):
    """a, b are (parentID, name) tuples. HFS sorts by parentID first,
    then by name using a case-insensitive-ish compare -- plain
    lowercased comparison is sufficient for the plain-ASCII names this
    tool ever deals with."""
    if a[0] != b[0]:
        return -1 if a[0] < b[0] else 1
    an, bn = a[1].lower(), b[1].lower()
    if an == bn:
        return 0
    return -1 if an < bn else 1


class HFSImage:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'r+b')
        self._read_mdb()

    def _read_mdb(self):
        self.f.seek(1024)
        mdb = bytearray(self.f.read(162))
        self.mdb_raw = mdb
        (sig, crdate, lsmod, atrb, nmfls, vbmst, allocptr, nmalblks,
         albiksiz, clpsiz, albist, nxtcnid, freebks) = struct.unpack(
            '>HIIHHHHHIIHIH', bytes(mdb[0:36]))
        assert sig == SIG_HFS
        vn_len = mdb[36]
        self.alloc_blk_size = albiksiz
        self.alloc_blk_start = albist
        self.num_alloc_blks = nmalblks
        self.vbm_start_512 = vbmst
        self.next_cnid = nxtcnid
        self.free_blks = freebks
        off = 36 + 28
        # Skip past drXTFlSize+drXTExtRec (the Extents Overflow file's
        # own extents) to reach the Catalog file's own -- conflating
        # these two was Bug 1 (docs/mac-platform.md).
        off4 = off + 28 + 32 + 6 + 16
        ctflsize, = struct.unpack('>I', mdb[off4:off4+4])
        ctextrec = mdb[off4+4:off4+16]
        self.cat_file_size = ctflsize
        self.cat_extents = self._parse_extents(ctextrec)
        self._off_freebks = 0x22
        self._off_nxtcnid = 0x1E

    def _parse_extents(self, raw):
        return [struct.unpack('>HH', raw[i*4:i*4+4]) for i in range(3)]

    def alloc_block_offset(self, n):
        return (self.alloc_blk_start * 512) + (n * self.alloc_blk_size)

    def read_alloc_blocks(self, start_blk, count):
        self.f.seek(self.alloc_block_offset(start_blk))
        return self.f.read(count * self.alloc_blk_size)

    def write_alloc_blocks(self, start_blk, data):
        assert len(data) % self.alloc_blk_size == 0
        self.f.seek(self.alloc_block_offset(start_blk))
        self.f.write(data)

    def read_catalog_bytes(self):
        buf = b''
        remaining = self.cat_file_size
        for (sb, bc) in self.cat_extents:
            if bc == 0:
                continue
            chunk = self.read_alloc_blocks(sb, bc)
            take = min(remaining, len(chunk))
            buf += chunk[:take]
            remaining -= take
        return buf

    def write_catalog_bytes(self, data):
        """Write back full catalog bytes across its (unchanged) extents."""
        pos = 0
        for (sb, bc) in self.cat_extents:
            if bc == 0:
                continue
            n = bc * self.alloc_blk_size
            chunk = data[pos:pos+n]
            if len(chunk) < n:
                chunk = chunk + b'\x00' * (n - len(chunk))
            self.write_alloc_blocks(sb, chunk)
            pos += n
        assert pos >= len(data), "catalog data larger than its extents!"

    def bump_next_cnid(self):
        cnid = self.next_cnid
        self.next_cnid += 1
        struct.pack_into('>I', self.mdb_raw, self._off_nxtcnid, self.next_cnid)
        return cnid

    def set_free_blks(self, n):
        self.free_blks = n
        struct.pack_into('>H', self.mdb_raw, self._off_freebks, n)

    def flush_mdb(self):
        self.f.seek(1024)
        self.f.write(bytes(self.mdb_raw))

    # ---- Volume Bitmap (separate from the B-tree's own node bitmap) ----
    def read_vbm(self):
        # VBM starts at absolute 512-byte block drVBMSt, runs until enough
        # bits exist to cover num_alloc_blks (1 bit/block).
        nbytes = (self.num_alloc_blks + 7) // 8
        nbytes = (nbytes + 511) // 512 * 512  # round up to whole 512B blocks
        self.f.seek(self.vbm_start_512 * 512)
        return bytearray(self.f.read(nbytes))

    def write_vbm(self, vbm):
        self.f.seek(self.vbm_start_512 * 512)
        self.f.write(bytes(vbm))

    def find_free_alloc_blocks(self, vbm, count):
        run_start = None
        run_len = 0
        for i in range(self.num_alloc_blks):
            used = (vbm[i // 8] >> (7 - (i % 8))) & 1
            if not used:
                if run_start is None:
                    run_start = i
                run_len += 1
                if run_len >= count:
                    return run_start
            else:
                run_start = None
                run_len = 0
        raise RuntimeError(f"no free run of {count} allocation blocks found")

    def mark_vbm_range(self, vbm, start, count, used=True):
        for i in range(start, start + count):
            byte_i, bit_i = i // 8, 7 - (i % 8)
            if used:
                vbm[byte_i] |= (1 << bit_i)
            else:
                vbm[byte_i] &= ~(1 << bit_i)


class CatalogBTree:
    def __init__(self, data, node_size=512):
        self.data = bytearray(data)
        self.node_size = node_size
        node0 = self.node(0)
        hdr = node0[14:14+30]
        (depth, root, leaf_recs, first_leaf, last_leaf, node_size2,
         max_key_len, total_nodes, free_nodes) = struct.unpack(
            '>HIIIIHHII', hdr)
        assert node_size2 == node_size
        self.depth = depth
        self.root = root
        self.leaf_recs = leaf_recs
        self.first_leaf = first_leaf
        self.last_leaf = last_leaf
        self.total_nodes = total_nodes
        self.free_nodes = free_nodes

    # ---- raw node access ----
    def node(self, n):
        return bytes(self.data[n*self.node_size:(n+1)*self.node_size])

    def set_node(self, n, raw):
        assert len(raw) == self.node_size
        self.data[n*self.node_size:(n+1)*self.node_size] = raw

    def node_descriptor(self, n):
        raw = self.node(n)
        flink, blink, kind, height, numrecs, resv = struct.unpack('>iiBBHH', raw[0:14])
        return dict(fLink=flink, bLink=blink, kind=kind, height=height, numRecords=numrecs)

    def record_offsets(self, n):
        raw = self.node(n)
        nd = self.node_descriptor(n)
        cnt = nd['numRecords']
        offs = []
        for i in range(cnt):
            pos = self.node_size - 2 - (i*2)
            (off,) = struct.unpack('>H', raw[pos:pos+2])
            offs.append(off)
        return offs

    def records(self, n):
        raw = self.node(n)
        offs = self.record_offsets(n)
        cnt = len(offs)
        trailer_start = self.node_size - 2 - (cnt*2)
        recs = []
        for i in range(cnt):
            start = offs[i]
            end = offs[i+1] if i+1 < cnt else trailer_start
            recs.append(raw[start:end])
        return recs

    def free_space(self, n):
        nd = self.node_descriptor(n)
        used = sum(len(r) for r in self.records(n))
        trailer_start = self.node_size - 2 - (nd['numRecords']*2)
        return trailer_start - used

    # ---- key parsing helpers ----
    @staticmethod
    def rec_key(rec):
        key_len = rec[0]
        parent_id, = struct.unpack('>I', rec[2:6])
        name_len = rec[6]
        name = rec[7:7+name_len].decode('mac-roman', errors='replace')
        return (parent_id, name)

    @staticmethod
    def rec_key_field_len(rec):
        key_len = rec[0]
        kfl = key_len + 1
        if kfl % 2 == 1:
            kfl += 1
        return kfl

    # ---- node usage bitmap (record 2 of node 0) ----
    def get_map_record(self):
        recs = self.records(0)
        return bytearray(recs[2])

    def find_free_node(self, map_bits):
        for i in range(self.total_nodes):
            used = (map_bits[i // 8] >> (7 - (i % 8))) & 1
            if not used:
                return i
        raise RuntimeError("no free B-tree node available")

    def mark_map_bit(self, map_bits, node_num, used=True):
        byte_i, bit_i = node_num // 8, 7 - (node_num % 8)
        if used:
            map_bits[byte_i] |= (1 << bit_i)
        else:
            map_bits[byte_i] &= ~(1 << bit_i)

    def write_header_node_fields(self, leaf_recs=None, first_leaf=None,
                                  last_leaf=None, free_nodes=None,
                                  map_bits=None):
        raw = bytearray(self.node(0))
        if leaf_recs is not None:
            self.leaf_recs = leaf_recs
            struct.pack_into('>I', raw, 14+6, leaf_recs)
        if first_leaf is not None:
            self.first_leaf = first_leaf
            struct.pack_into('>I', raw, 14+10, first_leaf)
        if last_leaf is not None:
            self.last_leaf = last_leaf
            struct.pack_into('>I', raw, 14+14, last_leaf)
        if free_nodes is not None:
            self.free_nodes = free_nodes
            struct.pack_into('>I', raw, 14+26, free_nodes)
        if map_bits is not None:
            offs = self.record_offsets(0)
            map_off = offs[2]
            raw[map_off:map_off+len(map_bits)] = map_bits
        self.set_node(0, bytes(raw))

    def build_node(self, node_num, flink, blink, kind, height, recs):
        """recs: list of raw record bytes, already in sorted order.

        Real HFS nodes store numRecords+1 offset-table entries: one per
        record's start, PLUS a final sentinel = the offset where free
        space begins (Inside Macintosh: Files). Omitting that sentinel
        (leaving it as a stray zero) caused a real, 100%-reproducible
        boot-time crash -- Mac OS/B-Tree Manager code reads it, not just
        this tool's own reader (which recomputes record ends itself and
        never needed it). See docs/mac-platform.md Bug 3.
        """
        out = bytearray(self.node_size)
        struct.pack_into('>iiBBH', out, 0, flink, blink, kind, height, len(recs))
        pos = 14
        offs = []
        for r in recs:
            offs.append(pos)
            out[pos:pos+len(r)] = r
            pos += len(r)
        free_space_offset = pos
        all_offs = offs + [free_space_offset]
        for i, off in enumerate(all_offs):
            trailer_pos = self.node_size - 2 - (i*2)
            struct.pack_into('>H', out, trailer_pos, off)
        return bytes(out)


def make_file_record(file_id, file_type, creator, data_len=0, rsrc_len=0,
                      data_extents=((0, 0), (0, 0), (0, 0)),
                      rsrc_extents=((0, 0), (0, 0), (0, 0)),
                      finder_flags=0):
    """HFSCatalogFile data (102 bytes), per offsets verified in hfsread.py:
    cdrType(1) cdrResrv2(1) filFlags(1) filTyp(1) FInfo(16) filFlNum(4)
    filStBlk(2) filLgLen(4) filPyLen(4) filRStBlk(2) filRLgLen(4)
    filRPyLen(4) filCrDat(4) filMdDat(4) filBkDat(4) FXInfo(16)
    filClpSize(2) filExtRec(12) filRExtRec(12) filReserved(4)
    """
    now = 0  # deterministic (0 = unknown/unset date) -- this tool must
             # not depend on a live clock read, same convention this
             # project's other scripted tools follow.
    finfo = struct.pack('>4s4sHiH', file_type, creator, finder_flags, 0, 0)
    data_first_start, _ = data_extents[0]
    rsrc_first_start, _ = rsrc_extents[0]
    ext_rec = b''.join(struct.pack('>HH', s, c) for (s, c) in data_extents)
    rext_rec = b''.join(struct.pack('>HH', s, c) for (s, c) in rsrc_extents)
    rec = struct.pack('>bbBb', 2, 0, 0, 0)
    rec += finfo
    rec += struct.pack('>I', file_id)
    rec += struct.pack('>H', data_first_start)
    rec += struct.pack('>II', data_len, data_len)  # LgLen/PyLen (no slack)
    rec += struct.pack('>H', rsrc_first_start)
    rec += struct.pack('>II', rsrc_len, rsrc_len)
    rec += struct.pack('>III', now, now, now)  # crDat/mdDat/bkDat
    rec += b'\x00' * 16  # FXInfo
    rec += struct.pack('>H', 0)  # filClpSize
    rec += ext_rec
    rec += rext_rec
    rec += struct.pack('>I', 0)  # reserved
    assert len(rec) == 102, len(rec)
    return rec
