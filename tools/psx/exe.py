"""PS-X EXE loader."""
import struct


class PsxExe:
    def __init__(self, path):
        data = open(path, "rb").read()
        assert data[:8] == b"PS-X EXE", "not a PS-X EXE"
        (self.pc0, self.gp0, self.t_addr, self.t_size, self.d_addr,
         self.d_size, self.b_addr, self.b_size, self.s_addr,
         self.s_size) = struct.unpack("<10I", data[0x10:0x38])
        self.text = data[0x800:0x800 + self.t_size]
        # Normalise to KSEG0 (0x80xxxxxx); some headers use KUSEG addresses.
        self.pc0 = 0x80000000 | (self.pc0 & 0x1FFFFFFF)
        self.base = 0x80000000 | (self.t_addr & 0x1FFFFFFF)
        self.end = self.base + len(self.text)

    @staticmethod
    def norm(addr):
        return 0x80000000 | (addr & 0x1FFFFFFF)

    def contains(self, addr):
        return self.base <= self.norm(addr) < self.end

    def word(self, addr):
        o = self.norm(addr) - self.base
        return struct.unpack_from("<I", self.text, o)[0]

    def half(self, addr):
        o = self.norm(addr) - self.base
        return struct.unpack_from("<H", self.text, o)[0]

    def byte(self, addr):
        return self.text[self.norm(addr) - self.base]

    def cstring(self, addr, maxlen=256):
        o = self.norm(addr) - self.base
        e = self.text.find(b"\0", o, o + maxlen)
        if e < 0:
            return None
        return self.text[o:e]
