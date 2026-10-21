#!/usr/bin/env python3
"""mkexe.py - turn a linked x86-64 ELF program into a MyDOS Sequel .exe (format version 2, see kernel/process.h).

    mkexe.py prog.elf prog.exe [--stack 65536]
"""
import struct, sys, argparse

USER_BASE = 0x8000000000
PT_LOAD = 1

def build(elf_path, stack=65536):
    d = open(elf_path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 1:
        raise SystemExit('mkexe: %s is not a 64-bit little-endian ELF file' % elf_path)
    entry, phoff = struct.unpack_from('<QQ', d, 24)
    phentsize, phnum = struct.unpack_from('<HH', d, 54)
    segs = []
    for i in range(phnum):
        t, flags, off, vaddr, paddr, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
        if t == PT_LOAD and memsz:
            segs.append((vaddr, off, filesz, memsz))
    if not segs:
        raise SystemExit('mkexe: no loadable segments')
    base = min(s[0] for s in segs)
    if base != USER_BASE:
        raise SystemExit('mkexe: image must be linked at 0x%X (found 0x%X) - use user/user.ld' % (USER_BASE, base))
    file_end = max(v + fsz for v, _, fsz, _ in segs) - base
    mem_end = max(v + m for v, _, _, m in segs) - base
    image = bytearray(file_end)
    for vaddr, off, filesz, _ in segs:
        image[vaddr - base: vaddr - base + filesz] = d[off: off + filesz]
    checksum = sum(image) & 0xFFFFFFFF
    header = b'MDXE' + struct.pack('<HHQQIIII', 2, 0, base, entry, len(image), mem_end, stack, checksum)
    assert len(header) == 40
    return header + bytes(image), entry, len(image), mem_end

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('elf'); ap.add_argument('exe'); ap.add_argument('--stack', type=int, default=65536)
    a = ap.parse_args()
    data, entry, fsz, msz = build(a.elf, a.stack)
    open(a.exe, 'wb').write(data)
    print('%s: %d bytes (image %d, memory %d, entry 0x%X)' % (a.exe, len(data), fsz, msz, entry))
