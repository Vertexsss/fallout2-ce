import harness_common
# -*- coding: utf-8 -*-
# Offline Fallout 2 map scanner: parses .map files straight from the DAT
# archives (no game launch). Ported from fallout2-ce reading code:
#   map.cc mapHeaderRead/_square_load, scripts.cc scriptRead/ExtentRead,
#   object.cc objectRead + proto.cc objectDataRead, dfile.cc DAT2 layout.
import io, os, re, struct, sys, zlib

GAME = harness_common.GAME
UPLOAD = os.environ.get("FALLOUT_UPLOAD_DIR", os.path.join(harness_common.GAME, "..", "upload"))

class Dat2:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0, 2)
        size = self.f.tell()
        self.f.seek(size - 8)
        tree_size, _data_size = struct.unpack('<II', self.f.read(8))
        self.f.seek(size - tree_size - 8)
        buf = self.f.read(tree_size)
        (count,) = struct.unpack_from('<I', buf, 0)
        p = 4
        self.entries = {}
        for _ in range(count):
            (nl,) = struct.unpack_from('<I', buf, p); p += 4
            name = buf[p:p + nl].decode('cp1251', 'replace'); p += nl
            comp, real, packed, off = struct.unpack_from('<BIII', buf, p); p += 13
            self.entries[name.lower().replace('/', '\\')] = (comp, real, packed, off)

    def read(self, name):
        e = self.entries.get(name.lower().replace('/', '\\'))
        if e is None:
            return None
        comp, real, packed, off = e
        self.f.seek(off)
        data = self.f.read(packed)
        return zlib.decompress(data) if comp else data

class FS:
    def __init__(self):
        self.loose = [os.path.join(UPLOAD, 'data')]
        self.dats = []
        for p in (os.path.join(UPLOAD, 'master.dat'), os.path.join(GAME, 'f2_res.dat'), os.path.join(GAME, 'ce.dat')):
            if os.path.exists(p):
                self.dats.append(Dat2(p))

    def read(self, name):
        rel = name.replace('\\', os.sep)
        for base in self.loose:
            p = os.path.join(base, rel)
            if os.path.exists(p):
                return open(p, 'rb').read()
        for dat in self.dats:
            data = dat.read(name)
            if data is not None:
                return data
        return None

class R:
    def __init__(self, data):
        self.d = data
        self.p = 0
    def i32(self):
        v = struct.unpack_from('>i', self.d, self.p)[0]
        self.p += 4
        return v
    def skip(self, n):
        self.p += n
    def bytes(self, n):
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

MSG_RE = re.compile(r'\{(\d+)\}\{[^{}]*\}\{([^{}]*)\}')

class Protos:
    def __init__(self, fs):
        self.fs = fs
        self.cache = {}
        self.names = {}
        for kind, msg in ((0, 'pro_item.msg'), (1, 'pro_crit.msg'), (2, 'pro_scen.msg'), (5, 'pro_misc.msg')):
            data = fs.read('text\\english\\game\\' + msg)
            table = {}
            if data is not None:
                for m in MSG_RE.finditer(data.decode('cp866', 'replace')):
                    table[int(m.group(1))] = m.group(2)
            self.names[kind] = table

    def lst(self, typ, sub):
        if not hasattr(self, '_lst'):
            self._lst = {}
        if typ not in self._lst:
            data = self.fs.read('proto\\%s\\%s.lst' % (sub, sub))
            lines = data.decode('cp1251', 'replace').splitlines() if data else []
            self._lst[typ] = [line.split()[0].strip() if line.split() else '' for line in lines]
        return self._lst[typ]

    def proto(self, pid):
        if pid in self.cache:
            return self.cache[pid]
        typ = (pid >> 24) & 0xff
        sub = {0: 'items', 1: 'critters', 2: 'scenery', 3: 'walls', 4: 'tiles', 5: 'misc'}.get(typ)
        data = None
        if sub is not None:
            # pid maps to a FILE through <sub>.lst (line pid&0xffffff, 1-based) -
            # the file names are NOT the pid (verified: pistol pid 0x8 lives in
            # 00000004.pro in this game's data).
            names = self.lst(typ, sub)
            index = (pid & 0xffffff) - 1
            if 0 <= index < len(names) and names[index]:
                data = self.fs.read('proto\\%s\\%s' % (sub, names[index]))
        info = {'msg': -1, 'sub': -1, 'cost': 0}
        if data is not None and len(data) >= 12:
            info['msg'] = struct.unpack_from('>i', data, 4)[0]
            if typ in (0, 2) and len(data) >= 36:
                info['sub'] = struct.unpack_from('>i', data, 32)[0]
            if typ == 0 and len(data) >= 52:
                info['cost'] = struct.unpack_from('>i', data, 48)[0]
        self.cache[pid] = info
        return info

    def name(self, pid):
        typ = (pid >> 24) & 0xff
        info = self.proto(pid)
        return self.names.get(typ, {}).get(info['msg'], 'pid_0x%x' % pid)

    def item_type(self, pid):
        return self.proto(pid)['sub']

    def scenery_type(self, pid):
        return self.proto(pid)['sub']

