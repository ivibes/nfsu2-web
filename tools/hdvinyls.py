#!/usr/bin/env python3
"""The HD vinyls' remaster pack: TEXPACKS/hdvinyls.pak in the game folder,
which the runtime (runtime/hdskin.c, on the Mac and in the browser) reads when
settings.ini has hd_vinyls = 1024 or 2048.  It holds remastered vinyl, mask
and doorline textures; the runtime paints each car's HD copy from them
(runtime/paintblend.c) and shows it in place of the game's 512 paint.

    tools/hdvinyls.py <game folder> <remaster folder>
    tools/hdvinyls.py --out <file.pak> <remaster folder>

<remaster folder> holds one DDS file per texture, named by the texture's full
name as the game hashes it: <TEXTURE NAME>.dds, e.g. SKYLINE_UNIQUE_7_DRAGON.dds
and SKYLINE_UNIQUE_7_DRAGON_MASK.dds (case does not matter: the name is
upper-cased).  Each is an uncompressed 32-bit DDS (A8R8G8B8, A8B8G8R8 or
X8R8G8B8, top row first; only level 0 is read), square, its side a power of
two from 64 to 4096: the stock size (512) times 2 for hd_vinyls = 1024 or
times 4 for 2048.  The runtime box-filters a larger one down and scales a
smaller one up (nearest), so one 2048 set serves both settings.  A mask's
coverage is its green channel.  A vinyl the player colours is a weight map
(red, green and blue weigh the player's three colours), as the stock one is.
docs/hd-vinyls.md says how to make them.

Pack layout (little-endian):
  header   'NHDV', version 1, entry count, 0
  entries  32 bytes each, sorted by hash:
           hash        bStringHash of the texture's name (TextureInfo +0x24)
           w, h (u16)  its size
           off, size   its pixels: w x h words 0xAARRGGBB (bytes B, G, R, A),
                       top row first, raw deflate (RFC 1951)
           name        file offset of its name (for the log)
           crc         CRC-32 (zlib's) of the pixels unpacked
           0, 0
  the names, then (at a 4096-byte boundary) the pixels, each at a 16-byte
  boundary; textures with the same pixels share them.
"""
import argparse, hashlib, os, struct, sys, zlib
from concurrent.futures import ProcessPoolExecutor

MAGIC, VERSION, ENT = b'NHDV', 1, 32
MAX_SIDE = 4096
MAX_PACK = 0x7FFFFFFF


def bstring_hash(s):
    """the game's name hash (bStringHash, 0x43DB50)"""
    h = 0xFFFFFFFF
    for c in s.encode('latin1'): h = (h * 33 + c) & 0xFFFFFFFF
    return h


def read_dds(path):
    """(w, h, pixels as B, G, R, A bytes) of an uncompressed 32-bit DDS, as runtime/hdskin.c hd_load_dds reads it;
    raises ValueError with the reason otherwise"""
    with open(path, 'rb') as f: b = f.read()
    if len(b) < 128 or b[:4] != b'DDS ' or struct.unpack_from('<I', b, 4)[0] != 124 or struct.unpack_from('<I', b, 76)[0] != 32:
        raise ValueError('not a DDS file')
    h, w = struct.unpack_from('<II', b, 12)
    pf, four, bits, rm, gm, bm, am = struct.unpack_from('<I4s5I', b, 80)
    if pf & 4: raise ValueError(f'compressed (FourCC {four.decode("latin1")}); only uncompressed 32-bit is read')
    if not pf & 0x40 or bits != 32 or gm != 0xFF00: raise ValueError(f'not 32-bit RGB (flags {pf:x}, {bits} bits)')
    if (rm, bm) == (0xFF0000, 0xFF): swap = False
    elif (rm, bm) == (0xFF, 0xFF0000): swap = True
    else: raise ValueError(f'channel masks {rm:08x} {gm:08x} {bm:08x} not handled')
    if w != h or w < 64 or w > MAX_SIDE or w & (w - 1):
        raise ValueError(f'size {w}x{h}: square, a power of two from 64 to {MAX_SIDE}')
    n = w * h * 4
    if len(b) < 128 + n: raise ValueError('truncated')
    px = bytearray(b[128:128 + n])
    if swap: px[0::4], px[2::4] = px[2::4], px[0::4]
    if not pf & 1 or am != 0xFF000000: px[3::4] = b'\xff' * (w * h)          # X8R8G8B8: opaque
    return w, h, bytes(px)


