#!/usr/bin/env python3
# RE Engine PAK archives (KPKA, major 4): read the entry table, decompress entries (zstd / raw deflate), and pull out
# the .sdf shader definitions, which hold the compiled NVN programs (see shader_split.py's sdf_shaders).
# Format reference: Ekey/REE.PAK.Tool (PakHeader / PakEntry / PakUnpack / Murmur3).
# Usage:
#   repak.py info <pak>
#   repak.py list <pak> [substring]                     entry index, sizes and offset
#   repak.py extract <pak> <outdir> [substring]         only useful with a name list: see names
#   repak.py names <pak> <listfile> [substring]         resolve the table's name hashes against a list of paths (one per line)
#   repak.py extract-names <pak> <listfile> <outdir> [substring]
#   repak.py sdf <pak> <outdir>                         find every file starting with "SDF\0" and write sdf_<index>.sdf + sdf_index.tsv
# The .sdf files of re_chunk_000.pak are the input of dm/gfx/tools/shader_split.py for this game; the name list is optional
# (it only gives the files their real paths, which makes the shader map's source column readable).
# Decompression uses libzstd through ctypes (the zstd CLI is the fallback) and zlib for deflate; no pip packages are needed.
import ctypes, ctypes.util, os, struct, subprocess, sys, zlib

KPKA = 0x414B504B
ENTRY = 48
FEATURES = {0: 'none', 8: 'encrypted resources', 12: 'dlc extra data', 24: 'extra data', 40: 'chunked resources', 44: 'dlc extra data 2', 104: 'dd2 extra flag'}
M32 = 0xFFFFFFFF


def die(msg):
    sys.exit('repak: ' + msg)


def rotl32(x, r):
    return ((x << r) | (x >> (32 - r))) & M32


def murmur3_32(data, seed=0xFFFFFFFF):
    """Murmur3 x86 32 as in REE.PAK.Tool's HashCore32 (a partial trailing word is XORed in without the rotate/mix)."""
    c1, c2 = 0xCC9E2D51, 0x1B873593
    h1 = seed
    n = len(data)
    for i in range(0, n - 3, 4):
        k1 = struct.unpack_from('<I', data, i)[0]
        k1 = rotl32((k1 * c1) & M32, 15)
        k1 = (k1 * c2) & M32
        h1 ^= k1
        h1 = (rotl32(h1, 13) * 5 + 0xE6546B64) & M32
    tail = data[n - n % 4:]
    if tail:
        k1 = int.from_bytes(tail + bytes(4 - len(tail)), 'little')
        k1 = rotl32((k1 * c1) & M32, 15)
        h1 ^= (k1 * c2) & M32
    h1 ^= n
    h1 ^= h1 >> 16
    h1 = (h1 * 0x85EBCA6B) & M32
    h1 ^= h1 >> 13
    h1 = (h1 * 0xC2B2AE35) & M32
    return h1 ^ (h1 >> 16)


def name_hash(name):
    return (murmur3_32(name.upper().encode('utf-16-le')) << 32) | murmur3_32(name.lower().encode('utf-16-le'))


class Pak:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')   # kept open: the sdf scan reads tens of thousands of entries, opening the file for each was most of its run time
        magic, major, minor, feat, total, fingerprint = struct.unpack('<IBBhII', self.f.read(16))
        if magic != KPKA:
            die('%s: not a KPKA pak (magic %#x)' % (path, magic))
        if major != 4:
            die('%s: KPKA major %d is not supported (only 4.x)' % (path, major))
        if feat not in FEATURES or feat != 0:
            die('%s: header feature %d (%s) needs an encrypted table, not supported' % (path, feat, FEATURES.get(feat, '?')))
        self.major, self.minor, self.fingerprint, self.total = major, minor, fingerprint, total
        tbl = self.f.read(ENTRY * total)
        if len(tbl) != ENTRY * total:
            die('%s: entry table truncated' % path)
        self.entries = []
        for i in range(total):
            lo, hi, off, comp, dec, attr, chk = struct.unpack_from('<IIqqqqQ', tbl, i * ENTRY)
            self.entries.append({'hash': (hi << 32) | lo, 'offset': off, 'compressed': comp, 'size': dec,
                                 'compression': attr & 0xF, 'encryption': (attr >> 16) & 0xFF, 'checksum': chk})

    def read(self, e):
        if e['encryption']:
            die('entry %#x: resource encryption %d is not supported' % (e['hash'], e['encryption']))
        self.f.seek(e['offset'])
        raw = self.f.read(e['compressed'])
        if len(raw) != e['compressed']:
            die('entry %#x: short read' % e['hash'])
        if e['compression'] == 0:
            out = raw
        elif e['compression'] == 1:
            out = zlib.decompress(raw, -15)
        elif e['compression'] == 2:
            out = zstd_decompress(raw, e['size'])
        else:
            die('entry %#x: unknown compression %d' % (e['hash'], e['compression']))
        if len(out) != e['size']:
            die('entry %#x: decompressed %d bytes, table says %d' % (e['hash'], len(out), e['size']))
        return out


