#!/usr/bin/env python3
"""extract-builtin-fw.py — dump the firmware blobs built into a Linux kernel (CONFIG_EXTRA_FIRMWARE).

    usage: extract-builtin-fw.py <vmlinux> <out-dir>
    (get vmlinux from a bzImage with: linux/scripts/extract-vmlinux bzImage > vmlinux)

Why: the Strawberry PS4 kernel build needs firmware blobs (Sony GPU microcode gladius_*/liverpool_*,
Marvell wifi sd8797/sd8897) that build.sh can't download — mrvl/sd8797_uapsta.bin is "useCustomFirmware".
A running PS4 kernel already carries them, so we lift them byte-for-byte from the kernel we know works.

Layout (include/linux/firmware.h): struct builtin_fw { const char *name; void *data; unsigned long size; }
in section .builtin_fw; name/data point into .rodata (virtual addresses).
"""
import hashlib, os, struct, sys

vmlinux, out = sys.argv[1], sys.argv[2]
blob = open(vmlinux, "rb").read()
assert blob[:4] == b"\x7fELF" and blob[4] == 2, "need a 64-bit ELF vmlinux"

e_shoff, = struct.unpack_from("<Q", blob, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", blob, 0x3A)
secs = []
for i in range(e_shnum):
    name, typ, flags, addr, off, size = struct.unpack_from("<IIQQQQ", blob, e_shoff + i * e_shentsize)
    secs.append([name, typ, addr, off, size])
strtab_off = secs[e_shstrndx][3]
for s in secs:
    end = blob.index(b"\0", strtab_off + s[0])
    s[0] = blob[strtab_off + s[0]:end].decode()

def va2off(va):
    for name, typ, addr, off, size in secs:
        if typ != 8 and addr and addr <= va < addr + size:   # 8 = SHT_NOBITS
            return off + (va - addr)
    raise ValueError(f"address {va:#x} not in any file-backed section")

fw = next(s for s in secs if s[0] == ".builtin_fw")
n = fw[4] // 24
print(f".builtin_fw @ {fw[2]:#x}: {n} entries")
os.makedirs(out, exist_ok=True)
for i in range(n):
    name_va, data_va, size = struct.unpack_from("<QQQ", blob, fw[3] + i * 24)
    o = va2off(name_va)
    name = blob[o:blob.index(b"\0", o)].decode()
    data = blob[va2off(data_va):va2off(data_va) + size]
    dest = os.path.join(out, name)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    open(dest, "wb").write(data)
    print(f"{hashlib.sha256(data).hexdigest()}  {size:>8}  {name}")