def one(path):
    """a DDS file: (w, h, crc, digest, deflated pixels), or the reason it is refused"""
    try: w, h, px = read_dds(path)
    except (ValueError, OSError) as e: return str(e)
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    return w, h, zlib.crc32(px), hashlib.sha256(px).digest(), c.compress(px) + c.flush()


def build(src, out):
    files = sorted(f for f in os.listdir(src) if f.lower().endswith('.dds') and not f.startswith('.'))
    if not files: sys.exit(f'{src}: no .dds files')
    names, by_hash = {}, {}
    refused, ents = [], []
    for f in files[:]:
        name = f[:-4].upper()
        try: h = bstring_hash(name)
        except UnicodeEncodeError:
            refused.append(f'{f}: not a texture name (its name has characters the game\'s names do not)'); files.remove(f); continue
        if h in by_hash: sys.exit(f'{src}: {f} and {by_hash[h]} are the same texture ({name})')
        by_hash[h] = f; names[f] = (name, h)
    data, at = bytearray(), {}
    with ProcessPoolExecutor() as ex:
        for i, (f, r) in enumerate(zip(files, ex.map(one, [os.path.join(src, f) for f in files]))):
            print(f'\r  {i + 1}/{len(files)} textures', end='', flush=True)
            if isinstance(r, str): refused.append(f'{f}: {r}'); continue
            w, h, crc, digest, z = r
            if digest not in at:
                at[digest] = (len(data), len(z)); data += z; data += bytes(-len(data) % 16)
            off, size = at[digest]
            name, nh = names[f]
            ents.append(dict(hash=nh, w=w, h=h, off=off, size=size, crc=crc, name=name))
    print()
    if not ents: sys.exit(f'{src}: no texture this can use\n  ' + '\n  '.join(refused))
    ents.sort(key=lambda e: e['hash'])
    table = 16 + ENT * len(ents)
    blob = bytearray()
    for e in ents:
        e['name_at'] = table + len(blob); blob += e['name'].encode('latin1') + b'\0'
    base = (table + len(blob) + 4095) & ~4095
    if base + len(data) > MAX_PACK:
        sys.exit(f'{src}: {(base + len(data)) / 1048576:.0f} MB packed; the runtime reads a pack of at most 2 GB (the browser: a 32-bit long)')
    head = bytearray(struct.pack('<4sIII', MAGIC, VERSION, len(ents), 0))
    for e in ents:
        head += struct.pack('<IHHIIIIII', e['hash'], e['w'], e['h'], base + e['off'], e['size'], e['name_at'], e['crc'], 0, 0)
    head += blob
    head += bytes(base - len(head))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + '.part'
    with open(tmp, 'wb') as f:
        f.write(head); f.write(data)
    os.replace(tmp, out)
    raw = sum(e['w'] * e['h'] * 4 for e in ents)
    sizes = sorted({f"{e['w']}x{e['h']}" for e in ents})
    print(f'hdvinyls: {len(ents)} textures ({", ".join(sizes)}), {raw / 1048576:.0f} MB as pixels, '
          f'{os.path.getsize(out) / 1048576:.1f} MB packed -> {out}')
    if refused: print(f'  refused: {len(refused)}:\n    ' + '\n    '.join(refused))


def main():
    ap = argparse.ArgumentParser(usage='tools/hdvinyls.py <game folder> <remaster folder>\n'
                                       '       tools/hdvinyls.py --out <file.pak> <remaster folder>')
    ap.add_argument('paths', nargs='+', metavar='folder')
    ap.add_argument('--out', help='write the pack here instead of <game folder>/TEXPACKS/hdvinyls.pak')
    a = ap.parse_args()
    if a.out:
        if len(a.paths) != 1: ap.error('--out takes one folder: the remasters\'')
        src, out = a.paths[0], a.out
    else:
        if len(a.paths) != 2: ap.error('a game folder and a remaster folder')
        game, src = a.paths
        if not any(n.upper() == 'SPEED2.EXE' for n in os.listdir(game)): sys.exit(f'{game}: no SPEED2.EXE, not the game folder')
        tp = next((os.path.join(game, n) for n in os.listdir(game) if n.upper() == 'TEXPACKS'), os.path.join(game, 'TEXPACKS'))
        out = os.path.join(tp, 'hdvinyls.pak')
    if not os.path.isdir(src): sys.exit(f'{src}: not a folder')
    build(src, out)


if __name__ == '__main__':
    main()