_zstd = None


def zstd_decompress(raw, size):
    global _zstd
    if _zstd is None:
        try:
            lib = ctypes.CDLL(ctypes.util.find_library('zstd') or 'libzstd.dylib')
            lib.ZSTD_decompress.restype = ctypes.c_size_t
            lib.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
            lib.ZSTD_isError.restype = ctypes.c_uint
            lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
            _zstd = lib
        except OSError:
            _zstd = False
    if _zstd:
        dst = ctypes.create_string_buffer(size)
        src = ctypes.create_string_buffer(raw, len(raw))
        n = _zstd.ZSTD_decompress(dst, size, src, len(raw))
        if _zstd.ZSTD_isError(n):
            _zstd.ZSTD_getErrorName.restype = ctypes.c_char_p
            _zstd.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
            die('zstd: ' + _zstd.ZSTD_getErrorName(n).decode())
        return dst.raw[:n]
    r = subprocess.run(['zstd', '-d', '-c', '-q'], input=raw, capture_output=True)
    if r.returncode:
        die('zstd: ' + r.stderr.decode(errors='replace').strip())
    return r.stdout


def load_names(path):
    m = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n').rstrip('\r')
            if line:
                m[name_hash(line)] = line
    return m


def main():
    if len(sys.argv) < 3:
        die(__doc__)
    mode, pakpath = sys.argv[1], sys.argv[2]
    p = Pak(pakpath)
    names = {}
    args = sys.argv[3:]
    if mode in ('names', 'extract-names'):
        if not args:
            die('missing list file')
        names = load_names(args[0])
        args = args[1:]
    if mode == 'info':
        print('%s: KPKA %d.%d, feature none, %d files, fingerprint %#010x' % (pakpath, p.major, p.minor, p.total, p.fingerprint))
        known = sum(1 for e in p.entries if e['hash'] in names) if names else 0
        if names:
            print('name list: %d / %d entries resolved' % (known, p.total))
        return
    if mode == 'sdf':
        if not args:
            die('missing output directory')
        out = args[0]
        os.makedirs(out, exist_ok=True)
        found = []
        for i, e in enumerate(p.entries):
            d = p.read(e)
            if d[:4] == b'SDF\0':
                name = 'sdf_%05d.sdf' % i
                open(os.path.join(out, name), 'wb').write(d)
                found.append((i, name, e['hash'], len(d)))
        with open(os.path.join(out, 'sdf_index.tsv'), 'w') as f:
            f.write('# entry_index\tname\tname_hash\tsize\n')
            for i, name, h, n in found:
                f.write('%d\t%s\t%016X\t%d\n' % (i, name, h, n))
        print('%d .sdf files -> %s' % (len(found), out))
        return
    # list / names / extract / extract-names
    flt = args[1] if mode in ('extract', 'extract-names') and len(args) > 1 else (args[0] if args and mode in ('list', 'names') else None)
    if mode in ('extract', 'extract-names'):
        if not args:
            die('missing output directory')
        out = args[0]
    hits = 0
    for i, e in enumerate(p.entries):
        name = names.get(e['hash'], '')
        if flt and flt not in name:
            continue
        hits += 1
        if mode in ('list', 'names'):
            print('%6d\t%12d\t%12d\t%016X\t%s' % (i, e['compressed'], e['size'], e['hash'], name or '__unknown'))
        else:
            if not name:
                name = '%05d_%016X.bin' % (i, e['hash'])
            dst = os.path.join(out, name)
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            open(dst, 'wb').write(p.read(e))
    if mode in ('extract', 'extract-names'):
        print('%d files -> %s' % (hits, out))
    elif mode in ('list', 'names'):
        print('%d / %d entries' % (hits, p.total), file=sys.stderr)


if __name__ == '__main__':
    main()
