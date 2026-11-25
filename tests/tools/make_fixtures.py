"""Builds test fixtures for Cider without a Mac.

  make_fixtures.py OUTDIR BUILD_TESTS_DIR

Creates:
  Cider Test.app       app bundle wrapping a native x86_64 test program
  Java Hello.app       Oracle-appbundler style Java app (needs javac)
  Electron Hello.app   Electron app skeleton (binary Info.plist in the framework)
  CiderTest.dmg        UDIF disk image (zlib chunks) holding an HFS+ volume with
                       "Cider Test.app", an "Applications" symlink and a hidden
                       .background folder, like a typical app download.
"""
import base64
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

HFS_EPOCH = 2082844800


def info_plist(entries):
    body = "".join(f"\t<key>{k}</key>\n\t<string>{v}</string>\n" for k, v in entries.items())
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
            f'<plist version="1.0">\n<dict>\n{body}</dict>\n</plist>\n')


def bplist(d):
    """Binary plist for a flat dict of strings."""
    objs, refs = [], {}

    def add(s):
        if s not in refs:
            refs[s] = len(objs)
            objs.append(s)
        return refs[s]

    keys = [add(k) for k in d]
    vals = [add(v) for v in d.values()]
    top = len(objs)
    out = bytearray(b"bplist00")
    offsets = []
    for s in objs:
        offsets.append(len(out))
        b = s.encode("ascii")
        out += bytes([0x50 | len(b)]) if len(b) < 15 else bytes([0x5F, 0x10, len(b)])
        out += b
    offsets.append(len(out))
    out += bytes([0xD0 | len(d)]) + bytes(keys) + bytes(vals)
    table = len(out)
    for o in offsets:
        out += struct.pack(">H", o)
    out += bytes(6) + bytes([2, 1]) + struct.pack(">QQQ", len(offsets), top, table)
    return bytes(out)


