#!/usr/bin/env python3
"""Extract the ISO9660 filesystem from a PS1 MODE2/2352 disc image.

Form 1 files are written as plain 2048-byte-per-sector data.
Files containing Form 2 sectors (XA audio / STR video) are written as raw
2352-byte sectors so subheaders survive, and get a `.raw2352` marker in the
listing.

Usage: extract_iso.py <track1.bin> <out_dir>
"""
import json
import os
import struct
import sys

SECTOR = 2352


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.nsectors = self.f.tell() // SECTOR

    def raw(self, lba):
        self.f.seek(lba * SECTOR)
        return self.f.read(SECTOR)

    def subheader(self, lba):
        return self.raw(lba)[16:24]

    def data(self, lba):
        s = self.raw(lba)
        return s[24:24 + 2048]


def parse_dir(disc, lba, size, path, out):
    nsec = (size + 2047) // 2048
    buf = b"".join(disc.data(lba + i) for i in range(nsec))
    pos = 0
    while pos < size:
        ln = buf[pos]
        if ln == 0:
            pos = ((pos // 2048) + 1) * 2048
            continue
        rec = buf[pos:pos + ln]
        ext = struct.unpack("<I", rec[2:6])[0]
        dlen = struct.unpack("<I", rec[10:14])[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        pos += ln
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii", "replace")
        full = path + "/" + name
        if flags & 2:
            parse_dir(disc, ext, dlen, full, out)
        else:
            out.append((full, ext, dlen))


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    disc = Disc(src)
    pvd = disc.data(16)
    assert pvd[1:6] == b"CD001", "no PVD"
    root = pvd[156:156 + 34]
    rlba = struct.unpack("<I", root[2:6])[0]
    rlen = struct.unpack("<I", root[10:14])[0]
    files = []
    parse_dir(disc, rlba, rlen, "", files)
    listing = []
    for full, lba, size in sorted(files, key=lambda x: x[1]):
        clean = full.split(";")[0].lstrip("/")
        dst = os.path.join(outdir, clean)
        os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
        nsec = (size + 2047) // 2048
        form2 = False
        for i in range(min(nsec, disc.nsectors - lba)):
            sm = disc.subheader(lba + i)[2]
            if sm & 0x20:
                form2 = True
                break
        if form2:
            # XA/STR files report size in 2048 units; dump raw sectors
            with open(dst, "wb") as o:
                for i in range(nsec):
                    o.write(disc.raw(lba + i))
        else:
            with open(dst, "wb") as o:
                data = b"".join(disc.data(lba + i) for i in range(nsec))
                o.write(data[:size])
        listing.append({"path": clean, "lba": lba, "size": size,
                        "sectors": nsec, "raw2352": form2})
        print(f"{lba:7d} {size:10d} {'RAW ' if form2 else '    '}{clean}")
    with open(os.path.join(outdir, "_listing.json"), "w") as o:
        json.dump(listing, o, indent=1)


if __name__ == "__main__":
    main()
