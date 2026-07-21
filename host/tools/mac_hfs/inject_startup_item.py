#!/usr/bin/env python3
"""Inject a file directly into System Folder:Startup Items on the
RetroWP Mac CI fixture's boot volume (M12 Phase 0), via targeted
catalog B-tree surgery (cascading node splits, standard bottom-up
B+tree insertion) plus Volume Bitmap allocation for the file's resource
fork. This is what makes headless CI possible at all for BasiliskII:
anything placed here auto-launches at boot with zero interaction,
avoiding Finder double-click automation entirely. See
docs/mac-platform.md for why this was needed (BasiliskII has no
equivalent to amiberry's host-directory-mountable boot volume) and for
the three real bugs found building it.

Not a general HFS writer -- just enough to make this one operation
correct for this project's own CI fixture.

Usage: inject_startup_item.py <image.img> <finf_path> <rsrc_path> <data_path_or_empty> [name]

  <finf_path>  a 32-byte Finder info blob; only the first 8 bytes
               (4-byte type + 4-byte creator) are used.
  <rsrc_path>  the resource fork to install (e.g. a Retro68 `.rsrc`
               sidecar file).
  <data_path>  the data fork, or empty string for none.
  [name]       catalog name for the new file (default "RetroWP CI").
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfsread import HFSVolume as ReadVol, BTree as ReadBTree, cat_key_parse, cat_data_parse
from hfswrite import HFSImage, CatalogBTree, cat_key_bytes, cat_key_cmp, make_file_record


def rec_key(rec):
    parent_id, = struct.unpack('>I', rec[2:6])
    name_len = rec[6]
    name = rec[7:7+name_len].decode('mac-roman', errors='replace')
    return (parent_id, name)


def rec_child(rec):
    key_len = rec[0]
    kfl = key_len + 1
    if kfl % 2 == 1:
        kfl += 1
    child_num, = struct.unpack('>I', rec[kfl:kfl+4])
    return child_num


def find_startup_items_cnid(rbt):
    node_num = rbt.first_leaf
    while True:
        for rec in rbt.records(node_num):
            key_len, parent_id, name = cat_key_parse(rec)
            if parent_id == 18 and name == 'Startup Items':
                parsed = cat_data_parse(rec, key_len)
                if parsed and parsed[0] == 1:
                    dir_id, = struct.unpack('>I', parsed[1][6:10])
                    return dir_id
        nd = rbt.node_descriptor(node_num)
        if nd['fLink'] == 0:
            break
        node_num = nd['fLink']
    raise RuntimeError("Startup Items not found under System Folder (CNID 18)")


def find_path(rbt, target_key):
    """Path from root to leaf: list of (node_num, chosen_record_index)."""
    path = []
    node_num = rbt.root
    while True:
        nd = rbt.node_descriptor(node_num)
        recs = rbt.records(node_num)
        if nd['kind'] == 255:
            path.append((node_num, None))
            return path
        chosen = 0
        for i, rec in enumerate(recs):
            if cat_key_cmp(rec_key(rec), target_key) <= 0:
                chosen = i
            else:
                break
        path.append((node_num, chosen))
        node_num = rec_child(recs[chosen])


def fits(bt, recs):
    # +1 for the trailing free-space-offset sentinel entry build_node()
    # always writes (see its own docstring for why that matters).
    used = sum(len(r) for r in recs)
    return used + 2 * (len(recs) + 1) <= bt.node_size - 14


def split_records(recs):
    n = len(recs)
    split_at = (n + 1) // 2
    return recs[:split_at], recs[split_at:]


def main():
    img_path, finf_path, rsrc_path = sys.argv[1], sys.argv[2], sys.argv[3]
    data_path = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else None
    new_name = sys.argv[5] if len(sys.argv) > 5 else 'RetroWP CI'

    finfo = open(finf_path, 'rb').read()
    file_type = finfo[0:4]
    creator = finfo[4:8]
    rsrc_bytes = open(rsrc_path, 'rb').read()
    data_bytes = open(data_path, 'rb').read() if data_path else b''

    # ---- discovery over the pristine image (read-only) ----
    rv = ReadVol(img_path)
    cat_bytes_ro = rv.read_file_by_extents(rv.cat_extents, rv.cat_file_size)
    rbt = ReadBTree(cat_bytes_ro)
    startup_cnid = find_startup_items_cnid(rbt)
    print(f"Startup Items CNID = {startup_cnid}")

    target_key = (startup_cnid, new_name)
    path = find_path(rbt, target_key)
    print(f"path root->leaf: {path}")

    # ---- open read-write ----
    img = HFSImage(img_path)
    bt = CatalogBTree(img.read_catalog_bytes())

    new_cnid = img.bump_next_cnid()
    print(f"new file CNID = {new_cnid}")

    vbm = img.read_vbm()
    blocks_needed = (len(rsrc_bytes) + img.alloc_blk_size - 1) // img.alloc_blk_size
    data_blocks_needed = (len(data_bytes) + img.alloc_blk_size - 1) // img.alloc_blk_size if data_bytes else 0
    rsrc_start = img.find_free_alloc_blocks(vbm, blocks_needed) if blocks_needed else 0
    if blocks_needed:
        img.mark_vbm_range(vbm, rsrc_start, blocks_needed, used=True)
    data_start = 0
    if data_blocks_needed:
        data_start = img.find_free_alloc_blocks(vbm, data_blocks_needed)
        img.mark_vbm_range(vbm, data_start, data_blocks_needed, used=True)
    print(f"resource fork: {len(rsrc_bytes)} bytes -> {blocks_needed} blocks @ {rsrc_start}")
    if data_bytes:
        print(f"data fork: {len(data_bytes)} bytes -> {data_blocks_needed} blocks @ {data_start}")

    new_file_data = make_file_record(
        file_id=new_cnid, file_type=file_type, creator=creator,
        data_len=len(data_bytes), rsrc_len=len(rsrc_bytes),
        data_extents=((data_start, data_blocks_needed), (0, 0), (0, 0)) if data_bytes else ((0, 0), (0, 0), (0, 0)),
        rsrc_extents=((rsrc_start, blocks_needed), (0, 0), (0, 0)) if rsrc_bytes else ((0, 0), (0, 0), (0, 0)),
        finder_flags=0,
    )
    new_key_bytes = cat_key_bytes(startup_cnid, new_name)
    new_rec = new_key_bytes + new_file_data
    print(f"new catalog record length: {len(new_rec)}")

    map_bits = bt.get_map_record()
    nodes_consumed = []

    def alloc_node():
        n = bt.find_free_node(map_bits)
        bt.mark_map_bit(map_bits, n, used=True)
        nodes_consumed.append(n)
        return n

    def rebuild_with_sibling_fix(node_num, new_fLink, new_bLink, kind, height, recs):
        node_bytes = bt.build_node(node_num, flink=new_fLink, blink=new_bLink,
                                    kind=kind, height=height, recs=recs)
        bt.set_node(node_num, node_bytes)

    def set_blink(node_num, new_blink):
        if node_num == 0:
            return
        raw = bytearray(bt.node(node_num))
        struct.pack_into('>i', raw, 4, new_blink)
        bt.set_node(node_num, bytes(raw))

    # ---- bottom-up insert with cascading splits ----
    # pending: (key, payload_bytes_after_key) to insert into the node at
    # this path level; starts as the new leaf record, becomes a
    # (separator_key -> child_node) index record if a split cascades up.
    pending_key = target_key
    pending_rec = new_rec
    new_root_needed = None  # (left_node, right_node, right_first_key) if root itself splits

    for level in range(len(path) - 1, -1, -1):
        node_num, _chosen_idx = path[level]
        is_leaf = (level == len(path) - 1)
        nd = bt.node_descriptor(node_num)
        old_recs = list(bt.records(node_num))

        insert_at = 0
        for i, r in enumerate(old_recs):
            if cat_key_cmp(rec_key(r), pending_key) < 0:
                insert_at = i + 1
            else:
                break
        merged = old_recs[:insert_at] + [pending_rec] + old_recs[insert_at:]

        if fits(bt, merged):
            rebuild_with_sibling_fix(node_num, nd['fLink'], nd['bLink'], nd['kind'], nd['height'], merged)
            break  # no further propagation needed

        # split needed
        left, right = split_records(merged)
        old_fLink = nd['fLink']
        new_node_num = alloc_node()
        node_kind = 255 if is_leaf else 0
        node_height = 1 if is_leaf else nd['height']
        rebuild_with_sibling_fix(node_num, new_node_num, nd['bLink'], node_kind, node_height, left)
        rebuild_with_sibling_fix(new_node_num, old_fLink, node_num, node_kind, node_height, right)
        if old_fLink != 0:
            set_blink(old_fLink, new_node_num)

        right_first_key = rec_key(right[0])
        sep_key_bytes = cat_key_bytes(right_first_key[0], right_first_key[1])
        sep_rec = sep_key_bytes + struct.pack('>I', new_node_num)

        if level == 0:
            # root itself split -- needs a brand new root
            new_root_needed = (node_num, new_node_num, right_first_key)
            break

        pending_key = right_first_key
        pending_rec = sep_rec
        # continue loop to propagate into path[level-1]

    new_root_num = None
    if new_root_needed is not None:
        old_root_num, right_of_root, right_first_key = new_root_needed
        old_root_recs = list(bt.records(old_root_num))
        left_first_key = rec_key(old_root_recs[0])
        new_root_num = alloc_node()
        left_key_bytes = cat_key_bytes(left_first_key[0], left_first_key[1])
        right_key_bytes = cat_key_bytes(right_first_key[0], right_first_key[1])
        root_recs = [left_key_bytes + struct.pack('>I', old_root_num),
                     right_key_bytes + struct.pack('>I', right_of_root)]
        assert fits(bt, root_recs)
        old_root_height = bt.node_descriptor(old_root_num)['height']
        rebuild_with_sibling_fix(new_root_num, 0, 0, 0, old_root_height + 1, root_recs)
        print(f"ROOT SPLIT: new root node = {new_root_num}, depth {bt.depth} -> {bt.depth+1}")

    # ---- header bookkeeping ----
    new_leaf_recs = bt.leaf_recs + 1
    new_free_nodes = bt.free_nodes - len(nodes_consumed)
    kwargs = dict(leaf_recs=new_leaf_recs, free_nodes=new_free_nodes, map_bits=map_bits)
    if new_root_num is not None:
        bt.root = new_root_num
        bt.depth = bt.depth + 1
    bt.write_header_node_fields(**kwargs)
    if new_root_num is not None:
        raw = bytearray(bt.node(0))
        struct.pack_into('>H', raw, 14+0, bt.depth)
        struct.pack_into('>I', raw, 14+2, bt.root)
        bt.set_node(0, bytes(raw))

    print(f"nodes consumed for splits: {nodes_consumed}")

    # ---- write everything back ----
    img.write_catalog_bytes(bytes(bt.data))
    if blocks_needed:
        rsrc_padded = rsrc_bytes + b'\x00' * (blocks_needed*img.alloc_blk_size - len(rsrc_bytes))
        img.write_alloc_blocks(rsrc_start, rsrc_padded)
    if data_blocks_needed:
        data_padded = data_bytes + b'\x00' * (data_blocks_needed*img.alloc_blk_size - len(data_bytes))
        img.write_alloc_blocks(data_start, data_padded)
    img.write_vbm(vbm)
    total_new_blocks = blocks_needed + data_blocks_needed
    img.set_free_blks(img.free_blks - total_new_blocks)
    img.flush_mdb()
    img.f.flush()
    print("Injection complete.")


if __name__ == '__main__':
    main()