def write(path, data, mode="wb"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as f:
        f.write(data)


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy(src, dst)


def make_native_app(root, exe):
    app = os.path.join(root, "Cider Test.app", "Contents")
    write(os.path.join(app, "Info.plist"), info_plist({
        "CFBundleExecutable": "cidertest", "CFBundleIdentifier": "org.cider.test",
        "CFBundleName": "Cider Test", "CFBundleShortVersionString": "1.0",
        "CFBundlePackageType": "APPL"}).encode())
    copy(exe, os.path.join(app, "MacOS", "cidertest"))
    write(os.path.join(app, "Resources", "readme.txt"), b"resources travel with the app\n")
    write(os.path.join(app, "PkgInfo"), b"APPL????")


def make_java_app(root):
    app = os.path.join(root, "Java Hello.app", "Contents")
    write(os.path.join(app, "Info.plist"), info_plist({
        "CFBundleExecutable": "JavaAppLauncher", "CFBundleIdentifier": "org.cider.javahello",
        "CFBundleName": "Java Hello", "JVMMainClassName": "hello.Main"}).encode())
    write(os.path.join(app, "MacOS", "JavaAppLauncher"), b"\xcf\xfa\xed\xfe" + bytes(60))
    src = tempfile.mkdtemp()
    java = os.path.join(src, "hello", "Main.java")
    write(java, b'package hello;\npublic class Main {\n  public static void main(String[] a) {\n'
                b'    System.out.println("Hello from a Java Mac app, running on " + '
                b'(System.getProperty("os.name").startsWith("Windows") ? "Windows" : "?"));\n  }\n}\n')
    subprocess.check_call(["javac", "-d", src, java])
    os.makedirs(os.path.join(app, "Java"))
    with zipfile.ZipFile(os.path.join(app, "Java", "hello.jar"), "w") as z:
        z.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\n\r\n")
        z.write(os.path.join(src, "hello", "Main.class"), "hello/Main.class")
    shutil.rmtree(src)


def make_electron_app(root, exe):
    app = os.path.join(root, "Electron Hello.app", "Contents")
    write(os.path.join(app, "Info.plist"), info_plist({
        "CFBundleExecutable": "Electron Hello", "CFBundleIdentifier": "org.cider.electronhello",
        "CFBundleName": "Electron Hello", "CFBundleShortVersionString": "2.1.0"}).encode())
    copy(exe, os.path.join(app, "MacOS", "Electron Hello"))
    fw = os.path.join(app, "Frameworks", "Electron Framework.framework", "Versions", "A", "Resources")
    write(os.path.join(fw, "Info.plist"), bplist({"CFBundleIdentifier": "com.github.Electron.framework",
                                                    "CFBundleVersion": "33.2.1"}))
    write(os.path.join(app, "Resources", "app", "package.json"), b'{"name":"electron-hello","main":"main.js"}\n')
    write(os.path.join(app, "Resources", "app", "main.js"),
          b"const { app, BrowserWindow } = require('electron');\n"
          b"app.whenReady().then(() => new BrowserWindow({ width: 640, height: 400 }).loadURL("
          b"'data:text/html,<h1>Hello from a Mac Electron app</h1>'));\n")


# ---------------------------------------------------------------------------
# HFS+ (HFSX) volume writer

class HFS:
    BS = 4096
    NODE = 8192

    def __init__(self, name):
        self.name = name
        self.next_id = 16
        self.items = []          # (parent, name, kind, id, data, extra)
        self.blocks = []         # list of (start_block, bytes)
        self.next_block = 0

    def add_tree(self, src, parent=2):
        for entry in sorted(os.listdir(src)):
            path = os.path.join(src, entry)
            if os.path.isdir(path):
                fid = self.mkdir(entry, parent)
                self.add_tree(path, fid)
            else:
                self.add_file(entry, open(path, "rb").read(), parent)

    def mkdir(self, name, parent=2):
        fid = self.next_id
        self.next_id += 1
        self.items.append((parent, name, "dir", fid, None))
        return fid

    def add_file(self, name, data, parent=2, symlink=False):
        fid = self.next_id
        self.next_id += 1
        self.items.append((parent, name, "link" if symlink else "file", fid, data))
        return fid

    def alloc(self, data):
        n = max(1, (len(data) + self.BS - 1) // self.BS)
        start = self.next_block
        self.next_block += n
        self.blocks.append((start, data))
        return start, n

    @staticmethod
    def fork(size, start, count):
        f = struct.pack(">QII", size, 0, count)
        f += struct.pack(">II", start, count) + bytes(56)
        return f

    @staticmethod
    def key(parent, name):
        u = name.encode("utf-16-be")
        return struct.pack(">HIH", 6 + len(u), parent, len(u) // 2) + u

    def build(self):
        now = HFS_EPOCH + 1700000000
        # block 0: volume header, 1: allocation bitmap, 2-3: extents tree, then file data
        self.next_block = 2 + self.NODE // self.BS
        records = []
        folders = {2: 0}
        for parent, name, kind, fid, data in self.items:
            folders[parent] = folders.get(parent, 0) + 1
        perms_dir = struct.pack(">IIBBHI", 501, 20, 0, 0, 0o40755, 0)
        root_rec = struct.pack(">hHII5I", 1, 0, folders.get(2, 0), 2, now, now, now, now, 0) + perms_dir + bytes(32) + bytes(8)
        records.append((self.key(1, self.name), root_rec))
        records.append((self.key(2, ""), struct.pack(">hhI", 3, 0, 1) + self.key(0, self.name)[6:]))
        nfiles = nfolders = 0
        for parent, name, kind, fid, data in self.items:
            if kind == "dir":
                nfolders += 1
                rec = struct.pack(">hHII5I", 1, 0, folders.get(fid, 0), fid, now, now, now, now, 0) + perms_dir + bytes(32) + bytes(8)
                records.append((self.key(parent, name), rec))
                records.append((self.key(fid, ""), struct.pack(">hhI", 3, 0, parent) + self.key(0, name)[6:]))
            else:
                nfiles += 1
                start, count = self.alloc(data) if data else (0, 0)
                if kind == "link":
                    perms = struct.pack(">IIBBHI", 501, 20, 0, 0, 0o120755, 0)
                    finfo = b"slnk" + b"rhap" + bytes(8)
                else:
                    perms = struct.pack(">IIBBHI", 501, 20, 0, 0, 0o100755, 0)
                    finfo = bytes(16)
                rec = struct.pack(">hHII5I", 2, 2, 0, fid, now, now, now, now, 0) + perms + finfo + bytes(16)
                rec += struct.pack(">II", 0, 0) + self.fork(len(data or b""), start, count) + self.fork(0, 0, 0)
                assert len(rec) == 248
                records.append((self.key(parent, name), rec))
                records.append((self.key(fid, ""), struct.pack(">hhI", 4, 0, parent) + self.key(0, name)[6:]))

        def sort_key(r):
            k = r[0]
            parent = struct.unpack(">I", k[2:6])[0]
            return (parent, k[8:])
        records.sort(key=sort_key)

        # catalog: header node + one or more leaf nodes chained together
        leaves, cur, used = [], [], 14
        for k, v in records:
            size = len(k) + len(v) + 2
            if used + size + 2 > self.NODE:
                leaves.append(cur)
                cur, used = [], 14
            cur.append(k + v)
            used += size
        leaves.append(cur)
        if len(leaves) > 1:
            raise SystemExit("fixture too large for a single-level catalog")

        def node(kind, height, recs, flink=0):
            body = bytearray(self.NODE)
            struct.pack_into(">IIbBHH", body, 0, flink, 0, kind, height, len(recs), 0)
            off = 14
            offs = []
            for r in recs:
                offs.append(off)
                body[off:off + len(r)] = r
                off += len(r)
            offs.append(off)
            for i, o in enumerate(offs):
                struct.pack_into(">H", body, self.NODE - 2 * (i + 1), o)
            return bytes(body)

        def header_node(depth, root, nleaf, first, last, total, maxkey, cmp):
            hdr = struct.pack(">HIIIIHHIIHIBBI", depth, root, nleaf, first, last, self.NODE, maxkey, total, 0, 0,
                              self.NODE * total, 0, cmp, 6) + bytes(64)
            used = (0xFF00 >> total) & 0xFF                      # one bit per node in use
            free_map = bytes([used]) + bytes(self.NODE - 14 - len(hdr) - 128 - 8 - 1)
            return node(1, 0, [hdr, bytes(128), free_map])

        total_nodes = 1 + len(leaves)
        cat = header_node(1, 1, len(records), 1, 1, total_nodes, 516, 0xBC)
        cat += node(-1, 1, leaves[0])
        ext = header_node(0, 0, 0, 0, 0, 1, 10, 0)

        ext_start, ext_count = 2, self.NODE // self.BS
        self.next_block = max(self.next_block, ext_start + ext_count)
        cat_start, cat_count = self.next_block, len(cat) // self.BS
        self.next_block += cat_count
        total_blocks = self.next_block + 4
        bitmap = bytearray(self.BS)
        for b in range(self.next_block):
            bitmap[b // 8] |= 0x80 >> (b % 8)
        bitmap[(total_blocks - 1) // 8] |= 0x80 >> ((total_blocks - 1) % 8)

        vol = bytearray(total_blocks * self.BS)
        for start, data in self.blocks:
            vol[start * self.BS:start * self.BS + len(data)] = data
        vol[1 * self.BS:2 * self.BS] = bitmap
        vol[ext_start * self.BS:ext_start * self.BS + len(ext)] = ext
        vol[cat_start * self.BS:cat_start * self.BS + len(cat)] = cat

        vh = struct.pack(">2sHI4sI4I2I8IQ", b"HX", 5, 0x100, b"10.0", 0, now, now, 0, now,
                         nfiles, nfolders, self.BS, total_blocks, 4, self.next_block, 0x10000, 0x10000,
                         self.next_id, 1, 1)
        vh += bytes(32)
        vh += self.fork(self.BS, 1, 1)
        vh += self.fork(len(ext), ext_start, ext_count)
        vh += self.fork(len(cat), cat_start, cat_count)
        vh += self.fork(0, 0, 0) + self.fork(0, 0, 0)
        assert len(vh) == 512, len(vh)
        vol[1024:1536] = vh
        vol[len(vol) - 1024:len(vol) - 512] = vh
        return bytes(vol)


# ---------------------------------------------------------------------------
# UDIF container

def checksum(crc):
    """UDIF checksum field: type 2 (CRC32), 32 bits, value in the first word."""
    return struct.pack(">III", 2, 32, crc) + bytes(124)


def mish(first_sector, chunks, crc):
    count = sum(c[3] for c in chunks)
    out = struct.pack(">4sIQQQII", b"mish", 1, first_sector, count, 0, 0, 0)
    out += bytes(24) + checksum(crc)
    out += struct.pack(">I", len(chunks) + 1)
    for c in chunks:
        out += struct.pack(">IIQQQQ", *c)
    out += struct.pack(">IIQQQQ", 0xFFFFFFFF, 0, count, 0, 0, 0)
    return out


def udif(volume, path):
    data = bytearray()
    parts = []
    # partition 0: 40 sectors of "partition map" that compresses to nothing
    parts.append(("Protective Master Boot Record (MBR : 0)", 0, [(0x00000002, 0, 0, 40, 0, 0)], zlib.crc32(bytes(40 * 512))))
    # partition 1: the HFS volume at sector 40, in 1 MiB zlib chunks (zero runs stored as zero-fill)
    chunks = []
    per = 2048
    nsect = len(volume) // 512
    for s in range(0, nsect, per):
        n = min(per, nsect - s)
        raw = volume[s * 512:(s + n) * 512]
        if not any(raw):
            chunks.append((0x00000000, 0, s, n, len(data), 0))
            continue
        comp = zlib.compress(raw, 9)
        if len(chunks) % 3 == 1:
            chunks.append((0x00000001, 0, s, n, len(data), len(raw)))   # some raw chunks too
            data += raw
        else:
            chunks.append((0x80000005, 0, s, n, len(data), len(comp)))
            data += comp
    parts.append(("disk image (Apple_HFSX : 1)", 40, chunks, zlib.crc32(volume)))

    blkx = ""
    for i, (name, first, ch, crc) in enumerate(parts):
        b64 = base64.b64encode(mish(first, ch, crc)).decode()
        blkx += (f"\t\t\t<dict>\n\t\t\t\t<key>Attributes</key>\n\t\t\t\t<string>0x0050</string>\n"
                 f"\t\t\t\t<key>Data</key>\n\t\t\t\t<data>{b64}</data>\n"
                 f"\t\t\t\t<key>ID</key>\n\t\t\t\t<string>{i - 1}</string>\n"
                 f"\t\t\t\t<key>Name</key>\n\t\t\t\t<string>{name}</string>\n\t\t\t</dict>\n")
    xml = ('<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0">\n<dict>\n'
           '\t<key>resource-fork</key>\n\t<dict>\n\t\t<key>blkx</key>\n\t\t<array>\n'
           f"{blkx}\t\t</array>\n\t</dict>\n</dict>\n</plist>\n").encode()
    xml_off = len(data)
    master = zlib.crc32(b"".join(struct.pack(">I", p[3]) for p in parts))
    total_sectors = 40 + len(volume) // 512
    koly = struct.pack(">4sIIIQQQQQII16sII128sQQ120sII128sIQ12s", b"koly", 4, 512, 1, 0, 0, len(data), 0, 0, 1, 1,
                       bytes(16), 2, 32, struct.pack(">I", zlib.crc32(data)) + bytes(124), xml_off, len(xml),
                       bytes(120), 2, 32, struct.pack(">I", master) + bytes(124), 1,
                       total_sectors, bytes(12))
    assert len(koly) == 512, len(koly)
    with open(path, "wb") as f:
        f.write(bytes(data) + xml + koly)


def main():
    out, tests = sys.argv[1], sys.argv[2]
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    make_native_app(out, os.path.join(tests, "chained", "t_libc"))
    make_electron_app(out, os.path.join(tests, "classic", "t_printf"))
    try:
        make_java_app(out)
    except (OSError, subprocess.CalledProcessError) as e:
        print("skipping Java fixture:", e)

    vol = HFS("Cider Test")
    app = vol.mkdir("Cider Test.app")
    vol.add_tree(os.path.join(out, "Cider Test.app"), app)
    vol.add_file("Applications", b"/Applications", symlink=True)
    bg = vol.mkdir(".background")
    vol.add_file("background.png", bytes(range(256)) * 64, bg)
    vol.add_file(".DS_Store", b"\0\0\0\1Bud1" + bytes(100))
    udif(vol.build(), os.path.join(out, "CiderTest.dmg"))
    print("fixtures written to", out)


if __name__ == "__main__":
    main()
