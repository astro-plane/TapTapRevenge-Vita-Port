"""
pvrtc.py - decode PowerVR PVRTC textures (the .pvrtc files some themes use)
to ordinary RGBA images.  Pure Python, no external tools.

    from pvrtc import decode
    image = decode('spinner.pvrtc')      # a PIL RGBA image

Handles the legacy PVR v2 container ("PVR!" header) with 4 bpp and 2 bpp
PVRTC1 data; only the first (largest) mip level is read.

How the format works: the texture stores two low-resolution colour images,
A and B (one colour each per 4x4 block, or 8x4 at 2 bpp), plus a per-pixel
modulation value.  Both colour images are scaled up bilinearly and each pixel
is a blend of the two, chosen by its modulation value.  Blocks are stored in
Morton (Z) order.  Colours are interpolated in floating point here, so the
result can differ from a hardware decoder by a rounding step.
"""
import struct
from PIL import Image

MOD4 = (0, 3, 5, 8)          # modulation weights, in eighths of colour B


def _colour_a(c):
    if c & 0x8000:                                   # opaque: 5:5:4 bits
        b = (c & 0x1e) | ((c & 0x1e) >> 4)
        return ((c >> 10) & 31, (c >> 5) & 31, b, 15)
    r = (c >> 8) & 15; g = (c >> 4) & 15; b = (c >> 1) & 7       # translucent: 3:4:4:3
    return ((r << 1) | (r >> 3), (g << 1) | (g >> 3), (b << 2) | (b >> 1), ((c >> 12) & 7) << 1)


def _colour_b(c):
    c >>= 16
    if c & 0x8000:                                   # opaque: 5:5:5
        return ((c >> 10) & 31, (c >> 5) & 31, c & 31, 15)
    r = (c >> 8) & 15; g = (c >> 4) & 15; b = c & 15             # translucent: 3:4:4:4
    return ((r << 1) | (r >> 3), (g << 1) | (g >> 3), (b << 1) | (b >> 3), ((c >> 12) & 7) << 1)


def _twiddle(xsize, ysize, x, y, y_low=True):
    """Morton index of block (x, y) in a grid of xsize by ysize blocks."""
    t, shift, bit = 0, 0, 1
    while bit < min(xsize, ysize):
        if (y if y_low else x) & bit:
            t |= bit << shift
        if (x if y_low else y) & bit:
            t |= bit << (shift + 1)
        bit <<= 1
        shift += 1
    rest = (x if xsize > ysize else y) >> shift
    return t | (rest << (2 * shift))


def _scale8(col):
    return (col[0] * 255.0 / 31, col[1] * 255.0 / 31, col[2] * 255.0 / 31, col[3] * 255.0 / 15)


