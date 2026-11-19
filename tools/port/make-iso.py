#!/usr/bin/env python3
"""Build a bootable PS2 disc image: the ELF, a SYSTEM.CNF, and the user's own game data.

  tools/port/make-iso.py <d2.elf> <data dir> <out.iso> [--no-video]

A plain ISO9660 (level 1) image with every file in the root directory, under its 8.3 name in
upper case with the ";1" version the PS2's CD driver expects. SYSTEM.CNF boots cdrom0:\\HS.ELF;1.
The data dir is the user's own Diablo II folder ($D2DATA): the image is built locally and never
committed. --no-video leaves out the classic cinematics (d2video.mpq, 441 MB), which the port
can't play; the expansion's (d2xvideo.mpq) stays, because the game won't start without it.
"""
import os, shutil, struct, sys, time

SECTOR = 2048


def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)


def dir_record(name, lba, size, is_dir, when):
    n = len(name)
    rec = bytearray()
    rec += bytes([0, 0])                       # length (patched below), extended attributes
    rec += both32(lba) + both32(size)
    rec += when
    rec += bytes([2 if is_dir else 0, 0, 0])   # flags, unit size, interleave gap
    rec += both16(1)                           # volume sequence number
    rec += bytes([n]) + name
    if len(rec) % 2: rec += b'\0'
    rec[0] = len(rec)
    return bytes(rec)


def text(s, n): return s.encode('ascii').ljust(n, b' ')[:n]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 3:
        sys.exit(__doc__)
    elf, data, out = args
    skip_video = '--no-video' in sys.argv
    files = []  # (iso name, source path or bytes)
    cnf = b'BOOT2 = cdrom0:\\HS.ELF;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n'
    files.append((b'SYSTEM.CNF;1', cnf))
    files.append((b'HS.ELF;1', elf))
    for f in sorted(os.listdir(data)):
        if not f.lower().endswith('.mpq'):
            continue
        if skip_video and f.lower() == 'd2video.mpq':
            continue
        stem, ext = f.rsplit('.', 1)
        if len(stem) > 8 or len(ext) > 3:
            sys.exit(f'{f}: not an 8.3 name')
        files.append(((stem.upper() + '.' + ext.upper() + ';1').encode(), os.path.join(data, f)))
    files.sort(key=lambda x: x[0])

    t = time.gmtime()
    when = bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])
    size_of = lambda src: len(src) if isinstance(src, bytes) else os.path.getsize(src)

    # Layout: 16 system sectors, the primary descriptor, the terminator, both path tables,
    # the root directory, then the files, each starting on a sector. The path tables sit at
    # sector 257 as on Sony-mastered discs: on a DVD the BIOS's CD driver reads the path table
    # from 257 whatever the descriptor says (PCSX2's read log: sector 16, then 257), and found
    # nothing at 18. A CD follows the descriptor, so the one layout serves both.
    pvd_lba, term_lba, lpt_lba, mpt_lba, root_lba = 16, 17, 257, 258, 259
    root_entries = [dir_record(b'\0', root_lba, 0, True, when), dir_record(b'\1', root_lba, 0, True, when)]
    root_entries += [dir_record(name, 0, 0, False, when) for name, _ in files]
    root_bytes = 0
    for e in root_entries:  # records may not cross a sector
        if root_bytes // SECTOR != (root_bytes + len(e) - 1) // SECTOR:
            root_bytes = (root_bytes // SECTOR + 1) * SECTOR
        root_bytes += len(e)
    root_sectors = (root_bytes + SECTOR - 1) // SECTOR
    # PCSX2 tells a DVD image from a CD image by the root directory's size: one
    # sector (2048 bytes) means CD. An image too big for a CD gets a second one.
    if sum(size_of(src) for _, src in files) > 650 << 20:
        root_sectors = max(root_sectors, 2)
    # On the disc, SYSTEM.CNF and the ELF come first (the boot reads them; the
    # directory lists entries by name, where they sit is free), then the archives.
    lba = root_lba + root_sectors
    placed = []
    boot_first = sorted(files, key=lambda f: (not f[0].startswith((b'SYSTEM.CNF', b'HS.ELF')), f[0]))
    for name, src in boot_first:
        n = size_of(src)
        placed.append((name, src, lba, n))
        lba += (n + SECTOR - 1) // SECTOR
    total = lba

    root = bytearray()
    entries = [dir_record(b'\0', root_lba, root_sectors * SECTOR, True, when),
               dir_record(b'\1', root_lba, root_sectors * SECTOR, True, when)]
    entries += [dir_record(name, l, n, False, when) for name, _, l, n in sorted(placed, key=lambda p: p[0])]
    for e in entries:
        if len(root) // SECTOR != (len(root) + len(e) - 1) // SECTOR:
            root += b'\0' * (SECTOR - len(root) % SECTOR)
        root += e
    root += b'\0' * (root_sectors * SECTOR - len(root))

    pt_entry = lambda big: bytes([1, 0]) + (struct.pack('>I', root_lba) if big else struct.pack('<I', root_lba)) + \
        (struct.pack('>H', 1) if big else struct.pack('<H', 1)) + b'\0\0'
    zero_date = b'0' * 16 + b'\0'
    pvd = bytearray(SECTOR)
    pvd[0:8] = b'\x01CD001\x01\x00'
    pvd[8:40] = text('PLAYSTATION', 32)
    pvd[40:72] = text('HELLSTATION', 32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(len(pt_entry(False)))
    pvd[140:144] = struct.pack('<I', lpt_lba)
    pvd[148:152] = struct.pack('>I', mpt_lba)
    pvd[156:190] = dir_record(b'\0', root_lba, root_sectors * SECTOR, True, when)
    pvd[190:318] = text('', 128)
    pvd[318:446] = text('', 128)
    pvd[446:574] = text('', 128)
    pvd[574:702] = text('', 128)
    pvd[702:813] = text('', 111)
    created = time.strftime('%Y%m%d%H%M%S00', t).encode() + b'\0'
    pvd[813:830] = created
    pvd[830:847] = created
    pvd[847:864] = zero_date
    pvd[864:881] = zero_date
    pvd[881] = 1
    term = bytearray(SECTOR)
    term[0:7] = b'\xffCD001\x01'

    with open(out, 'wb') as o:
        o.write(b'\0' * SECTOR * 16)
        o.write(pvd)
        o.write(term)
        o.write(b'\0' * SECTOR * (lpt_lba - term_lba - 1))
        o.write(pt_entry(False).ljust(SECTOR, b'\0'))
        o.write(pt_entry(True).ljust(SECTOR, b'\0'))
        o.write(root)
        for name, src, l, n in placed:
            assert o.tell() == l * SECTOR, name
            if isinstance(src, bytes):
                o.write(src)
            else:
                with open(src, 'rb') as f:
                    shutil.copyfileobj(f, o, 8 << 20)
            pad = (-n) % SECTOR
            if pad: o.write(b'\0' * pad)
    print(f'{out}: {total * SECTOR >> 20} MiB, {len(placed)} files')


if __name__ == '__main__':
    main()
