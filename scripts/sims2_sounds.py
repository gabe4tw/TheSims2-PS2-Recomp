#!/usr/bin/env python3
"""The Sims 2 (PAL, SLES-53718): the game's own menu sounds for the quick menu and the launcher, read from the player's disc image.

  python -I scripts/sims2_sounds.py <disc image> <out folder>     writes move.wav, select.wav, back.wav (48 kHz stereo 16-bit)

The three sounds are PS2 ADPCM samples inside /DATA/SAMPLES.ARC (found 2026-11-17 by matching the sound chip's memory of a save state
on the pause wheel against the disc): move at byte 283396032, select at 283387840, back at 249739136 of that file, all played at pitch
0x759 (22 050 Hz). Decoded here exactly as C:/PS2Native/re/extract_ui_sounds.py decodes them from a save state (same output, byte for
byte). The sounds come from the player's own disc and are never kept in the repository. Exit 2: not the SLES-53718 disc.
"""
import os
import struct
import sys
import wave

SOUNDS = (('move', 283396032), ('select', 283387840), ('back', 249739136))
FILTERS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]
SECTOR = 2048


class Iso:
    def __init__(self, path):
        self.f = open(path, 'rb')
        pvd = self.sector(16)
        if pvd[1:6] != b'CD001':
            raise SystemExit('%s: not an ISO 9660 disc image' % path)
        self.root = pvd[156:156 + 34]

    def sector(self, n, count=1):
        self.f.seek(n * SECTOR)
        return self.f.read(count * SECTOR)

    def entries(self, record):
        lba, size = struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
        data = self.sector(lba, (size + SECTOR - 1) // SECTOR)
        pos = 0
        while pos < size:
            length = data[pos]
            if length == 0:  # records do not cross sectors: go to the next one
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            rec = data[pos:pos + length]
            name = rec[33:33 + rec[32]].decode('ascii', 'replace').split(';')[0]
            yield name, rec
            pos += length

    def find(self, path):
        record = self.root
        for part in path.strip('/').split('/'):
            for name, rec in self.entries(record):
                if name.upper() == part.upper():
                    record = rec
                    break
            else:
                return None
        return struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]

    def read(self, path):
        found = self.find(path)
        if not found:
            return None
        self.f.seek(found[0] * SECTOR)
        return self.f.read(found[1])


def decode(data):
    s1 = s2 = 0
    pcm = []
    for pos in range(0, min(len(data), 4096 * 16), 16):
        block = data[pos:pos + 16]
        if len(block) < 16:
            break
        shift, filt, flags = block[0] & 0x0F, (block[0] >> 4) & 0x07, block[1]
        f0, f1 = FILTERS[min(filt, 4)]
        for b in block[2:]:
            for nib in (b & 0x0F, b >> 4):
                v = nib - 16 if nib >= 8 else nib
                v = (v << 12) >> shift
                v += (s1 * f0 + s2 * f1 + 32) >> 6
                v = max(-32768, min(32767, v))
                pcm.append(v)
                s2, s1 = s1, v
        if flags & 1:  # the end of the sample
            break
    return pcm


def write(path, pcm, rate_in):
    rate_out = 48000
    frames = bytearray()
    for i in range(int(len(pcm) * rate_out / rate_in)):
        x = i * rate_in / rate_out
        j = int(x)
        a = pcm[j] if j < len(pcm) else 0
        b = pcm[j + 1] if j + 1 < len(pcm) else 0
        v = int(a + (b - a) * (x - j))
        frames += struct.pack('<hh', v, v)
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate_out)
        w.writeframes(bytes(frames))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    iso = Iso(sys.argv[1])
    cnf = iso.read('SYSTEM.CNF')
    text = cnf.decode('ascii', 'replace') if cnf else ''
    if 'SLES_537.18' not in text.upper():
        print('This is not The Sims 2 (PAL, SLES-53718): no menu sounds taken from it.' + (' The US version (SLUS-21265) is not supported yet.' if 'SLUS_212.65' in text.upper() else ''))
        sys.exit(2)
    arc = iso.find('DATA/SAMPLES.ARC')
    if not arc:
        raise SystemExit('/DATA/SAMPLES.ARC is not on this disc image')
    os.makedirs(sys.argv[2], exist_ok=True)
    rate = 48000 * 0x759 / 4096
    for name, offset in SOUNDS:
        iso.f.seek(arc[0] * SECTOR + offset)
        pcm = decode(iso.f.read(4096 * 16))
        write(os.path.join(sys.argv[2], name + '.wav'), pcm, rate)
        print('%s.wav: %.3f s' % (name, len(pcm) / rate))


if __name__ == '__main__':
    main()
