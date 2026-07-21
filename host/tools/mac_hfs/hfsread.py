#!/usr/bin/env python3
"""Read-only HFS (Mac OS Standard) parser, for exploring the RetroWP Mac
CI fixture's boot volume (M12 Phase 0). Not a general-purpose HFS
library -- just enough to walk the catalog B-tree and dump what's
there. Reference: Inside Macintosh: Files. Every byte offset here was
verified empirically against the real fixture, not taken from the spec
alone -- see docs/mac-platform.md for the story behind a couple of them.

`inject_startup_item.py` uses this module for read-only discovery
(finding Startup Items' CNID, walking the tree to find the insertion
path) against the pristine tree before any write happens.
"""
import struct
import sys

class HFSVolume:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self._read_mdb()

    def _read_mdb(self):
        self.f.seek(1024)
        mdb = self.f.read(162)
        (sig, crdate, lsmod, atrb, nmfls, vbmst, allocptr, nmalblks,
         albiksiz, clpsiz, albist, nxtcnid, freebks) = struct.unpack(
            '>HIIHHHHHIIHIH', mdb[0:36])
        assert sig == 0x4244, f"bad signature {sig:#x}, not HFS"
        vn_len = mdb[36]
        vn = mdb[37:37+vn_len].decode('mac-roman', errors='replace')
        # offsets after drVN[28] (fixed 28-byte field starting at 0x24 = 36)
        off = 36 + 28
        (volbkup, vseqnum, wrcnt, xtclpsiz, ctclpsiz, nmrtdirs,
         filcnt, dircnt) = struct.unpack('>IHIIIHII', mdb[off:off+28])
        off2 = off + 28
        fndrinfo = mdb[off2:off2+32]
        off3 = off2 + 32
        (vcsize, vbmcsize, ctlcsize) = struct.unpack('>HHH', mdb[off3:off3+6])
        off4 = off3 + 6
        xtflsize, = struct.unpack('>I', mdb[off4:off4+4])
        xtextrec = mdb[off4+4:off4+16]
        # The Catalog file's own extents sit a further 16 bytes in, past
        # drXTFlSize+drXTExtRec (the Extents Overflow file's own record)
        # -- conflating these two was a real, silently-corrupting bug in
        # an earlier version of the writer counterpart to this reader
        # (docs/mac-platform.md has the story).
        off5 = off4 + 16
        ctflsize, = struct.unpack('>I', mdb[off5:off5+4])
        ctextrec = mdb[off5+4:off5+16]

        self.alloc_blk_size = albiksiz
        self.alloc_blk_start = albist  # in 512-byte blocks
        self.num_alloc_blks = nmalblks
        self.next_cnid = nxtcnid
        self.free_blks = freebks
        self.vol_name = vn
        self.nm_rt_dirs = nmrtdirs
        self.fil_cnt = filcnt
        self.dir_cnt = dircnt
        self.cat_file_size = ctflsize
        self.cat_extents = self._parse_extent_rec(ctextrec)
        self.ext_file_size = xtflsize
        self.ext_extents = self._parse_extent_rec(xtextrec)

    def _parse_extent_rec(self, raw):
        exts = []
        for i in range(3):
            sb, bc = struct.unpack('>HH', raw[i*4:i*4+4])
            exts.append((sb, bc))
        return exts

    def alloc_block_offset(self, alloc_block_num):
        # byte offset of the start of the given allocation block
        return (self.alloc_blk_start * 512) + (alloc_block_num * self.alloc_blk_size)

    def read_file_by_extents(self, extents, length):
        """Read `length` bytes from a file described by up to 3 extents
        (allocation-block based). Only handles files fitting in these
        3 extents (fine for the Catalog/Extents B-tree files here)."""
        buf = b''
        remaining = length
        for (start_blk, blk_count) in extents:
            if blk_count == 0:
                continue
            self.f.seek(self.alloc_block_offset(start_blk))
            want = min(remaining, blk_count * self.alloc_blk_size)
            buf += self.f.read(want)
            remaining -= want
            if remaining <= 0:
                break
        return buf