def decode_raw(data, width, height, bpp, y_low=True):
    bw_px = 8 if bpp == 2 else 4
    xb, yb = width // bw_px, height // 4
    ca = [[None] * xb for _ in range(yb)]
    cb = [[None] * xb for _ in range(yb)]
    # mod[y][x]: weight of colour B in eighths; -1 = to be interpolated (2 bpp)
    mod = [[0] * width for _ in range(height)]
    punch = [[False] * width for _ in range(height)]
    block_mode = [[0] * xb for _ in range(yb)]
    for by in range(yb):
        for bx in range(xb):
            off = _twiddle(xb, yb, bx, by, y_low) * 8
            m, c = struct.unpack_from('<II', data, off)
            ca[by][bx], cb[by][bx] = _scale8(_colour_a(c)), _scale8(_colour_b(c))
            mode = c & 1
            x0, y0 = bx * bw_px, by * 4
            if bpp == 4:
                for y in range(4):
                    for x in range(4):
                        v = m & 3
                        m >>= 2
                        if mode:                     # punch-through alpha mode
                            mod[y0 + y][x0 + x] = (0, 4, 4, 8)[v]
                            punch[y0 + y][x0 + x] = v == 2
                        else:
                            mod[y0 + y][x0 + x] = MOD4[v]
            elif not mode:                           # 2 bpp, one bit per pixel
                for y in range(4):
                    for x in range(8):
                        mod[y0 + y][x0 + x] = 8 if m & 1 else 0
                        m >>= 1
            else:                                    # 2 bpp, checkerboard + interpolation
                sub = 1                              # 1: both directions, 2: horizontal, 3: vertical
                if m & 1:
                    if m & (1 << 20):
                        sub = 3
                        m = (m & ~(1 << 20)) | ((m & (1 << 21)) >> 1)
                    else:
                        sub = 2
                    m = (m | 1) if m & 2 else (m & ~1)
                else:
                    m = (m | 1) if m & 2 else (m & ~1)
                block_mode[by][bx] = sub
                for y in range(4):
                    for x in range(8):
                        if (x ^ y) & 1:
                            mod[y0 + y][x0 + x] = -1
                        else:
                            mod[y0 + y][x0 + x] = MOD4[m & 3]
                            m >>= 2
    if bpp == 2:                                     # fill the checkerboard gaps
        def stored(x, y):
            v = mod[y % height][x % width]
            return v if v >= 0 else None
        fill = {}
        for y in range(height):
            for x in range(width):
                if mod[y][x] >= 0:
                    continue
                sub = block_mode[y // 4][x // 8]
                if sub == 2:
                    ns = [stored(x - 1, y), stored(x + 1, y)]
                elif sub == 3:
                    ns = [stored(x, y - 1), stored(x, y + 1)]
                else:
                    ns = [stored(x - 1, y), stored(x + 1, y), stored(x, y - 1), stored(x, y + 1)]
                ns = [n for n in ns if n is not None]
                fill[(x, y)] = sum(ns) / len(ns) if ns else 4
        for (x, y), v in fill.items():
            mod[y][x] = v

    out = bytearray(width * height * 4)
    half_x = bw_px // 2
    for y in range(height):
        gy = (y - 2) / 4.0
        j0 = int(gy // 1); fy = gy - j0
        j0 %= yb; j1 = (j0 + 1) % yb
        for x in range(width):
            gx = (x - half_x) / float(bw_px)
            i0 = int(gx // 1); fx = gx - i0
            i0 %= xb; i1 = (i0 + 1) % xb
            w00, w10, w01, w11 = (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy
            k = mod[y][x] / 8.0
            o = (y * width + x) * 4
            for ch in range(4):
                a = ca[j0][i0][ch] * w00 + ca[j0][i1][ch] * w10 + ca[j1][i0][ch] * w01 + ca[j1][i1][ch] * w11
                b = cb[j0][i0][ch] * w00 + cb[j0][i1][ch] * w10 + cb[j1][i0][ch] * w01 + cb[j1][i1][ch] * w11
                v = a + (b - a) * k
                out[o + ch] = 0 if v < 0 else 255 if v > 255 else int(v + 0.5)
            if punch[y][x]:
                out[o + 3] = 0
    return Image.frombytes('RGBA', (width, height), bytes(out))


def decode(path):
    with open(path, 'rb') as f:
        b = f.read()
    if len(b) < 52 or b[44:48] != b'PVR!':
        raise ValueError('not a PVR v2 file: ' + path)
    hdr_len, height, width, _mips, flags = struct.unpack_from('<5I', b, 0)
    fmt = flags & 0xff
    bpp = {0x18: 2, 0x19: 4, 0x0c: 2, 0x0d: 4}.get(fmt)
    if not bpp:
        raise ValueError('unsupported PVR pixel format 0x%02x in %s' % (fmt, path))
    need = width * height * bpp // 8
    if width & (width - 1) or height & (height - 1) or len(b) < hdr_len + need:
        raise ValueError('unexpected size in ' + path)
    return decode_raw(b[hdr_len:hdr_len + need], width, height, bpp)


if __name__ == '__main__':
    import sys
    decode(sys.argv[1]).save(sys.argv[2])
