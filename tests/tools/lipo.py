"""Minimal `lipo -create`: combine thin Mach-O files into a universal binary.

usage: lipo.py OUTPUT INPUT...
"""
import struct
import sys

CPU_NAMES = {0x01000007: "x86_64", 0x0100000C: "arm64"}


def main():
    out, inputs = sys.argv[1], sys.argv[2:]
    slices = []
    for path in inputs:
        data = open(path, "rb").read()
        magic, cputype, cpusubtype = struct.unpack_from("<III", data)
        if magic != 0xFEEDFACF:
            sys.exit(f"{path}: not a thin 64-bit Mach-O")
        slices.append((cputype, cpusubtype, data))

    align = 14  # 16 KiB, as Apple's lipo uses for arm64
    header = struct.pack(">II", 0xCAFEBABE, len(slices))
    offset = 1 << align
    entries, blobs = b"", []
    for cputype, cpusubtype, data in slices:
        entries += struct.pack(">IIIII", cputype, cpusubtype, offset, len(data), align)
        blobs.append((offset, data))
        offset += (len(data) + (1 << align) - 1) & ~((1 << align) - 1)

    image = bytearray(offset)
    image[: len(header) + len(entries)] = header + entries
    for off, data in blobs:
        image[off : off + len(data)] = data
    open(out, "wb").write(image)
    print(f"{out}: universal binary with {', '.join(CPU_NAMES.get(s[0], hex(s[0])) for s in slices)}")


if __name__ == "__main__":
    main()