class BTree:
    """Wraps the raw bytes of a B-tree file (Catalog or Extents)."""
    def __init__(self, data):
        self.data = data
        # node 0 is the header node
        node0 = self.node(0, node_size_hint=512)
        # BTHdrRec starts right after the 14-byte NodeDescriptor
        hdr = node0[14:14+106]
        (depth, root, leaf_recs, first_leaf, last_leaf, node_size,
         max_key_len, total_nodes, free_nodes) = struct.unpack(
            '>HIIIIHHII', hdr[0:30])
        self.node_size = node_size
        self.depth = depth
        self.root = root
        self.leaf_recs = leaf_recs
        self.first_leaf = first_leaf
        self.last_leaf = last_leaf
        self.total_nodes = total_nodes
        self.free_nodes = free_nodes

    def node(self, node_num, node_size_hint=None):
        ns = node_size_hint or self.node_size
        return self.data[node_num*ns:(node_num+1)*ns]

    def node_descriptor(self, node_num):
        raw = self.node(node_num)
        flink, blink, kind, height, numrecs, resv = struct.unpack(
            '>iiBBHH', raw[0:14])
        return dict(fLink=flink, bLink=blink, kind=kind, height=height,
                    numRecords=numrecs)

    def record_offsets(self, node_num):
        raw = self.node(node_num)
        nd = self.node_descriptor(node_num)
        n = nd['numRecords']
        offs = []
        for i in range(n):
            pos = self.node_size - 2 - (i*2)
            (off,) = struct.unpack('>H', raw[pos:pos+2])
            offs.append(off)
        return offs

    def records(self, node_num):
        raw = self.node(node_num)
        offs = self.record_offsets(node_num)
        # offs[i] is the start of record i; its end is offs[i+1], or (for
        # the last record) the start of the offset trailer table.
        n = len(offs)
        trailer_start = self.node_size - 2 - (n*2)
        recs = []
        for i in range(n):
            start = offs[i]
            end = offs[i+1] if i+1 < n else trailer_start
            recs.append(raw[start:end])
        return recs


def cat_key_parse(rec):
    key_len = rec[0]
    parent_id, = struct.unpack('>I', rec[2:6])
    name_len = rec[6]
    name = rec[7:7+name_len].decode('mac-roman', errors='replace')
    return key_len, parent_id, name


def cat_data_parse(rec, key_len):
    # data starts right after the key; key occupies 1(len byte)+key_len bytes,
    # padded to even length (HFS catalog keys are padded so record stays
    # even-aligned: total key field length = key_len+1, padded to even).
    key_field_len = key_len + 1
    if key_field_len % 2 == 1:
        key_field_len += 1
    data = rec[key_field_len:]
    if len(data) == 0:
        return None
    rec_type = data[0]
    return rec_type, data


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else str(
        __import__('pathlib').Path.home() / 'opt/mac/fixture/boot.img')
    vol = HFSVolume(path)
    print(f"Volume name: {vol.vol_name!r}")
    print(f"Alloc block size: {vol.alloc_blk_size}, count: {vol.num_alloc_blks}, start(512B blks): {vol.alloc_blk_start}")
    print(f"Free blocks: {vol.free_blks}")
    print(f"Root dirs: {vol.nm_rt_dirs}, files: {vol.fil_cnt}, dirs: {vol.dir_cnt}")
    print(f"Next CNID: {vol.next_cnid}")
    print(f"Catalog file size: {vol.cat_file_size}, extents: {vol.cat_extents}")
    print(f"Extents file size: {vol.ext_file_size}, extents: {vol.ext_extents}")

    cat_data = vol.read_file_by_extents(vol.cat_extents, vol.cat_file_size)
    bt = BTree(cat_data)
    print(f"\nCatalog B-tree: node_size={bt.node_size} depth={bt.depth} root={bt.root} "
          f"leaf_recs={bt.leaf_recs} first_leaf={bt.first_leaf} last_leaf={bt.last_leaf} "
          f"total_nodes={bt.total_nodes} free_nodes={bt.free_nodes}")

    # Walk every leaf node from first_leaf via fLink chain, dump records.
    node_num = bt.first_leaf
    count = 0
    print("\n--- Catalog leaf records ---")
    while node_num != 0 or count == 0:
        nd = bt.node_descriptor(node_num)
        for rec in bt.records(node_num):
            key_len, parent_id, name = cat_key_parse(rec)
            parsed = cat_data_parse(rec, key_len)
            if parsed is None:
                continue
            rec_type, data = parsed
            type_name = {1: 'DIR', 2: 'FILE', 3: 'DIR_THREAD', 4: 'FILE_THREAD'}.get(rec_type, f'?{rec_type}')
            extra = ''
            if rec_type == 1:  # HFSCatalogFolder: dirDirID at offset 6
                dir_id, = struct.unpack('>I', data[6:10])
                extra = f" dirID={dir_id}"
            elif rec_type == 2:  # HFSCatalogFile: fdType@4, filFlNum(fileID)@20
                file_type = data[4:8]
                file_id, = struct.unpack('>I', data[20:24])
                extra = f" fileID={file_id} type={file_type}"
            elif rec_type in (3, 4):  # HFSCatalogThread: thdParID@10, thdCName@14
                real_parent, = struct.unpack('>I', data[10:14])
                nlen = data[14]
                rname = data[15:15+nlen].decode('mac-roman', errors='replace')
                extra = f" -> parent={real_parent} name={rname!r}"
            print(f"  parent={parent_id:6d} name={name!r:30s} {type_name}{extra}")
            count += 1
        if nd['fLink'] == 0:
            break
        node_num = nd['fLink']
    print(f"\ntotal leaf records printed: {count}")


if __name__ == '__main__':
    main()
