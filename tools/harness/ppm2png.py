import harness_common
import glob, struct, sys, zlib, os
os.chdir(harness_common.GAME)
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
pattern = sys.argv[1] if len(sys.argv) > 1 else 'zoom_shot*.ppm'
for f in sorted(glob.glob(pattern)):
    data = open(f, 'rb').read()
    hdr, _, rest = data.partition(b'255 ')
    parts = hdr.split()
    w, h = int(parts[1]), int(parts[2])
    raw = rest[:w * h * 3]
    rows = [b'\x00' + raw[y * w * 3:(y + 1) * w * 3] for y in range(h)]
    png = (b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(b''.join(rows)))
        + chunk(b'IEND', b''))
    open(f.replace('.ppm', '.png'), 'wb').write(png)
    print(f, w, h, 'converted')