def read_obj(r, protos, version):
    o = {}
    o['id'] = r.i32()
    o['tile'] = r.i32()
    r.skip(4 * 4)                     # x, y, sx, sy
    o['frame'] = r.i32()
    o['rot'] = r.i32()
    o['fid'] = r.i32()
    o['flags'] = r.i32()
    o['elev'] = r.i32()
    o['pid'] = r.i32()
    r.skip(4 * 4)                     # cid, lightDistance, lightIntensity, field_74
    o['sid'] = r.i32()
    o['scridx'] = r.i32()
    inv_len = r.i32()
    r.skip(8)                         # capacity, items pointer
    pid = o['pid']
    typ = (pid >> 24) & 0xff
    o['count'] = 1
    if typ == 1:                      # critter
        r.skip(4 + 7 * 4 + 3 * 4)     # reaction, combat data, hp/rad/poison
    else:
        r.skip(4)                     # updated flags
        if typ == 0:                  # item
            it = protos.item_type(pid)
            if it == 3:
                r.skip(8)             # weapon: ammo qty + ammo pid
            elif it == 4:
                o['count'] = r.i32()  # ammo quantity
            elif it == 5:
                o['count'] = r.i32()  # misc charges (money!)
            elif it == 6:
                r.skip(4)             # key code
        elif typ == 2:                # scenery
            st = protos.scenery_type(pid)
            if st == 0:
                r.skip(4)             # door
            elif st == 1:             # stairs
                dest_tile = r.i32()
                dest_map = r.i32()
                o['dest'] = ('stairs', dest_map, dest_tile & 0x3FFFFFF, (dest_tile >> 29) & 7)
            elif st == 2:
                r.skip(8)             # elevator type + level
            elif st in (3, 4):        # ladders
                if version == 19:
                    dest_tile = r.i32()
                    o['dest'] = ('ladder', -1, dest_tile & 0x3FFFFFF, (dest_tile >> 29) & 7)
                else:
                    dest_map = r.i32()
                    dest_tile = r.i32()
                    o['dest'] = ('ladder', dest_map, dest_tile & 0x3FFFFFF, (dest_tile >> 29) & 7)
        elif typ == 5:                # misc
            if 0x5000010 <= pid <= 0x5000017:
                ex_map = r.i32()
                ex_tile = r.i32()
                ex_elev = r.i32()
                r.skip(4)             # rotation
                o['exit'] = (ex_map, ex_tile, ex_elev)
    o['inv'] = []
    for _ in range(inv_len):
        qty = r.i32()
        item = read_obj(r, protos, version)
        o['inv'].append((qty, item))
    return o

