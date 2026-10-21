#!/usr/bin/env python3
"""rpfs.py - host-side tool for RPFS Sequel disk images (an independent implementation of the format).

  rpfs.py mkfs  disk.img 16 [--label NAME] [--put host_file:/dest/path ...]   create a formatted image (size in MB)
  rpfs.py ls    disk.img [/path]
  rpfs.py tree  disk.img
  rpfs.py cat   disk.img /path
  rpfs.py put   disk.img host_file /dest/path          add a file (creates missing directories)
  rpfs.py check disk.img                               full consistency check (like the kernel's `fsck`)

Format (see kernel/rpfs.h): 1 KB blocks; block 0 superblock; block bitmap; inode table (64-byte inodes);
data blocks. Inodes have 12 direct pointers + 1 indirect block. Directories hold 32-byte entries.
"""
import argparse, os, struct, sys, time

BS, MAGIC, NDIRECT, PTRS = 1024, 0x53465052, 12, 256
MAXFILE = (NDIRECT + PTRS) * BS
SB  = struct.Struct('<13I16s')         # magic version bsize total icount bstart bblocks istart iblocks dstart fblocks finodes root label
INO = struct.Struct('<HHII12II')       # mode flags size mtime blocks[12] indirect
DE  = struct.Struct('<I28s')           # inode, name
FILE, DIR = 1, 2

class RPFSError(Exception): pass

