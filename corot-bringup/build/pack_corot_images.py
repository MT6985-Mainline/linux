#!/usr/bin/env python3
"""Pack GKI v4 boot.img (kernel only) and init_boot.img (ramdisk only).

Android boot image v3/v4 layout: the 1584-byte header occupies a whole
4096-byte page; kernel and ramdisk start at the NEXT page boundary.
The first attempt packed data right after the 1584-byte header and LK
panicked with "No magic number of compression is found".
"""
from pathlib import Path
import struct
import subprocess

WORK = Path("/home/mytiantian/corot-work")
KERNEL_TREE = Path("/home/mytiantian/linux-corot")
OUT = WORK / "images"
OUT.mkdir(parents=True, exist_ok=True)

BOOT_MAGIC = b"ANDROID!"
HEADER_SIZE_V4 = 1584
PAGE = 4096


def align(n, page=PAGE):
    return (n + page - 1) // page * page


def pack_v4(path, kernel: bytes, ramdisk: bytes):
    header = bytearray(PAGE)
    hdr = bytearray(HEADER_SIZE_V4)
    hdr[0:8] = BOOT_MAGIC
    struct.pack_into("<I", hdr, 8, len(kernel))
    struct.pack_into("<I", hdr, 12, len(ramdisk))
    struct.pack_into("<I", hdr, 16, 0)  # os_version
    struct.pack_into("<I", hdr, 20, HEADER_SIZE_V4)
    struct.pack_into("<I", hdr, 40, 4)  # header_version
    header[0:HEADER_SIZE_V4] = hdr
    blob = bytes(header)
    if kernel:
        blob += kernel
        blob += b"\x00" * (align(len(blob)) - len(blob))
    if ramdisk:
        blob += ramdisk
        blob += b"\x00" * (align(len(blob)) - len(blob))
    path.write_bytes(blob)
    return path.stat().st_size


image = (KERNEL_TREE / "arch/arm64/boot/Image").read_bytes()
(OUT / "Image").write_bytes(image)
dtb = (KERNEL_TREE / "arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dtb").read_bytes()
(OUT / "mt6985-xiaomi-corot.dtb").write_bytes(dtb)

lz4 = OUT / "Image.lz4"
subprocess.check_call(["lz4", "-l", "-12", "-f", str(OUT / "Image"), str(lz4)])
lz4_bytes = lz4.read_bytes()
if lz4_bytes[:4] != bytes.fromhex("02214c18"):
    raise SystemExit(f"unexpected lz4 magic {lz4_bytes[:4].hex()}")

ramdisk = (WORK / "corot-initramfs/initramfs.cpio.lz4").read_bytes()

boot_sz = pack_v4(OUT / "corot-boot.img", lz4_bytes, b"")
init_path = OUT / "corot-init_boot.img"
init_sz = pack_v4(init_path, b"", ramdisk)
init_boot_size = 8 * 1024 * 1024
if init_sz > init_boot_size:
    raise SystemExit(f"init_boot exceeds fixed 8 MiB size: {init_sz}")
init_path.write_bytes(init_path.read_bytes() + b"\x00" * (init_boot_size - init_sz))
init_sz = init_boot_size

print(f"Image {len(image)} Image.lz4 {len(lz4_bytes)} dtb {len(dtb)} ramdisk {len(ramdisk)}")
print(f"corot-boot.img {boot_sz} (header page 4096, kernel only)")
print(f"corot-init_boot.img {init_sz} (fixed 8 MiB, mainline initramfs)")