def parse_map(data, protos, extras=None):
    r = R(data)
    version = r.i32()
    r.bytes(16)                       # internal name
    r.skip(4 * 3)                     # entering tile/elev/rotation
    lvars = r.i32()
    r.skip(4)                         # script index
    flags = r.i32()
    r.skip(4)                         # darkness
    gvars = r.i32()
    r.skip(4 + 4)                     # map index, last visit time
    r.skip(44 * 4)
    r.skip(4 * (gvars + lvars))
    for elev in range(3):
        if (flags & (2 << elev)) == 0:
            r.skip(10000 * 4)         # squares
    for kind in range(5):             # script lists
        cnt = r.i32()
        if cnt <= 0:
            continue
        got = 0
        for _ in range((cnt + 15) // 16):
            for _ in range(16):
                sid = r.i32()
                r.skip(4)
                st = (sid >> 24) & 0xff
                built_tile = radius = None
                if st == 1:
                    built_tile = r.i32()
                    radius = r.i32()
                elif st == 2:
                    r.skip(4)         # timed: time
                r.skip(4)             # flags
                index = r.i32()
                r.skip(12 * 4)        # prg..field_50
                if extras is not None and got < cnt:
                    if st == 1:
                        extras.setdefault('spatial', []).append(
                            (sid, built_tile & 0x3FFFFFF, (built_tile >> 29) & 7, radius, index))
                    extras.setdefault('scripts', []).append((kind, sid, index))
                got += 1
            r.skip(8)                 # extent length + next
    r.i32()                           # total object count
    objects = []
    for elev in range(3):
        cnt = r.i32()
        for _ in range(cnt):
            objects.append(read_obj(r, protos, version))
    if r.p != len(data):
        sys.stderr.write('WARN: %d trailing bytes\n' % (len(data) - r.p))
    return objects

def map_names(fs):
    txt = fs.read('data\\maps.txt').decode('cp1251', 'replace')
    lookup = {}
    cur = None
    for line in txt.splitlines():
        m = re.match(r'\[Map (\d+)\]', line)
        if m:
            cur = int(m.group(1))
        m = re.match(r'map_name=(.*)', line.strip(), re.I)
        if m and cur is not None:
            lookup[cur] = m.group(1).strip()
    return lookup

def describe(o, protos, depth=0, qty=None):
    pid = o['pid']
    n = qty if qty is not None else o['count']
    line = '%s%s%s [pid 0x%x tile %d elev %d]' % (
        '  ' * depth, ('x%d ' % n) if n != 1 else '', protos.name(pid), pid, o['tile'], o['elev'])
    out = [line]
    for q, item in o['inv']:
        out.extend(describe(item, protos, depth + 1, q * max(item['count'], 1)))
    return out

def main():
    fs = FS()
    protos = Protos(fs)
    names = map_names(fs)
    out = io.open(os.path.join(GAME, 'scan_out.txt'), 'w', encoding='utf-8')
    for arg in sys.argv[1:]:
        name = names.get(int(arg), arg) if arg.isdigit() else arg
        data = fs.read('maps\\%s.map' % name)
        if data is None:
            out.write('=== %s: NOT FOUND ===\n' % name)
            continue
        objects = parse_map(data, protos)
        out.write('=== %s (%d objects) ===\n' % (name, len(objects)))
        for o in objects:
            typ = (o['pid'] >> 24) & 0xff
            interesting = False
            if typ == 0 and protos.item_type(o['pid']) == 1:
                interesting = True    # container
            elif typ == 1 and o['inv']:
                interesting = True    # critter carrying loot
            elif typ == 0:
                interesting = True    # loose ground item
            if interesting:
                kind = 'CONTAINER' if (typ == 0 and protos.item_type(o['pid']) == 1) else ('CRITTER' if typ == 1 else 'GROUND')
                lines = describe(o, protos)
                out.write('%s %s\n' % (kind, lines[0]))
                for line in lines[1:]:
                    out.write('%s\n' % line)
    out.close()
    print('done')

if __name__ == '__main__':
    main()