class Image:
    def __init__(self, path, rw=False):
        self.f = open(path, 'r+b' if rw else 'rb')
        self.rw = rw
        t = SB.unpack_from(self.rb(0))
        (self.magic, self.version, self.bsize, self.total, self.icount, self.bstart, self.bblocks,
         self.istart, self.iblocks, self.dstart, self.free_blocks, self.free_inodes, self.root, lab) = t[:13] + (t[13],)
        self.label = lab.split(b'\0')[0].decode('latin1')
        if self.magic != MAGIC or self.bsize != BS:
            raise RPFSError('not an RPFS Sequel image (bad magic)')
    # ---- raw blocks ----
    def rb(self, n):
        self.f.seek(n * BS); d = self.f.read(BS)
        return d + b'\0' * (BS - len(d))
    def wb(self, n, data):
        assert self.rw and len(data) <= BS
        self.f.seek(n * BS); self.f.write(data + b'\0' * (BS - len(data)))
    def flush_sb(self):
        raw = bytearray(self.rb(0))
        SB.pack_into(raw, 0, self.magic, self.version, self.bsize, self.total, self.icount, self.bstart, self.bblocks,
                     self.istart, self.iblocks, self.dstart, self.free_blocks, self.free_inodes, self.root,
                     self.label.encode('latin1'))
        self.wb(0, bytes(raw))
    # ---- inodes ----
    def read_inode(self, n):
        i = n - 1
        blk = self.rb(self.istart + i // 16)
        f = INO.unpack_from(blk, (i % 16) * 64)
        return {'mode': f[0], 'flags': f[1], 'size': f[2], 'mtime': f[3], 'blocks': list(f[4:16]), 'indirect': f[16]}
    def write_inode(self, n, ino):
        i = n - 1
        blk = bytearray(self.rb(self.istart + i // 16))
        INO.pack_into(blk, (i % 16) * 64, ino['mode'], ino['flags'], ino['size'], ino['mtime'], *ino['blocks'], ino['indirect'])
        self.wb(self.istart + i // 16, bytes(blk))
    def data_blocks(self, ino):
        """All data block numbers of an inode in file order (None for holes), without the indirect block itself."""
        out = list(ino['blocks'])
        if ino['indirect']:
            out += list(struct.unpack('<256I', self.rb(ino['indirect'])))
        else:
            out += [0] * PTRS
        n = (ino['size'] + BS - 1) // BS
        return out[:max(n, 0)] if ino['mode'] == FILE else [b for b in out if b]
    # ---- files and directories ----
    def read_file(self, ino):
        data = b''.join(self.rb(b) if b else b'\0' * BS for b in self.data_blocks(ino))
        return data[:ino['size']]
    def dir_entries(self, n):
        ino = self.read_inode(n)
        if ino['mode'] != DIR: raise RPFSError('not a directory')
        for b in self.data_blocks(ino):
            blk = self.rb(b)
            for i in range(BS // 32):
                e, name = DE.unpack_from(blk, i * 32)
                if e: yield b, i, e, name.split(b'\0')[0].decode('latin1')
    def lookup(self, dirino, name):
        for _, _, e, nm in self.dir_entries(dirino):
            if nm == name: return e
        return None
    def resolve(self, path):
        cur = self.root
        for part in [p for p in path.split('/') if p]:
            if part == '.': continue
            nxt = self.lookup(cur, part)
            if nxt is None: raise RPFSError('no such file or directory: ' + path)
            cur = nxt
        return cur
    # ---- allocation (lowest free block / inode, like the kernel) ----
    def alloc_block(self):
        for bb in range(self.bblocks):
            blk = bytearray(self.rb(self.bstart + bb))
            for bit in range(BS * 8):
                b = bb * BS * 8 + bit
                if b >= self.total: break
                if not blk[bit // 8] & (1 << (bit % 8)):
                    blk[bit // 8] |= 1 << (bit % 8)
                    self.wb(self.bstart + bb, bytes(blk))
                    self.free_blocks -= 1
                    self.wb(b, b'')
                    return b
        raise RPFSError('disk full')
    def alloc_inode(self, mode):
        for n in range(1, self.icount + 1):
            if self.read_inode(n)['mode'] == 0:
                self.write_inode(n, {'mode': mode, 'flags': 0, 'size': 0, 'mtime': int(time.time()), 'blocks': [0] * 12, 'indirect': 0})
                self.free_inodes -= 1
                return n
        raise RPFSError('out of inodes')
    def bmap_alloc(self, ino, idx):
        if idx < NDIRECT:
            if not ino['blocks'][idx]: ino['blocks'][idx] = self.alloc_block()
            return ino['blocks'][idx]
        idx -= NDIRECT
        if idx >= PTRS: raise RPFSError('file too large (max 268 KB)')
        if not ino['indirect']: ino['indirect'] = self.alloc_block()
        ptrs = list(struct.unpack('<256I', self.rb(ino['indirect'])))
        if not ptrs[idx]:
            ptrs[idx] = self.alloc_block()
            self.wb(ino['indirect'], struct.pack('<256I', *ptrs))
        return ptrs[idx]
    def dir_add(self, dirn, name, child):
        d = self.read_inode(dirn)
        nm = name.encode('latin1')
        for b in self.data_blocks(d):
            blk = bytearray(self.rb(b))
            for i in range(BS // 32):
                if DE.unpack_from(blk, i * 32)[0] == 0:
                    DE.pack_into(blk, i * 32, child, nm); self.wb(b, bytes(blk)); return
        b = self.bmap_alloc(d, d['size'] // BS)
        blk = bytearray(BS); DE.pack_into(blk, 0, child, nm); self.wb(b, bytes(blk))
        d['size'] += BS; self.write_inode(dirn, d)
    def mkdir(self, parent, name):
        n = self.alloc_inode(DIR)
        ino = self.read_inode(n)
        ino['blocks'][0] = self.alloc_block(); ino['size'] = BS
        blk = bytearray(BS)
        DE.pack_into(blk, 0, n, b'.'); DE.pack_into(blk, 32, parent or n, b'..')
        self.wb(ino['blocks'][0], bytes(blk)); self.write_inode(n, ino)
        if parent: self.dir_add(parent, name, n)
        return n
    def put(self, data, path):
        if len(data) > MAXFILE: raise RPFSError('file too large (max 268 KB): ' + path)
        parts = [p for p in path.split('/') if p]
        cur = self.root
        for p in parts[:-1]:
            nxt = self.lookup(cur, p)
            cur = nxt if nxt is not None else self.mkdir(cur, p)
        if self.lookup(cur, parts[-1]) is not None: raise RPFSError('already exists: ' + path)
        n = self.alloc_inode(FILE)
        self.dir_add(cur, parts[-1], n)
        ino = self.read_inode(n)
        for i in range(0, len(data), BS):
            self.wb(self.bmap_alloc(ino, i // BS), data[i:i + BS])
        ino['size'] = len(data); ino['mtime'] = int(time.time())
        self.write_inode(n, ino)

def mkfs(path, mb, label='RPFS'):
    total = min(mb * 1024, 1 << 20)
    if total < 64: raise RPFSError('image too small')
    with open(path, 'wb') as f: f.truncate(total * BS)
    ic = ((total // 4) & ~15); ic = min(max(ic, 32), 8192)
    bb = (total + BS * 8 - 1) // (BS * 8)
    istart = 1 + bb; iblocks = ic // 16; dstart = istart + iblocks
    img = Image.__new__(Image)
    img.f = open(path, 'r+b'); img.rw = True
    (img.magic, img.version, img.bsize, img.total, img.icount, img.bstart, img.bblocks, img.istart, img.iblocks, img.dstart,
     img.free_blocks, img.free_inodes, img.root, img.label) = (MAGIC, 1, BS, total, ic, 1, bb, istart, iblocks, dstart,
                                                              total - dstart, ic, 1, label[:15])
    for b in range(bb):
        blk = bytearray(BS)
        for bit in range(BS * 8):
            n = b * BS * 8 + bit
            if n < dstart or n >= total: blk[bit // 8] |= 1 << (bit % 8)
        img.wb(1 + b, bytes(blk))
    img.flush_sb()
    if img.mkdir(0, '') != 1: raise RPFSError('internal error: root is not inode 1')
    img.flush_sb()
    return img

def check(img):
    problems = []
    def bad(m): problems.append(m)
    used = bytearray((img.total + 7) // 8)
    def mark(b):
        if b < img.dstart or b >= img.total: bad('block %d out of range' % b); return
        if used[b // 8] >> (b % 8) & 1: bad('block %d used twice' % b); return
        used[b // 8] |= 1 << (b % 8)
    for b in range(img.dstart): used[b // 8] |= 1 << (b % 8)
    inodes = {}
    for n in range(1, img.icount + 1):
        ino = img.read_inode(n)
        if ino['mode'] == 0: continue
        if ino['mode'] not in (FILE, DIR): bad('inode %d has invalid mode' % n); continue
        inodes[n] = ino
        for b in ino['blocks']:
            if b: mark(b)
        if ino['indirect']:
            mark(ino['indirect'])
            for b in struct.unpack('<256I', img.rb(ino['indirect'])):
                if b: mark(b)
        if ino['mode'] == DIR and (ino['size'] == 0 or ino['size'] % BS): bad('directory %d has odd size' % n)
    refs, parent, dotdot = {}, {}, {}
    for n, ino in inodes.items():
        if ino['mode'] != DIR: continue
        saw = set()
        for _, _, e, name in img.dir_entries(n):
            if name == '.':
                saw.add('.')
                if e != n: bad('dir %d: "." wrong' % n)
            elif name == '..':
                saw.add('..'); dotdot[n] = e
                if e not in inodes: bad('dir %d: ".." points at unused inode' % n)
            elif e not in inodes: bad('dir %d: entry %r points at unused inode %d' % (n, name, e))
            else: refs[e] = refs.get(e, 0) + 1; parent[e] = n
        if saw != {'.', '..'}: bad('dir %d lacks . or ..' % n)
    for n in inodes:
        if n == 1:
            if refs.get(1): bad('root is listed in a directory')
            if dotdot.get(1) != 1: bad('root ".." must be itself')
            continue
        if refs.get(n, 0) != 1: bad('inode %d referenced %d times' % (n, refs.get(n, 0))); continue
        if inodes[n]['mode'] == DIR and dotdot.get(n) != parent[n]: bad('dir %d: ".." does not match parent' % n)
        cur, steps = n, 0
        while cur != 1 and steps < 64 and cur in parent: cur = parent[cur]; steps += 1
        if cur != 1: bad('inode %d unreachable from root' % n)
    free = 0
    for bb in range(img.bblocks):
        blk = img.rb(img.bstart + bb)
        for bit in range(BS * 8):
            b = bb * BS * 8 + bit
            if b >= img.total: break
            marked = blk[bit // 8] >> (bit % 8) & 1
            real = used[b // 8] >> (b % 8) & 1
            if not marked: free += 1
            if marked and not real: bad('block %d leaked' % b)
            if not marked and real: bad('block %d in use but marked free' % b)
    if free != img.free_blocks: bad('superblock free_blocks %d != actual %d' % (img.free_blocks, free))
    if img.icount - len(inodes) != img.free_inodes: bad('superblock free_inodes %d != actual %d' % (img.free_inodes, img.icount - len(inodes)))
    return problems

def fmt_time(t): return time.strftime('%Y-%m-%d %H:%M', time.gmtime(t)) if t else '----------------'

def ls(img, path):
    n = img.resolve(path)
    ino = img.read_inode(n)
    if ino['mode'] == FILE:
        print('- %7d %s %s' % (ino['size'], fmt_time(ino['mtime']), path)); return
    for _, _, e, name in img.dir_entries(n):
        if name in ('.', '..'): continue
        i = img.read_inode(e)
        print('%s %7d %s %s%s' % ('d' if i['mode'] == DIR else '-', i['size'], fmt_time(i['mtime']), name, '/' if i['mode'] == DIR else ''))

def tree(img, n=None, prefix=''):
    n = n or img.root
    ents = sorted((name, e) for _, _, e, name in img.dir_entries(n) if name not in ('.', '..'))
    for i, (name, e) in enumerate(ents):
        last = i == len(ents) - 1
        ino = img.read_inode(e)
        print(prefix + ('└── ' if last else '├── ') + name + ('/' if ino['mode'] == DIR else ' (%d bytes)' % ino['size']))
        if ino['mode'] == DIR: tree(img, e, prefix + ('    ' if last else '│   '))

def main():
    ap = argparse.ArgumentParser(description='RPFS Sequel image tool')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('mkfs'); p.add_argument('image'); p.add_argument('mb', type=int); p.add_argument('--label', default='RPFS'); p.add_argument('--put', action='append', default=[], metavar='HOST:DEST')
    p = sub.add_parser('ls'); p.add_argument('image'); p.add_argument('path', nargs='?', default='/')
    p = sub.add_parser('tree'); p.add_argument('image')
    p = sub.add_parser('cat'); p.add_argument('image'); p.add_argument('path')
    p = sub.add_parser('put'); p.add_argument('image'); p.add_argument('host'); p.add_argument('dest')
    p = sub.add_parser('check'); p.add_argument('image')
    a = ap.parse_args()
    try:
        if a.cmd == 'mkfs':
            img = mkfs(a.image, a.mb, a.label)
            for spec in a.put:
                host, dest = spec.rsplit(':', 1)
                img.put(open(host, 'rb').read(), dest)
            img.flush_sb()
            print('created %s: %d MB, label "%s", %d blocks free' % (a.image, a.mb, a.label, img.free_blocks))
        elif a.cmd == 'put':
            img = Image(a.image, rw=True); img.put(open(a.host, 'rb').read(), a.dest); img.flush_sb()
        else:
            img = Image(a.image)
            if a.cmd == 'ls': ls(img, a.path)
            elif a.cmd == 'tree': print('/'); tree(img)
            elif a.cmd == 'cat': sys.stdout.buffer.write(img.read_file(img.read_inode(img.resolve(a.path))))
            elif a.cmd == 'check':
                probs = check(img)
                print('clean' if not probs else '%d problem(s):\n  ' % len(probs) + '\n  '.join(probs))
                sys.exit(1 if probs else 0)
    except RPFSError as e:
        print('rpfs: %s' % e, file=sys.stderr); sys.exit(2)

if __name__ == '__main__':
    main()
